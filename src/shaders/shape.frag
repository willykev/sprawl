#version 440

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec2 vHalf;
layout(location = 2) in vec4 vShape;
layout(location = 3) in vec4 vFill;
layout(location = 4) in vec4 vBorder;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float scale;
} ubuf;

// How far a point is outside the shape (negative: inside), in units.
float outside(vec2 p, vec2 h, float kind, float radius) {
    if (kind < 0.5) {
        // A rectangle, with round corners when it has a radius.
        vec2 q = abs(p) - h + radius;
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    }
    if (kind < 1.5) {
        // An ellipse: its equation over the equation's slope is the distance
        // to first order, which is exact where it matters, at the edge.
        vec2 q = p / h;
        vec2 slope = 2.0 * q / h;
        return (dot(q, q) - 1.0) / max(length(slope), 1e-6);
    }
    if (kind < 2.5) {
        // A diamond: four straight edges.
        vec2 q = abs(p);
        return (q.x / h.x + q.y / h.y - 1.0) * (h.x * h.y) / length(h);
    }
    // An arrowhead: a triangle with its tip at the end of the x axis.
    vec2 slant = normalize(vec2(h.y, 2.0 * h.x));
    return max(-h.x - p.x, dot(vec2(p.x - h.x, abs(p.y)), slant));
}

void main() {
    float d = outside(vLocal, vHalf, vShape.x, vShape.y) * ubuf.scale;
    float whole = clamp(0.5 - d, 0.0, 1.0);
    // The border is the band just inside the edge; the fill is the rest.
    float within = clamp(0.5 - d - vShape.z * ubuf.scale, 0.0, 1.0);
    fragColor = (vFill * within + vBorder * (whole - within)) * ubuf.qt_Opacity;
}
