#pragma once

#include <QColor>
#include <QMatrix4x4>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QVector2D>

#include "geometry.h"

// The three ways the canvas draws with the GPU. Text and images use the
// scene graph's own nodes; everything else is one of these.
//
// Strokes and shapes each share one material between every node, and their
// shaders are compiled for batching, so the scene graph can draw many of
// them in one call (how the canvas arranges its nodes for that is in
// scene.cpp). Both need the true scale of the view to antialias by exactly
// a pixel, which is why they ask for the full matrix: nodes still batch
// when all that separates them is where they are.
//
// The grid asks for the whole matrix, the canvas's place in the window and
// all. Its shader works out which point of the board a pixel shows from
// where the pixel is in the canvas, and to batch a node the scene graph
// hands its shader the corners already moved to where they are in the
// window: every dot would be off by as much as the canvas is from the
// window's corner, the width of the rail. Asking for the whole matrix keeps
// the grid out of that, and its corners reach the shader as the canvas has
// them.

class StrokeMaterial : public QSGMaterial {
public:
    StrokeMaterial();
    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
};

class ShapeMaterial : public QSGMaterial {
public:
    ShapeMaterial();
    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
};

class GridMaterial : public QSGMaterial {
public:
    GridMaterial();
    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *other) const override;

    // Follows the view the board is drawn with: `view` takes the board to
    // the canvas at `viewZoom`, on a display of `devicePixels` to the pixel.
    void follow(const QMatrix4x4 &view, qreal viewZoom, qreal devicePixels);

    float zoom = 1, spacing = 24, fade = 1, radius = 1, dpr = 1;
    QVector2D origin;
    QColor color;
};

namespace Nodes {

// A node drawing the mesh, moved by `offset`. The material stays the caller's.
QSGGeometryNode *stroke(const Geo::StrokeMesh &mesh, QSGMaterial *material, QPointF offset = {});
QSGGeometryNode *shapes(const Geo::ShapeMesh &mesh, QSGMaterial *material, QPointF offset = {});
// Slides what a node draws, without building it again.
void shift(QSGGeometryNode *node, QPointF delta);
// Replaces what a node draws, for things that change every frame.
void setStroke(QSGGeometryNode *node, const Geo::StrokeMesh &mesh);
void setShapes(QSGGeometryNode *node, const Geo::ShapeMesh &mesh);

} // namespace Nodes
