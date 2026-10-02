// Fixed-function pipeline emulation: shaders generated from the current
// transform/lighting/texture-stage state, keyed so equal states share shaders.
#include <cstdio>
#include <string>

#include "shader.h"

namespace d3d {

namespace {

// Matches cbuffer FixedVS below.
struct FixedConstants {
    float world[4][16];
    float view[16];
    float proj[16];
    float texm[4][16];
    float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4];
    float mat_power[4];
    float global_ambient[4];
    float viewport[4];
    float fog[4];
    float l_diffuse[8][4], l_specular[8][4], l_ambient[8][4], l_position[8][4], l_direction[8][4];
    float l_atten[8][4], l_spot[8][4];
};

struct XLight {  // D3DLIGHT8
    DWORD Type;
    float Diffuse[4], Specular[4], Ambient[4];
    float Position[3], Direction[3];
    float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
};
struct XMaterial {  // D3DMATERIAL8
    float Diffuse[4], Ambient[4], Specular[4], Emissive[4];
    float Power;
};

FixedConstants g_fc;
XLight g_lights[8];
bool g_light_on[8];
bool g_fc_dirty = true;

const char* const kFixedCBuffer = R"(
UNIFORMS_VS(FixedVS, 1) {
    ROW_MAJOR float4x4 world[4];
    ROW_MAJOR float4x4 view;
    ROW_MAJOR float4x4 proj;
    ROW_MAJOR float4x4 texm[4];
    float4 mat_diffuse, mat_ambient, mat_specular, mat_emissive;
    float4 mat_power;
    float4 global_ambient;
    float4 viewport;
    float4 fog;
    float4 l_diffuse[8], l_specular[8], l_ambient[8], l_position[8], l_direction[8], l_atten[8], l_spot[8];
};
)";

bool HasReg(const Layout& L, UINT reg, UINT* xtype = nullptr) {
    for (const Element& e : L.elements)
        if (e.reg == reg) {
            if (xtype) *xtype = e.xtype;
            return true;
        }
    return false;
}

void Identity(float* m) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}

}  // namespace

void FixedInit() {
    for (auto& w : g_fc.world) Identity(w);
    Identity(g_fc.view), Identity(g_fc.proj);
    for (auto& t : g_fc.texm) Identity(t);
    for (int i = 0; i < 4; i++) g_fc.mat_diffuse[i] = g_fc.mat_ambient[i] = 1;
}

void FixedSetViewport(float x, float y, float w, float h) {
    float v[4] = {x, y, w, h};
    memcpy(g_fc.viewport, v, 16);
    g_fc_dirty = true;
}

// ---- Vertex shader -------------------------------------------------------------------------

std::string FixedVertexKey(const Layout& L) {
    using namespace xd3d;
    std::string k = L.signature + "|";
    bool lighting = RS(RS_LIGHTING) && HasReg(L, 2);
    k += lighting ? 'L' : 'l';
    if (lighting) {
        for (int i = 0; i < 8; i++) k += g_light_on[i] ? char('0' + g_lights[i].Type) : '-';
        char buf[32];
        snprintf(buf, sizeof buf, "%u%u%u%u%u%u%u", RS(RS_COLORVERTEX), RS(RS_DIFFUSEMATERIALSOURCE),
                 RS(RS_AMBIENTMATERIALSOURCE), RS(RS_SPECULARMATERIALSOURCE), RS(RS_EMISSIVEMATERIALSOURCE),
                 RS(RS_LOCALVIEWER), RS(RS_SPECULARENABLE));
        k += buf;
    }
    char buf[96];
    snprintf(buf, sizeof buf, "|vb%u|n%u|f%u%u", RS(RS_VERTEXBLEND), RS(RS_NORMALIZENORMALS), RS(RS_FOGENABLE),
             RS(RS_FOGTABLEMODE));
    k += buf;
    for (int s = 0; s < 4; s++) {
        snprintf(buf, sizeof buf, "|t%x,%x", TSS(s, TSS_TEXCOORDINDEX), TSS(s, TSS_TEXTURETRANSFORMFLAGS));
        k += buf;
    }
    return k;
}

