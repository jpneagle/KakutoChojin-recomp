// Render/texture state translation and draw calls.
//
// The title writes most render state straight into D3D's global arrays
// (XDK inline functions), so those arrays are the source of truth: each draw
// translates them into gpu.h state. Vertex and index data live in title
// memory and are streamed to the host per draw.
#include <algorithm>
#include <vector>

#include "../manifest.h"
#include "shader.h"

namespace d3d {

// shader.cpp / ffgen.cpp
const float* VertexShaderConstants();
bool CurrentVertexShaderIsProgrammable();
DWORD CurrentVertexShaderHandle();
const std::string* CurrentVertexShaderBody();
const uint32_t* CurrentPixelShaderDef();
void UploadFixedConstants();
void FixedInit();
void FixedSetViewport(float x, float y, float w, float h);
uint32_t CurrentFrame();
constexpr int kPsHandleDword = 54;  // PSTextureModes: identifies the shader in traces

namespace {

struct Stream {
    xd3d::Resource* vb = nullptr;
    UINT stride = 0;
};
Stream g_streams[16];
UINT g_base_vertex = 0;
xd3d::PixelContainer* g_textures[4] = {};

// ---- Value translation (Xbox values are NV2A/OpenGL-style enums) ----------------------------------

gpu::Compare Cmp(uint32_t v) {
    return (v >= 0x200 && v <= 0x207) ? gpu::Compare(v - 0x200 + 1) : gpu::Compare::Always;
}

gpu::Blend Blend(uint32_t v, bool alpha) {
    using gpu::Blend;
    switch (v) {
        case 0: return Blend::Zero;
        case 1: return Blend::One;
        case 0x300: return alpha ? Blend::SrcAlpha : Blend::SrcColor;
        case 0x301: return alpha ? Blend::InvSrcAlpha : Blend::InvSrcColor;
        case 0x302: return Blend::SrcAlpha;
        case 0x303: return Blend::InvSrcAlpha;
        case 0x304: return Blend::DestAlpha;
        case 0x305: return Blend::InvDestAlpha;
        case 0x306: return alpha ? Blend::DestAlpha : Blend::DestColor;
        case 0x307: return alpha ? Blend::InvDestAlpha : Blend::InvDestColor;
        case 0x308: return Blend::SrcAlphaSat;
        case 0x8001: case 0x8003: return Blend::Factor;
        case 0x8002: case 0x8004: return Blend::InvFactor;
        default: return Blend::One;
    }
}

gpu::BlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return gpu::BlendOp::Subtract;
        case 0x800B: return gpu::BlendOp::RevSubtract;
        case 0x8007: return gpu::BlendOp::Min;
        case 0x8008: return gpu::BlendOp::Max;
        default: return gpu::BlendOp::Add;
    }
}

gpu::StencilOp StencilOp(uint32_t v) {
    using gpu::StencilOp;
    switch (v) {
        case 0: return StencilOp::Zero;
        case 0x1E01: return StencilOp::Replace;
        case 0x1E02: return StencilOp::IncrSat;
        case 0x1E03: return StencilOp::DecrSat;
        case 0x150A: return StencilOp::Invert;
        case 0x8507: return StencilOp::Incr;
        case 0x8508: return StencilOp::Decr;
        default: return StencilOp::Keep;
    }
}

uint8_t ColorWrite(uint32_t v) {
    return ((v & 0x00010000) ? 1 : 0) | ((v & 0x00000100) ? 2 : 0) | ((v & 0x00000001) ? 4 : 0) | ((v & 0x01000000) ? 8 : 0);
}

gpu::Address Address(uint32_t v) {
    switch (v) {
        case 1: return gpu::Address::Wrap;
        case 2: return gpu::Address::Mirror;
        case 4: return gpu::Address::Border;
        default: return gpu::Address::Clamp;  // CLAMP and CLAMPTOEDGE
    }
}

void Unpack(uint32_t argb, float* out) {
    out[0] = ((argb >> 16) & 0xFF) / 255.f, out[1] = ((argb >> 8) & 0xFF) / 255.f;
    out[2] = (argb & 0xFF) / 255.f, out[3] = (argb >> 24) / 255.f;
}

