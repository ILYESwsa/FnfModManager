// ============================================================
// HookDLL/hooks.cpp  —  RTSS Clone
// Overlay styled exactly like RivaTuner Statistics Server:
// plain colored text, no boxes/bars, transparent background.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_dx11.h"
#include "MinHook.h"

#include "../Shared/SharedMemory.h"
#include "../Shared/FpsCounter.h"
#include "../Shared/vulkan_minimal.h"

#pragma comment(lib, "psapi.lib")

extern SharedMemHandle g_sharedMem;

// ----------------------------------------------------------------
// State
// ----------------------------------------------------------------
static FpsCounter g_fps;

typedef BOOL    (WINAPI* PFN_wglSwapBuffers)(HDC);
typedef HRESULT (WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef VkResult(VKAPI_ATTR VKAPI_CALL* PFN_VkQueuePresent)(VkQueue, const VkPresentInfoKHR*);

static PFN_wglSwapBuffers g_origSwap    = nullptr;
static PFN_Present        g_origPresent = nullptr;
static PFN_VkQueuePresent g_origVk      = nullptr;

static bool g_glInit   = false;
static bool g_dx11Init = false;
static HWND g_hwnd     = nullptr;

static ID3D11Device*           g_dx11Dev = nullptr;
static ID3D11DeviceContext*    g_dx11Ctx = nullptr;
static ID3D11RenderTargetView* g_dx11RTV = nullptr;

// Frametime graph ring buffer (60 samples like real RTSS)
static const int FT_SAMPLES = 60;
static float g_ftHistory[FT_SAMPLES] = {};
static int   g_ftHead = 0;

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------
static HWND FindGameWindow()
{
    struct F { HWND h; DWORD pid; };
    F f{ nullptr, ::GetCurrentProcessId() };
    ::EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD pid=0; ::GetWindowThreadProcessId(w,&pid);
        auto* f=(F*)lp;
        if(pid==f->pid && ::IsWindowVisible(w)){f->h=w;return FALSE;}
        return TRUE;
    },(LPARAM)&f);
    return f.h;
}

static void* VTbl(void* obj, int slot)
{
    return (*reinterpret_cast<void***>(obj))[slot];
}

static void PushFt()
{
    g_ftHistory[g_ftHead] = g_fps.GetFrameTimeMs();
    g_ftHead = (g_ftHead+1) % FT_SAMPLES;
}

static void UpdateShared(const char* api)
{
    if (!g_sharedMem.Valid()) return;
    RtssStats* s = g_sharedMem.Data();
    if (!s) return;
    s->fps         = g_fps.GetFps();
    s->frameTimeMs = g_fps.GetFrameTimeMs();
    s->totalFrames = g_fps.GetTotalFrames();
    strncpy_s(s->apiName, sizeof(s->apiName), api, _TRUNCATE);
}

// ----------------------------------------------------------------
// RTSS style — completely transparent, text only
// ----------------------------------------------------------------
static void ApplyRTSSStyle()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding     = ImVec2(6, 4);
    st.ItemSpacing       = ImVec2(0, 1);
    st.FramePadding      = ImVec2(0, 0);
    st.WindowRounding    = 0.f;
    st.WindowBorderSize  = 0.f;
    st.ChildBorderSize   = 0.f;
    // All transparent
    for (int i=0;i<ImGuiCol_COUNT;i++)
        st.Colors[i] = ImVec4(0,0,0,0);
    st.Colors[ImGuiCol_Text] = ImVec4(1,1,1,1);
}

// ----------------------------------------------------------------
// DrawRTSSLine — renders one stat line exactly like RTSS:
//   [LABEL]  [VALUE][UNIT]  [VALUE2][UNIT2]  ...
//
// label      = orange label text e.g. "GPU"
// val        = main value e.g. "99"
// unit       = unit suffix e.g. "%" (smaller, dimmer)
// val2/unit2 = optional second column
// ----------------------------------------------------------------
static const ImVec4 COL_LABEL = ImVec4(0.95f, 0.55f, 0.10f, 1.f); // orange
static const ImVec4 COL_VAL   = ImVec4(1.00f, 1.00f, 1.00f, 1.f); // white
static const ImVec4 COL_UNIT  = ImVec4(0.70f, 0.70f, 0.70f, 0.9f);// grey
static const ImVec4 COL_DIM   = ImVec4(0.55f, 0.55f, 0.55f, 1.f); // dimmed

