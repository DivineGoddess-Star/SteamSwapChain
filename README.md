# SteamSwapChain

A generic DirectX 11 in-process overlay framework built on **Dear ImGui** and **MinHook**.

Rather than relying on hardcoded vtable offsets — which break on every game or runtime update — this project locates `IDXGISwapChain::Present` and `IDXGISwapChain::ResizeBuffers` hook targets by scanning **Steam's overlay DLL at runtime**. Hook addresses are resolved dynamically on every injection, making the overlay resilient to Steam updates with zero maintenance.

---

## How It Works

Steam's `GameOverlayRenderer64.dll` installs its own hooks into the DXGI swap chain vtable before any user code runs. This project chains off those existing hooks rather than competing with them, ensuring both overlays coexist cleanly.

### Scanner Pipeline

```
GameOverlayRenderer64.dll  (already mapped in-process by Steam)
│
▼  1. String scan
"Hooking vtable for swap chain\n"  →  known stable anchor
│
▼  2. LEA RCX scan
48 8D 0D [rel32]  resolving to the anchor string
places us inside the vtable-hooking function
│
▼  3. Walk backwards
INT3 (0xCC) padding sled  →  function prologue
│
▼  4. Forward scan
Decode (vtable-slot, hookFn, origOut) triplets
from repeating  LEA RDX / LEA R8 / CALL  sequences
│
▼  5. Extract
slot 0x40  →  IDXGISwapChain::Present
slot 0x68  →  IDXGISwapChain::ResizeBuffers
```

MinHook then hooks Steam's hook functions directly, so our frame sits at the front of the call chain:

```
Game → Steam vtable patch → hkPresent (ours) → oPresent trampoline → Steam logic → D3D
```

---

## Project Structure

```
SteamSwapChain/
│
├── SteamSwapChain/
│   ├── dllmain.cpp              # DLL entry, scanner, hook installation
│   ├── globals.h                # Shared types, extern globals, Log()
│   │
│   ├── Hooks/
│   │   └── MinHook.h            # MinHook library header
│   │
│   ├── Overlay/
│   │   ├── overlay.h            # Public overlay API (5 functions)
│   │   └── overlay.cpp          # ImGui init, style, menu, RTV management
│   │
│   └── ImGui/                   # Dear ImGui library files
│       ├── imgui.h / .cpp       # Core ImGui
│       ├── imgui_impl_dx11.*    # DirectX 11 backend
│       ├── imgui_impl_win32.*   # Win32 platform backend
│       └── ...                  # Additional ImGui components
│
└── README.md
```

### Module Responsibilities

| File | Responsibility |
|------|----------------|
| `dllmain.cpp` | DLL lifecycle, scanner, hook installation, console |
| `globals.h` | Shared types (`PresentFn` etc.), `extern` globals, `Log()` |
| `Overlay/overlay.cpp` | ImGui context, DX11 backend, RTV, WndProc subclass, menu |
| `Overlay/overlay.h` | Minimal public API — the only surface `dllmain.cpp` depends on |

---

## Dependencies

