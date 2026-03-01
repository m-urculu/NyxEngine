#version 450

// triangle.frag — Fragment shader
//
// This shader runs once per pixel (fragment) covered by the triangle.
// It receives the interpolated color from the vertex shader and
// outputs it as the pixel color.
//
// "Interpolated" means: if vertex A is red and vertex B is green,
// a pixel halfway between them will be yellow (mix of red + green).

// Input from vertex shader (interpolated across the triangle)
layout(location = 0) in vec3 fragColor;

// Output: the final pixel color (RGBA)
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);  // RGB from vertex shader, alpha = 1 (fully opaque)
}
