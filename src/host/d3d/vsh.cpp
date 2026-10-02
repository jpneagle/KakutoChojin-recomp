// NV2A vertex program (microcode) -> shader dialect (see shader.h).
//
// Each instruction is 128 bits and drives two units in parallel: the MAC
// (vector ops) and the ILU (scalar ops), both reading the register file as it
// was before the instruction. Field layout follows the NV2A documentation
// reverse-engineered by the xemu/Cxbx-Reloaded projects.
#include <cstdio>

#include "shader.h"

namespace d3d {

namespace {

struct Field {
    int dword, shift, bits;
};

// clang-format off
constexpr Field kIlu{1, 25, 3}, kMac{1, 21, 4}, kConst{1, 13, 8}, kV{1, 9, 4};
constexpr Field kANeg{1, 8, 1}, kASwz{1, 0, 8}, kAR{2, 28, 4}, kAMux{2, 26, 2};
constexpr Field kBNeg{2, 25, 1}, kBSwz{2, 17, 8}, kBR{2, 13, 4}, kBMux{2, 11, 2};
constexpr Field kCNeg{2, 10, 1}, kCSwz{2, 2, 8}, kCRHigh{2, 0, 2}, kCRLow{3, 30, 2}, kCMux{3, 28, 2};
constexpr Field kOutMacMask{3, 24, 4}, kOutR{3, 20, 4}, kOutIluMask{3, 16, 4}, kOutOMask{3, 12, 4};
constexpr Field kOutOrb{3, 11, 1}, kOutAddress{3, 3, 8}, kOutMux{3, 2, 1}, kA0X{3, 1, 1}, kFinal{3, 0, 1};
// clang-format on

uint32_t Get(const uint32_t* ins, Field f) { return (ins[f.dword] >> f.shift) & ((1u << f.bits) - 1); }

enum Mac { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT,
           MAC_SGE, MAC_ARL };
enum Ilu { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

std::string Swizzle(uint32_t swz) {
    // Four 2-bit selectors, x in the highest pair.
    static const char comp[] = "xyzw";
    std::string s = ".";
    for (int i = 3; i >= 0; i--) s += comp[(swz >> (2 * i)) & 3];
    return s;
}

std::string Mask(uint32_t mask) {
    // x is the most significant bit.
    std::string s;
    if (mask & 8) s += 'x';
    if (mask & 4) s += 'y';
    if (mask & 2) s += 'z';
    if (mask & 1) s += 'w';
    return s;
}

std::string Source(const uint32_t* ins, uint32_t mux, uint32_t reg, uint32_t swz, uint32_t neg) {
    std::string s;
    char buf[64];
    switch (mux) {
        case 1:  // temporary register; r12 aliases oPos
            snprintf(buf, sizeof buf, reg == 12 ? "opos" : "r[%u]", reg);
            break;
        case 2:
            snprintf(buf, sizeof buf, "v[%u]", Get(ins, kV));
            break;
        case 3:
            if (Get(ins, kA0X))
                snprintf(buf, sizeof buf, "c[clamp(a0 + %u, 0, 191)]", Get(ins, kConst));
            else
                snprintf(buf, sizeof buf, "c[%u]", Get(ins, kConst));
            break;
        default:
            snprintf(buf, sizeof buf, "float4(0, 0, 0, 0)");
            break;
    }
    s = std::string(neg ? "-" : "") + buf + Swizzle(swz);
    return s;
}

const char* OutputName(uint32_t addr) {
    switch (addr) {
        case 0: return "opos";
        case 3: return "o.d0";
        case 4: return "o.d1";
        case 5: return "ofog";
        case 6: return "opts";
        case 7: return "ob0";
        case 8: return "ob1";
        case 9: return "o.t0";
        case 10: return "o.t1";
        case 11: return "o.t2";
        case 12: return "o.t3";
        default: return nullptr;
    }
}

}  // namespace

std::string TranslateVertexProgram(const uint32_t* program, UINT count, std::string* error) {
    std::string body = R"(
void XboxVS(float4 v[16], int vid, out VSOut o) {
    float4 r[12];
    UNROLL for (int k = 0; k < 12; k++) r[k] = float4(0, 0, 0, 0);
    float4 opos = float4(0, 0, 0, 1), ofog = float4(0, 0, 0, 0), opts = float4(0, 0, 0, 0);
    float4 ob0 = float4(0, 0, 0, 0), ob1 = float4(0, 0, 0, 0);
    int a0 = 0;
    o.d0 = float4(0, 0, 0, 1); o.d1 = float4(0, 0, 0, 1);
    o.t0 = float4(0, 0, 0, 1); o.t1 = float4(0, 0, 0, 1); o.t2 = float4(0, 0, 0, 1); o.t3 = float4(0, 0, 0, 1);
)";
    char buf[512];
    for (UINT i = 0; i < count; i++) {
        const uint32_t* ins = program + 4 * i;
        uint32_t mac = Get(ins, kMac), ilu = Get(ins, kIlu);
        if (mac > MAC_ARL) {
            snprintf(buf, sizeof buf, "unknown MAC opcode %u at instruction %u", mac, i);
            *error = buf;
            return "";
        }
        std::string A = Source(ins, Get(ins, kAMux), Get(ins, kAR), Get(ins, kASwz), Get(ins, kANeg));
        std::string B = Source(ins, Get(ins, kBMux), Get(ins, kBR), Get(ins, kBSwz), Get(ins, kBNeg));
        uint32_t c_reg = (Get(ins, kCRHigh) << 2) | Get(ins, kCRLow);
        std::string C = Source(ins, Get(ins, kCMux), c_reg, Get(ins, kCSwz), Get(ins, kCNeg));

        snprintf(buf, sizeof buf, "    { // %u: %08x %08x %08x %08x\n", i, ins[0], ins[1], ins[2], ins[3]);
        body += buf;
        std::string mac_expr, ilu_expr;
        switch (mac) {
            case MAC_MOV: mac_expr = A; break;
            case MAC_MUL: mac_expr = A + " * " + B; break;
            case MAC_ADD: mac_expr = A + " + " + C; break;
            case MAC_MAD: mac_expr = A + " * " + B + " + " + C; break;
            case MAC_DP3: mac_expr = "XXXX(dot((" + A + ").xyz, (" + B + ").xyz))"; break;
            case MAC_DPH: mac_expr = "XXXX(dot((" + A + ").xyz, (" + B + ").xyz) + (" + B + ").w)"; break;
            case MAC_DP4: mac_expr = "XXXX(dot(" + A + ", " + B + "))"; break;
            case MAC_DST:
                mac_expr = "float4(1, (" + A + ").y * (" + B + ").y, (" + A + ").z, (" + B + ").w)";
                break;
            case MAC_MIN: mac_expr = "min(" + A + ", " + B + ")"; break;
            case MAC_MAX: mac_expr = "max(" + A + ", " + B + ")"; break;
            case MAC_SLT: mac_expr = "SLT(" + A + ", " + B + ")"; break;
            case MAC_SGE: mac_expr = "SGE(" + A + ", " + B + ")"; break;
            case MAC_ARL: mac_expr = A; break;
        }
        std::string cx = "(" + C + ").x";
        switch (ilu) {
            case ILU_MOV: ilu_expr = C; break;
            case ILU_RCP: ilu_expr = "XXXX(1.0 / " + cx + ")"; break;
            case ILU_RCC:
                ilu_expr = "XXXX(sign(" + cx + ") * clamp(1.0 / abs(" + cx + "), 5.42101e-20, 1.884467e+19))";
                break;
            case ILU_RSQ: ilu_expr = "XXXX(rsqrt(abs(" + cx + ")))"; break;
            case ILU_EXP:
                ilu_expr = "float4(exp2(floor(" + cx + ")), " + cx + " - floor(" + cx + "), exp2(" + cx + "), 1)";
                break;
            case ILU_LOG:
                ilu_expr = "float4(floor(log2(abs(" + cx + "))), abs(" + cx + ") / exp2(floor(log2(abs(" + cx +
                           ")))), log2(abs(" + cx + ")), 1)";
                break;
            case ILU_LIT:
                ilu_expr = "lit((" + C + ").x, (" + C + ").y, clamp((" + C + ").w, -128, 128))";
                break;
        }
        // Both units read the pre-instruction register file.
        if (!mac_expr.empty()) body += "        float4 mac = " + mac_expr + ";\n";
        if (!ilu_expr.empty()) body += "        float4 ilu = " + ilu_expr + ";\n";

        uint32_t out_r = Get(ins, kOutR);
        uint32_t mac_mask = Get(ins, kOutMacMask), ilu_mask = Get(ins, kOutIluMask);
        auto temp = [&](uint32_t reg) { return reg == 12 ? std::string("opos") : "r[" + std::to_string(reg) + "]"; };
        if (mac == MAC_ARL) {
            body += "        a0 = int(floor(mac.x));\n";
        } else if (!mac_expr.empty() && mac_mask) {
            std::string m = Mask(mac_mask);
            body += "        " + temp(out_r) + "." + m + " = mac." + m + ";\n";
        }
        if (!ilu_expr.empty() && ilu_mask) {
            // With both units active the ILU result is hard-wired to r1.
            uint32_t reg = (!mac_expr.empty() && mac != MAC_ARL) ? 1 : out_r;
            std::string m = Mask(ilu_mask);
            body += "        " + temp(reg) + "." + m + " = ilu." + m + ";\n";
        }
        uint32_t o_mask = Get(ins, kOutOMask);
        if (o_mask) {
            std::string src = Get(ins, kOutMux) ? "ilu" : "mac";
            if ((src == "ilu" && ilu_expr.empty()) || (src == "mac" && mac_expr.empty())) src = "float4(0,0,0,0)";
            std::string m = Mask(o_mask);
            uint32_t addr = Get(ins, kOutAddress);
            if (Get(ins, kOutOrb)) {
                const char* name = OutputName(addr);
                if (name) body += "        " + std::string(name) + "." + m + " = (" + src + ")." + m + ";\n";
            } else {
                // Writes to constant memory (read/write programs).
                snprintf(buf, sizeof buf, "        /* c[%u].%s write ignored */\n", addr, m.c_str());
                body += buf;
            }
        }
        body += "    }\n";
        if (Get(ins, kFinal)) break;
    }
    // Undo the screen-space transform the XDK appends (c[-38] scale,
    // c[-37] offset) to get back to clip space.
    body += R"(
    float4 vscale = c[58], voffset = c[59];
    float w = opos.w;
    float3 ndc = (opos.xyz - voffset.xyz) / vscale.xyz;
    o.pos = float4(ndc * w, w);
    o.pos.x *= host_clip.x;
    o.d0 = saturate(o.d0);
    o.d1 = saturate(o.d1);
    o.fog = ofog.x;
}
)";
    return body;
}

}  // namespace d3d