std::string GenerateFixedVertexShader(const Layout& L, const std::string& key) {
    using namespace xd3d;
    std::string s = std::string(kShaderHostVS) + kFixedCBuffer;
    s += "void XboxVS(float4 v[16], int vid, out VSOut o) {\n";
    UINT pos_type = 0x32;
    HasReg(L, 0, &pos_type);
    bool rhw = pos_type == 0x42;
    bool has_normal = HasReg(L, 2), has_diffuse = HasReg(L, 3), has_specular = HasReg(L, 4);
    s += has_diffuse ? "    float4 vdiff = v[3];\n" : "    float4 vdiff = float4(1, 1, 1, 1);\n";
    s += has_specular ? "    float4 vspec = v[4];\n" : "    float4 vspec = float4(0, 0, 0, 0);\n";
    if (rhw) {
        // Pre-transformed: screen pixels (D3D9 pixel centres) -> clip space.
        s += R"(    float rhw = v[0].w != 0 ? v[0].w : 1;
    float2 ndc = float2((v[0].x + 0.5 - viewport.x) / viewport.z * 2 - 1, 1 - (v[0].y + 0.5 - viewport.y) / viewport.w * 2);
    o.pos = float4(ndc, v[0].z, 1) / rhw;
    float3 vpos = float3(0, 0, v[0].z);
    float3 vnorm = float3(0, 0, 1);
    o.d0 = vdiff;
    o.d1 = vspec;
)";
    } else {
        uint32_t vb = RS(RS_VERTEXBLEND);
        int weights = vb == 1 ? 1 : vb == 3 ? 2 : vb == 5 ? 3 : 0;
        if (weights && HasReg(L, 1)) {
            s += "    float w[4];\n    w[0] = v[1].x; w[1] = v[1].y; w[2] = v[1].z; w[3] = 0;\n    float last = 1;\n";
            s += "    float4 wpos = float4(0, 0, 0, 0); float3 wnorm = float3(0, 0, 0);\n";
            for (int i = 0; i <= weights; i++) {
                char buf[256];
                if (i < weights)
                    snprintf(buf, sizeof buf, "    { float wi = w[%d]; last -= wi;\n", i);
                else
                    snprintf(buf, sizeof buf, "    { float wi = last;\n");
                s += buf;
                snprintf(buf, sizeof buf,
                         "      wpos += wi * mul(float4(v[0].xyz, 1), world[%d]); wnorm += wi * mul(v[2].xyz, TO3X3(world[%d])); }\n",
                         i, i);
                s += buf;
            }
            s += "    float3 vpos = mul(wpos, view).xyz;\n    float3 vnorm = mul(wnorm, TO3X3(view));\n";
        } else {
            s += "    float3 vpos = mul(mul(float4(v[0].xyz, 1), world[0]), view).xyz;\n";
            s += "    float3 vnorm = mul(mul(v[2].xyz, TO3X3(world[0])), TO3X3(view));\n";
        }
        s += "    o.pos = mul(float4(vpos, 1), proj);\n";
        if (RS(RS_NORMALIZENORMALS) || true) s += "    vnorm = normalize(vnorm + 1e-12);\n";
        bool lighting = RS(RS_LIGHTING) && has_normal;
        if (!lighting) {
            s += "    o.d0 = vdiff;\n    o.d1 = vspec;\n";
        } else {
            // Material sources: 0 = material, 1 = diffuse colour, 2 = specular colour.
            auto src = [&](uint32_t rs, const char* mat) {
                uint32_t m = RS(xd3d::RenderState(rs));
                if (!RS(RS_COLORVERTEX) || m == 0) return std::string(mat);
                return std::string(m == 1 ? (has_diffuse ? "vdiff" : mat) : (has_specular ? "vspec" : mat));
            };
            s += "    float4 m_diff = " + src(RS_DIFFUSEMATERIALSOURCE, "mat_diffuse") + ";\n";
            s += "    float4 m_amb = " + src(RS_AMBIENTMATERIALSOURCE, "mat_ambient") + ";\n";
            s += "    float4 m_spec = " + src(RS_SPECULARMATERIALSOURCE, "mat_specular") + ";\n";
            s += "    float4 m_emis = " + src(RS_EMISSIVEMATERIALSOURCE, "mat_emissive") + ";\n";
            s += "    float3 diff = float3(0, 0, 0), spec = float3(0, 0, 0), amb = global_ambient.rgb;\n";
            s += RS(RS_LOCALVIEWER) ? "    float3 eye = -normalize(vpos);\n" : "    float3 eye = float3(0, 0, -1);\n";
            for (int i = 0; i < 8; i++) {
                if (!g_light_on[i]) continue;
                char buf[1024];
                // Light vectors are uploaded in view space.
                if (g_lights[i].Type == 3) {  // directional
                    snprintf(buf, sizeof buf, "    { float3 ld = -l_direction[%d].xyz; float att = 1;\n", i);
                } else {
                    snprintf(buf, sizeof buf,
                             "    { float3 d = l_position[%d].xyz - vpos; float dist = length(d); float3 ld = d / max(dist, 1e-6);\n"
                             "      float att = dist > l_atten[%d].x ? 0 : 1 / max(l_atten[%d].y + l_atten[%d].z * dist + l_atten[%d].w * dist * dist, 1e-6);\n",
                             i, i, i, i, i);
                }
                s += buf;
                if (g_lights[i].Type == 2) {  // spot
                    snprintf(buf, sizeof buf,
                             "      float rho = dot(-ld, normalize(l_direction[%d].xyz));\n"
                             "      att *= rho > l_spot[%d].y ? 1 : rho <= l_spot[%d].z ? 0 : pow(saturate((rho - l_spot[%d].z) / max(l_spot[%d].y - l_spot[%d].z, 1e-6)), l_spot[%d].x);\n",
                             i, i, i, i, i, i, i);
                    s += buf;
                }
                snprintf(buf, sizeof buf,
                         "      float nl = max(dot(vnorm, ld), 0);\n"
                         "      amb += l_ambient[%d].rgb * att;\n"
                         "      diff += l_diffuse[%d].rgb * nl * att;\n"
                         "      if (nl > 0) spec += l_specular[%d].rgb * pow(max(dot(vnorm, normalize(ld + eye)), 0), mat_power.x) * att; }\n",
                         i, i, i);
                s += buf;
            }
            s += "    o.d0 = float4(saturate(m_emis.rgb + m_amb.rgb * amb + m_diff.rgb * diff), m_diff.a);\n";
            s += RS(RS_SPECULARENABLE) ? "    o.d1 = float4(saturate(m_spec.rgb * spec), 0);\n" : "    o.d1 = float4(0, 0, 0, 0);\n";
        }
    }
    // Texture coordinates per stage.
    for (int st = 0; st < 4; st++) {
        uint32_t tci = TSS(st, TSS_TEXCOORDINDEX), ttff = TSS(st, TSS_TEXTURETRANSFORMFLAGS);
        uint32_t set = tci & 0xFFFF, gen = tci & 0xFFFF0000u;
        char buf[256];
        std::string coord;
        if (gen == 0x10000) coord = "float4(vnorm, 1)";
        else if (gen == 0x20000) coord = "float4(vpos, 1)";
        else if (gen == 0x30000) coord = "float4(reflect(normalize(vpos), vnorm), 1)";
        else if (gen == 0x40000) {  // sphere map
            coord = "float4(reflect(normalize(vpos), vnorm).xy * 0.5 + 0.5, 0, 1)";
        } else {
            UINT xt = 0;
            if (HasReg(L, 9 + (set & 3), &xt)) {
                snprintf(buf, sizeof buf, "v[%u]", 9 + (set & 3));
                coord = buf;
            } else {
                coord = "float4(0, 0, 0, 1)";
            }
        }
        if ((ttff & 0xFF) && !rhw) {
            // Count of components fed to the matrix: (u,v,1,0)-style expansion.
            snprintf(buf, sizeof buf, "    o.t%d = mul(%s, texm[%d]);\n", st, coord.c_str(), st);
        } else {
            snprintf(buf, sizeof buf, "    o.t%d = %s;\n", st, coord.c_str());
        }
        s += buf;
    }
    // Fog: table modes get view depth (factor computed per pixel); otherwise
    // the specular alpha carries the vertex fog factor.
    if (RS(RS_FOGTABLEMODE))
        s += rhw ? "    o.fog = v[0].z;\n" : "    o.fog = abs(vpos.z);\n";
    else
        s += "    o.fog = vspec.a;\n";
    s += "    o.pos.x *= host_clip.x;\n}\n";
    return s;
}

