#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

// ---------------------------------------------------------------------------
//  Function pointer typedefs
// ---------------------------------------------------------------------------

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using WndProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
//  Shared globals  (defined in dllmain.cpp)
// ---------------------------------------------------------------------------

extern PresentFn       oPresent;
extern ResizeBuffersFn oResizeBuffers;
extern WndProcFn       oWndProc;

extern ID3D11Device* g_pDevice;
extern ID3D11DeviceContext* g_pContext;
extern ID3D11RenderTargetView* g_pRenderTarget;
extern HWND                    g_hWnd;
extern bool                    g_ImGuiInit;
extern bool                    g_ShowMenu;

// ---------------------------------------------------------------------------
//  Logging
// ---------------------------------------------------------------------------

inline void Log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("[Overlay] %s\n", buf);
}