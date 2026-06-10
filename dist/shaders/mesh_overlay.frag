#version 450

// Occlusion-overlay fragment shader. Paired with a pipeline that tests depth GREATER
// and doesn't write depth, so it draws ONLY where the mesh is behind other geometry
// (occluded). Used to draw the third-person character as a uniform, see-through
// silhouette through terrain — one opacity level, exactly the character's shape.

layout(location = 0) in vec3 fragColor;   // from mesh.vert (the character's vertex colours)

layout(location = 0) out vec4 outColor;

void main() {
    // Opaque: where the character is occluded, draw it solid so it replaces the
    // terrain in front of it (you see the character, not a terrain+character mix).
    outColor = vec4(fragColor * 1.25 + 0.05, 1.0);   // slightly brightened so it reads clearly
}
