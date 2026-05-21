#version 460
// beam_vert.glsl — Vertex shader for Vulkan beam rasterisation.
// Not used directly in the compute path (beam rasterisation uses a compute shader),
// but provided as a graphics pipeline fallback for implementations that prefer
// a vertex/fragment pipeline over compute.
//
// Usage: draw one quad per laser segment; expand to a thick billboard in the vertex shader.

layout(location = 0) in vec2  inPosA;      // Segment start NDC
layout(location = 1) in vec2  inPosB;      // Segment end NDC
layout(location = 2) in vec3  inColorA;    // Start colour (linear RGB)
layout(location = 3) in vec3  inColorB;    // End colour
layout(location = 4) in float inBeamWidth; // Beam width in NDC space

layout(location = 0) out vec3  fragColor;
layout(location = 1) out float fragDist;   // Signed perpendicular distance (pixels)
layout(location = 2) out float fragSigma;  // Gaussian sigma (pixels)

layout(push_constant) uniform PC {
    float beamThickness; // pixels
    float rcpWidth;      // 1.0 / render_width
    float rcpHeight;     // 1.0 / render_height
    float pad;
} pc;

void main() {
    // Expand the quad around the segment AB using the vertex ID (0-3).
    // Vertex 0,1: near-end (A), Vertex 2,3: far-end (B)
    int   vid      = gl_VertexIndex;
    vec2  posA     = inPosA;
    vec2  posB     = inPosB;

    float sigma    = max(pc.beamThickness * 0.5, 0.5);
    float ndcPadX  = sigma * 3.0 * pc.rcpWidth  * 2.0;
    float ndcPadY  = sigma * 3.0 * pc.rcpHeight * 2.0;
    vec2  pad      = vec2(ndcPadX, ndcPadY);

    vec2  dir      = normalize(posB - posA + vec2(1e-8));
    vec2  perp     = vec2(-dir.y, dir.x);

    // Four corners of the beam quad
    vec2 corners[4];
    corners[0] = posA - dir * pad + perp * pad;
    corners[1] = posA - dir * pad - perp * pad;
    corners[2] = posB + dir * pad + perp * pad;
    corners[3] = posB + dir * pad - perp * pad;

    gl_Position = vec4(corners[vid], 0.0, 1.0);

    fragColor = (vid < 2) ? inColorA : inColorB;
    fragDist  = (vid == 0 || vid == 2) ? sigma * 3.0 : -sigma * 3.0;
    fragSigma = sigma;
}
