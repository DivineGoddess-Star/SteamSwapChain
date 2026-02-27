#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#include <dxgi.h>
#include <optional>
#include <vector>

#include "Hooks/MinHook.h"
#include "globals.h"
#include "Overlay/overlay.h"
#include "ImGui/imgui.h"
#include "ImGui/imgui_impl_win32.h"
#include "ImGui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Psapi.lib")

// ---------------------------------------------------------------------------
//  Global definitions  (declared extern in globals.h)
// ---------------------------------------------------------------------------

PresentFn       oPresent = nullptr;
ResizeBuffersFn oResizeBuffers = nullptr;
WndProcFn       oWndProc = nullptr;

ID3D11Device* g_pDevice = nullptr;
ID3D11DeviceContext* g_pContext = nullptr;
ID3D11RenderTargetView* g_pRenderTarget = nullptr;
HWND                    g_hWnd = nullptr;
bool                    g_ImGuiInit = false;
bool                    g_ShowMenu = true;

// ---------------------------------------------------------------------------
//  Console
// ---------------------------------------------------------------------------

static void AllocDebugConsole()
{
    AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    SetConsoleTitleA("Game Overlay — Debug");
}

// ---------------------------------------------------------------------------
//  Steam overlay scanner
//
//  Locates IDXGISwapChain::Present and ::ResizeBuffers hook functions
//  inside GameOverlayRenderer64.dll at runtime, without relying on fixed
//  offsets.  The scan anchors on a stable log string inside the hooking
//  function, walks back to the function prologue via INT3 padding, then
//  decodes the LEA sequences that set up each vtable patch.
// ---------------------------------------------------------------------------

struct SteamHooks { uintptr_t present; uintptr_t resize; };

static std::optional<SteamHooks> FindSteamHooks()
{
    HMODULE hSteam = GetModuleHandleA("GameOverlayRenderer64.dll");
    if (!hSteam) { Log("GameOverlayRenderer64.dll not found"); return std::nullopt; }

    const auto base = (uintptr_t)hSteam;
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), hSteam, &mi, sizeof(mi));
    const uintptr_t end = base + mi.SizeOfImage;
    Log("Steam  base=0x%llX  size=0x%X", base, mi.SizeOfImage);

    // ── Step 1 : locate anchor string ────────────────────────────────────────
    static constexpr char   kAnchor[] = "Hooking vtable for swap chain\n";
    static constexpr size_t kAnchorLen = sizeof(kAnchor) - 1;
    uintptr_t strAddr = 0;

    for (uintptr_t i = base; i < end - kAnchorLen; i++)
        if (memcmp((void*)i, kAnchor, kAnchorLen) == 0) { strAddr = i; break; }

    if (!strAddr) { Log("Anchor string not found"); return std::nullopt; }
    Log("Anchor  @ 0x%llX", strAddr);

    // ── Step 2 : find LEA RCX,[RIP+X]  (48 8D 0D)  →  resolves to strAddr ──
    uintptr_t leaAddr = 0;
    for (uintptr_t i = base; i < end - 7; i++)
    {
        const auto* b = (const uint8_t*)i;
        if (b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x0D)
            if (i + 7 + *(const int32_t*)(b + 3) == strAddr) { leaAddr = i; break; }
    }
    if (!leaAddr) { Log("LEA RCX not found"); return std::nullopt; }
    Log("LEA RCX @ 0x%llX", leaAddr);

    // ── Step 3 : walk back to function start via INT3 padding ────────────────
    uintptr_t fnStart = leaAddr;
    for (uintptr_t i = leaAddr; i > base; i--)
        if (*(const uint8_t*)i == 0xCC) { fnStart = i + 1; break; }
    Log("fn start @ 0x%llX  (RVA 0x%llX)", fnStart, fnStart - base);

    // ── Step 4 : decode (vtable-slot, hookFn, origOut) triplets ─────────────
    //
    //  Pattern emitted by Steam for each hook:
    //    mov rcx, [rdi + slot]      ; vtable slot read (1- or 4-byte disp)
    //    lea rdx, hookFn            ; 48 8D 15 rel32
    //    call sub_18008FCC0         ; already-hooked check (ignore)
    //    test al, al
    //    jnz  skip
    //    mov rcx, [rdi + slot]      ; repeated for the actual patch call
    //    lea r8,  &origOut          ; 4C 8D 05 rel32
    //    lea rdx, hookFn            ; repeated
    //    call sub_18008EDB0         ; vtable patcher  ← E8 triggers commit
    //
    struct Entry { uint32_t slot; uintptr_t hookFn; uintptr_t origOut; };
    std::vector<Entry> entries;

    uint32_t  pendSlot = 0;
    uintptr_t pendHook = 0, pendOrig = 0;

    for (uintptr_t i = fnStart, lim = fnStart + 0x500; i < lim && i < end - 7; )
    {
        const auto* b = (const uint8_t*)i;

        if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x4F)          // mov rcx,[rdi+1b]
        {
            pendSlot = b[3];                      i += 4; continue;
        }
        if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x8F)          // mov rcx,[rdi+4b]
        {
            pendSlot = *(const uint32_t*)(b + 3);   i += 7; continue;
        }
        if (b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x15)          // lea rdx (hook fn)
        {
            pendHook = i + 7 + *(const int32_t*)(b + 3); i += 7; continue;
        }
        if (b[0] == 0x4C && b[1] == 0x8D && b[2] == 0x05)          // lea r8  (orig out)
        {
            pendOrig = i + 7 + *(const int32_t*)(b + 3); i += 7; continue;
        }
        if (b[0] == 0xE8 && pendSlot && pendHook && pendOrig)   // call → commit
        {
            entries.push_back({ pendSlot, pendHook, pendOrig });
            Log("  slot=0x%02X  hookFn=0x%llX  orig=0x%llX", pendSlot, pendHook, pendOrig);
            pendSlot = 0; pendHook = 0; pendOrig = 0;
            i += 5; continue;
        }
        i++;
    }

    // ── Step 5 : extract the two we care about ───────────────────────────────
    SteamHooks result{};
    for (const auto& e : entries)
    {
        if (e.slot == 0x40) { result.present = e.hookFn; Log("Present hookFn @ 0x%llX", e.hookFn); }
        if (e.slot == 0x68) { result.resize = e.hookFn; Log("Resize  hookFn @ 0x%llX", e.hookFn); }
    }

    if (!result.present || !result.resize)
    {
        Log("Incomplete scan — present=0x%llX  resize=0x%llX", result.present, result.resize);
        return std::nullopt;
    }

    return result;
}