// Draw label + value + unit on current line, returns x after drawing
static float DrawLV(float x, float y, ImDrawList* dl,
    const char* val, ImVec4 valCol,
    const char* unit, float scale=1.f)
{
    // Value
    ImVec2 vsz = ImGui::CalcTextSize(val);
    dl->AddText(ImVec2(x,y), ImGui::ColorConvertFloat4ToU32(valCol), val);
    x += vsz.x + 2.f;

    // Unit (slightly smaller via opacity trick since we can't resize font inline)
    if (unit && unit[0]) {
        dl->AddText(ImVec2(x, y+2.f), ImGui::ColorConvertFloat4ToU32(COL_UNIT), unit);
        x += ImGui::CalcTextSize(unit).x + 8.f;
    }
    return x;
}

// ----------------------------------------------------------------
// RenderOverlay — pure RTSS text style
// ----------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_sharedMem.Valid()) return;
    const RtssStats* s = g_sharedMem.Data();
    if (!s || !s->overlayVisible) return;

    // Fully transparent, no decorations
    ImGui::SetNextWindowPos(ImVec2(4, 4), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);

    ImGuiWindowFlags wf =
        ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove          | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoInputs        | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("##rtss", nullptr, wf)) { ImGui::End(); return; }

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const float lh  = ImGui::GetTextLineHeight(); // line height
    const float lbl = 40.f; // label column width
    float y = ImGui::GetCursorScreenPos().y;
    float ox = ImGui::GetCursorScreenPos().x; // origin x

    char v1[32], v2[32], v3[32];

    // ---- GPU ----
    if (s->gpuUsagePercent >= 0.f) {
        // Label
        dl->AddText(ImVec2(ox, y), ImGui::ColorConvertFloat4ToU32(COL_LABEL), "GPU");
        float x = ox + lbl;

        // GPU usage %
        snprintf(v1,sizeof(v1),"%.0f", s->gpuUsagePercent);
        x = DrawLV(x,y,dl,v1,COL_VAL,"%");

        // GPU mem (shared RAM for iGPU)
        snprintf(v2,sizeof(v2),"%.0f", s->gpuMemUsedMB);
        x = DrawLV(x,y,dl,v2,COL_VAL,"MB");

        y += lh + 1.f;
    }

    // ---- CPU ----
    {
        dl->AddText(ImVec2(ox,y), ImGui::ColorConvertFloat4ToU32(COL_LABEL), "CPU");
        float x = ox + lbl;
        snprintf(v1,sizeof(v1),"%.0f", s->cpuUsagePercent);
        x = DrawLV(x,y,dl,v1,COL_VAL,"%");
        y += lh + 1.f;
    }

    // ---- RAM ----
    {
        dl->AddText(ImVec2(ox,y), ImGui::ColorConvertFloat4ToU32(COL_LABEL), "RAM");
        float x = ox + lbl;
        snprintf(v1,sizeof(v1),"%.0f", s->ramUsedMB);
        x = DrawLV(x,y,dl,v1,COL_VAL,"MB");
        y += lh + 1.f;
    }

    // ---- API + FPS + Frametime ----
    {
        // API label (D3D12 / OpenGL / DX11 etc)
        dl->AddText(ImVec2(ox,y), ImGui::ColorConvertFloat4ToU32(COL_LABEL),
            s->apiName[0] ? s->apiName : "...");
        float x = ox + lbl;

        // FPS — bigger feel via bright white
        snprintf(v1,sizeof(v1),"%.0f", s->fps);
        ImVec4 fpsCol = s->fps>=60.f ? ImVec4(1.f,1.f,1.f,1.f)
                      : s->fps>=30.f ? ImVec4(1.f,0.8f,0.2f,1.f)
                                     : ImVec4(1.f,0.3f,0.3f,1.f);
        x = DrawLV(x,y,dl,v1,fpsCol,"FPS");

        // Frametime
        snprintf(v2,sizeof(v2),"%.1f", s->frameTimeMs);
        x = DrawLV(x,y,dl,v2,COL_DIM,"ms");
        y += lh + 1.f;
    }

    // ---- Frametime label ----
    {
        dl->AddText(ImVec2(ox,y), ImGui::ColorConvertFloat4ToU32(COL_DIM), "Frametime");
        y += lh + 2.f;
    }

    // ---- Frametime graph — thin line like real RTSS ----
    {
        const float gw = 252.f;
        const float gh = 28.f;

        // Dark strip behind graph
        dl->AddRectFilled(ImVec2(ox,y), ImVec2(ox+gw,y+gh),
            IM_COL32(0,0,0,90));

        // Find max frametime for scaling
        float mx = 0.f;
        for (int i=0;i<FT_SAMPLES;i++) if(g_ftHistory[i]>mx) mx=g_ftHistory[i];
        if (mx < 16.7f) mx = 16.7f; // floor at 60fps line

        // 60fps reference line
        float line60y = y + gh - (16.7f/mx)*(gh-4.f) - 2.f;
        dl->AddLine(ImVec2(ox,line60y), ImVec2(ox+gw,line60y),
            IM_COL32(80,80,80,120));

        // Draw frametime as a connected line graph (like RTSS)
        float bw = gw / (float)FT_SAMPLES;
        for (int i=1;i<FT_SAMPLES;i++) {
            int ia = (g_ftHead + i - 1) % FT_SAMPLES;
            int ib = (g_ftHead + i)     % FT_SAMPLES;
            float fa = g_ftHistory[ia];
            float fb = g_ftHistory[ib];
            float xa = ox + (i-1)*bw;
            float xb = ox + i*bw;
            float ya2 = y + gh - 2.f - (fa/mx)*(gh-4.f);
            float yb2 = y + gh - 2.f - (fb/mx)*(gh-4.f);
            // Clamp
            if(ya2<y)ya2=y; if(yb2<y)yb2=y;
            if(ya2>y+gh)ya2=y+gh; if(yb2>y+gh)yb2=y+gh;
            ImU32 col = (fb>33.3f)?IM_COL32(220,60,60,220):
                        (fb>16.7f)?IM_COL32(220,180,40,220):
                                   IM_COL32(200,200,200,200);
            dl->AddLine(ImVec2(xa,ya2), ImVec2(xb,yb2), col, 1.2f);
        }

        // Current frametime label top-right of graph
        snprintf(v1,sizeof(v1),"%.1f ms", s->frameTimeMs);
        ImVec2 ftsz=ImGui::CalcTextSize(v1);
        dl->AddText(ImVec2(ox+gw-ftsz.x-2, y+2),
            IM_COL32(160,160,160,200), v1);

        y += gh + 2.f;
    }

    // Advance ImGui cursor to match what we drew
    ImGui::SetCursorScreenPos(ImVec2(ox, y));
    ImGui::Dummy(ImVec2(260, 0));

    ImGui::End();
}

