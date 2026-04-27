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
    // DirectSound は再生時に PCM の波形フォーマット情報を必要とする。
    WAVEFORMATEX format {};
    std::vector<BYTE> audioBytes;

    const WAVEFORMATEX* Format() const
    {
        return &format;
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
    // DirectSound3D ではなく、距離減衰と左右パンで簡易的な3D表現を行う。
    bool hasSourcePosition = false;
    Vector3 sourcePosition {};
    EffectType effectType = EffectType::None;
    ComPtr<IDirectSoundBuffer8> buffer;
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

// HRESULT をチェックして、失敗時は例外へ変換する。
// DirectSound は多くの API が HRESULT を返すため、
// この関数を通すことで各所の分岐を減らしている。
void ThrowIfFailed(HRESULT result, const char* message)
{
    if (FAILED(result))
    {
        throw std::runtime_error(message);
    }
}

// 初期化前の API 呼び出しを早めに検出する。
void EnsureInitialized()
{
    if (!g_state.initialized)
    {
        throw std::runtime_error("SoundLib is not initialized.");
    }
}

// 公開 API の音量レンジ 0～100 を保証する。
void ValidateVolume(int volume)
{
    if (volume < 0 || volume > 100)
    {
        throw std::out_of_range("Volume must be in the range [0, 100].");
    }
}

// RIFF/WAV のチャンクサイズ読み取り用。
DWORD ReadUInt32(std::ifstream& stream)
{
    DWORD value = 0;
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

// 16bit 値読み取り用の補助関数。
WORD ReadUInt16(std::ifstream& stream)
{
    WORD value = 0;
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

// wav ファイルから PCM フォーマット情報と波形データを取り出す。
// このライブラリは PCM wav をそのまま DirectSound バッファへ書く方針。
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

    // wav は複数のチャンクで構成される。
    // ここでは最低限必要な fmt と data を見つけるまで順に読む。
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
            // wav の fmt チャンクには「サンプリング周波数」「チャンネル数」などが入っている。
            std::vector<BYTE> formatBytes(chunkSize);
            file.read(reinterpret_cast<char*>(formatBytes.data()), chunkSize);

            // PCM の最低限の情報は PCMWAVEFORMAT 分あれば読める。
            if (chunkSize < sizeof(PCMWAVEFORMAT))
            {
                throw std::runtime_error("Unsupported wav format.");
            }

            // fmt チャンクが標準 PCM の 16 byte でも読めるように、
            // ある分だけ WAVEFORMATEX にコピーして足りない分は初期値に任せる。
            std::memcpy(&wavFile.format, formatBytes.data(), std::min<size_t>(chunkSize, sizeof(WAVEFORMATEX)));
            if (chunkSize < sizeof(WAVEFORMATEX))
            {
                wavFile.format.cbSize = 0;
            }
            foundFormatChunk = true;
        }
        else if (std::strncmp(chunkId, "data", 4) == 0)
        {
            // data チャンクが実際の波形データ本体。
            wavFile.audioBytes.resize(chunkSize);
            file.read(reinterpret_cast<char*>(wavFile.audioBytes.data()), chunkSize);
            foundDataChunk = true;
        }
        else
        {
            file.seekg(chunkSize, std::ios::cur);
        }

        // RIFF のチャンクは偶数境界に並ぶので、奇数サイズ後の 1 byte を読み飛ばす。
        if ((chunkSize % 2U) != 0U)
        {
            file.seekg(1, std::ios::cur);
        }
    }

    if (!foundFormatChunk || !foundDataChunk)
    {
        throw std::runtime_error("The wav file is missing required chunks.");
    }

    const auto* format = wavFile.Format();
    // このライブラリは PCM wav 専用。
    if (format->wFormatTag != WAVE_FORMAT_PCM)
    {
        throw std::runtime_error("Only PCM wav files are supported.");
    }

    return wavFile;
}

// BGM や環境音のように「必要時ロード」の音をキャッシュつきで取得する。
WavFile& GetStreamedWave(const std::wstring& filePath)
{
    auto iterator = g_state.streamedCache.find(filePath);
    if (iterator == g_state.streamedCache.end())
    {
        iterator = g_state.streamedCache.emplace(filePath, LoadWaveFile(filePath)).first;
    }

    return iterator->second;
}

// 0～100 の使いやすい音量指定を DirectSound の対数スケールへ変換する。
long ConvertVolumeToDirectSound(int volume)
{
    ValidateVolume(volume);

    if (volume == 0)
    {
        return kDirectSoundVolumeMin;
    }

    // DirectSound の音量は 0～100 の線形値ではなく 100分のdB 単位。
    const float normalized = static_cast<float>(volume) / 100.0f;
    const float decibel = 2000.0f * std::log10(normalized);
    long directSoundValue = static_cast<long>(std::lround(decibel));
    directSoundValue = std::clamp(directSoundValue, kDirectSoundVolumeMin, kDirectSoundVolumeMax);
    return directSoundValue;
}

