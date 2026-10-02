// Vertex shader objects (declarations + NV2A programs), FVF layouts, pixel
// shader objects and per-draw shader binding.
#include "shader.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace d3d {

// Host-side vertex shader constants (set by device.cpp ApplyViewport).
const char* const kShaderHostVS = R"(
UNIFORMS_VS(HostVS, 2) { float4 host_clip; };  // x: clip-space x scale (widescreen)
)";

const char* const kShaderPixelCommon = R"(
UNIFORMS_PS(PixelState, 0) {
    float4 psc0[8];      // PSConstant0 per combiner stage
    float4 psc1[8];      // PSConstant1 per combiner stage
    float4 fc0, fc1;     // final combiner constants
    float4 tfactor;
    float4 fog_color;
    float4 fog_params;   // start, end, density, table mode (0 = factor from VS)
    float4 alpha_test;   // ref, func (D3DCMP), enable, fog enable
    float4 bumpenv[4];   // m00, m01, m10, m11 per stage
    float4 bumplum[4];   // lscale, loffset
    float4 tex_scale[4]; // linear (LIN_*) textures are addressed in texels: 1/w, 1/h
    float4 tex_depth;    // per stage: 1 if the bound texture is a depth buffer
    float4 shadow;       // x: shadow compare func (D3DCMP), y: depth scale
};
float4 TexCoord(float4 c, int s) { return float4(c.xy * tex_scale[s].xy, c.zw); }
float FogFactor(float f) {
    if (fog_params.w == 1) return exp(-f * fog_params.z);
    if (fog_params.w == 2) return exp(-(f * fog_params.z) * (f * fog_params.z));
    if (fog_params.w == 3) return saturate((fog_params.y - f) / max(fog_params.y - fog_params.x, 1e-6));
    return saturate(f);
}
bool AlphaPasses(float a) {
    float r = alpha_test.x;
    int fn = int(alpha_test.y);
    a = round(a * 255) / 255;
    if (fn == 1) return false;
    if (fn == 2) return a < r;
    if (fn == 3) return a == r;
    if (fn == 4) return a <= r;
    if (fn == 5) return a > r;
    if (fn == 6) return a != r;
    if (fn == 7) return a >= r;
    return true;
}
float4 Finish(float4 color, float fog) {
    if (alpha_test.w > 0) color.rgb = lerp(fog_color.rgb, color.rgb, FogFactor(fog));
    if (alpha_test.z > 0 && !AlphaPasses(color.a)) discard;
    return color;
}
)";

// ---- Vertex element types -------------------------------------------------------------------

UINT XTypeSize(UINT t) {
    switch (t) {
        case 0x16: return 4;   // NORMPACKED3
        case 0x72: return 12;  // FLOAT2H (x, y, w)
        case 0x02: return 0;   // NONE
        case 0x40: return 4;   // D3DCOLOR
    }
    UINT count = t >> 4, fmt = t & 0xF;
    switch (fmt) {
        case 0x2: return 4 * count;            // FLOAT
        case 0x1: case 0x5: return 2 * count;  // NORMSHORT / SHORT
        case 0x4: return count;                // PBYTE
    }
    return 0;
}

void ConvertElement(UINT t, const uint8_t* src, float* out) {
    out[0] = out[1] = out[2] = 0, out[3] = 1;
    UINT count = t >> 4, fmt = t & 0xF;
    if (t == 0x16) {  // NORMPACKED3: 11:11:10 signed normalized
        uint32_t v;
        memcpy(&v, src, 4);
        int x = int32_t(v << 21) >> 21, y = int32_t(v << 10) >> 21, z = int32_t(v) >> 22;
        out[0] = x / 1023.0f, out[1] = y / 1023.0f, out[2] = z / 511.0f;
        return;
    }
    if (t == 0x72) {  // FLOAT2H: x, y, w -> (x, y, 0, w)
        float f[3];
        memcpy(f, src, 12);
        out[0] = f[0], out[1] = f[1], out[3] = f[2];
        return;
    }
    for (UINT i = 0; i < count && i < 4; i++) {
        int16_t s;
        switch (fmt) {
            case 0x2: memcpy(&out[i], src + 4 * i, 4); break;
            case 0x1: memcpy(&s, src + 2 * i, 2), out[i] = std::max(s / 32767.0f, -1.0f); break;
            case 0x5: memcpy(&s, src + 2 * i, 2), out[i] = s; break;
            case 0x4: out[i] = src[i] / 255.0f; break;
        }
    }
}