// ================================================================
// Vulkan hook
// ================================================================
static VkResult VKAPI_ATTR VKAPI_CALL HookedVkPresent(
    VkQueue q, const VkPresentInfoKHR* p)
{
    g_fps.OnFrame(); PushFt(); UpdateShared("Vulkan");
    return g_origVk(q,p);
}

// ================================================================
// OpenGL hook — wglSwapBuffers (Psych Engine primary)
// ================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static BOOL WINAPI HookedWglSwapBuffers(HDC hDC)
{
    g_fps.OnFrame(); PushFt(); UpdateShared("OpenGL");

    if (!g_glInit) {
        g_hwnd = ::WindowFromDC(hDC);
        if (!g_hwnd) g_hwnd = FindGameWindow();
        if (!g_hwnd) return g_origSwap(hDC); // retry next frame

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ApplyRTSSStyle();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplOpenGL3_Init("#version 130");
        g_glInit = true;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderOverlay();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return g_origSwap(hDC);
}

// ================================================================
// DX11 hook — IDXGISwapChain::Present (fallback)
// ================================================================
static void SetupDX11(IDXGISwapChain* pSwap)
{
    if (FAILED(pSwap->GetDevice(__uuidof(ID3D11Device),(void**)&g_dx11Dev))) return;
    g_dx11Dev->GetImmediateContext(&g_dx11Ctx);
    ID3D11Texture2D* pBack=nullptr;
    if (SUCCEEDED(pSwap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&pBack))) {
        g_dx11Dev->CreateRenderTargetView(pBack,nullptr,&g_dx11RTV);
        pBack->Release();
    }
    if (!g_dx11RTV) return;
    g_hwnd = FindGameWindow();
    if (!g_hwnd) { DXGI_SWAP_CHAIN_DESC d{}; pSwap->GetDesc(&d); g_hwnd=d.OutputWindow; }
    if (!g_hwnd) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename=nullptr;
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    ApplyRTSSStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dx11Dev,g_dx11Ctx);
    g_dx11Init=true;
}

