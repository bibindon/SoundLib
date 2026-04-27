#ifndef NOMINMAX
#define NOMINMAX
#endif

#pragma comment( lib, "d3d9.lib" )
#if defined(DEBUG) || defined(_DEBUG)
#pragma comment( lib, "d3dx9d.lib" )
#else
#pragma comment( lib, "d3dx9.lib" )
#endif

#include <d3d9.h>
#include <d3dx9.h>
#include <tchar.h>

#include <algorithm>
#include <cassert>
#include <crtdbg.h>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "..\SoundLib\SoundLib.h"

#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = NULL; } }

const int WINDOW_SIZE_W = 1600;
const int WINDOW_SIZE_H = 900;

LPDIRECT3D9 g_pD3D = NULL;
LPDIRECT3DDEVICE9 g_pd3dDevice = NULL;
LPD3DXFONT g_pFont = NULL;
LPD3DXMESH g_pMesh = NULL;
std::vector<D3DMATERIAL9> g_pMaterials;
std::vector<LPDIRECT3DTEXTURE9> g_pTextures;
DWORD g_dwNumMaterials = 0;
LPD3DXEFFECT g_pEffect = NULL;
bool g_bClose = false;

struct SampleAssets
{
    std::vector<std::filesystem::path> soundEffects;
    std::vector<std::filesystem::path> bgms;
    std::vector<std::filesystem::path> environments;
};

SampleAssets g_assets;
std::wstring g_audioError;
std::wstring g_statusMessage = L"Ready";
SoundLib::Vector3 g_listenerPosition { 0.0f, 3.0f, -18.0f };
SoundLib::Vector3 g_listenerFront { 0.0f, 0.0f, 1.0f };
SoundLib::Vector3 g_listenerTop { 0.0f, 1.0f, 0.0f };
bool g_isBgmPlaying = false;
size_t g_bgmIndex = 0;
int g_bgmVolume = 75;
int g_leftEnvironmentId = -1;
int g_rightEnvironmentId = -1;

static void TextDraw(LPD3DXFONT pFont, const TCHAR* text, int x, int y);
static void InitD3D(HWND hWnd);
static void InitAudio(HWND hWnd);
static void Cleanup();
static void Render();
static void HandleKeyDown(WPARAM key);
static SampleAssets DiscoverSampleAssets();
static std::wstring FileNameOnly(const std::filesystem::path& path);
static std::filesystem::path FindResourceDirectory();
static std::string WideToUtf8(const std::wstring& value);
static void UpdateListenerPosition(float deltaX, float deltaZ);
static std::wstring BuildUiText();
LRESULT WINAPI MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern int WINAPI _tWinMain(_In_ HINSTANCE hInstance,
                            _In_opt_ HINSTANCE hPrevInstance,
                            _In_ LPTSTR lpCmdLine,
                            _In_ int nCmdShow);

int WINAPI _tWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPTSTR lpCmdLine,
                     _In_ int nCmdShow)
{
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

    WNDCLASSEX wc { };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = MsgProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = _T("Window1");
    wc.hIconSm = NULL;

    ATOM atom = RegisterClassEx(&wc);
    assert(atom != 0);

    RECT rect;
    SetRect(&rect, 0, 0, WINDOW_SIZE_W, WINDOW_SIZE_H);
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    rect.right = rect.right - rect.left;
    rect.bottom = rect.bottom - rect.top;
    rect.top = 0;
    rect.left = 0;

    HWND hWnd = CreateWindow(_T("Window1"),
                             _T("SoundLib Sample"),
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT,
                             CW_USEDEFAULT,
                             rect.right,
                             rect.bottom,
                             NULL,
                             NULL,
                             wc.hInstance,
                             NULL);

    InitD3D(hWnd);
    InitAudio(hWnd);
    ShowWindow(hWnd, SW_SHOWDEFAULT);
    UpdateWindow(hWnd);

    MSG msg;

    while (true)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            DispatchMessage(&msg);
        }
        else
        {
            Sleep(16);
            Render();
        }

        if (g_bClose)
        {
            break;
        }
    }

    Cleanup();

    UnregisterClass(_T("Window1"), wc.hInstance);
    return 0;
}