// Matches UNIFORMS_PS(PixelState) in kShaderPixelCommon.
struct PixelConstants {
    float psc0[8][4], psc1[8][4], fc0[4], fc1[4], tfactor[4], fog_color[4], fog_params[4], alpha_test[4];
    float bumpenv[4][4], bumplum[4][4];
    float tex_scale[4][4], tex_depth[4], shadow[4];
};

}  // namespace

char TextureKind(int stage) {
    xd3d::PixelContainer* t = g_textures[stage];
    if (!t) return '-';
    return (t->Format & xd3d::kFormatCubemap) ? 'C' : '2';
}

void ApplyPipelineState() {
    using namespace xd3d;
    HostTarget* ds = CurrentDepthTarget();

    gpu::BlendState bs;
    bs.enable = RS(RS_ALPHABLENDENABLE) != 0;
    bs.src = Blend(RS(RS_SRCBLEND), false), bs.dst = Blend(RS(RS_DESTBLEND), false);
    bs.src_alpha = Blend(RS(RS_SRCBLEND), true), bs.dst_alpha = Blend(RS(RS_DESTBLEND), true);
    bs.op = BlendOp(RS(RS_BLENDOP));
    bs.write_mask = ColorWrite(RS(RS_COLORWRITEENABLE));
    uint32_t bc = RS(RS_BLENDCOLOR);
    if (RS(RS_SRCBLEND) == 0x8003 || RS(RS_SRCBLEND) == 0x8004 || RS(RS_DESTBLEND) == 0x8003 || RS(RS_DESTBLEND) == 0x8004)
        bc = (bc >> 24) * 0x01010101u;  // constant alpha
    Unpack(bc, bs.factor);
    gpu::SetBlendState(bs);

    gpu::DepthStencilState dd;
    dd.depth_enable = ds && RS(RS_ZENABLE);
    dd.depth_write = RS(RS_ZWRITEENABLE) != 0;
    dd.depth_func = Cmp(RS(RS_ZFUNC));
    dd.stencil_enable = ds && RS(RS_STENCILENABLE);
    dd.stencil_read_mask = uint8_t(RS(RS_STENCILMASK)), dd.stencil_write_mask = uint8_t(RS(RS_STENCILWRITEMASK));
    dd.fail = StencilOp(RS(RS_STENCILFAIL)), dd.depth_fail = StencilOp(RS(RS_STENCILZFAIL));
    dd.pass = StencilOp(RS(RS_STENCILPASS)), dd.stencil_func = Cmp(RS(RS_STENCILFUNC));
    dd.stencil_ref = uint8_t(RS(RS_STENCILREF));
    gpu::SetDepthStencilState(dd);

    gpu::RasterState rd;
    uint32_t fill = RS(RS_FILLMODE);
    rd.wireframe = fill == 0x1B01 || fill == 0x1B00;
    uint32_t cull = RS(RS_CULLMODE);
    if (cull != 0) {
        bool cull_cw = cull == 0x900;
        if (RS(RS_FRONTFACE) == 0x901) cull_cw = !cull_cw;
        rd.cull_back = true;
        rd.front_ccw = cull_cw;  // the culled winding is the back face
    }
    rd.depth_bias = -int(RS(RS_ZBIAS)) * 16;
    rd.slope_bias = -AsFloat(RS(RS_POLYGONOFFSETZSLOPESCALE)) * (RS(RS_SOLIDOFFSETENABLE) ? 1.f : 0.f);
    gpu::SetRasterState(rd);

    for (int s = 0; s < 4; s++) {
        gpu::SetTexture(s, HostTexture(g_textures[s]));
        gpu::SamplerState sd;
        uint32_t mag = TSS(s, TSS_MAGFILTER), min = TSS(s, TSS_MINFILTER), mip = TSS(s, TSS_MIPFILTER);
        sd.anisotropic = mag == 3 || min == 3;
        sd.min_linear = min >= 2, sd.mag_linear = mag >= 2, sd.mip_linear = mip >= 2;
        sd.u = Address(TSS(s, TSS_ADDRESSU));
        sd.v = Address(TSS(s, TSS_ADDRESSV));
        sd.w = Address(TSS(s, TSS_ADDRESSW));
        sd.lod_bias = AsFloat(TSS(s, TSS_MIPMAPLODBIAS));
        sd.max_anisotropy = std::clamp<UINT>(TSS(s, TSS_MAXANISOTROPY), 1, 16);
        Unpack(TSS(s, TSS_BORDERCOLOR), sd.border);
        sd.min_lod = float(TSS(s, TSS_MAXMIPLEVEL));
        sd.max_lod = mip == 0 ? sd.min_lod : 1000.f;
        gpu::SetSampler(s, sd);
    }

    // Constants.
    if (CurrentVertexShaderIsProgrammable()) {
        gpu::SetConstants(gpu::Stage::Vertex, 0, VertexShaderConstants(), 192 * 16);
    } else {
        UploadFixedConstants();
    }
    PixelConstants pc{};
    for (int i = 0; i < 8; i++) Unpack(RS(RenderState(10 + i)), pc.psc0[i]), Unpack(RS(RenderState(18 + i)), pc.psc1[i]);
    Unpack(RS(RenderState(43)), pc.fc0), Unpack(RS(RenderState(44)), pc.fc1);
    Unpack(RS(RS_TEXTUREFACTOR), pc.tfactor);
    Unpack(RS(RS_FOGCOLOR), pc.fog_color);
    pc.fog_params[0] = AsFloat(RS(RS_FOGSTART)), pc.fog_params[1] = AsFloat(RS(RS_FOGEND));
    pc.fog_params[2] = AsFloat(RS(RS_FOGDENSITY)), pc.fog_params[3] = float(RS(RS_FOGTABLEMODE));
    pc.alpha_test[0] = RS(RS_ALPHAREF) / 255.f, pc.alpha_test[1] = float(int(Cmp(RS(RS_ALPHAFUNC))));
    pc.alpha_test[2] = RS(RS_ALPHATESTENABLE) ? 1.f : 0.f, pc.alpha_test[3] = RS(RS_FOGENABLE) ? 1.f : 0.f;
    for (int s = 0; s < 4; s++) {
        pc.bumpenv[s][0] = AsFloat(TSS(s, TSS_BUMPENVMAT00)), pc.bumpenv[s][1] = AsFloat(TSS(s, TSS_BUMPENVMAT01));
        pc.bumpenv[s][2] = AsFloat(TSS(s, TSS_BUMPENVMAT10)), pc.bumpenv[s][3] = AsFloat(TSS(s, TSS_BUMPENVMAT11));
        pc.bumplum[s][0] = AsFloat(TSS(s, TSS_BUMPENVLSCALE)), pc.bumplum[s][1] = AsFloat(TSS(s, TSS_BUMPENVLOFFSET));
    }
    for (int s = 0; s < 4; s++) {
        pc.tex_scale[s][0] = pc.tex_scale[s][1] = 1;
        xd3d::PixelContainer* t = g_textures[s];
        if (!t) continue;
        FormatInfo fi = GetFormatInfo(FormatOf(t));
        UINT w, h, l;
        GetDimensions(t, &w, &h, &l);
        static const bool no_scale = os::EnvSet("KT_NO_TEXEL_SCALE");
        if (!fi.swizzled && !fi.compressed && !no_scale) pc.tex_scale[s][0] = 1.f / w, pc.tex_scale[s][1] = 1.f / h;
        pc.tex_depth[s] = fi.depth ? 1.f : 0.f;
    }
    pc.shadow[0] = float(int(Cmp(RS(RS_SHADOWFUNC))));
    pc.shadow[1] = 1.f / 16777215.f;
    gpu::SetConstants(gpu::Stage::Pixel, 0, &pc, sizeof pc);
}

