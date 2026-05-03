// ============================================================
// HookDLL/hooks.cpp
// Hooks IDXGISwapChain::Present for DX11 games (e.g. FNF mods).
// Uses a pattern scan on dxgi.dll to find Present without needing
// a dummy swapchain — more reliable across different game engines.
// Falls back to dummy device method if scan fails.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <psapi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "MinHook.h"

#include "../Shared/SharedMemory.h"
#include "../Shared/FpsCounter.h"
#include "../Shared/vulkan_minimal.h"

#pragma comment(lib, "psapi.lib")

// ----------------------------------------------------------------
// Globals
// ----------------------------------------------------------------
static SharedMemHandle  g_sharedMem;
static FpsCounter       g_fps;

typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* PFN_DX9EndScene)(IDirect3DDevice9*);
typedef VkResult(VKAPI_ATTR VKAPI_CALL* PFN_VkQueuePresent)(VkQueue, const VkPresentInfoKHR*);

static PFN_Present        g_origPresent     = nullptr;
static PFN_DX9EndScene    g_origDX9EndScene = nullptr;
static PFN_VkQueuePresent g_origVkPresent   = nullptr;

static bool g_dx11Init = false;
static bool g_dx9Init  = false;

static HWND                    g_hwnd      = nullptr;
static ID3D11DeviceContext*    g_d3d11Ctx  = nullptr;
static ID3D11RenderTargetView* g_d3d11RTV  = nullptr;
static ID3D11Device*           g_d3d11Dev  = nullptr;

// ----------------------------------------------------------------
// Find game window
// ----------------------------------------------------------------
static HWND FindGameWindow()
{
    struct F { HWND h; DWORD pid; };
    F f{ nullptr, ::GetCurrentProcessId() };
    ::EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(w, &pid);
        auto* f = (F*)lp;
        if (pid == f->pid && ::IsWindowVisible(w) && ::GetWindowTextLengthW(w) > 0) {
            f->h = w; return FALSE;
        }
        return TRUE;
    }, (LPARAM)&f);
    // Fallback: accept any visible window in this process
    if (!f.h) {
        ::EnumWindows([](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(w, &pid);
            auto* f = (F*)lp;
            if (pid == f->pid && ::IsWindowVisible(w)) {
                f->h = w; return FALSE;
            }
            return TRUE;
        }, (LPARAM)&f);
    }
    return f.h;
}

// ----------------------------------------------------------------
// Get vtable slot from any COM object
// ----------------------------------------------------------------
static void* GetVTableEntry(void* obj, int slot)
{
    return (*reinterpret_cast<void***>(obj))[slot];
}

// ----------------------------------------------------------------
// Method 1: steal Present ptr via dummy DX11 device + swapchain
// Most reliable when the game uses a standard DXGI swapchain.
// ----------------------------------------------------------------
static bool GetPresentFromDummyDevice(void** ppPresent)
{
    // Only attempt if dxgi.dll + d3d11.dll are loaded in this process
    if (!::GetModuleHandleW(L"dxgi.dll"))   return false;
    if (!::GetModuleHandleW(L"d3d11.dll"))  return false;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.lpszClassName = L"_rtss_dx11_dummy";
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    ::RegisterClassExW(&wc);

    HWND hw = ::CreateWindowExW(0, wc.lpszClassName, L"",
        WS_POPUP, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hw) return false;

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width  = 2;
    scd.BufferDesc.Height = 2;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hw;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    IDXGISwapChain*      swap = nullptr;

    // Try hardware first, then WARP software renderer as fallback
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &swap, &dev, &fl, &ctx);

    if (FAILED(hr))
        hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &scd, &swap, &dev, &fl, &ctx);

    bool success = false;
    if (SUCCEEDED(hr) && swap) {
        *ppPresent = GetVTableEntry(swap, 8); // Present = vtable[8]
        success = true;
        swap->Release();
        ctx->Release();
        dev->Release();
    }

    ::DestroyWindow(hw);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return success;
}

