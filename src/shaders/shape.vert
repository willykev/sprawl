#version 440

// Notes, shapes, arrowheads and the selection's chrome are each one quad.
// The fragment shader works out how far every pixel is from the shape's true
// edge, so edges and borders are sharp at any zoom with no extra triangles.

layout(location = 0) in vec2 position; // a corner of the shape's box
layout(location = 1) in vec2 local;    // the same corner, from the middle of the shape
layout(location = 2) in vec4 shape;    // kind, corner radius, border width
layout(location = 3) in vec2 axis;     // which way the shape's own x axis points
layout(location = 4) in vec4 fill;     // premultiplied
layout(location = 5) in vec4 border;   // premultiplied

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec2 vHalf;
layout(location = 2) out vec4 vShape;
layout(location = 3) out vec4 vFill;
layout(location = 4) out vec4 vBorder;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float scale; // screen pixels per unit
} ubuf;

void main() {
    // One more pixel all round, for the soft edge to fall in.
    vec2 grow = sign(local) / ubuf.scale;
    vec2 point = position + axis * grow.x + vec2(-axis.y, axis.x) * grow.y;
    vLocal = local + grow;
    vHalf = abs(local);
    vShape = shape;
    vFill = fill;
    vBorder = border;
    gl_Position = ubuf.qt_Matrix * vec4(point, 0.0, 1.0);
}
