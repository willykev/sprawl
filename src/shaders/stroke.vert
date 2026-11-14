#version 440

// A stroke is a ribbon of triangles along the pen's path. Each vertex knows
// which way is out, so the ribbon is widened here, on the GPU, by exactly
// what the current zoom needs: never thinner than one screen pixel, plus
// half a pixel for a soft edge. Zooming therefore costs nothing per stroke.

layout(location = 0) in vec2 position; // on the outline, at the stroke's own width
layout(location = 1) in vec2 normal;   // which way is out
layout(location = 2) in vec2 extent;   // half the width, and the side: -1, 0 or 1
layout(location = 3) in vec4 color;    // premultiplied

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vEdge;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float scale; // screen pixels per unit
} ubuf;

void main() {
    float halfPx = extent.x * ubuf.scale;
    float reachPx = max(halfPx, 0.5) + 0.5;
    vec2 point = position + normal * extent.y * (reachPx - halfPx) / ubuf.scale;
    // How far this vertex is from the middle of the stroke, in pixels, and
    // how wide half the stroke is.
    vEdge = vec2(extent.y * reachPx, halfPx);
    vColor = color;
    gl_Position = ubuf.qt_Matrix * vec4(point, 0.0, 1.0);
}