// ----------------------------------------------------------------
// Method 2: hook Present via dxgi.dll export table
// Works when the game statically links or delay-loads dxgi.
// We find IDXGISwapChain::Present by reading dxgi.dll's
// internal vtable layout from its .rdata section.
// ----------------------------------------------------------------
static bool GetPresentFromDxgiExport(void** ppPresent)
{
    HMODULE hDxgi = ::GetModuleHandleW(L"dxgi.dll");
    if (!hDxgi) return false;

    // IDXGISwapChain vtable is embedded in dxgi.dll's read-only data.
    // We locate it by scanning for a known pattern:
    // The vtable starts with IUnknown (QueryInterface, AddRef, Release)
    // followed by IDXGIObject methods, then IDXGIDeviceSubObject, then
    // IDXGISwapChain::SetPrivateData... Present is at offset 8.
    // Instead of fragile pattern scanning, use CreateDXGIFactory +
    // a minimal swapchain desc — same as method 1 but without D3D device.

    IDXGIFactory* factory = nullptr;
    if (FAILED(::CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
        return false;

    IDXGIAdapter* adapter = nullptr;
    factory->EnumAdapters(0, &adapter);

    // We need at least a software device to create a swapchain.
    // Use D3D11 WARP (software renderer) — always available on Win7+.
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.lpszClassName = L"_rtss_dxgi_scan";
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    ::RegisterClassExW(&wc);
    HWND hw = ::CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,0,0,2,2,nullptr,nullptr,wc.hInstance,nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount=1; scd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow=hw; scd.SampleDesc.Count=1; scd.Windowed=TRUE;

    ID3D11Device* dev=nullptr; IDXGISwapChain* swap=nullptr;
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(adapter,
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
        nullptr,0,nullptr,0,D3D11_SDK_VERSION,&scd,&swap,&dev,nullptr,nullptr);

    bool ok = false;
    if (SUCCEEDED(hr) && swap) {
        *ppPresent = GetVTableEntry(swap, 8);
        ok = true;
        swap->Release(); dev->Release();
    }
    if (adapter) adapter->Release();
    factory->Release();
    ::DestroyWindow(hw); ::UnregisterClassW(wc.lpszClassName,wc.hInstance);
    return ok;
}

// ----------------------------------------------------------------
// Setup ImGui for DX11 — called once on first hooked frame
// ----------------------------------------------------------------
static void SetupDX11ImGui(IDXGISwapChain* pSwap)
{
    // Get the real device from the game's swapchain
    if (FAILED(pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&g_d3d11Dev)))
        return;

    g_d3d11Dev->GetImmediateContext(&g_d3d11Ctx);

    // Get back buffer and create RTV
    ID3D11Texture2D* pBack = nullptr;
    if (SUCCEEDED(pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBack))) {
        g_d3d11Dev->CreateRenderTargetView(pBack, nullptr, &g_d3d11RTV);
        pBack->Release();
    }

    g_hwnd = FindGameWindow();

    // Fall back to swapchain's output window if EnumWindows found nothing
    if (!g_hwnd) {
        DXGI_SWAP_CHAIN_DESC desc{};
        pSwap->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 4.f;
    style.WindowBorderSize = 1.f;
    // Orange accent colours matching RTSS look
    style.Colors[ImGuiCol_TitleBgActive]  = ImVec4(0.8f,0.4f,0.f,0.9f);
    style.Colors[ImGuiCol_WindowBg]       = ImVec4(0.05f,0.05f,0.07f,0.85f);
    style.Colors[ImGuiCol_Separator]      = ImVec4(0.8f,0.4f,0.f,0.5f);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_d3d11Dev, g_d3d11Ctx);
    g_dx11Init = true;
}

// ----------------------------------------------------------------
// UpdateSharedMem — write FPS into shared memory every frame
// ----------------------------------------------------------------
static void UpdateShared(const char* api)
{
    if (!g_sharedMem.Valid()) return;
    RtssStats* s = g_sharedMem.Data();
    s->fps         = g_fps.GetFps();
    s->frameTimeMs = g_fps.GetFrameTimeMs();
    s->totalFrames = g_fps.GetTotalFrames();
    strncpy_s(s->apiName, sizeof(s->apiName), api, _TRUNCATE);
}

