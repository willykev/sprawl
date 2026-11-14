#include "materials.h"

#include <QSGMaterialShader>
#include <cmath>
#include <cstring>

namespace {

// Every shader's uniform block starts the same way: the matrix, the opacity,
// and then, for strokes and shapes, how many screen pixels a unit is.
void writeMatrixAndOpacity(QSGMaterialShader::RenderState &state) {
    QByteArray *uniforms = state.uniformData();
    const QMatrix4x4 matrix = state.combinedMatrix();
    std::memcpy(uniforms->data(), matrix.constData(), 64);
    const float opacity = state.opacity();
    std::memcpy(uniforms->data() + 64, &opacity, 4);
}

void writeScale(QSGMaterialShader::RenderState &state) {
    // The matrix takes a unit step to clip space, which is the viewport
    // across two: so it knows the zoom, the display's scale, and any
    // transform the item sits under, without being told.
    const QMatrix4x4 matrix = state.combinedMatrix();
    const QRect viewport = state.viewportRect();
    const float scale = float(std::hypot(matrix(0, 0) * viewport.width(), matrix(1, 0) * viewport.height()) / 2);
    std::memcpy(state.uniformData()->data() + 68, &scale, 4);
}

class ScaledShader : public QSGMaterialShader {
public:
    explicit ScaledShader(const QString &name) {
        setShaderFileName(VertexStage, ":/shaders/" + name + ".vert.qsb");
        setShaderFileName(FragmentStage, ":/shaders/" + name + ".frag.qsb");
    }
    bool updateUniformData(RenderState &state, QSGMaterial *, QSGMaterial *) override {
        writeMatrixAndOpacity(state);
        writeScale(state);
        return true;
    }
};

class GridShader : public QSGMaterialShader {
public:
    GridShader() {
        setShaderFileName(VertexStage, ":/shaders/grid.vert.qsb");
        setShaderFileName(FragmentStage, ":/shaders/grid.frag.qsb");
    }
    bool updateUniformData(RenderState &state, QSGMaterial *material, QSGMaterial *) override {
        writeMatrixAndOpacity(state);
        const auto *grid = static_cast<GridMaterial *>(material);
        const QColor color = grid->color;
        const float alpha = color.alphaF();
        // The block as grid.vert lays it out (std140).
        const float values[] = {grid->zoom,
                                grid->spacing,
                                grid->fade,
                                color.redF() * alpha,
                                color.greenF() * alpha,
                                color.blueF() * alpha,
                                alpha,
                                grid->origin.x(),
                                grid->origin.y(),
                                grid->radius,
                                grid->dpr};
        std::memcpy(state.uniformData()->data() + 68, values, sizeof(values));
        return true;
    }
};

const QSGGeometry::AttributeSet &strokeAttributes() {
    static const QSGGeometry::Attribute attributes[] = {
        QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),
        QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType),
        QSGGeometry::Attribute::create(2, 2, QSGGeometry::FloatType),
        QSGGeometry::Attribute::create(3, 4, QSGGeometry::UnsignedByteType),
    };
    static const QSGGeometry::AttributeSet set = {4, sizeof(Geo::StrokeVertex), attributes};
    return set;
}

const QSGGeometry::AttributeSet &shapeAttributes() {
    static const QSGGeometry::Attribute attributes[] = {
        QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),
        QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType),
        QSGGeometry::Attribute::create(2, 4, QSGGeometry::FloatType),
        QSGGeometry::Attribute::create(3, 2, QSGGeometry::FloatType),
        QSGGeometry::Attribute::create(4, 4, QSGGeometry::UnsignedByteType),
        QSGGeometry::Attribute::create(5, 4, QSGGeometry::UnsignedByteType),
    };
    static const QSGGeometry::AttributeSet set = {6, sizeof(Geo::ShapeVertex), attributes};
    return set;
}

QSGGeometryNode *nodeFor(QSGGeometry *geometry, QSGMaterial *material) {
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    auto *node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(material);
    return node;
}

void fillStroke(QSGGeometry *geometry, const Geo::StrokeMesh &mesh) {
    std::memcpy(geometry->vertexData(), mesh.vertices.constData(), mesh.vertices.size() * sizeof(Geo::StrokeVertex));
    if (geometry->indexType() == QSGGeometry::UnsignedIntType) {
        std::memcpy(geometry->indexData(), mesh.indices.constData(), mesh.indices.size() * sizeof(quint32));
    } else {
        quint16 *indices = geometry->indexDataAsUShort();
        for (qsizetype i = 0; i < mesh.indices.size(); ++i)
            indices[i] = quint16(mesh.indices[i]);
    }
}

void fillShapes(QSGGeometry *geometry, const Geo::ShapeMesh &mesh) {
    std::memcpy(geometry->vertexData(), mesh.vertices.constData(), mesh.vertices.size() * sizeof(Geo::ShapeVertex));
    // Two triangles per quad.
    quint16 *indices = geometry->indexDataAsUShort();
    for (int quad = 0; quad < mesh.count(); ++quad) {
        const quint16 first = quint16(quad * 4);
        for (quint16 corner : {0, 1, 2, 1, 3, 2})
            *indices++ = first + corner;
    }
}

