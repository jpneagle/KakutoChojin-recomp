// Xbox pixel shaders (NV2A register combiners, D3DPIXELSHADERDEF) -> shader
// dialect (see shader.h).
//
// A definition configures four texture stages (addressing modes) and up to
// eight general combiner stages followed by the final combiner. Field
// encodings follow the XDK's PS_* macros.
#include <cstdio>

#include "shader.h"

namespace d3d {

namespace {

// D3DPIXELSHADERDEF dword indices.
enum {
    kAlphaInputs = 0, kFinalABCD = 8, kFinalEFG = 9, kConstant0 = 10, kConstant1 = 18, kAlphaOutputs = 26,
    kRgbInputs = 34, kCompareMode = 42, kFinalConst0 = 43, kFinalConst1 = 44, kRgbOutputs = 45,
    kCombinerCount = 53, kTextureModes = 54, kDotMapping = 55, kInputTexture = 56,
};

enum TexMode {
    TM_NONE, TM_PROJECT2D, TM_PROJECT3D, TM_CUBEMAP, TM_PASSTHRU, TM_CLIPPLANE, TM_BUMPENVMAP, TM_BUMPENVMAP_LUM,
    TM_BRDF, TM_DOT_ST, TM_DOT_ZW, TM_DOT_RFLCT_DIFF, TM_DOT_RFLCT_SPEC, TM_DOT_STR_3D, TM_DOT_STR_CUBE,
    TM_DPNDNT_AR, TM_DPNDNT_GB, TM_DOTPRODUCT, TM_DOT_RFLCT_SPEC_CONST
};

const char* RegName(uint32_t r) {
    switch (r) {
        case 0x0: return "zero";
        case 0x1: return "c0";
        case 0x2: return "c1";
        case 0x3: return "fog";
        case 0x4: return "v0";
        case 0x5: return "v1";
        case 0x8: return "t0";
        case 0x9: return "t1";
        case 0xA: return "t2";
        case 0xB: return "t3";
        case 0xC: return "r0";
        case 0xD: return "r1";
        case 0xE: return "v1r0sum";
        case 0xF: return "efprod";
        default: return "zero";
    }
}

// One 8-bit combiner input: register, channel, mapping.
std::string Input(uint32_t in, bool alpha_portion) {
    uint32_t reg = in & 0xF;
    bool alpha_channel = (in & 0x10) != 0;
    std::string v = RegName(reg);
    // RGB portion: rgb, or alpha replicated. Alpha portion: alpha, or blue.
    if (alpha_portion)
        v += alpha_channel ? ".aaaa" : ".bbbb";
    else if (alpha_channel)
        v += ".aaaa";
    switch (in & 0xE0) {
        case 0x00: return "max(" + v + ", 0)";                     // UNSIGNED_IDENTITY
        case 0x20: return "(1 - saturate(" + v + "))";              // UNSIGNED_INVERT
        case 0x40: return "(2 * max(" + v + ", 0) - 1)";            // EXPAND_NORMAL
        case 0x60: return "(-2 * max(" + v + ", 0) + 1)";           // EXPAND_NEGATE
        case 0x80: return "(max(" + v + ", 0) - 0.5)";              // HALFBIAS_NORMAL
        case 0xA0: return "(-max(" + v + ", 0) + 0.5)";             // HALFBIAS_NEGATE
        case 0xC0: return v;                                        // SIGNED_IDENTITY
        default: return "(-" + v + ")";                             // SIGNED_NEGATE
    }
}

std::string OutputMapping(uint32_t flags, const std::string& x) {
    switch ((flags >> 3) & 7) {
        case 1: return "(" + x + " - 0.5)";        // BIAS
        case 2: return "(" + x + " * 2)";          // SHIFTLEFT_1
        case 3: return "((" + x + " - 0.5) * 2)";  // SHIFTLEFT_1_BIAS
        case 4: return "(" + x + " * 4)";          // SHIFTLEFT_2
        case 6: return "(" + x + " * 0.5)";        // SHIFTRIGHT_1
        default: return x;
    }
}

// Emits one portion (RGB or alpha) of a general combiner stage.
void EmitPortion(std::string& s, uint32_t inputs, uint32_t outputs, bool alpha, bool mux_msb) {
    std::string A = Input(inputs >> 24, alpha), B = Input(inputs >> 16, alpha), C = Input(inputs >> 8, alpha),
                D = Input(inputs, alpha);
    uint32_t cd_reg = outputs & 0xF, ab_reg = (outputs >> 4) & 0xF, sum_reg = (outputs >> 8) & 0xF;
    uint32_t flags = outputs >> 12;
    bool cd_dot = !alpha && (flags & 0x01), ab_dot = !alpha && (flags & 0x02), mux = (flags & 0x04) != 0;
    const char* sw = alpha ? ".a" : ".rgb";
    std::string ab = ab_dot ? "XXXX(dot((" + A + ").rgb, (" + B + ").rgb))" : "(" + A + ") * (" + B + ")";
    std::string cd = cd_dot ? "XXXX(dot((" + C + ").rgb, (" + D + ").rgb))" : "(" + C + ") * (" + D + ")";
    std::string p = alpha ? "a" : "c";
    s += "        float4 " + p + "ab = clamp(" + OutputMapping(flags, ab) + ", -1, 1);\n";
    s += "        float4 " + p + "cd = clamp(" + OutputMapping(flags, cd) + ", -1, 1);\n";
    std::string sum = mux ? std::string("(r0.a >= 0.5 ? ") + p + "cd : " + p + "ab)"
                          : OutputMapping(flags, "((" + ab + ") + (" + cd + "))");
    (void)mux_msb;  // LSB/MSB selection: both approximated by r0.a >= 0.5
    s += "        float4 " + p + "sum = clamp(" + sum + ", -1, 1);\n";
    auto write = [&](uint32_t reg, const std::string& val) {
        if (reg == 0) return;
        s += std::string("        n_") + RegName(reg) + sw + " = " + val + sw + ";\n";
    };
    write(ab_reg, p + "ab");
    write(cd_reg, p + "cd");
    write(sum_reg, p + "sum");
    if (!alpha) {
        // *_BLUE_TO_ALPHA: the dot product's blue also feeds the alpha portion.
        if ((flags & 0x80) && ab_reg) s += std::string("        n_") + RegName(ab_reg) + ".a = cab.b;\n";
        if ((flags & 0x40) && cd_reg) s += std::string("        n_") + RegName(cd_reg) + ".a = ccd.b;\n";
    }
}

}  // namespace

std::string TranslatePixelShader(const uint32_t* d, std::string* error) {
    std::string s = kShaderPixelCommon;
    uint32_t modes = d[kTextureModes];
    TexMode mode[4];
    for (int i = 0; i < 4; i++) mode[i] = TexMode((modes >> (5 * i)) & 0x1F);
    auto is_cube = [](TexMode m) {
        return m == TM_CUBEMAP || m == TM_DOT_RFLCT_DIFF || m == TM_DOT_RFLCT_SPEC || m == TM_DOT_STR_CUBE ||
               m == TM_DOT_RFLCT_SPEC_CONST;
    };
    char buf[512];
    for (int i = 0; i < 4; i++) {
        snprintf(buf, sizeof buf, "%s(tex%d, %d)\n", is_cube(mode[i]) ? "TEXTURECUBE" : "TEXTURE2D", i, i);
        s += buf;
    }
    s += R"(
float2 ProjUV(float4 c, int i) { return TexCoord(c, i).xy / (c.w != 0 ? c.w : 1); }
// Shadow buffers: compare the fragment depth against the stored depth.
float4 ShadowTest(float4 v, float4 c, int i) {
    if (tex_depth[i] > 0) {
        float ref = c.z / (c.w != 0 ? c.w : 1) * shadow.y;
        float stored = v.r;
        int fn = int(shadow.x);
        bool lit_ = fn == 2 ? ref < stored : fn == 3 ? ref == stored : fn == 4 ? ref <= stored :
                    fn == 5 ? ref > stored : fn == 6 ? ref != stored : fn == 7 ? ref >= stored : fn == 8;
        return float4(1, 1, 1, 1) * (lit_ ? 1.0 : 0.0);
    }
    return v;
}
float3 DotMap(float3 v) { return v * 2 - 1; }

float4 XboxPS(VSOut i) {
    float4 tc[4];
    tc[0] = i.t0; tc[1] = i.t1; tc[2] = i.t2; tc[3] = i.t3;
    float4 zero = float4(0, 0, 0, 0);
    float4 v0 = i.d0, v1 = i.d1;
    float4 fog = float4(fog_color.rgb, alpha_test.w > 0 ? FogFactor(i.fog) : 1);
    float4 t0 = zero, t1 = zero, t2 = zero, t3 = zero;
    float3 dotv = float3(0, 0, 0);
)";
    // Texture addressing stages.
    for (int i = 0; i < 4; i++) {
        std::string t = "t" + std::to_string(i), tc = "tc[" + std::to_string(i) + "]";
        std::string tex = "tex" + std::to_string(i);
        std::string prev = "t" + std::to_string(i ? i - 1 : 0);
        std::string ii = std::to_string(i);
        switch (mode[i]) {
            case TM_NONE: break;
            case TM_PROJECT2D:
            case TM_PROJECT3D:
                s += "    " + t + " = ShadowTest(SAMPLE(" + tex + ", ProjUV(" + tc + ", " + ii + ")), " + tc + ", " + ii +
                     ");\n";
                break;
            case TM_CUBEMAP:
                s += "    " + t + " = SAMPLE(" + tex + ", " + tc + ".xyz);\n";
                break;
            case TM_PASSTHRU:
                s += "    " + t + " = saturate(" + tc + ");\n";
                break;
            case TM_CLIPPLANE: {
                uint32_t cmp = (d[kCompareMode] >> (4 * i)) & 0xF;
                const char comp[] = "xyzw";
                for (int c = 0; c < 4; c++) {
                    // bit set: discard if >= 0, clear: discard if < 0
                    snprintf(buf, sizeof buf, "    if (%s.%c %s 0) discard;\n", tc.c_str(), comp[c],
                             (cmp >> c) & 1 ? ">=" : "<");
                    s += buf;
                }
                s += "    " + t + " = zero;\n";
                break;
            }
            case TM_BUMPENVMAP:
            case TM_BUMPENVMAP_LUM:
                snprintf(buf, sizeof buf,
                         "    { float2 duv = (%s.bg * 255 - 128) / 127;\n"
                         "      float4 c = %s; c.xy += float2(dot(duv, bumpenv[%d].xz), dot(duv, bumpenv[%d].yw)) / max(tex_scale[%d].xy, 1e-9) * tex_scale[%d].xy;\n"
                         "      %s = ShadowTest(SAMPLE(%s, ProjUV(c, %d)), c, %d);\n",
                         prev.c_str(), tc.c_str(), i, i, i, i, t.c_str(), tex.c_str(), i, i);
                s += buf;
                if (mode[i] == TM_BUMPENVMAP_LUM) {
                    snprintf(buf, sizeof buf, "      %s.rgb *= saturate(%s.r * bumplum[%d].x + bumplum[%d].y);\n",
                             t.c_str(), prev.c_str(), i, i);
                    s += buf;
                }
                s += "    }\n";
                break;
            case TM_DPNDNT_AR:
                s += "    " + t + " = SAMPLE(" + tex + ", " + prev + ".ar);\n";
                break;
            case TM_DPNDNT_GB:
                s += "    " + t + " = SAMPLE(" + tex + ", " + prev + ".gb);\n";
                break;
            case TM_DOTPRODUCT:
                s += "    dotv." + std::string(1, "xyz"[std::min(i - 1, 2)]) + " = dot(" + tc + ".xyz, DotMap(" + prev +
                     ".rgb));\n    " + t + " = XXXX(dotv.x);\n";
                break;
            case TM_DOT_ST:
                s += "    { float t_ = dot(" + tc + ".xyz, DotMap(" + prev + ".rgb)); " + t + " = SAMPLE(" + tex +
                     ", float2(dotv.x, t_)); }\n";
                break;
            case TM_DOT_ZW:
                s += "    { float w_ = dot(" + tc + ".xyz, DotMap(" + prev + ".rgb)); " + t + " = XXXX(dotv.x / max(w_, 1e-6)); }\n";
                break;
            case TM_DOT_STR_3D:
            case TM_DOT_STR_CUBE:
            case TM_DOT_RFLCT_DIFF:
            case TM_DOT_RFLCT_SPEC:
            case TM_DOT_RFLCT_SPEC_CONST: {
                // The last of three dot-product stages: (dotv.x, dotv.y, this) forms a normal.
                s += "    { float3 n = float3(dotv.x, dotv.y, dot(" + tc + ".xyz, DotMap(" + prev + ".rgb)));\n";
                if (mode[i] == TM_DOT_RFLCT_SPEC || mode[i] == TM_DOT_RFLCT_SPEC_CONST) {
                    s += "      float3 e = float3(tc[1].w, tc[2].w, " + tc + ".w);\n";
                    s += "      n = 2 * dot(n, e) / max(dot(n, n), 1e-6) * n - e;\n";
                }
                s += is_cube(mode[i]) ? "      " + t + " = SAMPLE(" + tex + ", n);\n"
                                      : "      " + t + " = SAMPLE(" + tex + ", n.xy);\n";
                s += "    }\n";
                break;
            }
            default:
                snprintf(buf, sizeof buf, "    %s = zero; // unsupported texture mode %d\n", t.c_str(), mode[i]);
                s += buf;
                break;
        }
    }
    // r0.a starts as t0.a.
    s += "    float4 r0 = float4(0, 0, 0, t0.a), r1 = zero;\n";

    uint32_t count = d[kCombinerCount] & 0xFF, flags = d[kCombinerCount] >> 8;
    bool unique_c0 = (flags & 0x10) != 0, unique_c1 = (flags & 0x100) != 0, mux_msb = (flags & 0x1) != 0;
    if (count > 8) {
        *error = "combiner count > 8";
        return "";
    }
    for (uint32_t st = 0; st < count; st++) {
        snprintf(buf, sizeof buf, "    { // combiner %u\n        float4 c0 = psc0[%u], c1 = psc1[%u];\n", st,
                 unique_c0 ? st : 0, unique_c1 ? st : 0);
        s += buf;
        // Writes go to n_* copies so every input reads pre-stage values.
        s += "        float4 n_r0 = r0, n_r1 = r1, n_t0 = t0, n_t1 = t1, n_t2 = t2, n_t3 = t3, n_v0 = v0, n_v1 = v1;\n";
        s += "        float4 n_zero = zero, n_c0 = c0, n_c1 = c1, n_fog = fog;\n";
        EmitPortion(s, d[kRgbInputs + st], d[kRgbOutputs + st], false, mux_msb);
        EmitPortion(s, d[kAlphaInputs + st], d[kAlphaOutputs + st], true, mux_msb);
        s += "        r0 = n_r0; r1 = n_r1; t0 = n_t0; t1 = n_t1; t2 = n_t2; t3 = n_t3; v0 = n_v0; v1 = n_v1;\n    }\n";
    }

    // Final combiner: rgb = A*B + (1-A)*C + D, alpha = G.
    uint32_t abcd = d[kFinalABCD], efg = d[kFinalEFG];
    if (abcd == 0 && efg == 0) {
        s += "    float4 result = r0;\n";
    } else {
        uint32_t settings = efg & 0xFF;
        s += "    float4 c0 = fc0, c1 = fc1;\n";
        std::string v1s = (settings & 0x40) ? "(1 - v1)" : "v1", r0s = (settings & 0x20) ? "(1 - r0)" : "r0";
        std::string sum = v1s + " + " + r0s;
        if (settings & 0x80) sum = "saturate(" + sum + ")";
        s += "    float4 v1r0sum = float4((" + sum + ").rgb, 0);\n";
        // Final combiner inputs only use the unsigned mappings.
        auto fin = [&](uint32_t in, bool alpha) { return "saturate(" + Input(in & 0x3F, alpha) + ")"; };
        s += "    float4 efprod = float4((" + fin(efg >> 24, false) + " * " + fin(efg >> 16, false) + ").rgb, 0);\n";
        std::string A = fin(abcd >> 24, false), B = fin(abcd >> 16, false), C = fin(abcd >> 8, false),
                    D = fin(abcd, false), G = fin(efg >> 8, true);
        s += "    float4 result = float4(saturate((" + A + ").rgb * (" + B + ").rgb + (1 - (" + A + ").rgb) * (" + C +
             ").rgb + (" + D + ").rgb), (" + G + ").a);\n";
    }
    // Fog is applied by the final combiner (inputs above), not Finish().
    s += "    float4 o = result;\n    if (alpha_test.z > 0 && !AlphaPasses(o.a)) discard;\n    return o;\n}\n";
    return s;
}

}  // namespace d3d