void TextDraw(LPD3DXFONT pFont, const TCHAR* text, int x, int y)
{
    RECT rect = { x, y, 0, 0 };

    HRESULT hResult = pFont->DrawText(NULL,
                                      text,
                                      -1,
                                      &rect,
                                      DT_LEFT | DT_NOCLIP,
                                      D3DCOLOR_ARGB(255, 0, 0, 0));

    assert((int)hResult >= 0);
}

SampleAssets DiscoverSampleAssets()
{
    SampleAssets assets;
    std::vector<std::pair<std::filesystem::path, uintmax_t>> nonEnvironmentSounds;
    const std::filesystem::path baseDirectory = FindResourceDirectory();

    for (const auto& entry : std::filesystem::directory_iterator(baseDirectory))
    {
        if (!entry.is_regular_file() || entry.path().extension() != L".wav")
        {
            continue;
        }

        const std::wstring fileName = entry.path().filename().wstring();
        const uintmax_t fileSize = entry.file_size();

        if (fileName.find(L"ENV") != std::wstring::npos)
        {
            assets.environments.push_back(entry.path());
        }
        else
        {
            nonEnvironmentSounds.emplace_back(entry.path(), fileSize);
        }
    }

    std::sort(assets.environments.begin(), assets.environments.end());

    std::sort(nonEnvironmentSounds.begin(),
              nonEnvironmentSounds.end(),
              [](const auto& left, const auto& right)
              {
                  return left.second < right.second;
              });

    for (size_t i = 0; i < std::min<size_t>(3, nonEnvironmentSounds.size()); ++i)
    {
        assets.soundEffects.push_back(nonEnvironmentSounds[i].first);
    }

    std::sort(nonEnvironmentSounds.begin(),
              nonEnvironmentSounds.end(),
              [](const auto& left, const auto& right)
              {
                  return left.second > right.second;
              });

    for (size_t i = 0; i < std::min<size_t>(3, nonEnvironmentSounds.size()); ++i)
    {
        assets.bgms.push_back(nonEnvironmentSounds[i].first);
    }

    return assets;
}

std::wstring FileNameOnly(const std::filesystem::path& path)
{
    return path.filename().wstring();
}

std::filesystem::path FindResourceDirectory()
{
    wchar_t modulePath[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (length == 0 || length >= std::size(modulePath))
    {
        throw std::runtime_error("Failed to resolve the module path.");
    }

    std::filesystem::path executablePath(modulePath);
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(executablePath.parent_path() / L"res");
    candidates.push_back(executablePath.parent_path().parent_path() / L"res");
    candidates.push_back(executablePath.parent_path().parent_path().parent_path() / L"res");
    candidates.push_back(executablePath.parent_path().parent_path().parent_path() / L"SoundLib" / L"res");

    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate))
        {
            return candidate;
        }
    }

    std::wstringstream stream;
    stream << L"Could not find the res directory. Last checked: "
           << candidates.back().wstring();
    throw std::runtime_error(WideToUtf8(stream.str()));
}

