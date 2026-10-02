// State shared by the D3D8 (Xbox) HLE translation units. Host rendering goes
// through gpu.h.
#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "../hle.h"
#include "../gpu/gpu.h"
#include "../host.h"
#include "xbox_d3d.h"

namespace d3d {

// The D3D state and gpu.h are single-threaded; every export that touches
// them holds this lock (titles call D3D from loader threads too).
extern std::recursive_mutex g_lock;
#define D3D_LOCK std::lock_guard<std::recursive_mutex> d3d_lock_guard_(d3d::g_lock)

extern UINT g_width, g_height;

// Xbox D3D global state arrays inside the title image (from the manifest).
extern uint32_t* g_render_state;   // [xd3d::RS_MAX]
extern uint32_t* g_texture_state;  // [4][xd3d::TSS_PER_STAGE]
inline uint32_t RS(xd3d::RenderState s) { return g_render_state[s]; }
inline uint32_t TSS(int stage, xd3d::TextureStageState s) { return g_texture_state[stage * xd3d::TSS_PER_STAGE + s]; }
inline float AsFloat(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// ---- Render targets (device.cpp / resource.cpp) ----
struct HostTarget {
    gpu::Texture* tex = nullptr;
    UINT width = 0, height = 0;
};
HostTarget* CurrentRenderTarget();
HostTarget* CurrentDepthTarget();
void CreateTarget(HostTarget* t, UINT w, UINT h, bool depth);
void BindTargets();

// ---- Resources (resource.cpp) ----
struct FormatInfo {
    uint32_t bpp;     // bits per texel (per block texel for DXT)
    bool swizzled;    // NV2A swizzled storage
    bool compressed;  // DXT / BCn
    bool depth;
    gpu::Format host;  // BCn for DXT, BGRA8 for everything converted, D24S8 for depth
    bool supported;
};
FormatInfo GetFormatInfo(uint32_t xfmt);
uint32_t FormatOf(const xd3d::PixelContainer* p);
void GetDimensions(const xd3d::PixelContainer* p, UINT* w, UINT* h, UINT* levels);

HostTarget* HostTargetFor(xd3d::Surface* s);
gpu::Texture* HostTexture(xd3d::PixelContainer* t);
xd3d::Surface* NewSurfaceHeader(uint32_t format, UINT w, UINT h, uint32_t data);
void RegisterTarget(xd3d::Surface* s, HostTarget* t);
void ResourcesBeginFrame();

// ---- Draw state (state.cpp) ----
void ApplyPipelineState();

}  // namespace d3d