// ---------------------------------------------------------------------------
//  Hooked IDXGISwapChain::Present
// ---------------------------------------------------------------------------

static HRESULT WINAPI hkPresent(IDXGISwapChain* pChain, UINT SyncInterval, UINT Flags)
{
    if (!g_ImGuiInit)
        Overlay_Init(pChain);

    if (GetAsyncKeyState(VK_INSERT) & 1)
        g_ShowMenu = !g_ShowMenu;

    if (g_ImGuiInit && g_pRenderTarget)
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_ShowMenu)
            Overlay_Draw();

        ImGui::Render();
        g_pContext->OMSetRenderTargets(1, &g_pRenderTarget, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return oPresent(pChain, SyncInterval, Flags);
}

// ---------------------------------------------------------------------------
//  Hooked IDXGISwapChain::ResizeBuffers
// ---------------------------------------------------------------------------

static HRESULT WINAPI hkResizeBuffers(IDXGISwapChain* pChain,
    UINT Count, UINT W, UINT H, DXGI_FORMAT Fmt, UINT Flags)
{
    Log("ResizeBuffers %ux%u", W, H);
    if (g_ImGuiInit) Overlay_InvalidateRTV();

    HRESULT hr = oResizeBuffers(pChain, Count, W, H, Fmt, Flags);

    if (g_ImGuiInit) Overlay_RebuildRTV(pChain);
    return hr;
}

// ---------------------------------------------------------------------------
//  Hook management
// ---------------------------------------------------------------------------

static void InstallHooks()
{
    Log("InstallHooks...");

    const auto steam = FindSteamHooks();
    if (!steam) { Log("Scanner failed — hooks not installed"); return; }

    MH_STATUS s;
    s = MH_Initialize();
    Log("MH_Initialize   : %s", MH_StatusToString(s));

    s = MH_CreateHook((void*)steam->present, hkPresent, (void**)&oPresent);
    Log("Hook Present  [0x%llX] : %s", steam->present, MH_StatusToString(s));

    s = MH_CreateHook((void*)steam->resize, hkResizeBuffers, (void**)&oResizeBuffers);
    Log("Hook Resize   [0x%llX] : %s", steam->resize, MH_StatusToString(s));

    s = MH_EnableHook(MH_ALL_HOOKS);
    Log("EnableHook      : %s", MH_StatusToString(s));
    Log("oPresent=0x%p  oResize=0x%p", oPresent, oResizeBuffers);
}

static void RemoveHooks()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Overlay_Shutdown();
    FreeConsole();
}

// ---------------------------------------------------------------------------
//  DLL entry point
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD
            {
                AllocDebugConsole();
                Log("Injected — PID %u", GetCurrentProcessId());
                Sleep(2000); // allow Steam overlay to finish hooking DXGI
                InstallHooks();
                return 0;
            }, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        RemoveHooks();
    }
    return TRUE;
}