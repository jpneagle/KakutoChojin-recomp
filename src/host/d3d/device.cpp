// Device creation, presentation, clears and render targets.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <string>

#include <cstdio>
#include <vector>

#include "../manifest.h"
#include "../platform/platform.h"
#include "d3d_internal.h"

namespace d3d {

struct XViewport {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
};
XViewport g_viewport{0, 0, 640, 480, 0, 1};  // logical (API) coordinates
void ApplyViewport();

void FixedInit();
void CompositeOverlay(HostTarget* out, float sx, float sy, float ox);
void SetScreenSpaceConstants(float x, float y, float w, float h, float min_z, float max_z);
uint32_t DrawStats(uint32_t* skipped);
void FixedSetViewport(float x, float y, float w, float h);

std::recursive_mutex g_lock;
UINT g_width = 640, g_height = 480;
uint32_t* g_render_state = nullptr;
uint32_t* g_texture_state = nullptr;

namespace {

HostTarget g_backbuffer_target, g_depth_target;
// The back buffer is rendered larger than its logical (D3D API) size: for
// NV2A supersampling (multisample types 0x2xxx), for a higher output
// resolution, and for widescreen. Logical x maps to ox + x * sx render
// pixels, so widescreen leaves equal space on both sides of the logical
// area; viewports spanning the logical width fill it by compressing clip
// space x by k (see ApplyViewport).
struct TargetMap {
    float sx = 1, sy = 1, ox = 0, k = 1;
    float xbox_sx = 1, xbox_sy = 1;  // logical -> NV2A pixels (supersampling only)
};
TargetMap g_bb_map;
UINT g_out_w = 640, g_out_h = 480;  // output image (g_output_target)
UINT g_ss_x = 1, g_ss_y = 1;        // render pixels per output pixel
HostTarget g_output_target;
xd3d::Surface* g_backbuffer = nullptr;
xd3d::Surface* g_depth = nullptr;
xd3d::Surface* g_cur_rt = nullptr;
xd3d::Surface* g_cur_ds = nullptr;
uint32_t g_frame = 0;
UINT g_present_interval = 1;  // vblanks per Swap (0 = unthrottled)
int g_dump_every = 0;  // KT_DUMP_FRAMES=N: write every Nth frame to frames/

// Video options: KT_<NAME> in the environment, else [video] <name> in
// KakutoChojin.ini next to the runtime.
int VideoOption(const char* name, int def) {
    char var[64];
    snprintf(var, sizeof var, "KT_%s", name);
    for (char* c = var; *c; c++) *c = char(toupper(*c));
    if (const char* v = os::Env(var)) return atoi(v);
    return os::IniInt("video", name, def);
}

void DumpFrame(uint32_t frame) {
    std::vector<uint8_t> px;
    UINT w = g_output_target.width, h = g_output_target.height;
    if (!gpu::ReadTexture(g_output_target.tex, {0, 0, int(w), int(h)}, &px)) return;
    std::error_code ec;
    std::filesystem::create_directories(HostDir() / "frames", ec);
    char name[32];
    snprintf(name, sizeof name, "frame_%05u.bmp", frame);
    if (FILE* f = os::OpenFile(HostDir() / "frames" / name, "wb")) {
#pragma pack(push, 2)
        struct {
            uint16_t type;
            uint32_t size, reserved, offset;
            uint32_t hsize;
            int32_t width, height;
            uint16_t planes, bits;
            uint32_t compression, image_size;
            int32_t xppm, yppm;
            uint32_t colors, important;
        } hdr{0x4D42, uint32_t(54 + w * h * 4), 0, 54, 40, int32_t(w), -int32_t(h), 1, 32, 0, 0, 0, 0, 0, 0};
#pragma pack(pop)
        fwrite(&hdr, sizeof hdr, 1, f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
    }
}

// Filters the (possibly supersampled) back buffer into the output image.
// Sampling at output pixel centres with bilinear filtering averages the
// 2x1 / 2x2 source pixels behind each output pixel exactly.
void Downsample() { gpu::BlitTexture(g_backbuffer_target.tex, g_output_target.tex); }

// Holds each Swap to the title's presentation interval at the NTSC field
// rate, as the NV2A does by waiting for vblank. KT_NO_FRAME_LIMIT=1 runs
// as fast as possible (unattended tests).
void PaceFrame() {
    using Clock = std::chrono::steady_clock;
    static const bool unlimited = os::EnvSet("KT_NO_FRAME_LIMIT");
    if (unlimited || g_present_interval == 0) return;
    static Clock::time_point next;
    auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(1001000000ll / 60) * g_present_interval);
    Clock::time_point now = Clock::now();
    if (next == Clock::time_point() || now - next > 4 * period) next = now;  // first frame or long stall
    next += period;
    // Sleep until shortly before the deadline (1 ms timer resolution), then spin.
    auto coarse = next - std::chrono::microseconds(1500);
    if (coarse > now) std::this_thread::sleep_until(coarse);
    while (Clock::now() < next) std::this_thread::yield();
}

// Shows the output image in the window, scaled to fit with its aspect ratio
// kept (black bars elsewhere).
void Present() { gpu::Present(g_output_target.tex); }

void ResolveStateArrays() {
    g_render_state = G2H<uint32_t>(HleVar("D3D8", "D3D_g_RenderState"));
    g_texture_state = G2H<uint32_t>(HleVar("D3D8", "D3D_g_DeferredTextureState"));
    if (!g_render_state || !g_texture_state)
        Fatal("D3D state arrays not found in manifest (D3D_g_RenderState / D3D_g_DeferredTextureState)");
}

// Direct3D_CreateDevice normally fills the state arrays (which live in the
// D3D section's BSS) with the D3D8 defaults; titles rely on them.
void InitDefaultStates(bool depth) {
    using namespace xd3d;
    auto f = [](float v) { uint32_t u; memcpy(&u, &v, 4); return u; };
    uint32_t* rs = g_render_state;
    memset(rs, 0, RS_MAX * 4);
    rs[RS_ZFUNC] = 0x203;           // LESSEQUAL
    rs[RS_ALPHAFUNC] = 0x207;       // ALWAYS
    rs[RS_SRCBLEND] = 1;            // ONE
    rs[RS_DESTBLEND] = 0;           // ZERO
    rs[RS_ZWRITEENABLE] = 1;
    rs[RS_SHADEMODE] = 0x1D01;      // GOURAUD
    rs[RS_COLORWRITEENABLE] = 0x01010101;
    rs[RS_STENCILZFAIL] = rs[RS_STENCILPASS] = rs[RS_STENCILFAIL] = 0x1E00;  // KEEP
    rs[RS_STENCILFUNC] = 0x207;
    rs[RS_STENCILMASK] = rs[RS_STENCILWRITEMASK] = 0xFF;
    rs[RS_BLENDOP] = 0x8006;        // ADD
    rs[RS_SWATHWIDTH] = 4;
    rs[RS_FOGEND] = f(1.f), rs[RS_FOGDENSITY] = f(1.f);
    rs[RS_LIGHTING] = 1, rs[RS_LOCALVIEWER] = 1, rs[RS_COLORVERTEX] = 1;
    rs[RS_SPECULARMATERIALSOURCE] = rs[RS_BACKSPECULARMATERIALSOURCE] = 2;  // COLOR2
    rs[RS_DIFFUSEMATERIALSOURCE] = rs[RS_BACKDIFFUSEMATERIALSOURCE] = 1;    // COLOR1
    rs[RS_POINTSIZE] = f(1.f), rs[RS_POINTSIZE_MIN] = f(1.f), rs[RS_POINTSCALE_A] = f(1.f);
    rs[RS_POINTSIZE_MAX] = f(64.f), rs[RS_PATCHSEGMENTS] = f(1.f);
    rs[RS_FILLMODE] = rs[RS_BACKFILLMODE] = 0x1B02;  // SOLID
    rs[RS_ZENABLE] = depth ? 1 : 0;
    rs[RS_FRONTFACE] = 0x900;       // CW
    rs[RS_CULLMODE] = 0x901;        // cull CCW
    rs[RS_TEXTUREFACTOR] = 0xFFFFFFFF;
    rs[RS_MULTISAMPLEANTIALIAS] = 1, rs[RS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    for (int s = 0; s < 4; s++) {
        uint32_t* t = g_texture_state + s * TSS_PER_STAGE;
        memset(t, 0, TSS_PER_STAGE * 4);
        t[TSS_ADDRESSU] = t[TSS_ADDRESSV] = t[TSS_ADDRESSW] = 1;  // WRAP
        t[TSS_MAGFILTER] = t[TSS_MINFILTER] = 1;                  // POINT
        t[TSS_MAXANISOTROPY] = 1;
        t[TSS_COLOROP] = s == 0 ? 4 : 1;  // MODULATE / DISABLE
        t[TSS_ALPHAOP] = s == 0 ? 2 : 1;  // SELECTARG1 / DISABLE
        t[TSS_COLORARG1] = t[TSS_ALPHAARG1] = 2;  // TEXTURE
        t[TSS_COLORARG2] = t[TSS_ALPHAARG2] = 1;  // CURRENT
        t[TSS_COLORARG0] = t[TSS_ALPHAARG0] = t[TSS_RESULTARG] = 1;
        t[TSS_TEXCOORDINDEX] = s;
    }
}

xd3d::Surface* AddRef(xd3d::Surface* s) {
    if (s) s->Common++;
    return s;
}

}  // namespace

void CreateTarget(HostTarget* t, UINT w, UINT h, bool depth) {
    t->tex = gpu::CreateRenderTarget(w, h, depth);
    t->width = w, t->height = h;
}

uint32_t CurrentFrame() { return g_frame; }

HostTarget* CurrentRenderTarget() { return g_cur_rt ? HostTargetFor(g_cur_rt) : nullptr; }

// Logical (D3D API) to render pixels for the current target: only the back
// buffer and its depth buffer are enlarged.
static TargetMap CurrentMap() {
    HostTarget* rt = g_cur_rt ? HostTargetFor(g_cur_rt) : nullptr;
    bool bb = rt == &g_backbuffer_target || (!rt && g_cur_ds && HostTargetFor(g_cur_ds) == &g_depth_target);
    return bb ? g_bb_map : TargetMap{};
}

// Host-only vertex shader constants (cbuffer HostVS, b2): x = clip-space x scale.
static void SetHostVsConstants(float clip_x) {
    float c[4] = {clip_x, 0, 0, 0};
    gpu::SetConstants(gpu::Stage::Vertex, 2, c, sizeof c);
}
HostTarget* CurrentDepthTarget() { return g_cur_ds ? HostTargetFor(g_cur_ds) : nullptr; }

void BindTargets() {
    HostTarget* rt = CurrentRenderTarget();
    HostTarget* ds = CurrentDepthTarget();
    gpu::SetRenderTargets(rt ? rt->tex : nullptr, ds ? ds->tex : nullptr);
}

// ---- Exports ----------------------------------------------------------------------

HRESULT WINAPI x_D3D_SetPushBufferSize(DWORD, DWORD) { return S_OK; }

HRESULT WINAPI x_Direct3D_CreateDevice(UINT, DWORD, uint32_t, DWORD, xd3d::PresentParameters* pp, uint32_t* out) {
    D3D_LOCK;
    ResolveStateArrays();
    InitDefaultStates(pp->EnableAutoDepthStencil != 0);
    g_width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    g_height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    Log("Direct3D_CreateDevice: %ux%u fmt=%02x ms=%x autodepth=%u depthfmt=%02x flags=%x interval=%x", g_width,
        g_height, pp->BackBufferFormat, pp->MultiSampleType, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat,
        pp->Flags, pp->FullScreen_PresentationInterval);
    g_dump_every = os::EnvInt("KT_DUMP_FRAMES", 0);
    // D3DPRESENT_INTERVAL_*: DEFAULT/ONE = every vblank, TWO = 2, THREE = 4 (bit), FOUR = 8, IMMEDIATE.
    uint32_t pi = pp->FullScreen_PresentationInterval;
    g_present_interval = (pi & 0x80000000) ? 0 : (pi & 8) ? 4 : (pi & 4) ? 3 : (pi & 2) ? 2 : 1;

    // D3DMULTISAMPLE_TYPE: the low byte holds the horizontal (high nibble)
    // and vertical (low nibble) sample layout, so the surface memory is that
    // much larger. Supersample types (0x2xxx) also render at that size with
    // scaled viewports; multisample types (0x1xxx) keep logical coordinates.
    // A higher output resolution replaces supersampling.
    uint32_t ms = pp->MultiSampleType;
    UINT mx = std::clamp<UINT>((ms >> 4) & 0xF, 1, 3), my = std::clamp<UINT>(ms & 0xF, 1, 3);
    UINT resolution = UINT(std::clamp(VideoOption("resolution", 0), 0, 4320));
    bool widescreen = VideoOption("widescreen", 0) != 0;
    bool supersample = (ms & 0xF000) == 0x2000 && !os::EnvSet("KT_NO_SUPERSAMPLE") &&
                       resolution <= g_height;
    g_ss_x = supersample ? mx : 1, g_ss_y = supersample ? my : 1;
    g_out_h = resolution ? resolution : g_height;
    float scale = float(g_out_h) / g_height;
    UINT logical_w = UINT(g_width * scale + 0.5f);
    g_out_w = widescreen ? std::max(logical_w, (g_out_h * 16 / 9 + 1) & ~1u) : logical_w;
    g_bb_map.sx = scale * g_ss_x, g_bb_map.sy = scale * g_ss_y;
    g_bb_map.ox = (float(g_out_w * g_ss_x) - g_width * g_bb_map.sx) / 2;
    g_bb_map.k = g_width * g_bb_map.sx / float(g_out_w * g_ss_x);
    g_bb_map.xbox_sx = float(g_ss_x), g_bb_map.xbox_sy = float(g_ss_y);

    platform::CreateMainWindow("Kakuto Chojin", int(g_out_w), int(g_out_h), VideoOption("fullscreen", 0) != 0);
    gpu::CreateDevice();

    Log("back buffer: %ux%u (surface %ux%u), rendered at %ux%u, output %ux%u%s", g_width, g_height, g_width * mx,
        g_height * my, g_out_w * g_ss_x, g_out_h * g_ss_y, g_out_w, g_out_h, widescreen ? " widescreen" : "");
    CreateTarget(&g_backbuffer_target, g_out_w * g_ss_x, g_out_h * g_ss_y, false);
    CreateTarget(&g_depth_target, g_out_w * g_ss_x, g_out_h * g_ss_y, true);
    CreateTarget(&g_output_target, g_out_w, g_out_h, false);
    g_backbuffer = NewSurfaceHeader(pp->BackBufferFormat, g_width * mx, g_height * my, 0);
    g_depth = NewSurfaceHeader(pp->AutoDepthStencilFormat ? pp->AutoDepthStencilFormat : xd3d::FMT_LIN_D24S8,
                               g_width * mx, g_height * my, 0);
    RegisterTarget(g_backbuffer, &g_backbuffer_target);
    RegisterTarget(g_depth, &g_depth_target);
    g_cur_rt = g_backbuffer;
    g_cur_ds = pp->EnableAutoDepthStencil ? g_depth : nullptr;
    BindTargets();
    FixedInit();
    g_viewport = {0, 0, g_width, g_height, 0, 1};
    ApplyViewport();

    // Xbox D3D is a singleton; hand back something non-null and stable.
    static void* device_object = GuestAlloc(0x3000);
    if (out) *out = H2G(device_object);
    return S_OK;
}


ULONG WINAPI x_D3DDevice_AddRef() { return 1; }
ULONG WINAPI x_D3DDevice_Release() { return 1; }

DWORD WINAPI x_D3DDevice_Swap(DWORD) {
    D3D_LOCK;
    uint32_t n = ++g_frame;
    Downsample();
    CompositeOverlay(&g_output_target, g_bb_map.sx / g_ss_x, g_bb_map.sy / g_ss_y, g_bb_map.ox / g_ss_x);
    if (g_dump_every > 0 && n % g_dump_every == 0) DumpFrame(n);
    Present();
    PaceFrame();
    BindTargets();  // presents and the blits above change bindings
    ApplyViewport();
    ResourcesBeginFrame();
    if (n <= 3 || n % 600 == 0) {
        uint32_t skipped, draws = DrawStats(&skipped);
        Log("Swap: frame %u (draws %u, skipped %u)", n, draws, skipped);
    }
    return n;
}

void WINAPI x_D3DDevice_BlockUntilVerticalBlank() {}

xd3d::Surface* WINAPI x_D3DDevice_GetBackBuffer2(INT) { return AddRef(g_backbuffer); }
xd3d::Surface* WINAPI x_D3DDevice_GetRenderTarget2() { return AddRef(g_cur_rt); }
xd3d::Surface* WINAPI x_D3DDevice_GetDepthStencilSurface2() { return AddRef(g_cur_ds); }

HRESULT WINAPI x_D3DDevice_SetViewport(const XViewport* v);

// Like D3D8, changing the render target resets the viewport to cover it.
HRESULT WINAPI x_D3DDevice_SetRenderTarget(xd3d::Surface* rt, xd3d::Surface* ds) {
    D3D_LOCK;
    if (rt) g_cur_rt = rt;
    g_cur_ds = ds;
    BindTargets();
    if (HostTarget* t = rt ? CurrentRenderTarget() : nullptr) {
        XViewport vp{0, 0, t->width, t->height, 0, 1};
        if (t == &g_backbuffer_target) vp.Width = g_width, vp.Height = g_height;
        x_D3DDevice_SetViewport(&vp);
    }
    return S_OK;
}


// The host viewport is in render pixels; the vertex-program screen-space
// constants are in NV2A pixels and pre-transformed (XYZRHW) vertices in
// logical ones, as the title expects.
//
// Widescreen: a viewport spanning the logical width is widened to the whole
// render target and clip-space x is compressed by k, so the logical area
// keeps its shape in the middle while 3D scenes extend to the sides. 2D
// layouts (and narrower viewports) stay within the logical area.
void ApplyViewport() {
    TargetMap m = CurrentMap();
    const XViewport& v = g_viewport;
    float x = m.ox + v.X * m.sx, y = v.Y * m.sy, w = v.Width * m.sx, h = v.Height * m.sy;
    float k = (m.k != 1 && v.X == 0 && v.Width >= g_width) ? m.k : 1;
    gpu::SetViewport({x + w / 2 - w / k / 2, y, w / k, h, v.MinZ, v.MaxZ});
    FixedSetViewport(float(v.X), float(v.Y), float(v.Width), float(v.Height));
    // Programs may compute oPos in screen pixels themselves, so the
    // constants stay in the pixels the title expects (as on the NV2A); the
    // host viewport alone maps to render pixels.
    SetScreenSpaceConstants(v.X * m.xbox_sx, v.Y * m.xbox_sy, v.Width * m.xbox_sx, v.Height * m.xbox_sy, v.MinZ,
                            v.MaxZ);
    SetHostVsConstants(k);
}

HRESULT WINAPI x_D3DDevice_SetViewport(const XViewport* v) {
    D3D_LOCK;
    g_viewport = *v;
    ApplyViewport();
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_GetViewport(XViewport* v) {
    *v = g_viewport;
    return S_OK;
}

// D3D8 semantics: clears the given rects clipped to the viewport, or the
// whole viewport when none are given.
HRESULT WINAPI x_D3DDevice_Clear(DWORD count, const xd3d::Rect* rects, DWORD flags, DWORD color, float z,
                                 DWORD stencil) {
    D3D_LOCK;
    HostTarget* rt = CurrentRenderTarget();
    HostTarget* ds = CurrentDepthTarget();
    HostTarget* any = rt ? rt : ds;
    if (!any) return S_OK;
    // Rects and the viewport are logical; the back buffer is enlarged. Edges
    // on the logical border extend to the target's (widescreen side areas).
    TargetMap m = CurrentMap();
    auto mx = [&](LONG x) { return x <= 0 ? 0 : x >= LONG(g_width) && m.ox > 0 ? LONG(any->width) : LONG(m.ox + x * m.sx); };
    auto my = [&](LONG y) { return LONG(y * m.sy); };
    gpu::Rect vp{mx(g_viewport.X), my(g_viewport.Y), mx(g_viewport.X + g_viewport.Width),
                 my(g_viewport.Y + g_viewport.Height)};
    std::vector<gpu::Rect> clip;
    if (count && rects) {
        for (DWORD i = 0; i < count; i++) {
            gpu::Rect r{std::max<int>(mx(rects[i].x1), vp.left), std::max<int>(my(rects[i].y1), vp.top),
                        std::min<int>(mx(rects[i].x2), vp.right), std::min<int>(my(rects[i].y2), vp.bottom)};
            if (r.right > r.left && r.bottom > r.top) clip.push_back(r);
        }
        if (clip.empty()) return S_OK;
    } else {
        clip.push_back(vp);
    }
    float c[4] = {((color >> 16) & 0xFF) / 255.f, ((color >> 8) & 0xFF) / 255.f, (color & 0xFF) / 255.f,
                  (color >> 24) / 255.f};
    uint8_t s8 = uint8_t(stencil);
    bool clear_color = (flags & xd3d::kClearTargetMask) && rt;
    bool clear_depth = (flags & xd3d::kClearZBuffer) && ds, clear_stencil = (flags & xd3d::kClearStencil) && ds;
    if (clear_color || clear_depth || clear_stencil)
        gpu::Clear(clip.data(), uint32_t(clip.size()), clear_color ? c : nullptr, clear_depth ? &z : nullptr,
                   clear_stencil ? &s8 : nullptr);
    return S_OK;
}

// D3DCAPS8 (53 dwords on x86) with NV2A limits; capability bitmasks report
// everything as supported.
HRESULT WINAPI x_D3DDevice_GetDeviceCaps(uint32_t* caps) {
    for (int i = 0; i < 53; i++) caps[i] = 0xFFFFFFFF;
    auto f = [](float v) { uint32_t u; memcpy(&u, &v, 4); return u; };
    caps[0] = 1;  // D3DDEVTYPE_HAL
    caps[1] = 0;  // adapter ordinal
    caps[22] = caps[23] = 4096;  // MaxTextureWidth/Height
    caps[24] = 512;              // MaxVolumeExtent
    caps[25] = 8192;             // MaxTextureRepeat
    caps[26] = 0;                // MaxTextureAspectRatio
    caps[27] = 4;                // MaxAnisotropy
    caps[28] = f(1e10f);         // MaxVertexW
    caps[29] = f(-1e8f), caps[30] = f(-1e8f), caps[31] = f(1e8f), caps[32] = f(1e8f);  // guard band
    caps[33] = f(0);             // ExtentsAdjust
    caps[37] = 4;                // MaxTextureBlendStages
    caps[38] = 4;                // MaxSimultaneousTextures
    caps[40] = 8;                // MaxActiveLights
    caps[41] = 4;                // MaxUserClipPlanes
    caps[42] = 4;                // MaxVertexBlendMatrices
    caps[43] = 0;                // MaxVertexBlendMatrixIndex
    caps[44] = f(64.f);          // MaxPointSize
    caps[45] = 0xFFFFF;          // MaxPrimitiveCount
    caps[46] = 0xFFFF;           // MaxVertexIndex
    caps[47] = 16;               // MaxStreams
    caps[48] = 255;              // MaxStreamStride
    caps[49] = 0xFFFE0101;       // VertexShaderVersion 1.1
    caps[50] = 192;              // MaxVertexShaderConst
    caps[51] = 0xFFFF0101;       // PixelShaderVersion 1.1
    caps[52] = f(1.f);           // MaxPixelShaderValue
    return S_OK;
}

struct XDisplayMode {
    UINT Width, Height, RefreshRate;
    DWORD Flags, Format;
};

HRESULT WINAPI x_D3DDevice_GetDisplayMode(XDisplayMode* m) {
    *m = {g_width, g_height, 60, 0, xd3d::FMT_LIN_X8R8G8B8};
    return S_OK;
}

HLE_EXPORT("D3D8", D3DDevice_GetDisplayMode);
HLE_EXPORT("D3D8", D3DDevice_GetDeviceCaps);
HLE_EXPORT("D3D8", D3D_SetPushBufferSize);
HLE_EXPORT("D3D8", Direct3D_CreateDevice);
HLE_EXPORT("D3D8", D3DDevice_AddRef);
HLE_EXPORT("D3D8", D3DDevice_Release);
HLE_EXPORT("D3D8", D3DDevice_Clear);
HLE_EXPORT("D3D8", D3DDevice_Swap);
HLE_EXPORT("D3D8", D3DDevice_BlockUntilVerticalBlank);
HLE_EXPORT("D3D8", D3DDevice_GetBackBuffer2);
HLE_EXPORT("D3D8", D3DDevice_GetRenderTarget2);
HLE_EXPORT("D3D8", D3DDevice_GetDepthStencilSurface2);
HLE_EXPORT("D3D8", D3DDevice_SetRenderTarget);
HLE_EXPORT("D3D8", D3DDevice_SetViewport);
HLE_EXPORT("D3D8", D3DDevice_GetViewport);

// Hardware features with no visible effect here.
HLE_IGNORE("D3D8", D3DDevice_SetSoftDisplayFilter);
HLE_IGNORE("D3D8", D3DDevice_SetFlickerFilter);
HLE_IGNORE("D3D8", D3DDevice_InsertFence);
HLE_IGNORE("D3D8", D3DDevice_SetShaderConstantMode);

}  // namespace d3d
