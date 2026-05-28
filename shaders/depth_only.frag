#version 450

// Depth-only fragment shader — does nothing. Paired with a pipeline whose colorWriteMask
// is 0, so depth is written but no color output is produced. Required by Vulkan because
// rasterization is enabled (rasterizerDiscardEnable would skip depth too).

void main() {}
