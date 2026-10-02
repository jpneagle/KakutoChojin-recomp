// Vertex layouts, shader objects and shader generation (the gpu.h dialect).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "d3d_internal.h"

namespace d3d {

// One vertex attribute, addressed by Xbox input register (v0..v15).
// Fixed-function registers: 0 position, 1 blend weights, 2 normal,
// 3 diffuse, 4 specular, 5 fog, 8 point size, 9..12 texcoord 0..3.
struct Element {
    UINT stream, offset, reg, xtype;  // xtype: Xbox D3DVSDT_* code
    UINT components;
    gpu::VertexFormat host;           // vertex fetch format (after conversion)
    bool integer;                     // fetched as int4 (SHORTn)
    bool convert;                     // rewritten to FLOAT4 on the CPU
    UINT host_offset;
};

struct Layout {
    std::vector<Element> elements;
    UINT host_stride[16] = {};
    bool convert[16] = {};
    bool used[16] = {};
    std::string signature;  // identifies the layout for caching
    void Finish();          // computes conversions/offsets/signature
};

// D3DVSDT_* type helpers.
UINT XTypeSize(UINT t);
void ConvertElement(UINT t, const uint8_t* src, float* out);

// gpu.h vertex inputs for a layout.
std::vector<gpu::VertexElement> VertexInputs(const Layout& L);

// Host-side vertex shader constants (UNIFORMS_VS slot 2).
extern const char* const kShaderHostVS;

// ---- Translators ----
// NV2A vertex program (microcode, 4 dwords per instruction) -> XboxVS().
std::string TranslateVertexProgram(const uint32_t* program, UINT instructions, std::string* error);
// Xbox register-combiner pixel shader definition (61 dwords) -> XboxPS().
std::string TranslatePixelShader(const uint32_t* psdef, std::string* error);
// Fixed function, generated from the current state.
std::string GenerateFixedVertexShader(const Layout& L, const std::string& key);
std::string FixedVertexKey(const Layout& L);
std::string GenerateFixedPixelShader(const std::string& key);
std::string FixedPixelKey();

// Shared pixel shader constants and helpers (fog, alpha test).
extern const char* const kShaderPixelCommon;

// ---- Draw-time binding (shader.cpp) ----
const Layout* CurrentLayout();
bool BindShaders();  // false: the draw can't be translated and is skipped

}  // namespace d3d