// ---- Pixel shader (texture stages) ------------------------------------------------------------

char TextureKind(int stage);  // state.cpp: '2', 'C' or '-' for the bound texture

std::string FixedPixelKey() {
    using namespace xd3d;
    std::string k;
    char buf[128];
    for (int s = 0; s < 4; s++) {
        snprintf(buf, sizeof buf, "%c%x,%x,%x,%x,%x,%x,%x,%x,%x,%x/", TextureKind(s), TSS(s, TSS_COLOROP),
                 TSS(s, TSS_COLORARG0), TSS(s, TSS_COLORARG1), TSS(s, TSS_COLORARG2), TSS(s, TSS_ALPHAOP),
                 TSS(s, TSS_ALPHAARG0), TSS(s, TSS_ALPHAARG1), TSS(s, TSS_ALPHAARG2), TSS(s, TSS_RESULTARG),
                 TSS(s, TSS_TEXTURETRANSFORMFLAGS));
        k += buf;
        if (TSS(s, TSS_COLOROP) <= 1) break;
    }
    k += RS(RS_SPECULARENABLE) ? "S" : "s";
    return k;
}

static std::string Arg(uint32_t a, int stage, bool alpha) {
    std::string base;
    switch (a & 0xF) {
        case 0: base = "d0"; break;
        case 1: base = "cur"; break;
        case 2: base = "tx" + std::to_string(stage); break;
        case 3: base = "tfactor"; break;
        case 4: base = "d1"; break;
        case 5: base = "temp"; break;
        default: base = "cur"; break;
    }
    if (a & 0x20) base = "(" + base + ").aaaa";  // ALPHAREPLICATE
    if (a & 0x10) base = "(1 - " + base + ")";   // COMPLEMENT
    return alpha ? "(" + base + ").a" : "(" + base + ").rgb";
}