static HRESULT WINAPI HookedDXGIPresent(IDXGISwapChain* pSwap,UINT sync,UINT flags)
{
    g_fps.OnFrame(); PushFt(); UpdateShared("DX11");
    if (!g_dx11Init) SetupDX11(pSwap);
    if (g_dx11Init&&g_dx11Ctx&&g_dx11RTV) {
        g_dx11Ctx->OMSetRenderTargets(1,&g_dx11RTV,nullptr);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderOverlay();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return g_origPresent(pSwap,sync,flags);
}

// ================================================================
// InstallHooks
// ================================================================
void InstallHooks()
{
    if (MH_Initialize()!=MH_OK) return;

    // OpenGL — Psych Engine
    {
        HMODULE hGL=::GetModuleHandleW(L"opengl32.dll");
        if (!hGL) hGL=::LoadLibraryW(L"opengl32.dll");
        if (hGL) {
            void* pfn=::GetProcAddress(hGL,"wglSwapBuffers");
            if (pfn&&MH_CreateHook(pfn,(void*)&HookedWglSwapBuffers,
                    (void**)&g_origSwap)==MH_OK)
                MH_EnableHook(pfn);
        }
    }

    // DX11 fallback
    if (::GetModuleHandleW(L"dxgi.dll")&&::GetModuleHandleW(L"d3d11.dll")) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=::DefWindowProcW; wc.lpszClassName=L"_rtss_d11";
        wc.hInstance=::GetModuleHandleW(nullptr);
        ::RegisterClassExW(&wc);
        HWND hw=::CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,
            0,0,2,2,nullptr,nullptr,wc.hInstance,nullptr);
        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferCount=1; scd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow=hw; scd.SampleDesc.Count=1; scd.Windowed=TRUE;
        ID3D11Device* dev=nullptr; IDXGISwapChain* swap=nullptr;
        HRESULT hr=::D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
            nullptr,0,nullptr,0,D3D11_SDK_VERSION,&scd,&swap,&dev,nullptr,nullptr);
        if (FAILED(hr))
            hr=::D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,
                nullptr,0,nullptr,0,D3D11_SDK_VERSION,&scd,&swap,&dev,nullptr,nullptr);
        if (SUCCEEDED(hr)&&swap) {
            void* pfn=VTbl(swap,8);
            if (MH_CreateHook(pfn,(void*)&HookedDXGIPresent,(void**)&g_origPresent)==MH_OK)
                MH_EnableHook(pfn);
            swap->Release(); dev->Release();
        }
        ::DestroyWindow(hw);
        ::UnregisterClassW(wc.lpszClassName,wc.hInstance);
    }

    // Vulkan fallback
    {
        HMODULE hVk=::GetModuleHandleW(L"vulkan-1.dll");
        if (hVk) {
            void* pfn=::GetProcAddress(hVk,"vkQueuePresentKHR");
            if (pfn&&MH_CreateHook(pfn,(void*)&HookedVkPresent,
                    (void**)&g_origVk)==MH_OK)
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
    if (g_glInit)        { ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
    else if (g_dx11Init) { ImGui_ImplDX11_Shutdown();   ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
    if (g_dx11RTV) { g_dx11RTV->Release(); g_dx11RTV=nullptr; }
    if (g_dx11Ctx) { g_dx11Ctx->Release(); g_dx11Ctx=nullptr; }
    if (g_dx11Dev) { g_dx11Dev->Release(); g_dx11Dev=nullptr; }
}
