#version 440

layout(location = 0) in vec2 vCanvas;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float zoom;
    float spacing;
    float fade;
    vec4 color;
    vec2 origin;
    float radius;
    float dpr;
} ubuf;

// How much of this pixel a dot of the grid with this spacing covers.
float dots(float spacing) {
    vec2 fromDot = (fract(vCanvas / spacing + 0.5) - 0.5) * spacing;
    float pixels = length(fromDot) * ubuf.zoom * ubuf.dpr;
    return clamp(ubuf.radius + 0.5 - pixels, 0.0, 1.0);
}

void main() {
    // Two grids, one twice as wide as the other. Zooming out fades the finer
    // one away as its dots crowd, so the grid never turns to noise.
    float coverage = max(dots(ubuf.spacing) * ubuf.fade, dots(ubuf.spacing * 2.0));
    fragColor = ubuf.color * (coverage * ubuf.qt_Opacity);
}