// Xbox D3DTOP_* values (see state.cpp for the enum order).
static std::string Op(uint32_t op, const std::string& a0, const std::string& a1, const std::string& a2, int stage,
                      bool alpha) {
    std::string tex = "tx" + std::to_string(stage) + (alpha ? ".a" : ".rgb");
    std::string d0a = "d0.a", cura = "cur.a", txa = "tx" + std::to_string(stage) + ".a", tfa = "tfactor.a";
    switch (op) {
        case 2: return a1;                                        // SELECTARG1
        case 3: return a2;                                        // SELECTARG2
        case 4: return a1 + " * " + a2;                           // MODULATE
        case 5: return "saturate(" + a1 + " * " + a2 + " * 2)";   // MODULATE2X
        case 6: return "saturate(" + a1 + " * " + a2 + " * 4)";   // MODULATE4X
        case 7: return "saturate(" + a1 + " + " + a2 + ")";       // ADD
        case 8: return "saturate(" + a1 + " + " + a2 + " - 0.5)"; // ADDSIGNED
        case 9: return "saturate((" + a1 + " + " + a2 + " - 0.5) * 2)";
        case 10: return "saturate(" + a1 + " - " + a2 + ")";      // SUBTRACT
        case 11: return "saturate(" + a1 + " + " + a2 + " - " + a1 + " * " + a2 + ")";  // ADDSMOOTH
        case 12: return "lerp(" + a2 + ", " + a1 + ", " + d0a + ")";   // BLENDDIFFUSEALPHA
        case 13: return "lerp(" + a2 + ", " + a1 + ", " + cura + ")";  // BLENDCURRENTALPHA
        case 14: return "lerp(" + a2 + ", " + a1 + ", " + txa + ")";   // BLENDTEXTUREALPHA
        case 15: return "lerp(" + a2 + ", " + a1 + ", " + tfa + ")";   // BLENDFACTORALPHA
        case 16: return "saturate(" + a1 + " + " + a2 + " * (1 - " + txa + "))";  // BLENDTEXTUREALPHAPM
        case 17: return a1;                                                      // PREMODULATE (approx.)
        case 18: case 19: case 20: case 21:  // MODULATE*_ADD*: approximated as ADD
            return "saturate(" + a1 + " + " + a2 + ")";
        case 22: {  // DOTPRODUCT3
            std::string d = "saturate(dot(" + a1 + " * 2 - 1, " + a2 + " * 2 - 1))";
            return alpha ? d : "XXX(" + d + ")";
        }
        case 23: return "saturate(" + a0 + " + " + a1 + " * " + a2 + ")";  // MULTIPLYADD
        case 24: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";        // LERP
        default: return a1;
    }
}

