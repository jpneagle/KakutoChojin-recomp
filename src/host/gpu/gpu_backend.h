// Shared by the gpu.h backends: object bases and the per-backend entry points
// that gpu.cpp dispatches to.
#pragma once

#include "gpu.h"

namespace gpu {

struct Texture {
    uint32_t w = 0, h = 0;
    Format format = Format::BGRA8;
    virtual ~Texture() = default;
};

struct Shader {
    virtual ~Shader() = default;
};

// Writes a generated shader to shaders/ when KT_DUMP_SHADERS is set.
void DumpShader(const std::string& src, const char* tag, const char* ext);

#define KT_GPU_BACKEND_FUNCTIONS                                                                                      \
    bool CreateDevice();                                                                                              \
    Texture* CreateTexture(uint32_t w, uint32_t h, uint32_t levels, Format f, bool cube, const SubresourceData* init); \
    Texture* CreateRenderTarget(uint32_t w, uint32_t h, bool depth);                                                  \
    void DestroyTexture(Texture* t);                                                                                  \
    void UpdateTexture(Texture* t, const Rect& r, const void* bgra, uint32_t pitch);                                  \
    void CopyTexture(Texture* dst, int dx, int dy, Texture* src, const Rect& r);                                      \
    bool ReadTexture(Texture* src, const Rect& r, std::vector<uint8_t>* bgra);                                        \
    Shader* CompileVertexShader(const std::string& src, const std::vector<VertexElement>& inputs, const char* tag);  \
    Shader* CompilePixelShader(const std::string& src, const char* tag);                                              \
    void SetRenderTargets(Texture* color, Texture* depth);                                                            \
    void SetViewport(const Viewport& vp);                                                                             \
    void SetBlendState(const BlendState& s);                                                                          \
    void SetDepthStencilState(const DepthStencilState& s);                                                            \
    void SetRasterState(const RasterState& s);                                                                        \
    void SetTexture(uint32_t slot, Texture* t);                                                                       \
    void SetSampler(uint32_t slot, const SamplerState& s);                                                            \
    void SetConstants(Stage stage, uint32_t slot, const void* data, uint32_t bytes);                                  \
    void SetShaders(Shader* vs, Shader* ps);                                                                          \
    void SetVertexStream(uint32_t stream, const void* data, uint32_t bytes, uint32_t stride);                         \
    void Draw(Topology t, uint32_t vertices);                                                                         \
    void DrawIndexed(Topology t, const uint16_t* indices, uint32_t count);                                            \
    void Clear(const Rect* rects, uint32_t count, const float* color, const float* depth, const uint8_t* stencil);    \
    void BlitTexture(Texture* src, Texture* dst);                                                                     \
    void Present(Texture* image);

#ifdef _WIN32
namespace d3d11 {
KT_GPU_BACKEND_FUNCTIONS
}
#endif
namespace gl {
KT_GPU_BACKEND_FUNCTIONS
}

}  // namespace gpu
