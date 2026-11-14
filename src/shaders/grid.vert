#version 440

// The dot grid is a single quad over the whole canvas; the fragment shader
// places the dots. It costs the same whether ten dots are showing or ten
// thousand.

layout(location = 0) in vec2 position;

layout(location = 0) out vec2 vCanvas;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float zoom;
    float spacing; // between the finer dots, in canvas units
    float fade;    // how visible the finer dots are
    vec4 color;    // premultiplied
    vec2 origin;   // the canvas point at the quad's corner, kept small
    float radius;  // of a dot, in screen pixels
    float dpr;
} ubuf;

void main() {
    vCanvas = ubuf.origin + position / ubuf.zoom;
    gl_Position = ubuf.qt_Matrix * vec4(position, 0.0, 1.0);
}