std::string WideToUtf8(const std::wstring& value)
{
    const int size = WideCharToMultiByte(CP_UTF8,
                                         0,
                                         value.c_str(),
                                         static_cast<int>(value.size()),
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
    if (size <= 0)
    {
        return "WideCharToMultiByte failed.";
    }

    std::string result(static_cast<size_t>(size), '\0');
    const int written = WideCharToMultiByte(CP_UTF8,
                                            0,
                                            value.c_str(),
                                            static_cast<int>(value.size()),
                                            result.data(),
                                            size,
                                            nullptr,
                                            nullptr);
    if (written <= 0)
    {
        return "WideCharToMultiByte failed.";
    }

    return result;
}

void InitAudio(HWND hWnd)
{
    try
    {
        g_assets = DiscoverSampleAssets();
        SoundLib::SoundLib::Initialize(hWnd);

        for (const auto& soundEffect : g_assets.soundEffects)
        {
            SoundLib::SoundLib::LoadSoundEffect(soundEffect.wstring());
        }

        std::wstringstream stream;
        stream << L"Loaded " << g_assets.soundEffects.size() << L" SE / "
               << g_assets.bgms.size() << L" BGM / "
               << g_assets.environments.size() << L" ENV";
        g_statusMessage = stream.str();
    }
    catch (const std::exception& exception)
    {
        g_audioError = std::wstring(L"Audio init failed: ") + std::wstring(exception.what(), exception.what() + std::strlen(exception.what()));
    }
}

void InitD3D(HWND hWnd)
{
    HRESULT hResult = E_FAIL;

    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    assert(g_pD3D != NULL);

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    d3dpp.BackBufferCount = 1;
    d3dpp.MultiSampleType = D3DMULTISAMPLE_NONE;
    d3dpp.MultiSampleQuality = 0;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    d3dpp.hDeviceWindow = hWnd;
    d3dpp.Flags = 0;
    d3dpp.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    hResult = g_pD3D->CreateDevice(D3DADAPTER_DEFAULT,
                                   D3DDEVTYPE_HAL,
                                   hWnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                   &d3dpp,
                                   &g_pd3dDevice);

    if (FAILED(hResult))
    {
        hResult = g_pD3D->CreateDevice(D3DADAPTER_DEFAULT,
                                       D3DDEVTYPE_HAL,
                                       hWnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                       &d3dpp,
                                       &g_pd3dDevice);

        assert(hResult == S_OK);
    }

    hResult = D3DXCreateFont(g_pd3dDevice,
                             20,
                             0,
                             FW_HEAVY,
                             1,
                             FALSE,
                             SHIFTJIS_CHARSET,
                             OUT_TT_ONLY_PRECIS,
                             CLEARTYPE_NATURAL_QUALITY,
                             FF_DONTCARE,
                             _T("ＭＳ ゴシック"),
                             &g_pFont);

    assert(hResult == S_OK);

    LPD3DXBUFFER pD3DXMtrlBuffer = NULL;

    hResult = D3DXLoadMeshFromX(_T("cube.x"),
                                D3DXMESH_SYSTEMMEM,
                                g_pd3dDevice,
                                NULL,
                                &pD3DXMtrlBuffer,
                                NULL,
                                &g_dwNumMaterials,
                                &g_pMesh);

    assert(hResult == S_OK);

    D3DXMATERIAL* d3dxMaterials = (D3DXMATERIAL*)pD3DXMtrlBuffer->GetBufferPointer();
    g_pMaterials.resize(g_dwNumMaterials);
    g_pTextures.resize(g_dwNumMaterials);

    for (DWORD i = 0; i < g_dwNumMaterials; i++)
    {
        g_pMaterials[i] = d3dxMaterials[i].MatD3D;
        g_pMaterials[i].Ambient = g_pMaterials[i].Diffuse;
        g_pTextures[i] = NULL;

        std::string pTexPath(d3dxMaterials[i].pTextureFilename);

        if (!pTexPath.empty())
        {
#ifdef UNICODE
            int len = MultiByteToWideChar(CP_ACP, 0, pTexPath.c_str(), -1, nullptr, 0);
            std::wstring pTexPathW(len, 0);
            MultiByteToWideChar(CP_ACP, 0, pTexPath.c_str(), -1, &pTexPathW[0], len);

            hResult = D3DXCreateTextureFromFileW(g_pd3dDevice, pTexPathW.c_str(), &g_pTextures[i]);
#else
            hResult = D3DXCreateTextureFromFileA(g_pd3dDevice, pTexPath.c_str(), &g_pTextures[i]);
#endif
            assert(hResult == S_OK);
        }
    }

    hResult = pD3DXMtrlBuffer->Release();
    assert(hResult == S_OK);

    hResult = D3DXCreateEffectFromFile(g_pd3dDevice,
                                       _T("simple.fx"),
                                       NULL,
                                       NULL,
                                       D3DXSHADER_DEBUG,
                                       NULL,
                                       &g_pEffect,
                                       NULL);

    assert(hResult == S_OK);
}

void Cleanup()
{
    if (g_leftEnvironmentId >= 0)
    {
        try
        {
            SoundLib::SoundLib::StopEnvironmentSound(g_leftEnvironmentId);
        }
        catch (...)
        {
        }
    }

    if (g_rightEnvironmentId >= 0)
    {
        try
        {
            SoundLib::SoundLib::StopEnvironmentSound(g_rightEnvironmentId);
        }
        catch (...)
        {
        }
    }

    try
    {
        SoundLib::SoundLib::Finalize();
    }
    catch (...)
    {
    }

    for (auto& texture : g_pTextures)
    {
        SAFE_RELEASE(texture);
    }

    SAFE_RELEASE(g_pMesh);
    SAFE_RELEASE(g_pEffect);
    SAFE_RELEASE(g_pFont);
    SAFE_RELEASE(g_pd3dDevice);
    SAFE_RELEASE(g_pD3D);
}

void UpdateListenerPosition(float deltaX, float deltaZ)
{
    g_listenerPosition.x += deltaX;
    g_listenerPosition.z += deltaZ;

    std::wstringstream stream;
    stream << L"Listener moved to (" << g_listenerPosition.x << L", "
           << g_listenerPosition.y << L", " << g_listenerPosition.z << L")";
    g_statusMessage = stream.str();
}

void HandleKeyDown(WPARAM key)
{
    if (!g_audioError.empty())
    {
        return;
    }

    try
    {
        switch (key)
        {
        case '1':
            if (!g_assets.soundEffects.empty())
            {
                SoundLib::SoundLib::PlaySoundEffect(g_assets.soundEffects[0].wstring(), 90);
                g_statusMessage = L"Played the smallest wav as SE.";
            }
            break;
        case '2':
            if (!g_assets.soundEffects.empty())
            {
                for (int i = 0; i < 5; ++i)
                {
                    const float x = -8.0f + static_cast<float>(i) * 4.0f;
                    SoundLib::Vector3 position { x, 0.0f, 8.0f + static_cast<float>(i) };
                    SoundLib::SoundLib::PlaySoundEffect(g_assets.soundEffects[0].wstring(),
                                                        80,
                                                        &position,
                                                        (i % 2 == 0) ? SoundLib::EffectType::None : SoundLib::EffectType::Radio);
                }

                g_statusMessage = L"Played 5 simultaneous sound effects.";
            }
            break;
        case '3':
            if (g_assets.soundEffects.size() >= 2)
            {
                SoundLib::Vector3 position { 10.0f, 0.0f, 4.0f };
                SoundLib::SoundLib::PlaySoundEffect(g_assets.soundEffects[1].wstring(),
                                                    85,
                                                    &position,
                                                    SoundLib::EffectType::Muffle);
                g_statusMessage = L"Played the second SE with 3D + muffle.";
            }
            break;
        case 'B':
            if (!g_assets.bgms.empty())
            {
                if (g_isBgmPlaying)
                {
                    SoundLib::SoundLib::StopBgm();
                    g_isBgmPlaying = false;
                    g_statusMessage = L"Stopped BGM with fade-out.";
                }
                else
                {
                    SoundLib::SoundLib::PlayBgm(g_assets.bgms[g_bgmIndex].wstring(), g_bgmVolume);
                    g_isBgmPlaying = true;
                    g_statusMessage = L"Started BGM with fade-in.";
                }
            }
            break;
        case 'N':
            if (!g_assets.bgms.empty())
            {
                g_bgmIndex = (g_bgmIndex + 1) % g_assets.bgms.size();
                SoundLib::SoundLib::PlayBgm(g_assets.bgms[g_bgmIndex].wstring(), g_bgmVolume);
                g_isBgmPlaying = true;
                g_statusMessage = L"Requested a different BGM.";
            }
            break;
        case 'E':
            if (!g_assets.environments.empty())
            {
                if (g_leftEnvironmentId >= 0)
                {
                    SoundLib::SoundLib::StopEnvironmentSound(g_leftEnvironmentId);
                    g_leftEnvironmentId = -1;
                    g_statusMessage = L"Stopped the left environment sound.";
                }
                else
                {
                    SoundLib::Vector3 position { -15.0f, 0.0f, 8.0f };
                    g_leftEnvironmentId = SoundLib::SoundLib::PlayEnvironmentSound(g_assets.environments[0].wstring(),
                                                                                     70,
                                                                                     &position,
                                                                                     SoundLib::EffectType::Cave,
                                                                                     true);
                    g_statusMessage = L"Started the left environment sound.";
                }
            }
            break;
        case 'R':
            if (g_assets.environments.size() >= 2)
            {
                if (g_rightEnvironmentId >= 0)
                {
                    SoundLib::SoundLib::StopEnvironmentSound(g_rightEnvironmentId);
                    g_rightEnvironmentId = -1;
                    g_statusMessage = L"Stopped the right environment sound.";
                }
                else
                {
                    SoundLib::Vector3 position { 15.0f, 0.0f, 8.0f };
                    g_rightEnvironmentId = SoundLib::SoundLib::PlayEnvironmentSound(g_assets.environments[1].wstring(),
                                                                                      70,
                                                                                      &position,
                                                                                      SoundLib::EffectType::Radio,
                                                                                      true);
                    g_statusMessage = L"Started the right environment sound.";
                }
            }
            break;
        case VK_LEFT:
            UpdateListenerPosition(-1.0f, 0.0f);
            break;
        case VK_RIGHT:
            UpdateListenerPosition(1.0f, 0.0f);
            break;
        case VK_UP:
            UpdateListenerPosition(0.0f, 1.0f);
            break;
        case VK_DOWN:
            UpdateListenerPosition(0.0f, -1.0f);
            break;
        case VK_OEM_PLUS:
        case VK_ADD:
            g_bgmVolume = std::min(100, g_bgmVolume + 5);
            SoundLib::SoundLib::SetBgmVolume(g_bgmVolume);
            g_statusMessage = L"Raised the BGM volume.";
            break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            g_bgmVolume = std::max(0, g_bgmVolume - 5);
            SoundLib::SoundLib::SetBgmVolume(g_bgmVolume);
            g_statusMessage = L"Lowered the BGM volume.";
            break;
        default:
            break;
        }
    }
    catch (const std::exception& exception)
    {
        g_audioError = std::wstring(L"Audio error: ") + std::wstring(exception.what(), exception.what() + std::strlen(exception.what()));
    }
}

std::wstring BuildUiText()
{
    std::wstringstream stream;
    stream << L"SoundLib sample controls\n";
    stream << L"1: Play SE  2: Play 5 simultaneous SE  3: Play 3D SE\n";
    stream << L"B: Toggle BGM  N: Next BGM  +/-: BGM volume (" << g_bgmVolume << L")\n";
    stream << L"E: Toggle left ENV  R: Toggle right ENV\n";
    stream << L"Arrow keys: Move listener\n\n";

    if (!g_assets.soundEffects.empty())
    {
        stream << L"SE1: " << FileNameOnly(g_assets.soundEffects[0]) << L"\n";
    }

    if (g_assets.soundEffects.size() >= 2)
    {
        stream << L"SE2: " << FileNameOnly(g_assets.soundEffects[1]) << L"\n";
    }

    if (!g_assets.bgms.empty())
    {
        stream << L"BGM: " << FileNameOnly(g_assets.bgms[g_bgmIndex]) << L"\n";
    }

    if (!g_assets.environments.empty())
    {
        stream << L"ENV-L: " << FileNameOnly(g_assets.environments[0]) << L"\n";
    }

    if (g_assets.environments.size() >= 2)
    {
        stream << L"ENV-R: " << FileNameOnly(g_assets.environments[1]) << L"\n";
    }

    stream << L"\nListener: (" << g_listenerPosition.x << L", "
           << g_listenerPosition.y << L", " << g_listenerPosition.z << L")\n";
    stream << L"Status: " << g_statusMessage << L"\n";

    if (!g_audioError.empty())
    {
        stream << L"Error: " << g_audioError << L"\n";
    }

    return stream.str();
}

void Render()
{
    HRESULT hResult = E_FAIL;

    static float f = 0.0f;
    f += 0.025f;

    D3DXMATRIX mat;
    D3DXMATRIX View, Proj;

    D3DXMatrixPerspectiveFovLH(&Proj,
                               D3DXToRadian(45),
                               (float)WINDOW_SIZE_W / WINDOW_SIZE_H,
                               1.0f,
                               10000.0f);

    D3DXVECTOR3 eye(g_listenerPosition.x, g_listenerPosition.y, g_listenerPosition.z);
    D3DXVECTOR3 target(g_listenerPosition.x + 10.0f * sinf(f),
                       g_listenerPosition.y,
                       g_listenerPosition.z + 10.0f * cosf(f));
    D3DXVECTOR3 up(0, 1, 0);
    D3DXMatrixLookAtLH(&View, &eye, &target, &up);
    D3DXMatrixIdentity(&mat);
    mat = mat * View * Proj;

    D3DXVECTOR3 forward = target - eye;
    D3DXVec3Normalize(&forward, &forward);
    g_listenerFront = { forward.x, forward.y, forward.z };
    g_listenerTop = { up.x, up.y, up.z };

    if (g_audioError.empty())
    {
        try
        {
            SoundLib::SoundLib::Update(g_listenerPosition, g_listenerFront, g_listenerTop);
        }
        catch (const std::exception& exception)
        {
            g_audioError = std::wstring(L"Audio update error: ") + std::wstring(exception.what(), exception.what() + std::strlen(exception.what()));
        }
    }

    hResult = g_pEffect->SetMatrix("g_matWorldViewProj", &mat);
    assert(hResult == S_OK);

    hResult = g_pd3dDevice->Clear(0,
                                  NULL,
                                  D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                  D3DCOLOR_XRGB(100, 100, 100),
                                  1.0f,
                                  0);

    assert(hResult == S_OK);

    hResult = g_pd3dDevice->BeginScene();
    assert(hResult == S_OK);

    const std::wstring uiText = BuildUiText();
    TextDraw(g_pFont, uiText.c_str(), 0, 0);

    hResult = g_pEffect->SetTechnique("Technique1");
    assert(hResult == S_OK);

    UINT numPass;
    hResult = g_pEffect->Begin(&numPass, 0);
    assert(hResult == S_OK);

    hResult = g_pEffect->BeginPass(0);
    assert(hResult == S_OK);

    for (DWORD i = 0; i < g_dwNumMaterials; i++)
    {
        hResult = g_pEffect->SetTexture("texture1", g_pTextures[i]);
        assert(hResult == S_OK);

        hResult = g_pEffect->CommitChanges();
        assert(hResult == S_OK);

        hResult = g_pMesh->DrawSubset(i);
        assert(hResult == S_OK);
    }

    hResult = g_pEffect->EndPass();
    assert(hResult == S_OK);

    hResult = g_pEffect->End();
    assert(hResult == S_OK);

    hResult = g_pd3dDevice->EndScene();
    assert(hResult == S_OK);

    hResult = g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
    assert(hResult == S_OK);
}

LRESULT WINAPI MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        HandleKeyDown(wParam);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        g_bClose = true;
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}