// 以下は簡易3D計算用のベクトル演算。
float Dot(const Vector3& left, const Vector3& right)
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

Vector3 Subtract(const Vector3& left, const Vector3& right)
{
    return { left.x - right.x, left.y - right.y, left.z - right.z };
}

Vector3 Cross(const Vector3& left, const Vector3& right)
{
    return {
        (left.y * right.z) - (left.z * right.y),
        (left.z * right.x) - (left.x * right.z),
        (left.x * right.y) - (left.y * right.x)
    };
}

float Length(const Vector3& value)
{
    return std::sqrt(Dot(value, value));
}

Vector3 Normalize(const Vector3& value)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return { 0.0f, 0.0f, 0.0f };
    }

    return { value.x / length, value.y / length, value.z / length };
}

// 秒指定の再生開始位置を、DirectSound バッファ内の byte 位置へ変換する。
DWORD SecondsToBufferOffset(const WavFile& wavFile, float seconds)
{
    if (seconds <= 0.0f)
    {
        return 0;
    }

    const WAVEFORMATEX* format = wavFile.Format();
    const DWORD bytesPerSecond = format->nAvgBytesPerSec;
    // 再生開始位置はサンプル境界に合わせる必要があるため blockAlign で丸める。
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

// エフェクト種別に応じた簡易音量補正。
int ApplyEffectVolumeAdjustment(int volume, EffectType effectType)
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

    return adjustedVolume;
}

// DirectSound バッファへ音量を反映する。
void SetBufferVolume(IDirectSoundBuffer8& buffer, int volume, EffectType effectType)
{
    const int adjustedVolume = ApplyEffectVolumeAdjustment(volume, effectType);
    ThrowIfFailed(buffer.SetVolume(ConvertVolumeToDirectSound(adjustedVolume)), "Failed to set buffer volume.");
}

// DirectSound バッファへ左右パンを反映する。
void SetBufferPan(IDirectSoundBuffer8& buffer, long pan)
{
    ThrowIfFailed(buffer.SetPan(std::clamp(pan, -10000L, 10000L)), "Failed to set buffer pan.");
}

// 音源位置がある音に対して、距離減衰と左右パンを設定する。
// DirectSound3D を使わず、ステレオ wav でも動くようにしている。
void ApplySpatialSettings(Voice& voice, int volume)
{
    if (!voice.hasSourcePosition)
    {
        SetBufferVolume(*voice.buffer.Get(), volume, voice.effectType);
        SetBufferPan(*voice.buffer.Get(), 0);
        return;
    }

    // 音源とリスナーの相対位置から、音量減衰と左右パンを決める。
    const Vector3 relative = Subtract(voice.sourcePosition, g_state.listenerPosition);
    const float distance = Length(relative);
    // 本格的な 3D API ではなくても、距離で音量を落とすだけでかなりそれらしく聞こえる。
    const float attenuation = 1.0f / (1.0f + (distance / 12.0f));
    const int attenuatedVolume = std::clamp(static_cast<int>(std::lround(volume * attenuation)), 0, 100);

    const Vector3 listenerRight = Normalize(Cross(g_state.listenerTop, g_state.listenerFront));
    const Vector3 relativeDirection = Normalize(relative);
    const float panRatio = std::clamp(Dot(relativeDirection, listenerRight), -1.0f, 1.0f);
    const long pan = static_cast<long>(std::lround(panRatio * 10000.0f));

    SetBufferVolume(*voice.buffer.Get(), attenuatedVolume, voice.effectType);
    SetBufferPan(*voice.buffer.Get(), pan);
}

// エフェクト種別ごとの簡易音色変化。
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