static bool NativeFormat(UINT t, gpu::VertexFormat* f, bool* integer) {
    using gpu::VertexFormat;
    *integer = false;
    switch (t) {
        case 0x12: *f = VertexFormat::Float1; return true;
        case 0x22: *f = VertexFormat::Float2; return true;
        case 0x32: *f = VertexFormat::Float3; return true;
        case 0x42: *f = VertexFormat::Float4; return true;
        case 0x40: *f = VertexFormat::Color; return true;
        case 0x21: *f = VertexFormat::Short2N; return true;
        case 0x41: *f = VertexFormat::Short4N; return true;
        case 0x25: *f = VertexFormat::Short2, *integer = true; return true;
        case 0x45: *f = VertexFormat::Short4, *integer = true; return true;
        case 0x44: *f = VertexFormat::UByte4N; return true;
        default: return false;
    }
}

void Layout::Finish() {
    for (Element& e : elements) {
        e.convert = !NativeFormat(e.xtype, &e.host, &e.integer);
        e.components = e.xtype == 0x40 ? 4 : std::min<UINT>(std::max<UINT>(e.xtype >> 4, 1), 4);
        used[e.stream] = true;
        if (e.convert) convert[e.stream] = true;
    }
    for (UINT s = 0; s < 16; s++) {
        if (!convert[s]) continue;
        UINT off = 0;  // rebuild the whole stream with FLOAT4 elements
        for (Element& e : elements)
            if (e.stream == s) {
                e.convert = true, e.host = gpu::VertexFormat::Float4, e.integer = false;
                e.host_offset = off, off += 16;
            }
        host_stride[s] = off;
    }
    signature.clear();
    for (Element& e : elements) {
        if (!e.convert) e.host_offset = e.offset;
        char buf[64];
        snprintf(buf, sizeof buf, "%u:%u:%u:%x;", e.stream, e.host_offset, e.reg, unsigned(e.host));
        signature += buf;
    }
}

std::vector<gpu::VertexElement> VertexInputs(const Layout& L) {
    std::vector<gpu::VertexElement> v;
    for (const Element& e : L.elements) v.push_back({e.reg, e.stream, e.host_offset, e.host});
    return v;
}