// ---- Vertex streaming ----------------------------------------------------------------------------

namespace {

// Rewrites primitives the host lacks (fans, quads, polygons, line loops) as lists.
bool Expand(uint32_t prim, const uint16_t* in, UINT count, std::vector<uint16_t>* out, gpu::Topology* topo) {
    auto at = [&](UINT i) { return in ? in[i] : uint16_t(i); };
    out->clear();
    switch (prim) {
        case xd3d::PT_QUADLIST:
            for (UINT q = 0; q + 3 < count; q += 4) out->insert(out->end(), {at(q), at(q + 1), at(q + 2), at(q), at(q + 2), at(q + 3)});
            break;
        case xd3d::PT_QUADSTRIP:
            for (UINT q = 0; q + 3 < count; q += 2)
                out->insert(out->end(), {at(q), at(q + 1), at(q + 3), at(q), at(q + 3), at(q + 2)});
            break;
        case xd3d::PT_TRIANGLEFAN:
        case xd3d::PT_POLYGON:
            for (UINT i = 1; i + 1 < count; i++) out->insert(out->end(), {at(0), at(i), at(i + 1)});
            break;
        case xd3d::PT_LINELOOP:
            for (UINT i = 0; i < count; i++) out->insert(out->end(), {at(i), at((i + 1) % count)});
            *topo = gpu::Topology::LineList;
            return true;
        default:
            return false;
    }
    *topo = gpu::Topology::TriangleList;
    return true;
}

gpu::Topology Topology(uint32_t prim) {
    switch (prim) {
        case xd3d::PT_POINTLIST: return gpu::Topology::PointList;
        case xd3d::PT_LINELIST: return gpu::Topology::LineList;
        case xd3d::PT_LINESTRIP: return gpu::Topology::LineStrip;
        case xd3d::PT_TRIANGLESTRIP: return gpu::Topology::TriangleStrip;
        default: return gpu::Topology::TriangleList;
    }
}

uint32_t g_draws = 0, g_skipped = 0;

// Draws vertices [first, first + count) of the bound streams, optionally
// indexed by `indices` (relative to `first`).
void DrawRange(uint32_t prim, UINT first, UINT count, const uint16_t* indices, UINT index_count) {
    if (!BindShaders()) {
        g_skipped++;
        return;
    }
    ApplyPipelineState();
    const Layout* L = CurrentLayout();
    for (UINT n = 0; n < 16; n++) {
        const Stream& s = g_streams[n];
        if (!L->used[n] || !s.vb || !s.stride) {
            gpu::SetVertexStream(n, nullptr, 0, 0);
            continue;
        }
        const uint8_t* base = G2H<const uint8_t>(s.vb->Data) + size_t(first) * s.stride;
        UINT stride = s.stride;
        if (L->convert[n]) {
            stride = L->host_stride[n];
            std::vector<uint8_t> tmp(size_t(count) * stride);
            for (UINT v = 0; v < count; v++)
                for (const Element& e : L->elements)
                    if (e.stream == n)
                        ConvertElement(e.xtype, base + v * s.stride + e.offset,
                                       reinterpret_cast<float*>(tmp.data() + v * stride + e.host_offset));
            gpu::SetVertexStream(n, tmp.data(), UINT(tmp.size()), stride);
        } else {
            gpu::SetVertexStream(n, base, count * stride, stride);
        }
    }
    std::vector<uint16_t> expanded;
    gpu::Topology topo = Topology(prim);
    if (Expand(prim, indices, indices ? index_count : count, &expanded, &topo)) {
        indices = expanded.data();
        index_count = UINT(expanded.size());
    }
    g_draws++;
    static const uint32_t trace_frame = uint32_t(os::EnvInt("KT_TRACE_FRAME", 0));
    static const bool trace_sprites = os::EnvSet("KT_TRACE_SPRITES");
    if (trace_sprites && CurrentVertexShaderIsProgrammable()) {
        const float* c = VertexShaderConstants();
        if (c[384] == 1.5f && c[400] >= 0.6f)
            Log("sprite f%u: c100 %g %g | c101 %g %g | c102 %g %g | c98 %g %g %g %g", CurrentFrame(), c[400], c[401],
                c[404], c[405], c[408], c[409], c[392], c[393], c[394], c[395]);
    }
    if (trace_frame && CurrentFrame() == trace_frame) {
        HostTarget* rt = CurrentRenderTarget();
        Log("draw: prim %u count %u idx %u | vs %s ps %08x | rt %ux%u | tex %p %p | "
            "blend %u %x/%x z %u/%u cull %x | color %08x",
            prim, count, indices ? index_count : 0, CurrentVertexShaderIsProgrammable() ? "prog" : "fixed",
            CurrentPixelShaderDef() ? CurrentPixelShaderDef()[kPsHandleDword] : 0, rt ? rt->width : 0,
            rt ? rt->height : 0, g_textures[0], g_textures[1],
            RS(xd3d::RS_ALPHABLENDENABLE), RS(xd3d::RS_SRCBLEND), RS(xd3d::RS_DESTBLEND), RS(xd3d::RS_ZENABLE),
            RS(xd3d::RS_ZWRITEENABLE), RS(xd3d::RS_CULLMODE), RS(xd3d::RS_COLORWRITEENABLE));
        static DWORD last_vs = 0;
        if (CurrentVertexShaderHandle() != last_vs) {
            last_vs = CurrentVertexShaderHandle();
            if (const std::string* body = CurrentVertexShaderBody()) Log("  vs body:%s", body->c_str());
        }
        const float* c = VertexShaderConstants();
        Log("  vs %08x c96 %g %g %g %g | c97 %g %g %g %g | c98 %g %g %g %g | c99 %g %g %g %g | c100 %g %g %g %g",
            CurrentVertexShaderHandle(), c[384], c[385], c[386], c[387], c[388], c[389], c[390], c[391], c[392],
            c[393], c[394], c[395], c[396], c[397], c[398], c[399], c[400], c[401], c[402], c[403]);
        Log("  c103 %g %g %g %g | c104 %g %g %g %g | c105 %g %g %g %g | c107 %g %g %g %g", c[412], c[413], c[414],
            c[415], c[416], c[417], c[418], c[419], c[420], c[421], c[422], c[423], c[428], c[429], c[430], c[431]);
        Log("  c58 %g %g %g %g | c59 %g %g %g %g | c101 %g %g %g %g | c102 %g %g %g %g | c106 %g %g %g %g", c[232],
            c[233], c[234], c[235], c[236], c[237], c[238], c[239], c[404], c[405], c[406], c[407], c[408], c[409],
            c[410], c[411], c[424], c[425], c[426], c[427]);
        if (xd3d::PixelContainer* t = g_textures[0]) {
            UINT w, h, l;
            GetDimensions(t, &w, &h, &l);
            Log("  tex0: common %08x data %08x format %08x size %08x -> fmt 0x%02x %ux%u addr %u/%u filt %u/%u", t->Common,
                t->Data, t->Format, t->Size, FormatOf(t), w, h, TSS(0, xd3d::TSS_ADDRESSU), TSS(0, xd3d::TSS_ADDRESSV),
                TSS(0, xd3d::TSS_MINFILTER), TSS(0, xd3d::TSS_MAGFILTER));
        }
        const Stream& s0 = g_streams[0];
        if (s0.vb) {
            const float* v = reinterpret_cast<const float*>(G2H<const uint8_t>(s0.vb->Data) + size_t(first) * s0.stride);
            UINT n = std::min<UINT>(s0.stride / 4, 10);
            for (UINT k = 0; k < std::min<UINT>(count, 4); k++) {
                char line[256];
                int len = 0;
                for (UINT j = 0; j < n; j++) len += snprintf(line + len, sizeof line - len, " %g", v[k * s0.stride / 4 + j]);
                Log("  v%u:%s", k, line);
            }
        }
    }
    if (indices)
        gpu::DrawIndexed(topo, indices, index_count);
    else
        gpu::Draw(topo, count);
}

void DrawUP(uint32_t prim, UINT count, const void* data, UINT stride, const uint16_t* indices, UINT index_count) {
    Stream saved = g_streams[0];
    static xd3d::Resource up;
    up.Data = H2G(data);
    g_streams[0] = {&up, stride};
    DrawRange(prim, 0, count, indices, index_count);
    g_streams[0] = saved;
}

void IndexRange(const uint16_t* idx, UINT n, UINT* lo, UINT* hi) {
    *lo = 0xFFFF, *hi = 0;
    for (UINT i = 0; i < n; i++) *lo = std::min<UINT>(*lo, idx[i]), *hi = std::max<UINT>(*hi, idx[i]);
}

}  // namespace