// 1回の再生インスタンスに対応する secondary buffer を生成する。
// 多重再生したい時は、同じ wav でもバッファを複数本用意する必要がある。
ComPtr<IDirectSoundBuffer8> CreateBuffer(const WavFile& wavFile, bool loop)
{
    DSBUFFERDESC description {};
    description.dwSize = sizeof(description);
    // secondary buffer は「1回の再生インスタンス」に相当する。
    // 効果音を同時に複数鳴らすときは、このバッファを必要数ぶん作る。
    description.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_GLOBALFOCUS;
    description.dwBufferBytes = static_cast<DWORD>(wavFile.audioBytes.size());
    description.lpwfxFormat = const_cast<WAVEFORMATEX*>(wavFile.Format());

    // ループ用途ではカーソル取得の扱いが少し安定するフラグを足している。
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

    // Lock/Unlock でバッファメモリを取得し、wav の生データを書き込む。
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

    // 作成した DirectSound バッファの中へ wav データをそのまま書き込む。
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

// 再生位置を先頭へ戻してから再生開始する。
void StartBuffer(IDirectSoundBuffer8& buffer, bool loop)
{
    DWORD playFlags = 0;
    if (loop)
    {
        playFlags = DSBPLAY_LOOPING;
    }

    ThrowIfFailed(buffer.SetCurrentPosition(0), "Failed to reset the play cursor.");
    ThrowIfFailed(buffer.Play(0, 0, playFlags), "Failed to start the buffer.");
}

// 再生済みの voice を一覧から回収する。
void RefreshFinishedVoices(std::vector<Voice>& voices)
{
    // 再生が終わった voice は一覧から外し、空いた枠を次の効果音に再利用する。
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

// 外部へ返す再生IDを発番する。
int AcquireVoiceId()
{
    return g_state.nextVoiceId++;
}

// 再生IDから対象 voice を探す。
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

// フェード開始情報を記録する。
void BeginFade(FadeState& fade, int currentVolume, int targetVolume, bool stopWhenDone)
{
    // フェードは別スレッドを使わず、Update ごとに少しずつ音量を変えて実現する。
    fade.active = true;
    fade.stopWhenDone = stopWhenDone;
    fade.startedAt = timeGetTime();
    fade.durationMs = kFadeDurationMilliseconds;
    fade.startVolume = currentVolume;
    fade.targetVolume = targetVolume;
}

// フェード中の現在音量を線形補間で求める。
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

// 効果音 / 環境音 1本ぶんのフェード更新。
void UpdateVoiceFade(Voice& voice)
{
    if (!voice.fade.active)
    {
        return;
    }

    // 現在時刻から補間して、その時点の音量を計算する。
    const int currentVolume = CalculateCurrentFadeVolume(voice.fade);
    ApplySpatialSettings(voice, currentVolume);

    const DWORD elapsed = timeGetTime() - voice.fade.startedAt;
    if (elapsed < voice.fade.durationMs)
    {
        return;
    }

    voice.fade.active = false;
    voice.targetVolume = voice.fade.targetVolume;
    ApplySpatialSettings(voice, voice.targetVolume);

    if (voice.fade.stopWhenDone)
    {
        ThrowIfFailed(voice.buffer->Stop(), "Failed to stop a voice.");
    }
}

// 環境音一覧のフェードと空間化を更新する。
void UpdateEnvironmentFades()
{
    for (auto& voice : g_state.environmentSounds)
    {
        UpdateVoiceFade(voice);
        if (!voice.fade.active)
        {
            ApplySpatialSettings(voice, voice.targetVolume);
        }
    }

    RefreshFinishedVoices(g_state.environmentSounds);
}

// 効果音一覧の空間化を更新する。
void UpdateSoundEffectSpatialization()
{
    for (auto& voice : g_state.soundEffects)
    {
        if (!voice.fade.active)
        {
            ApplySpatialSettings(voice, voice.targetVolume);
        }
    }
}

// BGM を停止し、保持中バッファも解放する。
void StopAndReleaseBgm()
{
    if (g_state.bgm.buffer)
    {
        // BGM は常に1本だけなので、停止時にバッファ自体も破棄してよい。
        g_state.bgm.buffer->Stop();
        g_state.bgm.buffer.Reset();
    }

    g_state.bgm.filePath.clear();
    g_state.bgm.fade = {};
    g_state.bgm.stopRequested = false;
}

// 次の BGM が予約されていれば、前曲停止後に開始する。
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

    // 別BGMへの切り替えは「今のBGMをフェードアウト → 次をフェードイン」でつなぐ。
    SoundLib::SoundLib::PlayBgm(pendingPath, pendingVolume, pendingStartSeconds);
}

// BGM 1本ぶんのフェード更新。
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

// primary buffer と listener を初期化する。
// primary buffer は実データ再生用ではなく、再生デバイス全体の基準設定用。
void ConfigurePrimaryBuffer()
{
    DSBUFFERDESC description {};
    description.dwSize = sizeof(description);
    description.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_PRIMARYBUFFER;

    // primary buffer は実データを持つ再生用ではなく、再生デバイス全体の設定用に使う。
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
    // DirectSound デバイスと primary buffer を用意し、
    // 以後の再生に必要な土台を作る。
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
    // 再生停止とキャッシュ解放、COM 解放をまとめて行う。
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
    // 毎フレーム呼ぶ更新関数。
    // リスナー位置の反映と、フェード・空間化の進行をまとめてここで行う。
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
    UpdateSoundEffectSpatialization();
    UpdateEnvironmentFades();
    UpdateBgmFade();
}

void SoundLib::LoadSoundEffect(const std::wstring& filePath)
{
    EnsureInitialized();
    // 効果音は Play 時にディスクアクセスが入ると遅延しやすいので、先読みしておく。
    g_state.soundEffectCache.try_emplace(filePath, LoadWaveFile(filePath));
}

int SoundLib::PlaySoundEffect(const std::wstring& filePath,
                              int volume,
                              const Vector3* sourcePosition,
                              EffectType effectType)
{
    // 読み込み済み効果音から、新しい再生インスタンスを1本起動する。
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
    voice.hasSourcePosition = (sourcePosition != nullptr);
    if (sourcePosition != nullptr)
    {
        voice.sourcePosition = *sourcePosition;
    }
    voice.effectType = effectType;
    // 同じ効果音を重ねて鳴らすため、Play のたびに独立したバッファを1本確保する。
    voice.buffer = CreateBuffer(wavFile, false);

    ApplyEffectType(*voice.buffer.Get(), *wavFile.Format(), effectType);
    ApplySpatialSettings(voice, volume);
    StartBuffer(*voice.buffer.Get(), false);

    g_state.soundEffects.push_back(std::move(voice));
    return g_state.soundEffects.back().id;
}

void SoundLib::StopSoundEffect(int id)
{
    // 効果音は仕様上フェードなしで即停止。
    EnsureInitialized();

    Voice& voice = FindVoiceById(g_state.soundEffects, id);
    ThrowIfFailed(voice.buffer->Stop(), "Failed to stop a sound effect.");
}

void SoundLib::PlayBgm(const std::wstring& filePath, int volume, float startSeconds)
{
    // BGM は常に1本だけ再生する。
    // 同じ曲の再要求は無視し、別曲ならフェードで切り替える。
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
            // すでに同じBGMなら、仕様どおり何もしない。
            return;
        }

        g_state.bgm.pendingFilePath = filePath;
        g_state.bgm.pendingVolume = volume;
        g_state.bgm.pendingStartSeconds = startSeconds;
        g_state.bgm.hasPendingPlay = true;

        int fadeStartVolume = g_state.bgm.targetVolume;
        if (g_state.bgm.fade.active)
        {
            fadeStartVolume = CalculateCurrentFadeVolume(g_state.bgm.fade);
        }
        BeginFade(g_state.bgm.fade, fadeStartVolume, 0, true);
        return;
    }

    WavFile& wavFile = GetStreamedWave(filePath);

    g_state.bgm.filePath = filePath;
    g_state.bgm.targetVolume = volume;
    // BGM は1本だけをループ再生する。
    g_state.bgm.buffer = CreateBuffer(wavFile, true);
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
    // BGM は急停止ではなくフェードアウト停止。
    EnsureInitialized();

    if (!g_state.bgm.buffer)
    {
        return;
    }

    int fadeStartVolume = g_state.bgm.targetVolume;
    if (g_state.bgm.fade.active)
    {
        fadeStartVolume = CalculateCurrentFadeVolume(g_state.bgm.fade);
    }
    g_state.bgm.hasPendingPlay = false;
    BeginFade(g_state.bgm.fade, fadeStartVolume, 0, true);
}

