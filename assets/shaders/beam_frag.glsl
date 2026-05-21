#version 460
// beam_frag.glsl — Fragment shader applying Gaussian beam profile.
// Used with beam_vert.glsl in the optional vertex/fragment pipeline.
// Outputs additively-blended HDR colour to the RGBA16F framebuffer.

layout(location = 0) in vec3  fragColor;
layout(location = 1) in float fragDist;    // Perpendicular distance from beam axis (pixels)
layout(location = 2) in float fragSigma;   // Gaussian sigma

layout(location = 0) out vec4 outColor;

void main() {
    // Gaussian profile: I(d) = exp( −d² / (2σ²) )
    // Peak intensity 1.0 at d=0, rolls off to 0.011 at d=3σ.
    float sigma      = max(fragSigma, 0.5);
    float inv2sigma2 = 1.0 / (2.0 * sigma * sigma);
    float d2         = fragDist * fragDist;
    float gaussian   = exp(-d2 * inv2sigma2);

    // The framebuffer uses additive blending (VK_BLEND_OP_ADD),
    // so we simply output the attenuated beam colour.
    // Alpha is not used for blending here — it carries the Gaussian weight
    // for potential future opacity-based compositing.
    outColor = vec4(fragColor * gaussian, gaussian);
}