| Library | Repository | Version |
|---------|-----------|---------|
| **Dear ImGui** | [ocornut/imgui](https://github.com/ocornut/imgui) | Included |
| **MinHook** | [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) | Header-only |
| **DirectX 11 SDK** | Windows SDK | Included with VS2019+ |

---

## Building

### Requirements

- Visual Studio 2019 or later (v142+ toolset)
- Windows SDK 10.0.19041.0 or later
- Target platform: **x64 only**
- C++ standard: **C++17 or later**

### Steps

**1.** Open `SteamSwapChain.sln` in Visual Studio.

**2.** Verify project configuration:
   - All ImGui source files are included in the project
   - MinHook header is accessible via include path

**3.** Build **Release | x64**.

**4.** Output DLL will be generated in `x64/Release/SteamSwapChain.dll`

### Build Configurations

- **Debug**: Includes debug symbols, console logging, and runtime checks
- **Release**: Optimized build with link-time code generation (LTCG)

---

## Usage

Inject the compiled `.dll` into any DirectX 11 process that runs through Steam (so that `GameOverlayRenderer64.dll` is present).

| Key | Action |
|-----|--------|
| `INSERT` | Toggle overlay visibility |

A debug console opens automatically on injection. Expected output on success:

```
[Overlay] Injected — PID 32732
[Overlay] InstallHooks...
[Overlay] Steam  base=0x7FFE32E20000  size=0x1AC000
[Overlay] Anchor  @ 0x7FFE32F3E8B0
[Overlay] LEA RCX @ 0x7FFE32EB59F9
[Overlay] fn start @ 0x7FFE32EB59B0  (RVA 0x959B0)
[Overlay]   slot=0x10  hookFn=0x7FFE32EB5310  orig=0x7FFE32F821D0
[Overlay]   slot=0x40  hookFn=0x7FFE32EB5160  orig=0x7FFE32F821D8
[Overlay]   slot=0x50  hookFn=0x7FFE32EB5690  orig=0x7FFE32F821E8
[Overlay]   slot=0x68  hookFn=0x7FFE32EB5530  orig=0x7FFE32F821E0
[Overlay]   slot=0xB0  hookFn=0x7FFE32EB4FB0  orig=0x7FFE32F821F0
[Overlay]   slot=0x130  hookFn=0x7FFE32EB5610  orig=0x7FFE32F821F8
[Overlay]   slot=0x138  hookFn=0x7FFE32EB53D0  orig=0x7FFE32F82200
[Overlay] Present hookFn @ 0x7FFE32EB5160
[Overlay] Resize  hookFn @ 0x7FFE32EB5530
[Overlay] MH_Initialize   : MH_OK
[Overlay] Hook Present  [0x7FFE32EB5160] : MH_OK
[Overlay] Hook Resize   [0x7FFE32EB5530] : MH_OK
[Overlay] EnableHook      : MH_OK
[Overlay] oPresent=0x00007FFE32C00FC0  oResize=0x00007FFE32C00F80
[Overlay] Overlay_Init...
[Overlay] Device @ 0x0000019D8BAF1898
[Overlay] HWND=0x0000000000120EF6 fmt=87 bufs=2
[Overlay] RTV @ 0x0000019DC2E33778
[Overlay] ImGui_ImplWin32_Init : OK
[Overlay] ImGui_ImplDX11_Init  : OK
[Overlay] WndProc subclassed, original=0x00000000FFFF0D31
[Overlay] Overlay ready
```

---

## Extending the Menu

All menu content lives in `SteamSwapChain/Overlay/overlay.cpp`.

Add tabs by defining a new `static void Tab_Foo()` function and registering it in `Overlay_Draw()`:

```cpp
static void Tab_Foo()
{
    ImGui::Text("Hello from Tab_Foo");
}

void Overlay_Draw()
{
    ImGui::SetNextWindowSize({ 400.f, 300.f }, ImGuiCond_FirstUseEver);
    ImGui::Begin("Game Overlay  |  INSERT to toggle", &g_ShowMenu,
        ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("##MainTabs"))
    {
        if (ImGui::BeginTabItem("Visuals")) { Tab_Visuals(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Foo"))     { Tab_Foo();     ImGui::EndTabItem(); } // ← new
        if (ImGui::BeginTabItem("Misc"))    { Tab_Misc();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Info"))    { Tab_Info();    ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
```

---

## Update Resilience

The scanner uses **no hardcoded addresses or offsets** into any binary.

It resolves everything at runtime from the log string `"Hooking vtable for swap chain\n"`, which has been present in Steam's overlay for many years. Provided Steam continues to emit that string during initialization, the scanner will locate the correct functions regardless of internal refactoring or version changes.

**Limitations**: If Steam significantly refactors the hooking mechanism, removes the anchor string, or changes the instruction patterns used for vtable patching, the scanner may fail. In such cases, the debug console will indicate which stage of the scan failed, allowing for targeted updates to the pattern matching logic.

---

## License

MIT — see `LICENSE` for details.

---

## Acknowledgments

- **Dear ImGui**: Omar Cornut and contributors for the excellent GUI library
- **MinHook**: Tsuda Kageyu for the robust hooking library
- **Steam**: Valve Corporation for the overlay infrastructure

---

**Platform**: Windows x64  
**DirectX Version**: 11  
**Language**: C++17