std::string GenerateFixedPixelShader(const std::string&) {
    using namespace xd3d;
    std::string s = kShaderPixelCommon;
    for (int st = 0; st < 4; st++) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s(tex%d, %d)\n", TextureKind(st) == 'C' ? "TEXTURECUBE" : "TEXTURE2D", st, st);
        s += buf;
    }
    s += "float4 XboxPS(VSOut i) {\n    float4 d0 = i.d0, d1 = i.d1, cur = d0, temp = float4(0, 0, 0, 0);\n";
    s += "    float4 tc[4];\n    tc[0] = i.t0; tc[1] = i.t1; tc[2] = i.t2; tc[3] = i.t3;\n    float2 bump = float2(0, 0);\n";
    for (int st = 0; st < 4; st++) {
        uint32_t cop = TSS(st, TSS_COLOROP), aop = TSS(st, TSS_ALPHAOP);
        if (cop <= 1) break;
        char kind = TextureKind(st);
        char buf[512];
        bool projected = (TSS(st, TSS_TEXTURETRANSFORMFLAGS) & 0x100) != 0;
        std::string tcs = "TexCoord(tc[" + std::to_string(st) + "], " + std::to_string(st) + ")";
        std::string uv = projected ? tcs + ".xy / " + tcs + ".w" : tcs + ".xy";
        if (kind == 'C')
            snprintf(buf, sizeof buf, "    float4 tx%d = SAMPLE(tex%d, tc[%d].xyz);\n", st, st, st);
        else if (kind == '2')
            snprintf(buf, sizeof buf, "    float4 tx%d = SAMPLE(tex%d, %s + bump);\n", st, st, uv.c_str());
        else
            snprintf(buf, sizeof buf, "    float4 tx%d = float4(1, 1, 1, 1);\n", st);
        s += buf;
        s += "    bump = float2(0, 0);\n";
        if (cop == 25 || cop == 26) {  // BUMPENVMAP(LUMINANCE): perturb the next stage
            snprintf(buf, sizeof buf,
                     "    { float2 duv = (tx%d.bg * 255 - 128) / 127; bump = float2(dot(duv, bumpenv[%d].xz), dot(duv, bumpenv[%d].yw)); }\n",
                     st, st, st);
            s += buf;
            continue;
        }
        std::string c0 = Arg(TSS(st, TSS_COLORARG0), st, false), c1 = Arg(TSS(st, TSS_COLORARG1), st, false),
                    c2 = Arg(TSS(st, TSS_COLORARG2), st, false);
        std::string a0 = Arg(TSS(st, TSS_ALPHAARG0), st, true), a1 = Arg(TSS(st, TSS_ALPHAARG1), st, true),
                    a2 = Arg(TSS(st, TSS_ALPHAARG2), st, true);
        std::string dst = TSS(st, TSS_RESULTARG) == 5 ? "temp" : "cur";
        std::string rgb = Op(cop, c0, c1, c2, st, false);
        std::string alpha = aop <= 1 ? "cur.a" : Op(aop, a0, a1, a2, st, true);
        s += "    { float3 rgb = " + rgb + "; float a = " + alpha + "; " + dst + " = float4(rgb, a); }\n";
    }
    if (RS(RS_SPECULARENABLE)) s += "    cur.rgb = saturate(cur.rgb + d1.rgb);\n";
    s += "    return Finish(cur, i.fog);\n}\n";
    return s;
}

// ---- Constant upload and exports ---------------------------------------------------------------

