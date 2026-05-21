// renderer.cpp — Factory implementation selecting the platform renderer.

#include "renderer.h"

#ifdef _WIN32
#include "d3d12_renderer.h"
#else
#include "vulkan_renderer.h"
#endif

namespace idhmfis {

std::unique_ptr<IRenderer> create_renderer(const RenderConfig& cfg) {
#ifdef _WIN32
    auto r = std::make_unique<D3D12Renderer>();
    (void)cfg; // cfg is passed via init(); factory just allocates the object
    return r;
#else
    auto r = std::make_unique<VulkanRenderer>();
    (void)cfg;
    return r;
#endif
}

} // namespace idhmfis
