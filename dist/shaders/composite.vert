#version 450

// Fullscreen-triangle vertex shader for the composite pass — no vertex buffer.
// Emits a single triangle that covers the entire viewport in NDC, and passes the
// implied UV [0,1] to the fragment stage.

layout(location = 0) out vec2 vUv;

void main() {
    vec2 ndc = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                    (gl_VertexIndex == 2) ? 3.0 : -1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    // ndc xy in [-1,1] → uv in [0,1]. Vulkan's screen-space Y points down already,
    // so this matches the HDR scene image's storage orientation directly.
    vUv = ndc * 0.5 + 0.5;
}