void SoundLib::SetBgmVolume(int volume)
{
    // BGM の目標音量を更新する。
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
    // 環境音は必要時ロードして、そのまま再生インスタンスを起動する。
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
    voice.hasSourcePosition = (sourcePosition != nullptr);
    if (sourcePosition != nullptr)
    {
        voice.sourcePosition = *sourcePosition;
    }
    voice.effectType = effectType;
    // 環境音も効果音と同様に voice 単位で個別バッファを持つ。
    voice.buffer = CreateBuffer(wavFile, loop);

    ApplyEffectType(*voice.buffer.Get(), *wavFile.Format(), effectType);
    ApplySpatialSettings(voice, 0);
    StartBuffer(*voice.buffer.Get(), loop);
    BeginFade(voice.fade, 0, volume, false);

    g_state.environmentSounds.push_back(std::move(voice));
    return g_state.environmentSounds.back().id;
}

void SoundLib::StopEnvironmentSound(int id)
{
    // 環境音は BGM と同様にフェードアウト停止。
    EnsureInitialized();

    Voice& voice = FindVoiceById(g_state.environmentSounds, id);
    int fadeStartVolume = voice.targetVolume;
    if (voice.fade.active)
    {
        fadeStartVolume = CalculateCurrentFadeVolume(voice.fade);
    }
    BeginFade(voice.fade, fadeStartVolume, 0, true);
}

void SoundLib::SetEnvironmentSoundVolume(int id, int volume)
{
    // 指定した環境音 voice の目標音量を更新する。
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

    ApplySpatialSettings(voice, volume);
}

}