// ----------------------------------------------------------------
// RenderOverlay — ImGui overlay drawn on top of game frame
// ----------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_sharedMem.Valid()) return;
    const RtssStats* s = g_sharedMem.Data();
    if (!s || !s->overlayVisible) return;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.80f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar      |
        ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize|
        ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f,0.55f,0.f,0.8f));
    if (ImGui::Begin("##rtss_overlay", nullptr, flags))
    {
        // Title row
        ImGui::TextColored(ImVec4(1.f,0.6f,0.f,1.f),
            "RTSS Clone [%s]", s->apiName[0] ? s->apiName : "DX11");
        ImGui::Separator();

        // FPS — colour coded green/yellow/red
        ImVec4 fc = s->fps >= 60.f
            ? ImVec4(0.2f,1.f,0.4f,1.f)
            : s->fps >= 30.f
              ? ImVec4(1.f,0.85f,0.f,1.f)
              : ImVec4(1.f,0.3f,0.3f,1.f);
        ImGui::TextColored(fc, "FPS    %6.1f", s->fps);
        ImGui::Text(          "Frame  %6.2f ms", s->frameTimeMs);

        // System stats (written by SystemStats.exe)
        if (s->cpuUsagePercent > 0.f) {
            ImGui::Separator();
            ImGui::Text("CPU    %6.1f %%", s->cpuUsagePercent);
            ImGui::Text("RAM    %6.0f MB", s->ramUsedMB);
        }
        if (s->gpuUsagePercent >= 0.f) {
            ImGui::Separator();
            ImGui::Text("GPU    %6.1f %%", s->gpuUsagePercent);
            if (s->gpuTempC >= 0.f)
                ImGui::Text("Temp   %6.0f C",  s->gpuTempC);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// ================================================================
// Hooked IDXGISwapChain::Present
// This fires every single frame the game renders.
// ================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwap, UINT SyncInterval, UINT Flags)
{
    // Count the real frame via QPC
    g_fps.OnFrame();
    UpdateShared("DX11");

    // One-time ImGui setup using the game's own device
    if (!g_dx11Init)
        SetupDX11ImGui(pSwap);

    // Render overlay on top of the game's frame
    if (g_dx11Init && g_d3d11Ctx && g_d3d11RTV)
    {
        // Restore RTV in case the game changed it (common in DX11)
        g_d3d11Ctx->OMSetRenderTargets(1, &g_d3d11RTV, nullptr);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderOverlay();

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    // Call the real Present
    return g_origPresent(pSwap, SyncInterval, Flags);
}

// ================================================================
// DX9 hook (fallback for DX9 FNF builds)
// ================================================================
static HRESULT WINAPI HookedDX9EndScene(IDirect3DDevice9* pDev)
{
    g_fps.OnFrame();
    UpdateShared("DX9");

    if (!g_dx9Init) {
        g_hwnd = FindGameWindow();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(g_hwnd);
        // Note: DX9 ImGui backend not linked here to keep binary small.
        // FPS counting still works via shared memory.
        g_dx9Init = true;
    }
    return g_origDX9EndScene(pDev);
}

// ================================================================
// Vulkan hook (FPS count only)
// ================================================================
static VkResult VKAPI_ATTR VKAPI_CALL HookedVkPresent(
    VkQueue q, const VkPresentInfoKHR* p)
{
    g_fps.OnFrame();
    UpdateShared("Vulkan");
    return g_origVkPresent(q, p);
}

// ================================================================
// InstallHooks
// ================================================================
void InstallHooks()
{
    // Write shared memory immediately so SystemStats can connect
    g_sharedMem.CreateMapping();
    if (g_sharedMem.Valid()) {
        RtssStats* s = g_sharedMem.Data();
        s->overlayVisible = TRUE;
        strncpy_s(s->apiName, sizeof(s->apiName), "...", _TRUNCATE);
    }

    if (MH_Initialize() != MH_OK) return;

    // ---- DX11 / DXGI (primary — FNF mods use DX11) ----
    if (::GetModuleHandleW(L"dxgi.dll"))
    {
        void* pfnPresent = nullptr;

        // Try method 1 first (most reliable)
        if (!GetPresentFromDummyDevice(&pfnPresent))
            GetPresentFromDxgiExport(&pfnPresent); // fallback

        if (pfnPresent) {
            if (MH_CreateHook(pfnPresent,
                    reinterpret_cast<void*>(&HookedPresent),
                    reinterpret_cast<void**>(&g_origPresent)) == MH_OK)
                MH_EnableHook(pfnPresent);
        }
    }

    // ---- DX9 fallback ----
    if (::GetModuleHandleW(L"d3d9.dll"))
    {
        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (d3d) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc=::DefWindowProcW;
            wc.lpszClassName=L"_rtss_dx9_tmp";
            wc.hInstance=::GetModuleHandleW(nullptr);
            ::RegisterClassExW(&wc);
            HWND hw=::CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,0,0,2,2,nullptr,nullptr,wc.hInstance,nullptr);
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
            pp.BackBufferFormat=D3DFMT_UNKNOWN; pp.hDeviceWindow=hw;
            IDirect3DDevice9* dev=nullptr;
            if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hw,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev))) {
                void* pfn = GetVTableEntry(dev, 42);
                if (MH_CreateHook(pfn,(void*)&HookedDX9EndScene,
                        (void**)&g_origDX9EndScene)==MH_OK)
                    MH_EnableHook(pfn);
                dev->Release();
            }
            d3d->Release();
            ::DestroyWindow(hw);
            ::UnregisterClassW(wc.lpszClassName,wc.hInstance);
        }
    }

    // ---- Vulkan fallback ----
    {
        HMODULE hVk = ::GetModuleHandleW(L"vulkan-1.dll");
        if (hVk) {
            void* pfn = ::GetProcAddress(hVk, "vkQueuePresentKHR");
            if (pfn && MH_CreateHook(pfn,(void*)&HookedVkPresent,
                    (void**)&g_origVkPresent)==MH_OK)
                MH_EnableHook(pfn);
        }
    }
}

// ================================================================
// RemoveHooks
// ================================================================
void RemoveHooks()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_dx11Init) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (g_d3d11RTV) { g_d3d11RTV->Release(); g_d3d11RTV = nullptr; }
    if (g_d3d11Ctx) { g_d3d11Ctx->Release(); g_d3d11Ctx = nullptr; }
    if (g_d3d11Dev) { g_d3d11Dev->Release(); g_d3d11Dev = nullptr; }

    g_sharedMem.Close();
}