namespace {

// ---- Vertex shader objects --------------------------------------------------------------------

struct VsObject {
    bool programmable = false;
    Layout layout;
    std::vector<uint32_t> program;
    UINT instructions = 0;
    std::string body, error;  // translated program
    gpu::Shader* vs = nullptr;
    bool tried = false;
    UINT skipped = 0;
};

DWORD g_vs_handle = 0;
float g_vs_constants[192][4];
Layout g_fvf_layout;
DWORD g_fvf_built = 0xFFFFFFFF;
const Layout* g_layout = &g_fvf_layout;

// Handles index these tables (odd vertex shader handles are programs; even
// ones are FVF codes). Objects are never deleted.
std::vector<VsObject*> g_vs_handles;
std::vector<struct PsObject*> g_ps_handles;

VsObject* VsFromHandle(DWORD h) {
    if (!(h & 1) || (h >> 1) == 0 || (h >> 1) > g_vs_handles.size()) return nullptr;
    return g_vs_handles[(h >> 1) - 1];
}
struct PsObject* PsFromHandle(DWORD h) { return h && h <= g_ps_handles.size() ? g_ps_handles[h - 1] : nullptr; }

Layout ParseDeclaration(const uint32_t* tok) {
    Layout L;
    UINT stream = 0, offset[16] = {};
    for (; *tok != 0xFFFFFFFF; tok++) {
        uint32_t t = *tok, type = t >> 29;
        if (type == 1) {  // STREAM
            stream = t & 0xF;
        } else if (type == 2) {  // STREAMDATA
            if (t & 0x10000000) {  // SKIP (dwords) / SKIPBYTES
                offset[stream] += (t & 0x08000000) ? ((t >> 16) & 0x7FF) : 4 * ((t >> 16) & 0xF);
            } else {
                UINT reg = t & 0x1F, xtype = (t >> 16) & 0xFF, size = XTypeSize(xtype);
                // A register declared twice takes its last definition.
                L.elements.erase(std::remove_if(L.elements.begin(), L.elements.end(),
                                                [&](const Element& e) { return e.reg == reg; }),
                                 L.elements.end());
                if (size) L.elements.push_back({stream, offset[stream], reg, xtype});
                offset[stream] += size;
            }
        } else if (type == 4) {  // CONSTMEM: count * 4 dwords follow
            tok += 4 * ((t >> 25) & 0xF);
        } else if (type == 5) {  // EXT
            tok += (t >> 24) & 0x1F;
        }
    }
    L.Finish();
    return L;
}

// FVF -> fixed-function register layout on stream 0.
Layout ParseFvf(DWORD fvf) {
    Layout L;
    UINT off = 0;
    auto add = [&](UINT reg, UINT xtype) {
        L.elements.push_back({0, off, reg, xtype});
        off += XTypeSize(xtype);
    };
    switch (fvf & 0xE) {
        case 0x2: add(0, 0x32); break;                   // XYZ
        case 0x4: add(0, 0x42); break;                   // XYZRHW
        case 0x6: add(0, 0x32), add(1, 0x12); break;     // XYZB1
        case 0x8: add(0, 0x32), add(1, 0x22); break;     // XYZB2
        case 0xA: add(0, 0x32), add(1, 0x32); break;     // XYZB3
        case 0xC: add(0, 0x32), add(1, 0x42); break;     // XYZB4
    }
    if (fvf & 0x10) add(2, 0x32);  // NORMAL
    if (fvf & 0x20) add(8, 0x12);  // PSIZE
    if (fvf & 0x40) add(3, 0x40);  // DIFFUSE
    if (fvf & 0x80) add(4, 0x40);  // SPECULAR
    UINT tex = (fvf >> 8) & 0xF;
    for (UINT i = 0; i < tex && i < 4; i++) {
        static const UINT kTexType[4] = {0x22, 0x32, 0x42, 0x12};
        add(9 + i, kTexType[(fvf >> (16 + 2 * i)) & 3]);
    }
    L.Finish();
    return L;
}

std::string ProgrammableSource(const VsObject& o) {
    return std::string(kShaderHostVS) + "UNIFORMS_VS(XboxConstants, 0) { float4 c[192]; };\n" + o.body;
}

// ---- Pixel shader objects -------------------------------------------------------------------

constexpr UINT kPsDefDwords = 60;  // D3DPIXELSHADERDEF

struct PsObject {
    uint32_t def[kPsDefDwords];
    std::string source, error;
    bool tried = false;
};

DWORD g_ps_handle = 0;

}  // namespace

const Layout* CurrentLayout() { return g_layout; }

// D3D keeps the viewport transform in c[-38] (scale) and c[-37] (offset);
// programs end with code that maps oPos to screen space using them.
void SetScreenSpaceConstants(float x, float y, float w, float h, float min_z, float max_z) {
    constexpr float kZMax = 16777215.0f;  // D24 depth buffer
    float scale[4] = {w / 2, -h / 2, (max_z - min_z) * kZMax, 0};
    float offset[4] = {x + w / 2, y + h / 2, min_z * kZMax, 0};
    memcpy(g_vs_constants[58], scale, 16);
    memcpy(g_vs_constants[59], offset, 16);
}

