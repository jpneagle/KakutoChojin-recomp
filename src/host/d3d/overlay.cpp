// Video overlay (EnableOverlay/UpdateOverlay), composited at Swap.
//
// The Xbox scans the overlay (usually a YUY2 movie frame) out under the
// graphics plane; with a colour key, graphics pixels equal to the key show the
// video. Here the overlay is blended into the back buffer before presenting.
#include "shader.h"

namespace d3d {

namespace {

bool g_enabled = false;
xd3d::Surface* g_surface = nullptr;
RECT g_src{}, g_dst{};
bool g_use_key = false;
DWORD g_key = 0;

const char* const kOverlayVS = R"(
UNIFORMS_VS(OverlayVS, 0) { float4 src_uv; float4 dst; };
void XboxVS(float4 v[16], int vid, out VSOut o) {
    float2 t = float2(vid & 1, vid >> 1);
    o.pos = float4(lerp(dst.xy, dst.zw, t) * float2(2, -2) + float2(-1, 1), 0, 1);
    o.t0 = float4(lerp(src_uv.xy, src_uv.zw, t), 0, 0);
    o.d0 = float4(0, 0, 0, 0); o.d1 = o.d0; o.t1 = o.d0; o.t2 = o.d0; o.t3 = o.d0;
    o.fog = 0;
}
)";

const char* const kOverlayPS = R"(
UNIFORMS_PS(OverlayPS, 0) { float4 key; };
TEXTURE2D(video, 0)
TEXTURE2D(graphics, 1)
float4 XboxPS(VSOut i) {
    float4 g = LOAD2D(graphics, i.pos.xy);
    float4 v = float4(SAMPLE(video, i.t0.xy).rgb, 1);
    float3 d = abs(g.rgb - key.rgb);
    if (key.w > 0 && max(max(d.r, d.g), d.b) > 1.5 / 255) return g;
    return v;
}
)";

gpu::Shader* g_vs = nullptr;
gpu::Shader* g_ps = nullptr;
gpu::Texture* g_copy = nullptr;

bool Init() {
    if (!g_vs) g_vs = gpu::CompileVertexShader(kOverlayVS, {}, "overlay_vs");
    if (!g_ps) g_ps = gpu::CompilePixelShader(kOverlayPS, "overlay_ps");
    return g_vs && g_ps;
}

}  // namespace

// Draws the video overlay into the output image. Overlay rectangles are
// logical (back buffer) pixels; logical x maps to ox + x * sx output pixels.
void CompositeOverlay(HostTarget* bb, float sx, float sy, float ox) {
    if (!g_enabled || !g_surface || !Init()) return;
    gpu::Texture* video = HostTexture(g_surface);
    static int logged = 0;
    if (logged++ < 3 || logged % 300 == 0) {
        UINT w, h, l;
        GetDimensions(g_surface, &w, &h, &l);
        const uint8_t* d = G2H<const uint8_t>(g_surface->Data);
        uint32_t sum = 0;
        for (UINT i = 0; d && i < w * 2 * h; i += 997) sum += d[i];
        Log("overlay: surface %p fmt 0x%02x %ux%u data %08x sample-sum %u tex %p key %d/%08lx", g_surface,
            FormatOf(g_surface), w, h, g_surface->Data, sum, video, g_use_key, g_key);
    }
    if (!video) return;
    if (!g_copy) g_copy = gpu::CreateRenderTarget(bb->width, bb->height, false);
    gpu::CopyTexture(g_copy, 0, 0, bb->tex, {0, 0, int(bb->width), int(bb->height)});

    UINT w, h, l;
    GetDimensions(g_surface, &w, &h, &l);
    RECT src = (g_src.right > g_src.left) ? g_src : RECT{0, 0, LONG(w), LONG(h)};
    RECT dst = (g_dst.right > g_dst.left) ? g_dst : RECT{0, 0, LONG(g_width), LONG(g_height)};
    float vs[8] = {float(src.left) / w, float(src.top) / h, float(src.right) / w, float(src.bottom) / h,
                   (ox + dst.left * sx) / bb->width, dst.top * sy / bb->height, (ox + dst.right * sx) / bb->width,
                   dst.bottom * sy / bb->height};
    float ps[4] = {((g_key >> 16) & 255) / 255.f, ((g_key >> 8) & 255) / 255.f, (g_key & 255) / 255.f,
                   g_use_key ? 1.f : 0.f};
    gpu::SetRenderTargets(bb->tex, nullptr);
    gpu::SetViewport({0, 0, float(bb->width), float(bb->height), 0, 1});
    gpu::SetBlendState({});
    gpu::SetDepthStencilState({});
    gpu::SetRasterState({});
    gpu::SetConstants(gpu::Stage::Vertex, 0, vs, sizeof vs);
    gpu::SetConstants(gpu::Stage::Pixel, 0, ps, sizeof ps);
    gpu::SamplerState linear;
    linear.min_linear = linear.mag_linear = linear.mip_linear = true;
    linear.u = linear.v = linear.w = gpu::Address::Clamp;
    gpu::SetSampler(0, linear);
    gpu::SetTexture(0, video);
    gpu::SetTexture(1, g_copy);
    gpu::SetTexture(2, nullptr);
    gpu::SetTexture(3, nullptr);
    gpu::SetShaders(g_vs, g_ps);
    for (uint32_t n = 0; n < 16; n++) gpu::SetVertexStream(n, nullptr, 0, 0);
    gpu::Draw(gpu::Topology::TriangleStrip, 4);
    gpu::SetTexture(0, nullptr);
    gpu::SetTexture(1, nullptr);
}

void WINAPI x_D3DDevice_EnableOverlay(BOOL enable) {
    D3D_LOCK;
    g_enabled = enable != 0;
    Log("EnableOverlay(%d)", enable);
}

void WINAPI x_D3DDevice_UpdateOverlay(xd3d::Surface* surface, const RECT* src, const RECT* dst, BOOL use_key,
                                      DWORD key) {
    D3D_LOCK;
    g_surface = surface;
    g_src = src ? *src : RECT{};
    g_dst = dst ? *dst : RECT{};
    g_use_key = use_key != 0;
    g_key = key;
}

BOOL WINAPI x_D3DDevice_GetOverlayUpdateStatus() { return TRUE; }

HLE_EXPORT("D3D8", D3DDevice_EnableOverlay);
HLE_EXPORT("D3D8", D3DDevice_UpdateOverlay);
HLE_EXPORT("D3D8", D3DDevice_GetOverlayUpdateStatus);

}  // namespace d3d