// Indices are 16 bit unless a stroke is too long for that; nodes that agree
// can share a batch.
QSGGeometry::Type indexTypeFor(qsizetype vertices) {
    return vertices > 0xffff ? QSGGeometry::UnsignedIntType : QSGGeometry::UnsignedShortType;
}

} // namespace

StrokeMaterial::StrokeMaterial() {
    setFlag(Blending);
    setFlag(RequiresFullMatrixExceptTranslate);
}

QSGMaterialType *StrokeMaterial::type() const {
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader *StrokeMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new ScaledShader("stroke");
}

ShapeMaterial::ShapeMaterial() {
    setFlag(Blending);
    setFlag(RequiresFullMatrixExceptTranslate);
}

QSGMaterialType *ShapeMaterial::type() const {
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader *ShapeMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new ScaledShader("shape");
}

GridMaterial::GridMaterial() {
    setFlag(Blending);
    // So its corners reach the shader as the canvas has them (see materials.h).
    setFlag(RequiresFullMatrix);
}

QSGMaterialType *GridMaterial::type() const {
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader *GridMaterial::createShader(QSGRendererInterface::RenderMode) const { return new GridShader; }

int GridMaterial::compare(const QSGMaterial *other) const { return this == other ? 0 : (this < other ? -1 : 1); }

void GridMaterial::follow(const QMatrix4x4 &view, qreal viewZoom, qreal devicePixels) {
    // Two grids a factor of two apart; the finer fades out as zooming
    // out crowds its dots.
    qreal step = 24;
    while (step * viewZoom < 16)
        step *= 2;
    while (step * viewZoom >= 32)
        step /= 2;
    zoom = float(viewZoom);
    spacing = float(step);
    fade = float(qBound(0.0, (step * viewZoom - 16) / 8, 1.0));
    dpr = float(devicePixels);
    radius = dpr;
    // The canvas point at the corner, wrapped to the grid in double
    // precision here so the shader's floats stay small and exact however
    // far the board has been panned.
    const qreal period = step * 2;
    origin = QVector2D(float(std::fmod(-view(0, 3) / viewZoom, period)),
                       float(std::fmod(-view(1, 3) / viewZoom, period)));
}

namespace Nodes {

// Both kinds of vertex start with where they are, so one loop moves either.
static void slide(QSGGeometry *geometry, QPointF delta) {
    if (delta.isNull())
        return;
    const float dx = float(delta.x()), dy = float(delta.y());
    char *vertex = static_cast<char *>(geometry->vertexData());
    const int stride = geometry->sizeOfVertex();
    for (int i = 0; i < geometry->vertexCount(); ++i, vertex += stride) {
        float at[2];
        std::memcpy(at, vertex, sizeof(at));
        at[0] += dx;
        at[1] += dy;
        std::memcpy(vertex, at, sizeof(at));
    }
}

QSGGeometryNode *stroke(const Geo::StrokeMesh &mesh, QSGMaterial *material, QPointF offset) {
    auto *geometry = new QSGGeometry(strokeAttributes(), int(mesh.vertices.size()), int(mesh.indices.size()),
                                     indexTypeFor(mesh.vertices.size()));
    fillStroke(geometry, mesh);
    slide(geometry, offset);
    return nodeFor(geometry, material);
}

QSGGeometryNode *shapes(const Geo::ShapeMesh &mesh, QSGMaterial *material, QPointF offset) {
    auto *geometry = new QSGGeometry(shapeAttributes(), int(mesh.vertices.size()), mesh.count() * 6,
                                     QSGGeometry::UnsignedShortType);
    fillShapes(geometry, mesh);
    slide(geometry, offset);
    return nodeFor(geometry, material);
}

void shift(QSGGeometryNode *node, QPointF delta) {
    slide(node->geometry(), delta);
    node->markDirty(QSGNode::DirtyGeometry);
}

void setStroke(QSGGeometryNode *node, const Geo::StrokeMesh &mesh) {
    QSGGeometry *geometry = node->geometry();
    if (geometry->indexType() != indexTypeFor(mesh.vertices.size())) {
        // Grown past what 16 bit indices reach: it needs a new buffer.
        geometry = new QSGGeometry(strokeAttributes(), int(mesh.vertices.size()), int(mesh.indices.size()),
                                   indexTypeFor(mesh.vertices.size()));
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry); // the node owns it, and deletes the old one
    } else {
        geometry->allocate(int(mesh.vertices.size()), int(mesh.indices.size()));
    }
    fillStroke(geometry, mesh);
    node->markDirty(QSGNode::DirtyGeometry);
}

void setShapes(QSGGeometryNode *node, const Geo::ShapeMesh &mesh) {
    QSGGeometry *geometry = node->geometry();
    geometry->allocate(int(mesh.vertices.size()), mesh.count() * 6);
    fillShapes(geometry, mesh);
    node->markDirty(QSGNode::DirtyGeometry);
}

} // namespace Nodes