// Exposed for state.cpp (constant uploads).
const float* VertexShaderConstants() { return &g_vs_constants[0][0]; }
DWORD CurrentVertexShaderHandle() { return g_vs_handle; }
const std::string* CurrentVertexShaderBody() {
    VsObject* o = VsFromHandle(g_vs_handle);
    return o ? &o->body : nullptr;
}
bool CurrentVertexShaderIsProgrammable() {
    VsObject* o = VsFromHandle(g_vs_handle);
    return o && o->programmable;
}
const uint32_t* CurrentPixelShaderDef() {
    auto* p = PsFromHandle(g_ps_handle);
    return p ? p->def : nullptr;
}

bool BindShaders() {
    VsObject* o = VsFromHandle(g_vs_handle);
    gpu::Shader* vs = nullptr;
    if (o && o->programmable) {
        g_layout = &o->layout;
        if (!o->tried) {
            o->tried = true;
            if (!o->body.empty()) o->vs = gpu::CompileVertexShader(ProgrammableSource(*o), VertexInputs(o->layout), "xbox_vs");
            if (!o->vs) Log("vertex shader %p: %s; its draws are skipped", o, o->error.empty() ? "compile failed" : o->error.c_str());
        }
        vs = o->vs;
        if (!vs) {
            o->skipped++;
            return false;
        }
    } else {
        if (o) {
            g_layout = &o->layout;
        } else {
            if (g_fvf_built != g_vs_handle) g_fvf_layout = ParseFvf(g_vs_handle), g_fvf_built = g_vs_handle;
            g_layout = &g_fvf_layout;
        }
        std::string key = FixedVertexKey(*g_layout);
        static std::unordered_map<std::string, gpu::Shader*> ff_cache;
        auto it = ff_cache.find(key);
        if (it == ff_cache.end())
            it = ff_cache
                     .emplace(key, gpu::CompileVertexShader(GenerateFixedVertexShader(*g_layout, key),
                                                            VertexInputs(*g_layout), "fixed_vs"))
                     .first;
        vs = it->second;
        if (!vs) return false;
    }

    gpu::Shader* ps = nullptr;
    auto* p = PsFromHandle(g_ps_handle);
    static const bool force_fixed = os::EnvSet("KT_FORCE_FIXED_PS");
    if (p && !force_fixed) {
        if (!p->tried) {
            p->tried = true;
            p->source = TranslatePixelShader(p->def, &p->error);
            if (p->source.empty()) Log("pixel shader %p: %s; using fixed-function stages", p, p->error.c_str());
        }
        if (!p->source.empty()) ps = gpu::CompilePixelShader(p->source, "xbox_ps");
    }
    if (!ps) {
        std::string key = FixedPixelKey();
        static std::unordered_map<std::string, gpu::Shader*> ff_cache;
        auto it = ff_cache.find(key);
        if (it == ff_cache.end()) it = ff_cache.emplace(key, gpu::CompilePixelShader(GenerateFixedPixelShader(key), "fixed_ps")).first;
        ps = it->second;
        if (!ps) return false;
    }
    gpu::SetShaders(vs, ps);
    return true;
}

// ---- Exports -----------------------------------------------------------------------------------