uint32_t DrawStats(uint32_t* skipped) {
    *skipped = g_skipped;
    return g_draws;
}

// ---- Exports ---------------------------------------------------------------------------------------

// Simple render states: the inline caller already stored the D3D value in
// the state array and passes us the NV2A method; nothing to do.
void __fastcall x_D3DDevice_SetRenderState_Simple(DWORD, DWORD) {}

#define STORE_RS(fn, index)                                    \
    void WINAPI x_D3DDevice_SetRenderState_##fn(DWORD value) { \
        g_render_state[xd3d::index] = value;                   \
    }                                                          \
    HLE_EXPORT("D3D8", D3DDevice_SetRenderState_##fn)

STORE_RS(PSTextureModes, RS_PSTEXTUREMODES);
STORE_RS(VertexBlend, RS_VERTEXBLEND);
STORE_RS(FogColor, RS_FOGCOLOR);
STORE_RS(FillMode, RS_FILLMODE);
STORE_RS(BackFillMode, RS_BACKFILLMODE);
STORE_RS(TwoSidedLighting, RS_TWOSIDEDLIGHTING);
STORE_RS(NormalizeNormals, RS_NORMALIZENORMALS);
STORE_RS(ZEnable, RS_ZENABLE);
STORE_RS(StencilEnable, RS_STENCILENABLE);
STORE_RS(StencilFail, RS_STENCILFAIL);
STORE_RS(FrontFace, RS_FRONTFACE);
STORE_RS(CullMode, RS_CULLMODE);
STORE_RS(TextureFactor, RS_TEXTUREFACTOR);
STORE_RS(ZBias, RS_ZBIAS);
STORE_RS(LogicOp, RS_LOGICOP);
STORE_RS(EdgeAntiAlias, RS_EDGEANTIALIAS);
STORE_RS(MultiSampleAntiAlias, RS_MULTISAMPLEANTIALIAS);
STORE_RS(MultiSampleMask, RS_MULTISAMPLEMASK);
STORE_RS(MultiSampleMode, RS_MULTISAMPLEMODE);
STORE_RS(MultiSampleRenderTargetMode, RS_MULTISAMPLERENDERTARGETMODE);
STORE_RS(ShadowFunc, RS_SHADOWFUNC);
STORE_RS(LineWidth, RS_LINEWIDTH);
STORE_RS(SampleAlpha, RS_SAMPLEALPHA);
STORE_RS(Dxt1NoiseEnable, RS_DXT1NOISEENABLE);
STORE_RS(YuvEnable, RS_YUVENABLE);
STORE_RS(OcclusionCullEnable, RS_OCCLUSIONCULLENABLE);
STORE_RS(StencilCullEnable, RS_STENCILCULLENABLE);
STORE_RS(RopZCmpAlwaysRead, RS_ROPZCMPALWAYSREAD);
STORE_RS(RopZRead, RS_ROPZREAD);
STORE_RS(DoNotCullUncompressed, RS_DONOTCULLUNCOMPRESSED);

void WINAPI x_D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD v) {
    g_texture_state[stage * xd3d::TSS_PER_STAGE + xd3d::TSS_TEXCOORDINDEX] = v;
}
void WINAPI x_D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD v) {
    g_texture_state[stage * xd3d::TSS_PER_STAGE + xd3d::TSS_BORDERCOLOR] = v;
}

