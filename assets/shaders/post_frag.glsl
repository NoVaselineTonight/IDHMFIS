#version 460
// post_frag.glsl — Fragment shader for the Vulkan post-process graphics pass.
// Used as a fallback when the compute post-process path is unavailable.
// Applies bloom (single-pass approximation using MIP levels), haze, exposure,
// Reinhard tone-map, gamma correction, and optional film grain.
//
// NOTE: The primary Vulkan path uses the compute shader (kPostComputeGLSL in
// vulkan_renderer.cpp) which has proper separable bloom.  This fragment shader
// provides a simpler single-pass bloom using hardware bilinear MIP sampling.

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gHDR;
layout(set = 0, binding = 1) uniform sampler2D gBloomTmp;

layout(push_constant) uniform PC {
    uint  width;
    uint  height;
    float bloomRadius;
    float hazeDensity;
    float exposure;
    uint  filmGrain;
    float grainAmount;
    uint  frameCounter;
    uint  passMode;
} pc;

// Integer hash → [0,1)
float hash(uvec2 p, uint seed) {
    uint n = p.x * 1619u + p.y * 31337u + seed * 1013904223u;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = n ^ (n >> 16u);
    return float(n) * (1.0 / 4294967296.0);
}

// Reinhard extended tone-map
vec3 reinhardExt(vec3 v, float maxWhite) {
    return (v * (1.0 + v / (maxWhite * maxWhite))) / (1.0 + v);
}

void main() {
    vec3 hdr   = texture(gHDR,      fragUV).rgb;
    vec3 bloom = texture(gBloomTmp, fragUV).rgb;

    vec3 color = hdr + bloom;

    // Haze
    vec2  centred = fragUV * 2.0 - 1.0;
    float dist    = length(centred);
    float haze_t  = exp(-dist * dist * pc.hazeDensity * 2.0);
    vec3  haze_col = vec3(0.06, 0.08, 0.12) * pc.hazeDensity;
    color = color * (1.0 - pc.hazeDensity * 0.15) + haze_col * (1.0 - haze_t);

    // Exposure
    color *= pc.exposure;

    // Tone-map
    color = reinhardExt(color, 4.0);

    // Gamma
    color = pow(max(color, 0.0), vec3(1.0 / 2.2));

    // Film grain
    if (pc.filmGrain != 0u) {
        uvec2 px   = uvec2(fragUV * vec2(pc.width, pc.height));
        float noise = hash(px, pc.frameCounter) * 2.0 - 1.0;
        color += noise * pc.grainAmount;
    }

    color = clamp(color, 0.0, 1.0);
    outColor = vec4(color, 1.0);
}
