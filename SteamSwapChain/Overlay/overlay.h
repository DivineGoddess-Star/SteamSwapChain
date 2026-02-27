#pragma once

#include <dxgi.h>

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void Overlay_Init(IDXGISwapChain* pChain);
void Overlay_Draw();
void Overlay_Shutdown();
void Overlay_InvalidateRTV();
void Overlay_RebuildRTV(IDXGISwapChain* pChain);