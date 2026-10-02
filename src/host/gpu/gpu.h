// Host graphics interface used by the D3D8 emulation (src/host/d3d).
//
// A small, D3D11-shaped API with two implementations: Direct3D 11
// (gpu_d3d11.cpp, Windows) and OpenGL 4.5 core (gpu_gl.cpp, any SDL
// platform). Conventions follow D3D: render target row 0 is the top, clip
// space z is [0, 1], textures are addressed from the top-left.
//
// Shaders are written in a small dialect that each backend turns into its
// own language with a prelude of macros (see kShaderDialect below). Calls
// come from any thread but never concurrently (the D3D layer serializes them).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpu {

// ---- Enumerations (numeric values match D3D11 where it has the concept) ----

enum class Format : uint8_t { BGRA8, BC1, BC2, BC3, D24S8 };
enum class Compare : uint8_t { Never = 1, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Blend : uint8_t {
    Zero = 1, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DestAlpha, InvDestAlpha, DestColor, InvDestColor,
    SrcAlphaSat, Factor = 14, InvFactor
};
enum class BlendOp : uint8_t { Add = 1, Subtract, RevSubtract, Min, Max };
enum class StencilOp : uint8_t { Keep = 1, Zero, Replace, IncrSat, DecrSat, Invert, Incr, Decr };
enum class Address : uint8_t { Wrap = 1, Mirror, Clamp, Border };
enum class Topology : uint8_t { PointList, LineList, LineStrip, TriangleList, TriangleStrip };
// Vertex fetch formats. Color is a D3DCOLOR (B, G, R, A bytes) read as RGBA.
enum class VertexFormat : uint8_t { Float1, Float2, Float3, Float4, Color, UByte4N, Short2N, Short4N, Short2, Short4 };
enum class Stage : uint8_t { Vertex, Pixel };

inline bool IsInteger(VertexFormat f) { return f == VertexFormat::Short2 || f == VertexFormat::Short4; }

// ---- State ----

struct BlendState {
    bool enable = false;
    Blend src = Blend::One, dst = Blend::Zero, src_alpha = Blend::One, dst_alpha = Blend::Zero;
    BlendOp op = BlendOp::Add;
    uint8_t write_mask = 0xF;  // bit 0 red, 1 green, 2 blue, 3 alpha
    float factor[4] = {};
};

struct DepthStencilState {
    bool depth_enable = false, depth_write = true;
    Compare depth_func = Compare::Less;
    bool stencil_enable = false;
    uint8_t stencil_read_mask = 0xFF, stencil_write_mask = 0xFF, stencil_ref = 0;
    StencilOp fail = StencilOp::Keep, depth_fail = StencilOp::Keep, pass = StencilOp::Keep;
    Compare stencil_func = Compare::Always;
};

struct RasterState {
    bool wireframe = false;
    bool cull_back = false;  // cull back faces
    bool front_ccw = false;  // counter-clockwise (in render target pixels) is the front
    int depth_bias = 0;      // in units of the depth format's resolution
    float slope_bias = 0;
};

struct SamplerState {
    bool min_linear = false, mag_linear = false, mip_linear = false, anisotropic = false;
    uint32_t max_anisotropy = 1;
    Address u = Address::Wrap, v = Address::Wrap, w = Address::Wrap;
    float border[4] = {};
    float lod_bias = 0, min_lod = 0, max_lod = 1000;
};

struct VertexElement {
    uint32_t reg;     // shader input register v[reg]
    uint32_t stream;  // vertex stream (SetVertexStream)
    uint32_t offset;  // bytes into the vertex
    VertexFormat format;
};

struct Viewport {
    float x, y, w, h, min_z, max_z;
};

struct Rect {
    int left, top, right, bottom;
};

struct Texture;  // backend object
struct Shader;   // backend object (a vertex shader includes its input layout)

struct SubresourceData {
    const void* data;
    uint32_t row_pitch;  // bytes per row (per block row for BCn)
};

// ---- Device ----

// Creates the device for the platform window (null: offscreen only).
// KT_GPU / [video] gpu = d3d11 | gl picks the backend.
void CreateDevice();
const char* BackendName();

// ---- Resources ----

// `init` holds faces * levels entries, face-major (cube maps: +X -X +Y -Y +Z -Z).
Texture* CreateTexture(uint32_t w, uint32_t h, uint32_t levels, Format f, bool cube, const SubresourceData* init);
Texture* CreateRenderTarget(uint32_t w, uint32_t h, bool depth);  // BGRA8 color or D24S8
void DestroyTexture(Texture* t);
// Rectangles of level 0 (BGRA8 textures and color targets).
void UpdateTexture(Texture* t, const Rect& r, const void* bgra, uint32_t pitch);
void CopyTexture(Texture* dst, int dx, int dy, Texture* src, const Rect& r);
bool ReadTexture(Texture* src, const Rect& r, std::vector<uint8_t>* bgra);  // rows tightly packed

// ---- Shaders ----

// Shader sources use the dialect: HLSL-style types and intrinsics plus the
// macros below; vertex shaders define
//     void XboxVS(float4 v[16], int vid, out VSOut o)
// (v: inputs by register, (0, 0, 0, 1) where none is bound) and pixel
// shaders `float4 XboxPS(VSOut i)`. VSOut is { pos, d0, d1, t0..t3, fog }.
//   UNIFORMS_VS(name, slot) { ... };  UNIFORMS_PS(name, slot) { ... };  (float4 / float4x4 members)
//   ROW_MAJOR (matrix members), TEXTURE2D(name, slot), TEXTURECUBE(name, slot),
//   SAMPLE(name, coord), LOAD2D(name, int2 pixel), TO3X3(m), XXXX(s) / XXX(s)
//   (scalar replicate), SLT / SGE (componentwise compares as 0/1), UNROLL.
// Returns null (and logs) when compilation fails.
Shader* CompileVertexShader(const std::string& src, const std::vector<VertexElement>& inputs, const char* tag);
Shader* CompilePixelShader(const std::string& src, const char* tag);

// ---- Draw state and commands ----

void SetRenderTargets(Texture* color, Texture* depth);
void SetViewport(const Viewport& vp);
void SetBlendState(const BlendState& s);
void SetDepthStencilState(const DepthStencilState& s);
void SetRasterState(const RasterState& s);
void SetTexture(uint32_t slot, Texture* t);  // slots 0-3; a bound render target reads as null
void SetSampler(uint32_t slot, const SamplerState& s);
void SetConstants(Stage stage, uint32_t slot, const void* data, uint32_t bytes);
void SetShaders(Shader* vs, Shader* ps);
// Copies `bytes` of vertex data for this draw (data null: stream unused).
void SetVertexStream(uint32_t stream, const void* data, uint32_t bytes, uint32_t stride);
void Draw(Topology t, uint32_t vertices);
void DrawIndexed(Topology t, const uint16_t* indices, uint32_t count);

// Clears parts of the bound targets (rects null: everything), ignoring
// masks and the viewport.
void Clear(const Rect* rects, uint32_t count, const float* color, const float* depth, const uint8_t* stencil);

// Scales `src` into all of `dst` with bilinear filtering.
void BlitTexture(Texture* src, Texture* dst);
// Shows `image` in the window, scaled to fit with its aspect kept.
void Present(Texture* image);

}  // namespace gpu