void UploadFixedConstants() {
    // Lights are specified in world space; the shader works in view space.
    auto xform_point = [](const float* m, const float* p, float* out) {
        for (int c = 0; c < 3; c++) out[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
    };
    auto xform_dir = [](const float* m, const float* d, float* out) {
        for (int c = 0; c < 3; c++) out[c] = d[0] * m[c] + d[1] * m[4 + c] + d[2] * m[8 + c];
    };
    for (int i = 0; i < 8; i++) {
        const XLight& l = g_lights[i];
        memcpy(g_fc.l_diffuse[i], l.Diffuse, 16);
        memcpy(g_fc.l_specular[i], l.Specular, 16);
        memcpy(g_fc.l_ambient[i], l.Ambient, 16);
        xform_point(g_fc.view, l.Position, g_fc.l_position[i]);
        xform_dir(g_fc.view, l.Direction, g_fc.l_direction[i]);
        float att[4] = {l.Range > 0 ? l.Range : 1e30f, l.Attenuation0, l.Attenuation1, l.Attenuation2};
        memcpy(g_fc.l_atten[i], att, 16);
        float spot[4] = {l.Falloff, cosf(l.Theta / 2), cosf(l.Phi / 2), float(l.Type)};
        memcpy(g_fc.l_spot[i], spot, 16);
    }
    uint32_t amb = RS(xd3d::RS_AMBIENT);
    float ga[4] = {((amb >> 16) & 255) / 255.f, ((amb >> 8) & 255) / 255.f, (amb & 255) / 255.f, (amb >> 24) / 255.f};
    memcpy(g_fc.global_ambient, ga, 16);
    gpu::SetConstants(gpu::Stage::Vertex, 1, &g_fc, sizeof g_fc);
}

HRESULT WINAPI x_D3DDevice_SetTransform(DWORD state, const float* m) {
    D3D_LOCK;
    if (state == xd3d::TS_VIEW) memcpy(g_fc.view, m, 64);
    else if (state == xd3d::TS_PROJECTION) memcpy(g_fc.proj, m, 64);
    else if (state >= xd3d::TS_TEXTURE0 && state <= xd3d::TS_TEXTURE3) memcpy(g_fc.texm[state - 2], m, 64);
    else if (state >= xd3d::TS_WORLD && state < xd3d::TS_MAX) memcpy(g_fc.world[state - xd3d::TS_WORLD], m, 64);
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_GetTransform(DWORD state, float* m) {
    if (state == xd3d::TS_VIEW) memcpy(m, g_fc.view, 64);
    else if (state == xd3d::TS_PROJECTION) memcpy(m, g_fc.proj, 64);
    else if (state >= xd3d::TS_TEXTURE0 && state <= xd3d::TS_TEXTURE3) memcpy(m, g_fc.texm[state - 2], 64);
    else if (state >= xd3d::TS_WORLD && state < xd3d::TS_MAX) memcpy(m, g_fc.world[state - xd3d::TS_WORLD], 64);
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_SetMaterial(const XMaterial* m) {
    D3D_LOCK;
    memcpy(g_fc.mat_diffuse, m->Diffuse, 16);
    memcpy(g_fc.mat_ambient, m->Ambient, 16);
    memcpy(g_fc.mat_specular, m->Specular, 16);
    memcpy(g_fc.mat_emissive, m->Emissive, 16);
    g_fc.mat_power[0] = m->Power;
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_GetMaterial(XMaterial* m) {
    memcpy(m->Diffuse, g_fc.mat_diffuse, 16);
    memcpy(m->Ambient, g_fc.mat_ambient, 16);
    memcpy(m->Specular, g_fc.mat_specular, 16);
    memcpy(m->Emissive, g_fc.mat_emissive, 16);
    m->Power = g_fc.mat_power[0];
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_SetLight(DWORD i, const XLight* l) {
    D3D_LOCK;
    if (i < 8) g_lights[i] = *l;
    return S_OK;
}

HRESULT WINAPI x_D3DDevice_LightEnable(DWORD i, BOOL on) {
    D3D_LOCK;
    if (i < 8) g_light_on[i] = on != 0;
    return S_OK;
}

HLE_EXPORT("D3D8", D3DDevice_SetTransform);
HLE_EXPORT("D3D8", D3DDevice_GetTransform);
HLE_EXPORT("D3D8", D3DDevice_SetMaterial);
HLE_EXPORT("D3D8", D3DDevice_GetMaterial);
HLE_EXPORT("D3D8", D3DDevice_SetLight);
HLE_EXPORT("D3D8", D3DDevice_LightEnable);

}  // namespace d3d