HRESULT WINAPI x_D3DDevice_CreateVertexShader(const uint32_t* decl, const uint32_t* function, DWORD* handle, DWORD) {
    D3D_LOCK;
    auto* o = new VsObject;
    o->programmable = function != nullptr;
    if (decl) o->layout = ParseDeclaration(decl);
    if (function) {
        // Header: type (0x20 = normal program), version, instruction count.
        o->instructions = (function[0] >> 16) & 0xFF;
        o->program.assign(function + 1, function + 1 + 4 * o->instructions);
        o->body = TranslateVertexProgram(o->program.data(), o->instructions, &o->error);
    }
    g_vs_handles.push_back(o);
    *handle = DWORD(g_vs_handles.size() << 1) | 1;
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_SetVertexShader(DWORD h) {
    D3D_LOCK;
    g_vs_handle = h;
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_GetVertexShader(DWORD* h) {
    *h = g_vs_handle;
    return S_OK;
}

// Objects are kept: handles may still be referenced by recorded state.
HRESULT WINAPI x_D3DDevice_DeleteVertexShader(DWORD) { return S_OK; }

HRESULT WINAPI x_D3DDevice_GetVertexShaderSize(DWORD h, UINT* size) {
    VsObject* o = VsFromHandle(h);
    *size = o ? o->instructions * 16 : 0;
    return S_OK;
}

// Register is already biased to 0..191; NotInline counts DWORDs.
void __fastcall x_D3DDevice_SetVertexShaderConstantNotInline(DWORD reg, const float* data, DWORD dwords) {
    D3D_LOCK;
    if (reg < 192) memcpy(&g_vs_constants[reg][0], data, std::min<DWORD>(dwords, (192 - reg) * 4) * 4);
}
void __fastcall x_D3DDevice_SetVertexShaderConstantNotInlineFast(DWORD reg, const float* data, DWORD dwords) {
    x_D3DDevice_SetVertexShaderConstantNotInline(reg, data, dwords);
}
void __fastcall x_D3DDevice_SetVertexShaderConstant1(DWORD reg, const float* data) {
    x_D3DDevice_SetVertexShaderConstantNotInline(reg, data, 4);
}
void __fastcall x_D3DDevice_SetVertexShaderConstant4(DWORD reg, const float* data) {
    x_D3DDevice_SetVertexShaderConstantNotInline(reg, data, 16);
}

HRESULT WINAPI x_D3DDevice_CreatePixelShader(const uint32_t* def, DWORD* handle) {
    auto* p = new PsObject;
    memcpy(p->def, def, sizeof p->def);
    g_ps_handles.push_back(p);
    *handle = DWORD(g_ps_handles.size());
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_SetPixelShader(DWORD h) {
    D3D_LOCK;
    g_ps_handle = h;
    // As on the Xbox, the definition's first 57 dwords become the PS render
    // states (PSTextureModes lives in the complex-state range instead).
    if (auto* p = PsFromHandle(h)) {
        for (UINT i = 0; i < 57; i++)
            if (i != 54) g_render_state[i] = p->def[i];
        g_render_state[xd3d::RS_PSTEXTUREMODES] = p->def[54];
    }
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_GetPixelShader(DWORD* h) {
    *h = g_ps_handle;
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_DeletePixelShader(DWORD) { return S_OK; }

HLE_EXPORT("D3D8", D3DDevice_CreateVertexShader);
HLE_EXPORT("D3D8", D3DDevice_SetVertexShader);
HLE_EXPORT("D3D8", D3DDevice_GetVertexShader);
HLE_EXPORT("D3D8", D3DDevice_DeleteVertexShader);
HLE_EXPORT("D3D8", D3DDevice_GetVertexShaderSize);
HLE_EXPORT("D3D8", D3DDevice_SetVertexShaderConstantNotInline);
HLE_EXPORT("D3D8", D3DDevice_SetVertexShaderConstantNotInlineFast);
HLE_EXPORT("D3D8", D3DDevice_SetVertexShaderConstant1);
HLE_EXPORT("D3D8", D3DDevice_SetVertexShaderConstant4);
HLE_EXPORT("D3D8", D3DDevice_CreatePixelShader);
HLE_EXPORT("D3D8", D3DDevice_SetPixelShader);
HLE_EXPORT("D3D8", D3DDevice_GetPixelShader);
HLE_EXPORT("D3D8", D3DDevice_DeletePixelShader);

}  // namespace d3d
