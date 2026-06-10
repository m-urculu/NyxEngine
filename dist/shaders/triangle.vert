#version 450

// triangle.vert — Vertex shader
//
// This shader runs once per vertex. For our hardcoded triangle,
// it runs 3 times (once for each corner).
//
// The triangle vertices and colors are defined right here in the shader.
// In future phases, we'll pass vertex data from CPU via vertex buffers.

// Output to the fragment shader
layout(location = 0) out vec3 fragColor;

// Hardcoded triangle: positions and colors for 3 vertices
// Vulkan's coordinate system: X right, Y down, Z into screen
// Normalized Device Coordinates (NDC): -1 to +1 on each axis
void main() {
    // Vertex positions (X, Y) — forms a triangle centered on screen
    vec2 positions[3] = vec2[](
        vec2( 0.0, -0.5),   // Top center
        vec2( 0.5,  0.5),   // Bottom right
        vec2(-0.5,  0.5)    // Bottom left
    );

    // Vertex colors (RGB) — one color per vertex
    vec3 colors[3] = vec3[](
        vec3(1.0, 0.0, 0.0),   // Red
        vec3(0.0, 1.0, 0.0),   // Green
        vec3(0.0, 0.0, 1.0)    // Blue
    );

    // gl_VertexIndex is the built-in vertex index (0, 1, or 2)
    // gl_Position is the built-in output position (vec4: x, y, z, w)
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}