HRESULT WINAPI x_D3DDevice_SetTexture(DWORD stage, xd3d::PixelContainer* t) {
    D3D_LOCK;
    if (stage < 4) g_textures[stage] = t;
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_SetStreamSource(UINT n, xd3d::Resource* vb, UINT stride) {
    D3D_LOCK;
    if (n < 16) g_streams[n] = {vb, stride};
    return S_OK;
}

// Inline DrawIndexedVertices passes D3D__IndexData (set here) as its index
// pointer, so it must be maintained like the original does.
HRESULT WINAPI x_D3DDevice_SetIndices(xd3d::Resource* ib, UINT base_vertex) {
    D3D_LOCK;
    static auto* index_data = G2H<uint32_t>(HleVar("D3D8", "D3D__IndexData"));
    if (index_data) *index_data = ib ? ib->Data : 0;
    g_base_vertex = base_vertex;
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_DrawVertices(DWORD prim, UINT start, UINT count) {
    D3D_LOCK;
    if (count) DrawRange(prim, start, count, nullptr, 0);
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_DrawVerticesUP(DWORD prim, UINT count, const void* data, UINT stride) {
    D3D_LOCK;
    if (count) DrawUP(prim, count, data, stride, nullptr, 0);
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_DrawIndexedVertices(DWORD prim, UINT count, const uint16_t* indices) {
    D3D_LOCK;
    if (!count) return S_OK;
    UINT lo, hi;
    IndexRange(indices, count, &lo, &hi);
    std::vector<uint16_t> rel(indices, indices + count);
    for (auto& i : rel) i = uint16_t(i - lo);
    DrawRange(prim, g_base_vertex + lo, hi - lo + 1, rel.data(), count);
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_DrawIndexedVerticesUP(DWORD prim, UINT count, const uint16_t* indices, const void* data,
                                                 UINT stride) {
    D3D_LOCK;
    if (!count) return S_OK;
    UINT lo, hi;
    IndexRange(indices, count, &lo, &hi);
    std::vector<uint16_t> rel(indices, indices + count);
    for (auto& i : rel) i = uint16_t(i - lo);
    DrawUP(prim, hi - lo + 1, static_cast<const uint8_t*>(data) + size_t(lo) * stride, stride, rel.data(), count);
    return S_OK;
}

// Pixel shader binding mirrors the Xbox: the definition's first 57 dwords are
// the PS render states, and constants are read from there at draw time.
HRESULT WINAPI x_D3DDevice_SetPixelShaderConstant(DWORD reg, const float* data, DWORD count) {
    D3D_LOCK;
    const uint32_t* def = CurrentPixelShaderDef();
    if (!def) return S_OK;
    auto pack = [](const float* f) {
        auto c = [](float v) { return uint32_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
        return c(f[3]) << 24 | c(f[0]) << 16 | c(f[1]) << 8 | c(f[2]);
    };
    for (DWORD i = 0; i < count; i++) {
        DWORD r = reg + i;
        uint32_t packed = pack(data + 4 * i);
        for (int st = 0; st < 8; st++) {
            if (((def[57] >> (4 * st)) & 0xF) == r) g_render_state[10 + st] = packed;  // PSC0Mapping
            if (((def[58] >> (4 * st)) & 0xF) == r) g_render_state[18 + st] = packed;  // PSC1Mapping
        }
        if ((def[59] & 0xF) == r) g_render_state[43] = packed;  // final combiner C0
        if (((def[59] >> 4) & 0xF) == r) g_render_state[44] = packed;
    }
    return S_OK;
}

HLE_EXPORT("D3D8", D3DDevice_SetRenderState_Simple);
HLE_EXPORT("D3D8", D3DDevice_SetTextureState_TexCoordIndex);
HLE_EXPORT("D3D8", D3DDevice_SetTextureState_BorderColor);
HLE_EXPORT("D3D8", D3DDevice_SetTexture);
HLE_EXPORT("D3D8", D3DDevice_SetStreamSource);
HLE_EXPORT("D3D8", D3DDevice_SetIndices);
HLE_EXPORT("D3D8", D3DDevice_DrawVertices);
HLE_EXPORT("D3D8", D3DDevice_DrawVerticesUP);
HLE_EXPORT("D3D8", D3DDevice_DrawIndexedVertices);
HLE_EXPORT("D3D8", D3DDevice_DrawIndexedVerticesUP);
HLE_EXPORT("D3D8", D3DDevice_SetPixelShaderConstant);

}  // namespace d3d
