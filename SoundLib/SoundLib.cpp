#include "SoundLib.h"

#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "winmm.lib")

#include <dsound.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SoundLib
{
namespace
{

using Microsoft::WRL::ComPtr;

constexpr int kMaxSimultaneousSoundEffects = 16;
constexpr int kMaxSimultaneousEnvironmentSounds = 16;
constexpr DWORD kFadeDurationMilliseconds = 600;
constexpr long kDirectSoundVolumeMin = DSBVOLUME_MIN;
constexpr long kDirectSoundVolumeMax = DSBVOLUME_MAX;

struct WavFile
{
    std::vector<BYTE> formatBytes;
    std::vector<BYTE> audioBytes;

    const WAVEFORMATEX* Format() const
    {
        return reinterpret_cast<const WAVEFORMATEX*>(formatBytes.data());
    }
};

struct FadeState
{
    bool active = false;
    bool stopWhenDone = false;
    DWORD startedAt = 0;
    DWORD durationMs = 0;
    int startVolume = 0;
    int targetVolume = 0;
};

struct Voice
{
    int id = -1;
    int targetVolume = 100;
    bool isLoop = false;
    bool isEnvironment = false;
    EffectType effectType = EffectType::None;
    ComPtr<IDirectSoundBuffer8> buffer;
    ComPtr<IDirectSound3DBuffer8> buffer3D;
    FadeState fade;
};

struct BgmState
{
    std::wstring filePath;
    int targetVolume = 100;
    ComPtr<IDirectSoundBuffer8> buffer;
    FadeState fade;
    bool stopRequested = false;

    std::wstring pendingFilePath;
    int pendingVolume = 100;
    float pendingStartSeconds = 0.0f;
    bool hasPendingPlay = false;
};

struct SoundState
{
    bool initialized = false;
    HWND windowHandle = nullptr;
    int nextVoiceId = 1;
    Vector3 listenerPosition {};
    Vector3 listenerFront { 0.0f, 0.0f, 1.0f };
    Vector3 listenerTop { 0.0f, 1.0f, 0.0f };
    ComPtr<IDirectSound8> directSound;
    ComPtr<IDirectSoundBuffer> primaryBuffer;
    ComPtr<IDirectSound3DListener8> listener3D;
    std::unordered_map<std::wstring, WavFile> soundEffectCache;
    std::unordered_map<std::wstring, WavFile> streamedCache;
    std::vector<Voice> soundEffects;
    std::vector<Voice> environmentSounds;
    BgmState bgm;
};

SoundState g_state;

void ThrowIfFailed(HRESULT result, const char* message)
{
    if (FAILED(result))
    {
        throw std::runtime_error(message);
    }
}

void EnsureInitialized()
{
    if (!g_state.initialized)
    {
        throw std::runtime_error("SoundLib is not initialized.");
    }
}

void ValidateVolume(int volume)
{
    if (volume < 0 || volume > 100)
    {
        throw std::out_of_range("Volume must be in the range [0, 100].");
    }
}

DWORD ReadUInt32(std::ifstream& stream)
{
    DWORD value = 0;
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

WORD ReadUInt16(std::ifstream& stream)
{
    WORD value = 0;
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

WavFile LoadWaveFile(const std::wstring& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to open wav file.");
    }

    char riff[4] {};
    file.read(riff, sizeof(riff));
    if (std::strncmp(riff, "RIFF", 4) != 0)
    {
        throw std::runtime_error("The file is not a RIFF wave file.");
    }

    static_cast<void>(ReadUInt32(file));

    char wave[4] {};
    file.read(wave, sizeof(wave));
    if (std::strncmp(wave, "WAVE", 4) != 0)
    {
        throw std::runtime_error("The file is not a WAVE file.");
    }

    WavFile wavFile;
    bool foundFormatChunk = false;
    bool foundDataChunk = false;

    while (file && (!foundFormatChunk || !foundDataChunk))
    {
        char chunkId[4] {};
        file.read(chunkId, sizeof(chunkId));
        if (file.gcount() != sizeof(chunkId))
        {
            break;
        }

        const DWORD chunkSize = ReadUInt32(file);
        if (std::strncmp(chunkId, "fmt ", 4) == 0)
        {
            wavFile.formatBytes.resize(chunkSize);
            file.read(reinterpret_cast<char*>(wavFile.formatBytes.data()), chunkSize);
            foundFormatChunk = true;
        }
        else if (std::strncmp(chunkId, "data", 4) == 0)
        {
            wavFile.audioBytes.resize(chunkSize);
            file.read(reinterpret_cast<char*>(wavFile.audioBytes.data()), chunkSize);
            foundDataChunk = true;
        }
        else
        {
            file.seekg(chunkSize, std::ios::cur);
        }

        if ((chunkSize % 2U) != 0U)
        {
            file.seekg(1, std::ios::cur);
        }
    }

    if (!foundFormatChunk || !foundDataChunk)
    {
        throw std::runtime_error("The wav file is missing required chunks.");
    }

    if (wavFile.formatBytes.size() < sizeof(WAVEFORMATEX))
    {
        throw std::runtime_error("Unsupported wav format.");
    }

    const auto* format = wavFile.Format();
    if (format->wFormatTag != WAVE_FORMAT_PCM)
    {
        throw std::runtime_error("Only PCM wav files are supported.");
    }

    return wavFile;
}

WavFile& GetStreamedWave(const std::wstring& filePath)
{
    auto iterator = g_state.streamedCache.find(filePath);
    if (iterator == g_state.streamedCache.end())
    {
        iterator = g_state.streamedCache.emplace(filePath, LoadWaveFile(filePath)).first;
    }

    return iterator->second;
}

long ConvertVolumeToDirectSound(int volume)
{
    ValidateVolume(volume);

    if (volume == 0)
    {
        return kDirectSoundVolumeMin;
    }

    const float normalized = static_cast<float>(volume) / 100.0f;
    const float decibel = 2000.0f * std::log10(normalized);
    long directSoundValue = static_cast<long>(std::lround(decibel));
    directSoundValue = std::clamp(directSoundValue, kDirectSoundVolumeMin, kDirectSoundVolumeMax);
    return directSoundValue;
}

DWORD SecondsToBufferOffset(const WavFile& wavFile, float seconds)
{
    if (seconds <= 0.0f)
    {
        return 0;
    }

    const WAVEFORMATEX* format = wavFile.Format();
    const DWORD bytesPerSecond = format->nAvgBytesPerSec;
    const DWORD blockAlign = std::max<DWORD>(format->nBlockAlign, 1);

    const double rawOffset = static_cast<double>(bytesPerSecond) * static_cast<double>(seconds);
    DWORD offset = static_cast<DWORD>(rawOffset);
    offset -= offset % blockAlign;

    if (offset >= wavFile.audioBytes.size())
    {
        return 0;
    }

    return offset;
}

void SetBufferVolume(IDirectSoundBuffer8& buffer, int volume, EffectType effectType)
{
    int adjustedVolume = volume;

    switch (effectType)
    {
    case EffectType::Muffle:
        adjustedVolume = std::max(0, volume - 12);
        break;
    case EffectType::Radio:
        adjustedVolume = std::max(0, volume - 18);
        break;
    case EffectType::Cave:
        adjustedVolume = std::max(0, volume - 6);
        break;
    case EffectType::None:
    default:
        break;
    }

    ThrowIfFailed(buffer.SetVolume(ConvertVolumeToDirectSound(adjustedVolume)), "Failed to set buffer volume.");
}

void ApplyEffectType(IDirectSoundBuffer8& buffer, const WAVEFORMATEX& format, EffectType effectType)
{
    DWORD frequency = format.nSamplesPerSec;

    switch (effectType)
    {
    case EffectType::Muffle:
        frequency = static_cast<DWORD>(std::max(100.0f, frequency * 0.75f));
        break;
    case EffectType::Radio:
        frequency = static_cast<DWORD>(std::max(100.0f, frequency * 1.20f));
        break;
    case EffectType::Cave:
        frequency = static_cast<DWORD>(std::max(100.0f, frequency * 0.90f));
        break;
    case EffectType::None:
    default:
        break;
    }

    ThrowIfFailed(buffer.SetFrequency(frequency), "Failed to set buffer frequency.");
}

ComPtr<IDirectSoundBuffer8> CreateBuffer(const WavFile& wavFile, bool use3D, bool loop)
{
    DSBUFFERDESC description {};
    description.dwSize = sizeof(description);
    description.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS;
    description.dwBufferBytes = static_cast<DWORD>(wavFile.audioBytes.size());
    description.lpwfxFormat = const_cast<WAVEFORMATEX*>(wavFile.Format());

    if (use3D)
    {
        description.dwFlags |= DSBCAPS_CTRL3D;
        description.guid3DAlgorithm = DS3DALG_DEFAULT;
    }

    if (loop)
    {
        description.dwFlags |= DSBCAPS_GETCURRENTPOSITION2;
    }

    ComPtr<IDirectSoundBuffer> baseBuffer;
    ThrowIfFailed(g_state.directSound->CreateSoundBuffer(&description, &baseBuffer, nullptr), "Failed to create a sound buffer.");

    ComPtr<IDirectSoundBuffer8> buffer;
    ThrowIfFailed(baseBuffer->QueryInterface(IID_IDirectSoundBuffer8,
                                            reinterpret_cast<void**>(buffer.GetAddressOf())),
                  "Failed to query IDirectSoundBuffer8.");

    void* firstRegion = nullptr;
    void* secondRegion = nullptr;
    DWORD firstRegionSize = 0;
    DWORD secondRegionSize = 0;

    ThrowIfFailed(buffer->Lock(0,
                               description.dwBufferBytes,
                               &firstRegion,
                               &firstRegionSize,
                               &secondRegion,
                               &secondRegionSize,
                               0),
                  "Failed to lock the sound buffer.");

    std::memcpy(firstRegion, wavFile.audioBytes.data(), firstRegionSize);

    if (secondRegion != nullptr && secondRegionSize > 0)
    {
        std::memcpy(secondRegion,
                    wavFile.audioBytes.data() + firstRegionSize,
                    secondRegionSize);
    }

    ThrowIfFailed(buffer->Unlock(firstRegion, firstRegionSize, secondRegion, secondRegionSize),
                  "Failed to unlock the sound buffer.");

    return buffer;
}

void Configure3DBuffer(IDirectSoundBuffer8& buffer,
                       const Vector3* sourcePosition,
                       IDirectSound3DBuffer8** outBuffer3D)
{
    if (outBuffer3D != nullptr)
    {
        *outBuffer3D = nullptr;
    }

    if (sourcePosition == nullptr)
    {
        return;
    }

    ComPtr<IDirectSound3DBuffer8> buffer3D;
    ThrowIfFailed(buffer.QueryInterface(IID_IDirectSound3DBuffer8,
                                        reinterpret_cast<void**>(buffer3D.GetAddressOf())),
                  "Failed to query the 3D sound buffer interface.");

    ThrowIfFailed(buffer3D->SetMode(DS3DMODE_NORMAL, DS3D_DEFERRED), "Failed to set the 3D mode.");
    ThrowIfFailed(buffer3D->SetPosition(sourcePosition->x,
                                        sourcePosition->y,
                                        sourcePosition->z,
                                        DS3D_DEFERRED),
                  "Failed to set the 3D position.");

    if (outBuffer3D != nullptr)
    {
        *outBuffer3D = buffer3D.Detach();
    }
}

void StartBuffer(IDirectSoundBuffer8& buffer, bool loop)
{
    ThrowIfFailed(buffer.SetCurrentPosition(0), "Failed to reset the play cursor.");
    ThrowIfFailed(buffer.Play(0, 0, loop ? DSBPLAY_LOOPING : 0), "Failed to start the buffer.");
}

void RefreshFinishedVoices(std::vector<Voice>& voices)
{
    voices.erase(std::remove_if(voices.begin(),
                                voices.end(),
                                [](Voice& voice)
                                {
                                    if (voice.fade.active)
                                    {
                                        return false;
                                    }

                                    DWORD status = 0;
                                    ThrowIfFailed(voice.buffer->GetStatus(&status), "Failed to query a voice status.");
                                    return (status & DSBSTATUS_PLAYING) == 0;
                                }),
                 voices.end());
}

int AcquireVoiceId()
{
    return g_state.nextVoiceId++;
}

Voice& FindVoiceById(std::vector<Voice>& voices, int id)
{
    auto iterator = std::find_if(voices.begin(),
                                 voices.end(),
                                 [id](const Voice& voice)
                                 {
                                     return voice.id == id;
                                 });

    if (iterator == voices.end())
    {
        throw std::runtime_error("The specified voice ID was not found.");
    }

    return *iterator;
}

void BeginFade(FadeState& fade, int currentVolume, int targetVolume, bool stopWhenDone)
{
    fade.active = true;
    fade.stopWhenDone = stopWhenDone;
    fade.startedAt = timeGetTime();
    fade.durationMs = kFadeDurationMilliseconds;
    fade.startVolume = currentVolume;
    fade.targetVolume = targetVolume;
}

int CalculateCurrentFadeVolume(const FadeState& fade)
{
    if (!fade.active || fade.durationMs == 0)
    {
        return fade.targetVolume;
    }

    const DWORD now = timeGetTime();
    const DWORD elapsed = now - fade.startedAt;
    const float t = std::min(1.0f, static_cast<float>(elapsed) / static_cast<float>(fade.durationMs));
    const float volume = static_cast<float>(fade.startVolume) +
        (static_cast<float>(fade.targetVolume - fade.startVolume) * t);
    return static_cast<int>(std::lround(volume));
}

void UpdateVoiceFade(Voice& voice)
{
    if (!voice.fade.active)
    {
        return;
    }

    const int currentVolume = CalculateCurrentFadeVolume(voice.fade);
    SetBufferVolume(*voice.buffer.Get(), currentVolume, voice.effectType);

    const DWORD elapsed = timeGetTime() - voice.fade.startedAt;
    if (elapsed < voice.fade.durationMs)
    {
        return;
    }

    voice.fade.active = false;
    voice.targetVolume = voice.fade.targetVolume;
    SetBufferVolume(*voice.buffer.Get(), voice.targetVolume, voice.effectType);

    if (voice.fade.stopWhenDone)
    {
        ThrowIfFailed(voice.buffer->Stop(), "Failed to stop a voice.");
    }
}

void UpdateEnvironmentFades()
{
    for (auto& voice : g_state.environmentSounds)
    {
        UpdateVoiceFade(voice);
    }

    RefreshFinishedVoices(g_state.environmentSounds);
}

void StopAndReleaseBgm()
{
    if (g_state.bgm.buffer)
    {
        g_state.bgm.buffer->Stop();
        g_state.bgm.buffer.Reset();
    }

    g_state.bgm.filePath.clear();
    g_state.bgm.fade = {};
    g_state.bgm.stopRequested = false;
}

void StartPendingBgmIfNeeded()
{
    if (!g_state.bgm.hasPendingPlay)
    {
        return;
    }

    const std::wstring pendingPath = g_state.bgm.pendingFilePath;
    const int pendingVolume = g_state.bgm.pendingVolume;
    const float pendingStartSeconds = g_state.bgm.pendingStartSeconds;

    g_state.bgm.hasPendingPlay = false;
    g_state.bgm.pendingFilePath.clear();

    SoundLib::SoundLib::PlayBgm(pendingPath, pendingVolume, pendingStartSeconds);
}

void UpdateBgmFade()
{
    if (!g_state.bgm.buffer || !g_state.bgm.fade.active)
    {
        return;
    }

    const int currentVolume = CalculateCurrentFadeVolume(g_state.bgm.fade);
    SetBufferVolume(*g_state.bgm.buffer.Get(), currentVolume, EffectType::None);

    const DWORD elapsed = timeGetTime() - g_state.bgm.fade.startedAt;
    if (elapsed < g_state.bgm.fade.durationMs)
    {
        return;
    }

    const bool shouldStop = g_state.bgm.fade.stopWhenDone;
    g_state.bgm.fade.active = false;
    g_state.bgm.targetVolume = g_state.bgm.fade.targetVolume;

    if (shouldStop)
    {
        StopAndReleaseBgm();
        StartPendingBgmIfNeeded();
        return;
    }

    SetBufferVolume(*g_state.bgm.buffer.Get(), g_state.bgm.targetVolume, EffectType::None);
}

void ConfigurePrimaryBuffer()
{
    DSBUFFERDESC description {};
    description.dwSize = sizeof(description);
    description.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_PRIMARYBUFFER;

    ThrowIfFailed(g_state.directSound->CreateSoundBuffer(&description,
                                                         g_state.primaryBuffer.GetAddressOf(),
                                                         nullptr),
                  "Failed to create the primary sound buffer.");

    WAVEFORMATEX format {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    ThrowIfFailed(g_state.primaryBuffer->SetFormat(&format), "Failed to set the primary format.");
    ThrowIfFailed(g_state.primaryBuffer->QueryInterface(IID_IDirectSound3DListener,
                                                        reinterpret_cast<void**>(g_state.listener3D.GetAddressOf())),
                  "Failed to query the listener interface.");

    ThrowIfFailed(g_state.listener3D->SetDistanceFactor(1.0f, DS3D_IMMEDIATE), "Failed to set the listener distance factor.");
    ThrowIfFailed(g_state.listener3D->SetRolloffFactor(1.0f, DS3D_IMMEDIATE), "Failed to set the listener rolloff factor.");
    ThrowIfFailed(g_state.listener3D->SetDopplerFactor(1.0f, DS3D_IMMEDIATE), "Failed to set the listener doppler factor.");
}

template <typename T>
void ResetContainer(T& container)
{
    container.clear();
}

}

void SoundLib::Initialize(HWND windowHandle)
{
    if (g_state.initialized)
    {
        return;
    }

    if (windowHandle == nullptr)
    {
        throw std::invalid_argument("A valid window handle is required.");
    }

    g_state.windowHandle = windowHandle;

    ThrowIfFailed(DirectSoundCreate8(nullptr, g_state.directSound.GetAddressOf(), nullptr),
                  "Failed to create DirectSound8.");
    ThrowIfFailed(g_state.directSound->SetCooperativeLevel(windowHandle, DSSCL_PRIORITY),
                  "Failed to set the cooperative level.");

    ConfigurePrimaryBuffer();

    g_state.soundEffects.reserve(kMaxSimultaneousSoundEffects);
    g_state.environmentSounds.reserve(kMaxSimultaneousEnvironmentSounds);
    g_state.initialized = true;
}

void SoundLib::Finalize()
{
    if (!g_state.initialized)
    {
        return;
    }

    for (auto& voice : g_state.soundEffects)
    {
        if (voice.buffer)
        {
            voice.buffer->Stop();
        }
    }

    for (auto& voice : g_state.environmentSounds)
    {
        if (voice.buffer)
        {
            voice.buffer->Stop();
        }
    }

    StopAndReleaseBgm();
    ResetContainer(g_state.soundEffects);
    ResetContainer(g_state.environmentSounds);
    ResetContainer(g_state.soundEffectCache);
    ResetContainer(g_state.streamedCache);
    g_state.listener3D.Reset();
    g_state.primaryBuffer.Reset();
    g_state.directSound.Reset();
    g_state.initialized = false;
    g_state.windowHandle = nullptr;
    g_state.nextVoiceId = 1;
}

void SoundLib::Update(const Vector3& listenerPosition,
                      const Vector3& listenerFront,
                      const Vector3& listenerTop)
{
    EnsureInitialized();

    g_state.listenerPosition = listenerPosition;
    g_state.listenerFront = listenerFront;
    g_state.listenerTop = listenerTop;

    ThrowIfFailed(g_state.listener3D->SetPosition(listenerPosition.x,
                                                  listenerPosition.y,
                                                  listenerPosition.z,
                                                  DS3D_DEFERRED),
                  "Failed to update the listener position.");
    ThrowIfFailed(g_state.listener3D->SetOrientation(listenerFront.x,
                                                     listenerFront.y,
                                                     listenerFront.z,
                                                     listenerTop.x,
                                                     listenerTop.y,
                                                     listenerTop.z,
                                                     DS3D_DEFERRED),
                  "Failed to update the listener orientation.");
    ThrowIfFailed(g_state.listener3D->CommitDeferredSettings(), "Failed to commit deferred 3D settings.");

    RefreshFinishedVoices(g_state.soundEffects);
    UpdateEnvironmentFades();
    UpdateBgmFade();
}

void SoundLib::LoadSoundEffect(const std::wstring& filePath)
{
    EnsureInitialized();
    g_state.soundEffectCache.try_emplace(filePath, LoadWaveFile(filePath));
}

int SoundLib::PlaySoundEffect(const std::wstring& filePath,
                              int volume,
                              const Vector3* sourcePosition,
                              EffectType effectType)
{
    EnsureInitialized();
    ValidateVolume(volume);

    auto cacheIterator = g_state.soundEffectCache.find(filePath);
    if (cacheIterator == g_state.soundEffectCache.end())
    {
        throw std::runtime_error("The requested sound effect has not been loaded.");
    }

    RefreshFinishedVoices(g_state.soundEffects);
    if (g_state.soundEffects.size() >= kMaxSimultaneousSoundEffects)
    {
        throw std::runtime_error("The sound effect voice limit was exceeded.");
    }

    const WavFile& wavFile = cacheIterator->second;
    Voice voice {};
    voice.id = AcquireVoiceId();
    voice.targetVolume = volume;
    voice.effectType = effectType;
    voice.buffer = CreateBuffer(wavFile, sourcePosition != nullptr, false);

    IDirectSound3DBuffer8* raw3D = nullptr;
    Configure3DBuffer(*voice.buffer.Get(), sourcePosition, &raw3D);
    voice.buffer3D.Attach(raw3D);

    ApplyEffectType(*voice.buffer.Get(), *wavFile.Format(), effectType);
    SetBufferVolume(*voice.buffer.Get(), volume, effectType);
    StartBuffer(*voice.buffer.Get(), false);

    g_state.soundEffects.push_back(std::move(voice));
    return g_state.soundEffects.back().id;
}

void SoundLib::StopSoundEffect(int id)
{
    EnsureInitialized();

    Voice& voice = FindVoiceById(g_state.soundEffects, id);
    ThrowIfFailed(voice.buffer->Stop(), "Failed to stop a sound effect.");
}

void SoundLib::PlayBgm(const std::wstring& filePath, int volume, float startSeconds)
{
    EnsureInitialized();
    ValidateVolume(volume);

    if (startSeconds < 0.0f)
    {
        throw std::out_of_range("The start time must not be negative.");
    }

    if (g_state.bgm.buffer)
    {
        if (g_state.bgm.filePath == filePath)
        {
            return;
        }

        g_state.bgm.pendingFilePath = filePath;
        g_state.bgm.pendingVolume = volume;
        g_state.bgm.pendingStartSeconds = startSeconds;
        g_state.bgm.hasPendingPlay = true;

        const int fadeStartVolume = g_state.bgm.fade.active ?
            CalculateCurrentFadeVolume(g_state.bgm.fade) :
            g_state.bgm.targetVolume;
        BeginFade(g_state.bgm.fade, fadeStartVolume, 0, true);
        return;
    }

    WavFile& wavFile = GetStreamedWave(filePath);

    g_state.bgm.filePath = filePath;
    g_state.bgm.targetVolume = volume;
    g_state.bgm.buffer = CreateBuffer(wavFile, false, true);
    g_state.bgm.stopRequested = false;

    ApplyEffectType(*g_state.bgm.buffer.Get(), *wavFile.Format(), EffectType::None);
    SetBufferVolume(*g_state.bgm.buffer.Get(), 0, EffectType::None);
    ThrowIfFailed(g_state.bgm.buffer->SetCurrentPosition(SecondsToBufferOffset(wavFile, startSeconds)),
                  "Failed to set the BGM start position.");
    ThrowIfFailed(g_state.bgm.buffer->Play(0, 0, DSBPLAY_LOOPING), "Failed to start BGM playback.");

    BeginFade(g_state.bgm.fade, 0, volume, false);
}

void SoundLib::StopBgm()
{
    EnsureInitialized();

    if (!g_state.bgm.buffer)
    {
        return;
    }

    const int fadeStartVolume = g_state.bgm.fade.active ?
        CalculateCurrentFadeVolume(g_state.bgm.fade) :
        g_state.bgm.targetVolume;
    g_state.bgm.hasPendingPlay = false;
    BeginFade(g_state.bgm.fade, fadeStartVolume, 0, true);
}

void SoundLib::SetBgmVolume(int volume)
{
    EnsureInitialized();
    ValidateVolume(volume);

    g_state.bgm.targetVolume = volume;

    if (!g_state.bgm.buffer)
    {
        return;
    }

    if (g_state.bgm.fade.active)
    {
        const int currentVolume = CalculateCurrentFadeVolume(g_state.bgm.fade);
        BeginFade(g_state.bgm.fade, currentVolume, volume, false);
        return;
    }

    SetBufferVolume(*g_state.bgm.buffer.Get(), volume, EffectType::None);
}

int SoundLib::PlayEnvironmentSound(const std::wstring& filePath,
                                   int volume,
                                   const Vector3* sourcePosition,
                                   EffectType effectType,
                                   bool loop)
{
    EnsureInitialized();
    ValidateVolume(volume);

    RefreshFinishedVoices(g_state.environmentSounds);
    if (g_state.environmentSounds.size() >= kMaxSimultaneousEnvironmentSounds)
    {
        throw std::runtime_error("The environment sound voice limit was exceeded.");
    }

    WavFile& wavFile = GetStreamedWave(filePath);

    Voice voice {};
    voice.id = AcquireVoiceId();
    voice.targetVolume = volume;
    voice.isLoop = loop;
    voice.isEnvironment = true;
    voice.effectType = effectType;
    voice.buffer = CreateBuffer(wavFile, sourcePosition != nullptr, loop);

    IDirectSound3DBuffer8* raw3D = nullptr;
    Configure3DBuffer(*voice.buffer.Get(), sourcePosition, &raw3D);
    voice.buffer3D.Attach(raw3D);

    ApplyEffectType(*voice.buffer.Get(), *wavFile.Format(), effectType);
    SetBufferVolume(*voice.buffer.Get(), 0, effectType);
    StartBuffer(*voice.buffer.Get(), loop);
    BeginFade(voice.fade, 0, volume, false);

    g_state.environmentSounds.push_back(std::move(voice));
    return g_state.environmentSounds.back().id;
}

void SoundLib::StopEnvironmentSound(int id)
{
    EnsureInitialized();

    Voice& voice = FindVoiceById(g_state.environmentSounds, id);
    const int fadeStartVolume = voice.fade.active ?
        CalculateCurrentFadeVolume(voice.fade) :
        voice.targetVolume;
    BeginFade(voice.fade, fadeStartVolume, 0, true);
}

void SoundLib::SetEnvironmentSoundVolume(int id, int volume)
{
    EnsureInitialized();
    ValidateVolume(volume);

    Voice& voice = FindVoiceById(g_state.environmentSounds, id);
    voice.targetVolume = volume;

    if (voice.fade.active)
    {
        const int currentVolume = CalculateCurrentFadeVolume(voice.fade);
        BeginFade(voice.fade, currentVolume, volume, false);
        return;
    }

    SetBufferVolume(*voice.buffer.Get(), volume, voice.effectType);
}

}
