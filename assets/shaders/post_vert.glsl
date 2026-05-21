#version 460
// post_vert.glsl — Full-screen triangle vertex shader for the Vulkan post-process pass.
// Generates a full-screen triangle using only gl_VertexIndex (no vertex buffer required).

layout(location = 0) out vec2 fragUV;

void main() {
    // Classic full-screen triangle trick:
    // Vertex 0: (-1, -1), UV (0, 0)
    // Vertex 1: ( 3, -1), UV (2, 0)
    // Vertex 2: (-1,  3), UV (0, 2)
    // The triangle covers the entire [-1,1] NDC square.
    vec2 pos = vec2(
        (gl_VertexIndex == 1) ?  3.0 : -1.0,
        (gl_VertexIndex == 2) ?  3.0 : -1.0
    );
    fragUV      = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
