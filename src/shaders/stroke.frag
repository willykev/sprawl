#version 440

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vEdge;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float scale;
} ubuf;

void main() {
    // Full inside the stroke, fading to nothing over the pixel at its edge.
    float coverage = clamp(max(vEdge.y, 0.5) + 0.5 - abs(vEdge.x), 0.0, 1.0);
    // A stroke thinner than a pixel is drawn a pixel wide and fainter, so a
    // zoomed-out board keeps unbroken hairlines instead of sparkling ones.
    coverage *= min(vEdge.y * 2.0, 1.0);
    fragColor = vColor * (coverage * ubuf.qt_Opacity);
}
