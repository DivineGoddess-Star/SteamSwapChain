#include "overlay.h"
#include "../globals.h"
#include "../ImGui/imgui.h"
#include "../ImGui/imgui_impl_win32.h"
#include "../ImGui/imgui_impl_dx11.h"

// Forward-declared by imgui_impl_win32 — needed for WndProc subclassing
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

static void CreateRTV(IDXGISwapChain* pChain)
{
    ID3D11Texture2D* pBB = nullptr;
    HRESULT hr = pChain->GetBuffer(0, IID_PPV_ARGS(&pBB));
    if (FAILED(hr)) { Log("GetBuffer FAILED 0x%08X", hr); return; }

    hr = g_pDevice->CreateRenderTargetView(pBB, nullptr, &g_pRenderTarget);
    pBB->Release();

    if (FAILED(hr)) Log("CreateRTV FAILED 0x%08X", hr);
    else            Log("RTV @ 0x%p", g_pRenderTarget);
}

static void DestroyRTV()
{
    if (g_pRenderTarget) { g_pRenderTarget->Release(); g_pRenderTarget = nullptr; }
}

// Subclassed WndProc — routes input to ImGui when the menu is open
static LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_ShowMenu && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return TRUE;
    return CallWindowProcA((WNDPROC)oWndProc, hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Style
// ---------------------------------------------------------------------------

static void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding = 6.f;
    s.FrameRounding = 4.f;
    s.ScrollbarRounding = 4.f;
    s.GrabRounding = 4.f;
    s.ItemSpacing = { 8.f, 6.f };
    s.WindowPadding = { 12.f, 12.f };
    s.FramePadding = { 6.f, 4.f };

    // Purple accent palette
    const ImVec4 accent = { 0.35f, 0.10f, 0.55f, 1.f };
    const ImVec4 accentHover = { 0.50f, 0.20f, 0.75f, 1.f };
    const ImVec4 accentMark = { 0.75f, 0.40f, 1.00f, 1.f };

    s.Colors[ImGuiCol_TitleBgActive] = accent;
    s.Colors[ImGuiCol_CheckMark] = accentMark;
    s.Colors[ImGuiCol_SliderGrab] = accentMark;
    s.Colors[ImGuiCol_SliderGrabActive] = accentHover;
    s.Colors[ImGuiCol_Button] = accent;
    s.Colors[ImGuiCol_ButtonHovered] = accentHover;
    s.Colors[ImGuiCol_ButtonActive] = accentMark;
    s.Colors[ImGuiCol_Header] = accent;
    s.Colors[ImGuiCol_HeaderHovered] = accentHover;
    s.Colors[ImGuiCol_HeaderActive] = accentMark;
    s.Colors[ImGuiCol_Tab] = accent;
    s.Colors[ImGuiCol_TabHovered] = accentHover;
    s.Colors[ImGuiCol_TabActive] = accentMark;
    s.Colors[ImGuiCol_FrameBgHovered] = { 0.25f, 0.08f, 0.40f, 1.f };
    s.Colors[ImGuiCol_FrameBgActive] = accentHover;
}

// ---------------------------------------------------------------------------
//  Menu content
// ---------------------------------------------------------------------------

static void DrawMenu()
{
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Text("Device:   0x%p", g_pDevice);
    ImGui::Text("Context:  0x%p", g_pContext);
    ImGui::Text("HWND:     0x%p", g_hWnd);
    ImGui::Text("RTV:      0x%p", g_pRenderTarget);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextDisabled("Press INSERT to hide/show this menu.");
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void Overlay_Init(IDXGISwapChain* pChain)
{
    if (g_ImGuiInit) return;
    Log("Overlay_Init...");

    // Grab device from swap chain
    HRESULT hr = pChain->GetDevice(IID_PPV_ARGS(&g_pDevice));
    if (FAILED(hr)) { Log("GetDevice FAILED 0x%08X", hr); return; }
    Log("Device @ 0x%p", g_pDevice);

    g_pDevice->GetImmediateContext(&g_pContext);

    DXGI_SWAP_CHAIN_DESC sd{};
    pChain->GetDesc(&sd);
    g_hWnd = sd.OutputWindow;
    Log("HWND=0x%p fmt=%u bufs=%u", g_hWnd, sd.BufferDesc.Format, sd.BufferCount);

    CreateRTV(pChain);
    if (!g_pRenderTarget) { Log("RTV null — aborting init"); return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyStyle();

    Log("ImGui_ImplWin32_Init : %s", ImGui_ImplWin32_Init(g_hWnd) ? "OK" : "FAIL");
    Log("ImGui_ImplDX11_Init  : %s", ImGui_ImplDX11_Init(g_pDevice, g_pContext) ? "OK" : "FAIL");

    // Subclass the game window so ImGui receives input
    oWndProc = (WndProcFn)SetWindowLongPtrA(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    Log("WndProc subclassed, original=0x%p", oWndProc);

    g_ImGuiInit = true;
    Log("Overlay ready");
}

void Overlay_Draw()
{
    ImGui::SetNextWindowSize({ 350.f, 250.f }, ImGuiCond_FirstUseEver);
    ImGui::Begin("SteamChain Overlay", &g_ShowMenu, ImGuiWindowFlags_NoCollapse);

    DrawMenu();

    ImGui::End();
}

void Overlay_Shutdown()
{
    if (!g_ImGuiInit) return;

    if (g_hWnd && oWndProc)
        SetWindowLongPtrA(g_hWnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyRTV();

    if (g_pContext) { g_pContext->Release(); g_pContext = nullptr; }
    if (g_pDevice) { g_pDevice->Release();  g_pDevice = nullptr; }

    g_ImGuiInit = false;
    Log("Overlay shut down");
}

void Overlay_InvalidateRTV()
{
    ImGui_ImplDX11_InvalidateDeviceObjects();
    DestroyRTV();
}

void Overlay_RebuildRTV(IDXGISwapChain* pChain)
{
    CreateRTV(pChain);
    ImGui_ImplDX11_CreateDeviceObjects();
}
