// The half of CanvasView that draws: items are prepared on the GUI thread
// (text laid out, strokes cut into triangles) and handed to the scene graph
// when it synchronizes.
//
// How the nodes are arranged is what keeps a big board quick, and what keeps
// it within what a GPU can be asked for. The scene graph draws things that
// blend in the order they come, and can only draw two of them in one call
// when nothing between them might show through. A board drawn item by item
// (this note's paper, its words, the next note's paper, its words) leaves it
// nothing to join, and it ends up making a call or two for every item, every
// frame. So the board is drawn like this instead:
//
//   layers   Every item is given a layer: one more than the highest layer
//            of anything underneath that it overlaps. Things in the same
//            layer never overlap, so the order they are drawn in among
//            themselves does not matter, and what does matter (this is on
//            top of that) is kept by drawing the layers in turn. A board
//            where nothing overlaps is one layer; a pile of three notes is
//            three.
//   buckets  A layer is cut into squares of the board, and the items of a
//            square are a bucket (or several, when there are many). What
//            is bigger than a square goes by the square its middle is in,
//            in buckets kept apart from smaller things. In a bucket all
//            the shapes come first, then the strokes, the pictures, and
//            the text, style by style, so each is one call. A bucket is a
//            batch of its own to the scene graph: changing an item costs
//            its bucket, never the board.
//   view     Only what is near the window is in the scene graph at all: a
//            bucket's square is cut again into sixteen parts (and what is
//            bigger than a square has a part to itself), and the parts out
//            of view are taken out of it. They cost nothing, however big
//            the board is. That matters twice over, because a GPU is not
//            done with a triangle that is off screen for free: measured,
//            it holds on to those as it does the ones in view.
//            (Taken out, not just blocked: the scene graph still calls on
//            every piece of text under a blocked node, every frame.)
//   detail   What is too small on screen to make out is left out: type
//            under a few pixels tall, and the fine curves of pen strokes,
//            which are kept in three levels of detail. And when what is in
//            view would still come to more triangles than a frame's budget
//            (see m_loadBudget), more is left out, the least missed first,
//            until it fits. What a frame asks of the GPU is bounded by the
//            window, never by the board.
//
// Panning and zooming change one matrix above all of it, which the scene
// graph answers without visiting a single item.
#include "canvasview.h"

#include <QLineF>
#include <QMap>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QVarLengthArray>
#include <cmath>

#include "imagestore.h"
#include "materials.h"

namespace {

using Geo::packColor;

// How long preparing items that are not on screen may take out of a frame.
const qint64 backgroundBudget = 3'000'000; // nanoseconds
// More selected items than this get one box around them all rather than one each.
const int outlineLimit = 300;
// The squares the board is cut into, in units. A few of them cover a window
// at 100%, so little is drawn that is not on screen; and a board seen whole
// is not many of them, so it is not many calls.
const qreal cellSize = 2048;
// A square is cut again, this many parts a side, for deciding what of it is
// in view: a part is about a third of a window across at 100%.
const int partsAcross = 4, partCount = partsAcross * partsAcross;
// How much a bucket holds. Small enough that rebuilding one is nothing,
// big enough that a board is not many of them.
const int bucketItems = 256, bucketVertices = 48'000;
// Past this many things to draw, the scene graph will treat a bucket as a
// batch of its own (its QSG_RENDERER_BATCH_NODE_THRESHOLD).
const int rootThreshold = 64;

// The levels of detail a long pen stroke is kept in. Each is used while a
// unit is fewer screen pixels than its `below`, and strays from the true
// curve by no more than its tolerance in units: under half a pixel, always.
const int detailLevels = 3;
const qreal coarseTolerance[detailLevels] = {0, 0.4, 1.6};
const qreal coarseBelow[detailLevels] = {0, 1.0, 0.25};
// A stroke of fewer triangles than this is not worth keeping three times.
const int heavyStroke = 96;
// Type under this many screen pixels tall is a smudge, and is not drawn.
const qreal legibleText = 5;
// Roughly what the GPU keeps of one triangle of each kind until the frame
// is drawn: three corners, each with what its shader hands on.
const int shapeBytes = 240, strokeBytes = 120, glyphBytes = 96, imageBytes = 120;

// Text in the same type and color can be drawn in one call.
quint64 styleOf(qreal pixelSize, bool bold, const QColor &color) {
    return (quint64(qMax(1, qRound(pixelSize))) << 33) | (quint64(bold) << 32) | quint64(color.rgba());
}
qreal sizeOf(quint64 style) { return qreal(style >> 33); }

struct Load {
    qint64 triangles = 0, bytes = 0;
    static Load of(qint64 triangles, int bytesEach) { return {triangles, triangles * bytesEach}; }
    Load &operator+=(const Load &other) {
        triangles += other.triangles;
        bytes += other.bytes;
        return *this;
    }
    Load &operator-=(const Load &other) {
        triangles -= other.triangles;
        bytes -= other.bytes;
        return *this;
    }
};

QSGNode *group(QSGNode *parent) {
    auto *node = new QSGNode;
    node->setFlag(QSGNode::OwnedByParent);
    parent->appendChildNode(node);
    return node;
}

// Puts a node into the scene graph, or takes it out: a node that is out,
// and everything under it, is not batched, uploaded, drawn, or so much as
// looked at until it is put back. True when that changed anything.
bool place(QSGNode *parent, QSGNode *node, bool in) {
    if (in == (node->parent() != nullptr))
        return false;
    if (in)
        parent->appendChildNode(node);
    else
        node->parent()->removeChildNode(node);
    return true;
}

void setMesh(QSGNode *parent, QSGGeometryNode *&node, const Geo::ShapeMesh &mesh, QSGMaterial *material) {
    if (mesh.count() == 0) {
        if (node) {
            parent->removeChildNode(node);
            delete node;
            node = nullptr;
        }
    } else if (!node) {
        node = Nodes::shapes(mesh, material);
        node->setFlag(QSGNode::OwnedByParent);
        parent->appendChildNode(node);
    } else {
        Nodes::setShapes(node, mesh);
    }
}

void setMesh(QSGNode *parent, QSGGeometryNode *&node, const Geo::StrokeMesh &mesh, QSGMaterial *material) {
    if (mesh.indices.isEmpty()) {
        if (node) {
            parent->removeChildNode(node);
            delete node;
            node = nullptr;
        }
    } else if (!node) {
        node = Nodes::stroke(mesh, material);
        node->setFlag(QSGNode::OwnedByParent);
        parent->appendChildNode(node);
    } else {
        Nodes::setStroke(node, mesh);
    }
}

// One kind of thing in a bucket (its shapes, say): a group, and for each
// part of the bucket's square a group that is in it or not. A part out of
// view is left out without the kind being drawn in more than one call. The
// parts are the slot's own to delete: the scene graph only owns what is in it.
struct Slot {
    QSGNode *node = nullptr;
    QSGNode *parts[partCount] = {};

    void make(QSGNode *parent) { node = group(parent); }
    // Made out of the scene; the next look at what is in view puts it in.
    QSGNode *part(int index) {
        if (!parts[index])
            parts[index] = new QSGNode;
        return parts[index];
    }
    void clear() {
        for (QSGNode *&part : parts) {
            if (!part)
                continue;
            if (part->parent())
                part->parent()->removeChildNode(part);
            delete part;
            part = nullptr;
        }
    }
};

// A connector's line and what is at its ends, into the meshes they are
// drawn with.
void addConnector(const Item &connector, QPointF offset, quint32 color, Geo::ShapeMesh &shapes,
                  Geo::StrokeMesh &strokes) {
    const Geo::ConnectorArt art = Geo::connectorArt(connector);
    for (const QVector<QPointF> &line : art.lines)
        Geo::strokePolyline(strokes, line, offset, connector.width / 2, color, Geo::Cap::Butt, Geo::Join::Miter);
    // An end that is not filled is a line just inside its edge, as wide as
    // the connector's.
    for (const Geo::ConnectorArt::Head &head : art.heads)
        shapes.add(head.center + offset, head.half, head.axis, head.outline, 0, head.hollow ? connector.width : 0,
                   head.hollow ? 0 : color, head.hollow ? color : 0);
}

struct Bucket {
    // Of the square as a whole.
    ScenePlace place;
    // In its layer's node when any of it is in view, and its own otherwise.
    QSGTransformNode *node = nullptr;
    QSGNode *layer = nullptr;
    // What it holds, in the order it is drawn. `strokes` is what is drawn
    // with few triangles (lines, frames, arrows, shapes with corners);
    // `diagram` and `detail` what takes many, at each level of detail, of
    // which one is let in at a time. The diagram is shapes with curves and
    // long dashed lines, which a full frame draws more plainly but never
    // leaves out on their own, only with the part of the board they are
    // in; `detail` is the pen's long strokes, which are left out when even
    // their plainest is too much.
    Slot shapes, strokes, diagram[detailLevels], detail[detailLevels], images;
    QSGNode *texts = nullptr;
    struct Style {
        Slot slot;
        qreal size = 0;
        int runs = 0;
    };
    QHash<quint64, Style> styles;
    // What is in each part of the square, what that covers, and what it
    // asks of the GPU: of what is always drawn, of the diagram and the long
    // strokes at each level, and of the type in each style. And whether a
    // piece of the stroke being drawn is in it (see SceneNodes::apply()).
    struct Part {
        QSet<quint32> members;
        QRectF bounds;
        bool stale = false, shown = false, drawing = false;
        Load fixed, diagramLoad[detailLevels], detailLoad[detailLevels];
        QHash<quint64, Load> type;
    };
    Part parts[partCount];
    QRectF bounds;
    bool stale = false;
    int items = 0, renderables = 0, vertices = 0;
    bool rooted = false, shown = false;

    template <class Each> void eachSlot(Each each) {
        each(shapes);
        each(strokes);
        for (Slot &level : diagram)
            each(level);
        for (Slot &level : detail)
            each(level);
        each(images);
        for (Style &style : styles)
            each(style.slot);
    }
};

// The nodes that draw one item, and the bucket they are in.
struct Home {
    Bucket *bucket = nullptr;
    int part = 0;
    QPointF position;
    QRectF bounds;
    QSGGeometryNode *shapes = nullptr, *strokes = nullptr;
    QSGGeometryNode *detail[detailLevels] = {};
    // Whether those are part of the diagram (see Bucket) or a long stroke.
    bool diagram = false;
    QSGImageNode *image = nullptr;
    QRectF imageRect;
    // Shared between the items showing the same picture.
    std::shared_ptr<QSGTexture> texture;
    struct Run {
        QSGTextNode *node;
        quint64 style;
        Load load;
    };
    QVarLengthArray<Run, 2> texts;
    Load fixed, detailLoad[detailLevels];
    int vertices = 0;

    int renderables() const {
        return (shapes ? 1 : 0) + (strokes ? 1 : 0) + (detail[0] ? 1 : 0) + (image ? 1 : 0) + int(texts.size());
    }
};

ScenePlace squareOf(ScenePlace place) {
    place.part = 0;
    return place;
}

} // namespace

// Everything the render thread owns. It is the node updatePaintNode returns,
// so it is deleted with the scene graph, on the render thread.
struct SceneNodes : QSGNode {
    StrokeMaterial strokeMaterial;
    ShapeMaterial shapeMaterial;
    GridMaterial gridMaterial;
    QSGGeometryNode *grid = nullptr;
    QSizeF gridSize;
    QSGTransformNode *content = nullptr; // pans and zooms
    QSGNode *items = nullptr;            // the board: its layers, lowest first
    QSGNode *draft = nullptr;            // what is being drawn right now
    QSGNode *chrome = nullptr;           // selection boxes and handles, in screen pixels
    QSGGeometryNode *draftShapes = nullptr, *draftStrokes = nullptr, *chromeShapes = nullptr;
    // What the draft and the chrome ask of the GPU in every frame: all of
    // it, in view or not.
    Load draftLoad, chromeLoad;
    QMap<int, QSGNode *> layers;
    QHash<ScenePlace, QVector<Bucket *>> squares;
    QSet<Bucket *> buckets;
    QHash<quint32, Home> homes;
    // The pictures on the GPU, by the image each was made from: a picture as
    // it was read, so that one read again, changed on disk, is a picture of
    // its own. Each is held by the items that show it, and by this until
    // none does: an item built again, in a new theme or at a new size, finds
    // the picture it showed still here (see letGoOfPictures()).
    QHash<qint64, std::shared_ptr<QSGTexture>> textures;
    // While a board that started over is built again, the picture each of
    // its items will show, held for it until it is built (see
    // updatePaintNode()).
    QHash<quint32, std::shared_ptr<QSGTexture>> keptFor;

    SceneNodes() {
        auto *quad = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
        quad->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        grid = new QSGGeometryNode;
        grid->setGeometry(quad);
        grid->setFlag(QSGNode::OwnsGeometry);
        grid->setMaterial(&gridMaterial);
        appendChildNode(grid);
        content = new QSGTransformNode;
        appendChildNode(content);
        items = group(content);
        draft = group(content);
        chrome = group(this);
    }

    // Everything goes before the materials it draws with, and before the
    // textures the pictures show: first the board, of which much is out of
    // the scene graph and nobody else's to delete, then the rest.
    ~SceneNodes() override {
        clear();
        // What is being drawn, when a frame went without it.
        if (!draft->parent())
            delete draft;
        while (QSGNode *child = firstChild()) {
            removeChildNode(child);
            delete child;
        }
    }

    QSGNode *layerNode(int layer) {
        if (QSGNode *node = layers.value(layer))
            return node;
        auto *node = new QSGNode;
        node->setFlag(QSGNode::OwnedByParent);
        // In its place among the layers that are there.
        const auto above = layers.upperBound(layer);
        if (above != layers.end())
            items->insertChildNodeBefore(node, *above);
        else
            items->appendChildNode(node);
        layers.insert(layer, node);
        return node;
    }

    // A bucket of that square of the board with room for the item, made
    // when there is none. What is bigger than a square has a part of a
    // bucket to itself, so a bucket of those has room for one in each part.
    Bucket *roomIn(const ScenePlace &square, int vertices) {
        const int most = square.wide ? partCount : bucketItems;
        QVector<Bucket *> &there = squares[square];
        for (auto it = there.crbegin(); it != there.crend(); ++it)
            if ((*it)->items < most && (*it)->vertices + vertices <= bucketVertices)
                return *it;
        auto *bucket = new Bucket;
        bucket->place = square;
        // Out of the scene, until the next look at what is in view.
        bucket->node = new QSGTransformNode;
        bucket->layer = layerNode(square.layer);
        bucket->shapes.make(bucket->node);
        bucket->strokes.make(bucket->node);
        for (Slot &level : bucket->diagram)
            level.make(bucket->node);
        for (Slot &level : bucket->detail)
            level.make(bucket->node);
        bucket->images.make(bucket->node);
        bucket->texts = group(bucket->node);
        there.append(bucket);
        buckets.insert(bucket);
        return bucket;
    }

    void attach(quint32 id, Home &home, const ScenePlace &place) {
        Bucket *bucket = roomIn(squareOf(place), home.vertices);
        // What is bigger than a square takes the first part that is free:
        // roomIn() gave it a bucket with one.
        int at = qBound(0, int(place.part), partCount - 1);
        if (place.wide)
            while (at + 1 < partCount && !bucket->parts[at].members.isEmpty())
                ++at;
        Bucket::Part &part = bucket->parts[at];
        home.bucket = bucket;
        home.part = at;
        if (home.shapes)
            bucket->shapes.part(at)->appendChildNode(home.shapes);
        if (home.strokes)
            bucket->strokes.part(at)->appendChildNode(home.strokes);
        Slot *levels = home.diagram ? bucket->diagram : bucket->detail;
        for (int level = 0; level < detailLevels; ++level)
            if (home.detail[level])
                levels[level].part(at)->appendChildNode(home.detail[level]);
        if (home.image)
            bucket->images.part(at)->appendChildNode(home.image);
        for (const Home::Run &run : home.texts) {
            Bucket::Style &style = bucket->styles[run.style];
            if (!style.slot.node) {
                style.slot.make(bucket->texts);
                style.size = sizeOf(run.style);
            }
            style.slot.part(at)->appendChildNode(run.node);
            ++style.runs;
            part.type[run.style] += run.load;
        }
        part.members.insert(id);
        part.fixed += home.fixed;
        Load *loads = home.diagram ? part.diagramLoad : part.detailLoad;
        for (int level = 0; level < detailLevels; ++level)
            loads[level] += home.detailLoad[level];
        part.bounds |= home.bounds;
        bucket->bounds |= home.bounds;
        ++bucket->items;
        bucket->renderables += home.renderables();
        bucket->vertices += home.vertices;
    }

    // Takes an item's nodes out of the scene. They are still the item's.
    void detach(quint32 id, Home &home) {
        Bucket *bucket = home.bucket;
        if (!bucket)
            return;
        home.bucket = nullptr;
        Bucket::Part &part = bucket->parts[home.part];
        auto unparent = [](QSGNode *node) {
            if (node && node->parent())
                node->parent()->removeChildNode(node);
        };
        unparent(home.shapes);
        unparent(home.strokes);
        for (QSGGeometryNode *level : home.detail)
            unparent(level);
        unparent(home.image);
        for (const Home::Run &run : home.texts) {
            unparent(run.node);
            const auto load = part.type.find(run.style);
            if (load != part.type.end()) {
                *load -= run.load;
                if (load->triangles <= 0)
                    part.type.erase(load);
            }
            // The last of its style in the bucket takes the style with it.
            const auto style = bucket->styles.find(run.style);
            if (style != bucket->styles.end() && --style->runs <= 0) {
                style->slot.clear();
                bucket->texts->removeChildNode(style->slot.node);
                delete style->slot.node;
                bucket->styles.erase(style);
            }
        }
        part.members.remove(id);
        part.fixed -= home.fixed;
        Load *loads = home.diagram ? part.diagramLoad : part.detailLoad;
        for (int level = 0; level < detailLevels; ++level)
            loads[level] -= home.detailLoad[level];
        part.stale = bucket->stale = true;
        --bucket->items;
        bucket->renderables -= home.renderables();
        bucket->vertices -= home.vertices;
        if (bucket->items > 0)
            return;
        // The last thing in it: the bucket goes too.
        QVector<Bucket *> &there = squares[bucket->place];
        there.removeOne(bucket);
        if (there.isEmpty())
            squares.remove(bucket->place);
        buckets.remove(bucket);
        bucket->eachSlot([](Slot &slot) { slot.clear(); });
        unparent(bucket->node);
        delete bucket->node;
        delete bucket;
    }

    void destroy(quint32 id, Home &home) {
        detach(id, home);
        delete home.shapes;
        delete home.strokes;
        for (QSGGeometryNode *level : home.detail)
            delete level;
        delete home.image;
        for (const Home::Run &run : home.texts)
            delete run.node;
        home = Home();
    }

    void removeItem(quint32 id) {
        // Gone before it was built again, it needs nothing kept for it.
        keptFor.remove(id);
        const auto home = homes.find(id);
        if (home == homes.end())
            return;
        destroy(id, *home);
        homes.erase(home);
    }

    void clear() {
        for (auto home = homes.begin(); home != homes.end(); ++home)
            destroy(home.key(), *home);
        homes.clear();
        keptFor.clear();
        for (QSGNode *layer : std::as_const(layers)) {
            items->removeChildNode(layer);
            delete layer;
        }
        layers.clear();
        squares.clear();
        buckets.clear();
    }

    // Lets go of the pictures held only here, which nothing shows any more or
    // is waiting to show, and says how many the GPU is left holding.
    int letGoOfPictures() {
        textures.removeIf([](QHash<qint64, std::shared_ptr<QSGTexture>>::iterator it) { return it->use_count() <= 1; });
        return int(textures.size());
    }

    void move(quint32 id, QPointF position) {
        const auto found = homes.find(id);
        if (found == homes.end())
            return;
        Home &home = *found;
        const QPointF delta = position - home.position;
        if (delta.isNull())
            return;
        home.position = position;
        home.bounds.translate(delta);
        if (home.shapes)
            Nodes::shift(home.shapes, delta);
        if (home.strokes)
            Nodes::shift(home.strokes, delta);
        for (QSGGeometryNode *level : home.detail)
            if (level)
                Nodes::shift(level, delta);
        if (home.image) {
            home.imageRect.translate(delta);
            home.image->setRect(home.imageRect);
        }
        QMatrix4x4 matrix;
        matrix.translate(float(position.x()), float(position.y()));
        for (const Home::Run &run : home.texts) {
            run.node->setMatrix(matrix);
            run.node->markDirty(QSGNode::DirtyMatrix);
        }
        if (Bucket *bucket = home.bucket) {
            // Never smaller than what is in it; made exact again at the
            // next look at what is in view.
            Bucket::Part &part = bucket->parts[home.part];
            part.bounds |= home.bounds;
            bucket->bounds |= home.bounds;
            part.stale = bucket->stale = true;
        }
    }

    void rehome(quint32 id, const ScenePlace &place) {
        const auto home = homes.find(id);
        if (home == homes.end() || !home->bucket)
            return;
        // What is bigger than a square is in whichever part was free.
        if (home->bucket->place == squareOf(place) && (place.wide || home->part == place.part))
            return;
        detach(id, *home);
        attach(id, *home, place);
    }

    void measure(Bucket *bucket) {
        bucket->bounds = QRectF();
        for (Bucket::Part &part : bucket->parts) {
            if (part.stale) {
                part.bounds = QRectF();
                for (quint32 id : std::as_const(part.members)) {
                    const auto home = homes.constFind(id);
                    if (home != homes.constEnd())
                        part.bounds |= home->bounds;
                }
                part.stale = false;
            }
            bucket->bounds |= part.bounds;
        }
        bucket->stale = false;
    }

    // Decides what of the board this frame draws: what is near `view` (in
    // units), in as much detail as shows at `scale` screen pixels to the
    // unit, and no more than `budget` in all. `pieces` are what a stroke
    // still being drawn has put on the board so far (see
    // CanvasView::carryOn()). `waiting` is set when there is something left
    // to do that has to wait for the next frame.
    SceneLoad apply(const QRectF &view, qreal scale, qint64 budget, bool everything, const QVector<quint32> &pieces,
                    bool *waiting) {
        // A part of the board comes in when it nears the view, and stays
        // until it is clear of it: panning back and forth over its edge
        // does not keep putting it in and taking it out.
        const qreal reach = qMax(view.width(), view.height());
        const QRectF nearby = view.adjusted(-reach * 0.05, -reach * 0.05, reach * 0.05, reach * 0.05);
        const QRectF around = view.adjusted(-reach * 0.2, -reach * 0.2, reach * 0.2, reach * 0.2);

        SceneLoad load;
        load.buckets = int(buckets.size());
        Load fixed, diagram[detailLevels], detail[detailLevels];
        QMap<qreal, Load> type; // by size
        for (Bucket *bucket : std::as_const(buckets)) {
            if (bucket->stale)
                measure(bucket);
            const bool near = everything || bucket->bounds.intersects(bucket->shown ? around : nearby);
            bucket->shown = false;
            for (Bucket::Part &part : bucket->parts) {
                part.shown = near && !part.members.isEmpty() &&
                             (everything || part.bounds.intersects(part.shown ? around : nearby));
                part.drawing = false;
                if (!part.shown)
                    continue;
                bucket->shown = true;
                load.items += int(part.members.size());
                fixed += part.fixed;
                for (int level = 0; level < detailLevels; ++level) {
                    diagram[level] += part.diagramLoad[level];
                    detail[level] += part.detailLoad[level];
                }
                for (auto it = part.type.constBegin(); it != part.type.constEnd(); ++it)
                    type[sizeOf(it.key())] += *it;
                // The part nearest the middle of the view is drawn whatever
                // it holds (see below), and this one could be that: all that
                // is always drawn of it, and its diagram at its plainest.
                load.floor = qMax(load.floor, part.fixed.bytes + part.diagramLoad[detailLevels - 1].bytes);
            }
            if (bucket->shown)
                ++load.shown;
        }
        // The long strokes of the parts in view that the stroke being drawn
        // has pieces in, its pieces and anything there with them, are kept
        // apart from the rest: they are not hidden with them (see below).
        // Those parts, in the order the pieces went down.
        QVector<Bucket::Part *> drawingParts;
        Load stroke[detailLevels];
        for (quint32 id : pieces) {
            const auto home = homes.constFind(id);
            if (home == homes.constEnd() || !home->bucket)
                continue;
            Bucket::Part &part = home->bucket->parts[home->part];
            if (!part.shown || part.drawing)
                continue;
            part.drawing = true;
            drawingParts.append(&part);
            for (int level = 0; level < detailLevels; ++level) {
                detail[level] -= part.detailLoad[level];
                stroke[level] += part.detailLoad[level];
            }
        }

        // What shows at this size on screen.
        int shows = 0;
        while (shows + 1 < detailLevels && scale < coarseBelow[shows + 1])
            ++shows;
        // The level the pen's strokes are drawn at, and the diagram's, which
        // is never plainer.
        int level = shows, diagramLevel = shows;
        qreal smallest = legibleText / qMax(scale, 1e-6);
        // The type drawn: the sizes from `drawn` up, at first every size big
        // enough to read.
        const auto legible = std::as_const(type).lowerBound(smallest);
        auto drawn = legible;
        bool hidden = false;
        auto total = [&] {
            Load sum = fixed;
            sum += diagram[diagramLevel];
            sum += stroke[level];
            if (!hidden)
                sum += detail[level];
            for (auto it = drawn; it != type.cend(); ++it)
                sum += *it;
            return sum;
        };

        // And what a frame can hold. Over that, type goes first, the
        // smallest first, which is the least read; then the curves of the
        // pen's strokes and of the diagram, a level at a time; then the pen's
        // strokes themselves, but for the stroke being drawn, which whoever
        // is drawing it sees whole for as long as the rest can make room. The
        // diagram is drawn more plainly, but never left out on its own:
        // without it, what is written on its shapes and the heads of its
        // lines would be left with nothing around them.
        Load sum = total();
        if (sum.bytes > budget) {
            load.reduced = true;
            while (sum.bytes > budget && drawn != type.cend()) {
                ++drawn;
                sum = total();
            }
            auto giveWay = [&] {
                while (sum.bytes > budget && level + 1 < detailLevels) {
                    diagramLevel = ++level;
                    sum = total();
                }
                if (sum.bytes > budget && detail[level].triangles > 0) {
                    hidden = true;
                    sum = total();
                }
                // A level of the strokes, or all of them, is often far more
                // than the frame was still over by once the type had gone.
                // When all of them went, the room that leaves brings back the
                // curves of the stroke being drawn first, which did not go
                // with them, and the diagram's with its, as the diagram is
                // never plainer...
                while (hidden && level > shows && stroke[level].triangles > 0 &&
                       sum.bytes - stroke[level].bytes - diagram[level].bytes + stroke[level - 1].bytes +
                               diagram[level - 1].bytes <=
                           budget) {
                    diagramLevel = --level;
                    sum = total();
                }
                // ...then the curves of the diagram, as they went after the
                // type: a level at a time, as far as it goes...
                while (diagramLevel > shows &&
                       sum.bytes - diagram[diagramLevel].bytes + diagram[diagramLevel - 1].bytes <= budget) {
                    --diagramLevel;
                    sum = total();
                }
            };
            giveWay();
            // ...and then the type, the largest first, as far as what is left
            // of the room goes. A level of the diagram that no longer fits is
            // often far more than the frame was over by, too, so the type can
            // come back when the diagram gives way, as it can when the strokes
            // do.
            auto giveBack = [&] {
                auto next = drawn;
                while (next != legible && sum.bytes + std::prev(next)->bytes <= budget) {
                    --next;
                    // A size all of which went with the parts it was in (see
                    // below) is passed over, and not counted as drawn: there
                    // is none of it left to draw.
                    if (next->bytes > 0) {
                        drawn = next;
                        sum = total();
                    }
                }
            };
            giveBack();
            // What is left is paper, shapes, lines and pictures, and a window
            // only holds too much of those when they are piled thousands deep.
            // Then the parts furthest from the middle of the view go, so
            // that whatever else, a frame is never more than its budget.
            if (sum.bytes > budget) {
                struct Far {
                    qreal distance;
                    Bucket *bucket;
                    Bucket::Part *part;
                };
                QVector<Far> parts;
                for (Bucket *bucket : std::as_const(buckets))
                    for (Bucket::Part &part : bucket->parts)
                        if (part.shown)
                            parts.append({QLineF(part.bounds.center(), view.center()).length(), bucket, &part});
                // The furthest first. Parts as far off as each other (those of
                // the buckets that share a square can be) go in the same order
                // every frame, so that a bucket made for something new, say a
                // piece of a stroke being drawn, does not change which of them
                // go and which come back.
                std::sort(parts.begin(), parts.end(), [](const Far &a, const Far &b) {
                    return a.distance != b.distance ? a.distance > b.distance
                                                    : std::less<const Bucket::Part *>()(a.part, b.part);
                });
                // All but the nearest, which is drawn whatever it holds (of
                // the board but for the stroke being drawn, when there is more
                // to it), and those the stroke being drawn is in, which stay
                // with it as long as they and the nearest fit at their
                // plainest. Past that (a scribble long enough, over a part
                // piled high enough), its oldest pieces give way first, as any
                // long stroke does, and the parts they are in can go too.
                const auto rest = std::find_if(parts.crbegin(), parts.crend(),
                                               [](const Far &far) { return !far.part->drawing; });
                const Bucket::Part *nearest = rest != parts.crend() ? rest->part : parts.constLast().part;
                auto keeps = [nearest](const Bucket::Part &part) {
                    Load kept;
                    if (part.drawing || &part == nearest) {
                        kept += part.fixed;
                        kept += part.diagramLoad[detailLevels - 1];
                    }
                    if (part.drawing)
                        kept += part.detailLoad[detailLevels - 1];
                    return kept;
                };
                Load kept;
                for (const Far &far : std::as_const(parts))
                    kept += keeps(*far.part);
                for (Bucket::Part *part : std::as_const(drawingParts)) {
                    if (kept.bytes <= budget)
                        break;
                    kept -= keeps(*part);
                    part->drawing = false;
                    kept += keeps(*part);
                    for (int at = 0; at < detailLevels; ++at) {
                        stroke[at] -= part->detailLoad[at];
                        detail[at] += part->detailLoad[at];
                    }
                    sum = total();
                    giveWay();
                }
                // A part that goes takes its diagram, its strokes and its type
                // with it. One with nothing in it that is still drawn by now
                // (only type, or long strokes, which have gone already) would
                // free nothing by going: it stays, and what follows decides
                // what of it is drawn.
                for (qsizetype i = 0; i < parts.size() && sum.bytes > budget; ++i) {
                    Bucket::Part &part = *parts[i].part;
                    if (part.drawing || &part == nearest ||
                        part.fixed.bytes + part.diagramLoad[diagramLevel].bytes == 0)
                        continue;
                    part.shown = false;
                    load.items -= int(part.members.size());
                    fixed -= part.fixed;
                    for (int at = 0; at < detailLevels; ++at) {
                        diagram[at] -= part.diagramLoad[at];
                        detail[at] -= part.detailLoad[at];
                    }
                    for (auto it = part.type.constBegin(); it != part.type.constEnd(); ++it)
                        type[sizeOf(it.key())] -= *it;
                    sum = total();
                }
                for (Bucket *bucket : std::as_const(buckets)) {
                    if (!bucket->shown)
                        continue;
                    bucket->shown = std::any_of(std::begin(bucket->parts), std::end(bucket->parts),
                                                [](const Bucket::Part &part) { return part.shown; });
                    if (!bucket->shown)
                        --load.shown;
                }
                // The last part to go can be far more than the frame was still
                // over by, too. What is left is drawn as it would be were it
                // all that was in view: its strokes and its diagram come back
                // first, in as much detail as there is room for, and then its
                // type.
                level = diagramLevel = shows;
                hidden = false;
                sum = total();
                giveWay();
                giveBack();
            }
        }
        // Type is left out below the smallest size drawn: from halfway past
        // the largest that is not, as sizes are whole units.
        if (drawn != legible)
            smallest = std::prev(drawn).key() + 0.5;

        const bool attached = parent() != nullptr;
        for (Bucket *bucket : std::as_const(buckets)) {
            if (!bucket->shown) {
                place(bucket->layer, bucket->node, false);
                bucket->rooted = false;
                continue;
            }
            // What is inside first, so that a bucket coming in comes in
            // with only what it is going to show.
            bool changed = false;
            // A slot is shown in the parts that are when `whole`, and when
            // `drawing`, in those of them the stroke being drawn is in.
            auto show = [&](Slot &slot, bool whole, bool drawing = false) {
                for (int i = 0; i < partCount; ++i)
                    if (slot.parts[i])
                        changed |= place(slot.node, slot.parts[i],
                                         (whole || (drawing && bucket->parts[i].drawing)) && bucket->parts[i].shown);
            };
            show(bucket->shapes, true);
            show(bucket->strokes, true);
            show(bucket->images, true);
            for (int i = 0; i < detailLevels; ++i) {
                show(bucket->diagram[i], i == diagramLevel);
                show(bucket->detail[i], !hidden && i == level, i == level);
            }
            for (Bucket::Style &style : bucket->styles)
                show(style.slot, style.size >= smallest);
            changed |= place(bucket->layer, bucket->node, true);
            // A bucket with enough in it is made a batch of its own, so
            // that from then on a change inside it is its own business.
            // The scene graph does that for a transform node that is
            // moved, when enough is under it; moving this one by nothing
            // is enough, and is said again whenever what is under it has
            // changed. Nothing said here reaches the scene graph until
            // this node is in its tree, which it is not the first time
            // through.
            if (changed)
                bucket->rooted = false;
            if (!bucket->rooted && bucket->renderables > rootThreshold) {
                if (attached) {
                    bucket->node->markDirty(QSGNode::DirtyMatrix);
                    bucket->rooted = true;
                } else {
                    *waiting = true;
                }
            }
        }

        load.triangles = sum.triangles;
        load.bytes = sum.bytes;
        load.strokeDetail = level;
        load.strokesHidden = hidden;
        load.diagramDetail = diagramLevel;
        load.smallestText = smallest;
        return load;
    }
};

qreal CanvasView::devicePixels() const { return window() ? window()->effectiveDevicePixelRatio() : 1.0; }

QMatrix4x4 CanvasView::viewMatrix() const {
    // Panned by whole screen pixels, so lines that are a pixel wide at 100%
    // stay exactly a pixel wide.
    const qreal pixels = devicePixels();
    QMatrix4x4 matrix;
    matrix.translate(float(std::round(-m_origin.x() * m_zoom * pixels) / pixels),
                     float(std::round(-m_origin.y() * m_zoom * pixels) / pixels));
    matrix.scale(float(m_zoom));
    return matrix;
}

QColor CanvasView::colorFor(const QString &name, const QColor &neutral) const {
    return name.isEmpty() ? neutral : m_palette.swatch(name, neutral);
}

void CanvasView::requestFrame() {
    // What measuring changes is seen to by the polish that is measuring,
    // which asks for the next frame itself when anything is left to make
    // ready. Asked for again from inside it, that polish would be run again
    // before the frame is drawn, and again for as long as anything grew: the
    // whole board made ready in one frame.
    if (!m_measuring)
        polish();
    update();
}

void CanvasView::markDirty(quint32 id) {
    const qsizetype before = m_dirty.size();
    m_dirty.insert(id);
    m_moved.remove(id);
    if (m_dirty.size() != before)
        emit pendingItemsChanged();
    requestFrame();
}

void CanvasView::markAllDirty() {
    m_dirty.clear();
    m_waiting.clear();
    m_moved.clear();
    m_unmeasured.clear();
    m_unfitted.clear();
    m_reaching.clear();
    if (m_board)
        for (quint32 id : m_board->order()) {
            m_dirty.insert(id);
            markUnmeasured(id);
        }
    m_chromeDirty = true;
    m_settledPending = true;
    emit pendingItemsChanged();
    requestFrame();
}

// --- Preparing ------------------------------------------------------------------

std::unique_ptr<CanvasView::Prepared> CanvasView::prepare(const Item &item) {
    auto prepared = std::make_unique<Prepared>();
    Prepared &p = *prepared;
    p.position = item.rect.topLeft();
    p.bounds = m_board->bounds(item.id);
    const QRectF box(QPointF(0, 0), item.rect.size());
    // Corners follow the desktop's, up to a point: a note is not a pill.
    const qreal radius = qMin(m_radius, 12);
    const QColor dim = Theme::mix(m_palette.background, m_palette.foreground, .62);

    // The item being typed in shows its text in the editor instead.
    auto write = [&p](const QString &text, const TextStyle &style) {
        p.text = std::make_unique<TextBlock>(text, style);
        p.textOffset = p.text->origin();
        p.textColor = style.color;
        p.titleColor = style.titleColor;
        p.titled = style.titled;
        p.titleStyle = styleOf(style.titleSize, true, style.titleColor);
        p.bodyStyle = styleOf(style.font.pixelSize(), style.font.bold(), style.color);
    };
    if (item.hasText() && !item.text.isEmpty() && item.id != m_editing)
        write(item.text, TextMetrics::styleFor(item, m_palette));

    switch (item.kind) {
    case Item::Kind::Note:
        p.shapes.add(box, Geo::Outline::Rect, radius, 0, packColor(m_palette.note(item.color)), 0);
        break;
    case Item::Kind::Shape: {
        const quint32 fill = packColor(m_palette.raised), border = packColor(colorFor(item.color, dim));
        if (Geo::isQuad(item.shape)) {
            p.shapes.add(box, Geo::outlineOf(item.shape), Geo::cornerRadius(item.shape, box.size(), radius), 1, fill,
                         border);
            break;
        }
        // Any other outline is drawn as it is described: filled, with a
        // border that is the unit just inside its box, as the others' is.
        const QSizeF inside(qMax(1.0, box.width() - 1), qMax(1.0, box.height() - 1));
        const Geo::Figure figure = Geo::figure(item.shape, inside);
        Geo::strokeFigure(p.strokes, figure, QPointF(0.5, 0.5), fill, border);
        // One with curves is many edges, and like a long stroke is cut
        // again with fewer for when it is small on screen. Unlike a stroke,
        // it is part of the diagram (see Bucket): a full frame draws it more
        // plainly, and never leaves it out on its own.
        if (p.strokes.indices.size() / 3 >= heavyStroke) {
            p.diagram = true;
            for (int level = 1; level < detailLevels; ++level)
                Geo::strokeFigure(p.coarse[level - 1], Geo::simplified(figure, coarseTolerance[level]),
                                  QPointF(0.5, 0.5), fill, border);
        }
        break;
    }
    case Item::Kind::Text:
        break;
    case Item::Kind::Stroke: {
        const QVector<QPointF> curve = Geo::smooth(item.points, Geo::strokeTolerance);
        const quint32 color = packColor(colorFor(item.color, m_palette.bright));
        Geo::strokePolyline(p.strokes, curve, {}, item.width / 2, color);
        // A long one is cut again with fewer points and plain corners, for
        // when it is too small on screen for the difference to show.
        if (p.strokes.indices.size() / 3 >= heavyStroke)
            for (int level = 1; level < detailLevels; ++level)
                Geo::strokePolyline(p.coarse[level - 1], Geo::simplify(curve, coarseTolerance[level]), {},
                                    item.width / 2, color, Geo::Cap::Butt, Geo::Join::Bevel);
        break;
    }
    case Item::Kind::Connector: {
        const quint32 color = packColor(colorFor(item.color, m_palette.foreground));
        addConnector(item, -p.position, color, p.shapes, p.strokes);
        // A long dashed or dotted one is dashes by the hundred. They are
        // kept until they are too small on screen to tell apart, and from
        // there it is drawn as the one line it then looks like. It is part
        // of the diagram too.
        if (item.dash != Item::Dash::Solid && p.strokes.indices.size() / 3 >= heavyStroke) {
            p.diagram = true;
            p.coarse[0] = p.strokes;
            Item plain = item;
            plain.dash = Item::Dash::Solid;
            Geo::ShapeMesh ends; // drawn already
            addConnector(plain, -p.position, color, ends, p.coarse[1]);
        }
        if (p.text)
            p.textOffset = TextMetrics::connectorLabelCorner(item, p.text->size()) - p.position;
        break;
    }
    case Item::Kind::Image:
        p.imageRect = box;
        p.image = m_images->image(item.asset);
        if (p.image.isNull()) {
            // Still being read, or missing: say which.
            const bool missing = m_images->failed(item.asset);
            p.shapes.add(box, Geo::Outline::Rect, 0, 1, packColor(m_palette.raised),
                         packColor(missing ? m_palette.swatch("red", m_palette.foreground) : m_palette.muted));
            if (missing) {
                TextStyle style;
                style.font = TextMetrics::font(TextMetrics::small);
                style.color = style.titleColor = m_palette.foreground;
                style.alignment = Qt::AlignHCenter | Qt::AlignVCenter;
                style.box = QRectF(8, 0, qMax(1.0, box.width() - 16), box.height());
                write(tr("The picture %1 is missing").arg(item.asset), style);
            }
        }
        break;
    case Item::Kind::Frame: {
        const qreal title = p.text ? p.text->size().width() : 0;
        const qreal gapFrom = TextMetrics::frameTitleInset - TextMetrics::frameTitleGap;
        const qreal gapTo = TextMetrics::frameTitleInset + title + TextMetrics::frameTitleGap;
        Geo::strokePolyline(p.strokes, Geo::frameEdge(box.size(), gapFrom, title > 0 ? gapTo : gapFrom), {}, 0.5,
                            packColor(colorFor(item.color, m_palette.muted)), Geo::Cap::Butt, Geo::Join::Miter);
        break;
    }
    }
    return prepared;
}

QRectF CanvasView::itemBoxOnScreen(const Item &item) const {
    // A little clear of the item, on whole screen pixels.
    const qreal pixels = devicePixels();
    const qreal reach = 3 + (item.kind == Item::Kind::Stroke ? item.width / 2 * m_zoom : 0);
    const QPointF corner = toItem(item.rect.topLeft()), opposite = toItem(item.rect.bottomRight());
    auto snap = [pixels](qreal value) { return std::round(value * pixels) / pixels; };
    return QRectF(QPointF(snap(corner.x() - reach), snap(corner.y() - reach)),
                  QPointF(snap(opposite.x() + reach), snap(opposite.y() + reach)));
}

void CanvasView::prepareDecorations(Geo::ShapeMesh &shapes) {
    shapes.clear();
    if (!m_board)
        return;
    const quint32 accent = packColor(m_palette.accent), paper = packColor(m_palette.background);

    if (m_drag == Drag::Marquee && m_dragStarted)
        shapes.add(m_marquee, Geo::Outline::Rect, 0, 1, packColor(m_palette.accent, 0.12), accent);

    // What a connector being drawn would attach to.
    if ((m_drag == Drag::Connect || m_drag == Drag::End) && m_connectTarget)
        if (const Item *target = m_board->item(m_connectTarget))
            shapes.add(itemBoxOnScreen(*target), Geo::Outline::Rect, 0, 2, 0, accent);

    if (selection().size() > outlineLimit) {
        shapes.add(selectionBox(), Geo::Outline::Rect, 0, 1, 0, accent);
        return;
    }
    for (quint32 id : selection()) {
        const Item *item = m_board->item(id);
        if (item && item->kind != Item::Kind::Connector)
            shapes.add(itemBoxOnScreen(*item), Geo::Outline::Rect, 0, 1, 0, accent);
    }
    // Handles are for a thing at rest.
    const bool moving = m_drag == Drag::Move && m_dragStarted;
    if (moving || editing())
        return;
    const Item *sole = soleSelection();
    const auto spots = handleSpots();
    for (const HandleSpot &spot : spots) {
        const QRectF handle(spot.at.x() - 4, spot.at.y() - 4, 8, 8);
        const bool round = sole && sole->kind == Item::Kind::Connector;
        shapes.add(handle, round ? Geo::Outline::Ellipse : Geo::Outline::Rect, 0, 1, paper, accent);
    }
}

void CanvasView::prepareDraft(Geo::ShapeMesh &shapes, Geo::StrokeMesh &strokes) {
    shapes.clear();
    strokes.clear();
    if (!m_board || !m_dragStarted)
        return;
    const QColor dim = Theme::mix(m_palette.background, m_palette.foreground, .62);
    if (m_drag == Drag::Stroke) {
        // Smoothed as it will be once it is on the board, so letting go
        // changes nothing that shows; and cut as it was drawn, a point at a
        // time (see Geo::Pen).
        strokes = m_pen.mesh();
    } else if (m_drag == Drag::Box) {
        const QRectF box = m_draftBox.normalized();
        if (m_tool == Tool::Frame) {
            Geo::strokePolyline(strokes, Geo::frameEdge(box.size(), 0, 0), box.topLeft(), 0.5, packColor(m_palette.muted),
                                Geo::Cap::Butt, Geo::Join::Miter);
        } else if (Geo::isQuad(m_shape)) {
            shapes.add(box, Geo::outlineOf(m_shape), Geo::cornerRadius(m_shape, box.size(), qMin(m_radius, 12)), 1,
                       packColor(m_palette.raised), packColor(dim));
        } else if (box.width() > 1 && box.height() > 1) {
            Geo::strokeFigure(strokes, Geo::figure(m_shape, box.size() - QSizeF(1, 1)), box.topLeft() + QPointF(0.5, 0.5),
                              packColor(m_palette.raised), packColor(dim));
        }
    } else if (m_drag == Drag::Connect) {
        // As it will be once it is let go of.
        Item preview = connectorInHand();
        preview.p1 = m_connectStart;
        preview.p2 = m_connectEnd;
        preview.sideways = m_connectSideways;
        addConnector(preview, {}, packColor(m_palette.foreground), shapes, strokes);
    }
}

// The stroke being drawn is drawn whole, wherever it goes, so it is given a
// quarter of what a frame may hold and the board keeps the rest. Or less,
// where the board could not give way that far: it never leaves out what is
// always drawn of the part of it nearest the middle of the view, which a
// part piled high enough can make more than the rest of a frame (see
// SceneLoad::floor, as of the last frame). Never less than a stroke the
// board can leave out, though (see heavyStroke), or what it puts down would
// only add to that.
qsizetype CanvasView::strokeLimit() const {
    const qint64 room = m_loadBudget - m_load.floor - m_load.editor - m_load.chrome;
    return qsizetype(qMin(m_loadBudget / 4, qMax(room, qint64(heavyStroke) * strokeBytes)) / strokeBytes);
}

// Prepares every item on screen that is waiting to be, and then, while
// the canvas is polished and there is time left in the frame, some of those
// that are not. Text is measured only while it is polished (`measure`).
void CanvasView::prepareVisible(bool measure) {
    if (!m_board || m_dirty.isEmpty())
        return;
    m_polishTimer.start();
    auto ready = [this, measure](quint32 id) {
        if (measure) {
            m_unmeasured.remove(id);
            m_unfitted.remove(id);
            m_reaching.remove(id);
        }
        const Item *item = m_board->item(id);
        if (!item)
            return;
        auto prepared = prepare(*item);
        if (measure && id != m_editing) {
            // Text is as big as its type makes it, and a note or a shape as
            // big as what is written on it needs, whatever the file said: as
            // they are made when they are typed in. What is being typed in is
            // made so as it is typed.
            Item fitted = *item;
            if (TextMetrics::fit(fitted, prepared->text.get())) {
                // Telling the board places it again by its new box, and
                // carries its connectors to its new edges, to be prepared
                // again. Text need not be: its lines are set by where it
                // wraps, not by its box, which only decides what it covers.
                // A note's paper and a shape's outline are drawn again, at
                // their new size.
                m_measuring = id;
                m_board->setMeasuredRect(id, fitted.rect);
                m_measuring = 0;
                // Whatever heard of that could have taken it off the board.
                item = m_board->item(id);
                if (!item)
                    return;
                if (item->kind == Item::Kind::Text)
                    prepared->bounds = m_board->bounds(id);
                else
                    prepared = prepare(*item);
            }
        }
        m_ready.insert(id, std::shared_ptr<Prepared>(std::move(prepared)));
    };

    // What is on screen has to be right in this frame: what is in the window,
    // and as near it as the scene lets things in (see SceneNodes::apply()).
    // Only that, however much of the board it leaves for the frames after.
    // Text not measured yet is on the board at a guess of its size, a line to
    // each paragraph, and a note or a shape not yet made big enough for what
    // is written on it is at the size its file gave it. Either could be on
    // screen far past where that ends: whatever could be is measured first,
    // found by all it could cover (see placeUnmeasured()). So is anything at
    // either end of a connector on screen, however far off it is, as the
    // connector is drawn from where it ends. Then what is on screen, as it is
    // now, is prepared.
    const QRectF area = visibleArea();
    const qreal margin = qMax(area.width(), area.height()) * 0.05;
    const QRectF nearby = area.adjusted(-margin, -margin, margin, margin);
    QVector<quint32> near;
    if (measure && m_reaching.count() > 0) {
        auto measureFirst = [&](quint32 id) {
            if (m_unmeasured.contains(id) || m_unfitted.contains(id)) {
                m_dirty.remove(id);
                ready(id);
            }
        };
        QVector<quint32> reaching;
        m_reaching.query(nearby, reaching);
        for (quint32 id : std::as_const(reaching))
            measureFirst(id);
        m_board->itemsIn(nearby, near);
        for (quint32 id : std::as_const(near)) {
            const Item *item = m_board->item(id);
            if (!item || item->kind != Item::Kind::Connector)
                continue;
            const quint32 from = item->from, to = item->to;
            measureFirst(from);
            measureFirst(to);
        }
    }
    m_board->itemsIn(nearby, near);
    for (quint32 id : std::as_const(near))
        if (m_dirty.remove(id))
            ready(id);
    // While the scene graph synchronizes the board is not told anything, so
    // that is all: what is on screen a polish has measured already, and text
    // off screen may never have been, which waits for the next polish.
    if (!measure)
        return;
    // The rest a little each frame, so a board of any size opens at once
    // and is all there before anyone pans to it. Text not measured yet goes
    // first: in type wider than its reach allows for, it could be on screen
    // already. It is taken in turn from a line, made again whenever that runs
    // out: the first of what is left of a set is found only by looking past
    // every place emptied before it, which on a big board takes longer than
    // preparing it.
    while (!m_dirty.isEmpty() && m_polishTimer.nsecsElapsed() < backgroundBudget) {
        if (m_waiting.isEmpty()) {
            m_waiting = m_unmeasured.keys();
            for (quint32 id : std::as_const(m_dirty))
                if (!m_unmeasured.contains(id))
                    m_waiting.append(id);
        }
        // What was prepared meanwhile, or taken off the board, is passed over.
        const quint32 id = m_waiting.takeFirst();
        if (m_dirty.remove(id) || m_unmeasured.contains(id))
            ready(id);
    }
    // With all of it prepared, any text still to measure, or note or shape
    // still to fit, was taken off the board first.
    if (m_dirty.isEmpty()) {
        m_waiting.clear();
        m_unmeasured.clear();
        m_unfitted.clear();
        m_reaching.clear();
    }
}

// --- Layers -----------------------------------------------------------------------

ScenePlace CanvasView::placeFor(quint32 id, int layer) const {
    ScenePlace place;
    place.layer = layer;
    const QRectF bounds = m_board->bounds(id);
    // By its middle. However far out a board goes, the squares can be counted.
    const QPointF middle = bounds.center();
    place.column = qint32(qBound(-1e9, std::floor(middle.x() / cellSize), 1e9));
    place.row = qint32(qBound(-1e9, std::floor(middle.y() / cellSize), 1e9));
    // Something bigger than a square is kept apart from smaller things, and
    // has a part to itself (see SceneNodes::attach). In a part with anything
    // else it would bring all of that into view wherever any of it came into
    // view, and come into view itself wherever any of the rest did: long
    // texts side by side would all be drawn while one was read, and a row
    // of big frames while one was looked at. On its own it comes into view
    // only where it is, and it still shares a bucket with things as big near
    // it: a view of many of them is a few calls, not a call or two for each.
    if (bounds.width() > cellSize || bounds.height() > cellSize) {
        place.wide = true;
        return place;
    }
    const qreal partSize = cellSize / partsAcross;
    const int across = qBound(0, int((middle.x() - place.column * cellSize) / partSize), partsAcross - 1);
    const int down = qBound(0, int((middle.y() - place.row * cellSize) / partSize), partsAcross - 1);
    place.part = quint8(down * partsAcross + across);
    return place;
}

void CanvasView::setLayer(quint32 id, int layer) {
    const ScenePlace place = placeFor(id, layer);
    const auto found = m_place.find(id);
    if (found != m_place.end() && *found == place)
        return;
    m_place.insert(id, place);
    if (m_built.contains(id))
        m_rehome.insert(id);
}

// Every item's layer, from nothing: back to front, each goes one above the
// highest thing under it that it overlaps.
void CanvasView::relayerAll() {
    m_layersStale = false;
    ++m_layerings;
    if (!m_board) {
        m_place.clear();
        return;
    }
    QHash<quint32, int> layers;
    layers.reserve(m_board->count());
    QVector<quint32> near;
    for (quint32 id : m_board->order()) {
        const QRectF bounds = m_board->bounds(id);
        m_board->itemsIn(bounds, near);
        int layer = 0;
        for (quint32 other : std::as_const(near)) {
            // Only what is under it has been given a layer so far.
            const auto under = layers.constFind(other);
            if (under != layers.constEnd() && *under >= layer && m_board->bounds(other).intersects(bounds))
                layer = *under + 1;
        }
        layers.insert(id, layer);
    }
    QHash<quint32, ScenePlace> places;
    places.reserve(layers.size());
    for (auto it = layers.constBegin(); it != layers.constEnd(); ++it) {
        const ScenePlace place = placeFor(it.key(), it.value());
        const auto was = m_place.constFind(it.key());
        if (m_built.contains(it.key()) && (was == m_place.constEnd() || *was != place))
            m_rehome.insert(it.key());
        places.insert(it.key(), place);
    }
    m_place = places;
}

// Text not measured yet, and a note or a shape not yet made big enough for
// what is written on it, is placed by all it could cover from where it is
// now: so whatever of it could be on screen is found by looking near the
// screen, not at all of it, and is measured before it is drawn.
void CanvasView::placeUnmeasured(quint32 id) {
    QRectF reach;
    if (m_unmeasured.contains(id))
        reach = m_unmeasured.value(id);
    else if (m_unfitted.contains(id))
        reach = m_unfitted.value(id);
    else
        return;
    if (m_board && m_board->item(id))
        m_reaching.update(id, reach.translated(m_board->bounds(id).topLeft()));
}

// Text is measured as it is prepared, and a note or a shape made big enough
// for what is written on it: until then each could be anywhere it reaches,
// and is placed by all of that. Text comes onto the board at a guess of its
// size however it comes (read from a file, pasted, put back by undo, changed
// from outside), and a note or a shape at the size it came with.
void CanvasView::markUnmeasured(quint32 id) {
    const Item *item = m_board ? m_board->item(id) : nullptr;
    if (!item)
        return;
    QRectF reach;
    if (item->kind == Item::Kind::Text) {
        reach = TextMetrics::reach(*item);
        m_unmeasured.insert(id, reach);
    } else if (item->kind == Item::Kind::Note || item->kind == Item::Kind::Shape) {
        reach = TextMetrics::reach(*item);
        if (reach.size() == item->rect.size())
            return;
        m_unfitted.insert(id, reach);
    } else {
        return;
    }
    m_reaching.update(id, reach.translated(m_board->bounds(id).topLeft()));
}

// One item's layer, after it moved or changed: worked out from what is
// under it now, and then whatever is on top of it is kept on top of it.
// That only ever pushes other things up, which is always right but not
// always tidy; relayerAll() tidies, when a drag is over.
void CanvasView::relayer(quint32 id) {
    // Whatever it could reach moved with it.
    placeUnmeasured(id);
    if (m_layersStale || !m_board || !m_board->item(id))
        return;
    const QRectF bounds = m_board->bounds(id);
    // On top of the stack, as whatever was just put down is, it is over
    // everything it overlaps and under nothing. How deep each thing is need
    // not be asked: after something is put down, the board would count out
    // its whole stack again to say, and a paste puts down many.
    const QVector<quint32> &order = m_board->order();
    const bool onTop = !order.isEmpty() && order.constLast() == id;
    if (!onTop)
        ++m_depthsAsked;
    const int depth = onTop ? int(order.size()) - 1 : m_board->depth(id);
    QVector<quint32> near;
    m_board->itemsIn(bounds, near);
    int layer = 0;
    for (quint32 other : std::as_const(near))
        if (other != id && (onTop || m_board->depth(other) < depth) && m_board->bounds(other).intersects(bounds))
            layer = qMax(layer, m_place.value(other).layer + 1);
    setLayer(id, layer);
    if (onTop)
        return;
    for (quint32 other : std::as_const(near))
        if (other != id && m_board->depth(other) > depth && m_place.value(other).layer <= layer &&
            m_board->bounds(other).intersects(bounds))
            raiseLayer(other, layer + 1);
}

void CanvasView::raiseLayer(quint32 id, int layer) {
    setLayer(id, layer);
    const QRectF bounds = m_board->bounds(id);
    const int depth = m_board->depth(id);
    QVector<quint32> near;
    m_board->itemsIn(bounds, near);
    for (quint32 other : std::as_const(near))
        if (other != id && m_board->depth(other) > depth && m_place.value(other).layer <= layer &&
            m_board->bounds(other).intersects(bounds))
            raiseLayer(other, layer + 1);
}

void CanvasView::updatePolish() {
    // The stroke being drawn, cut again for a view or a pen that has changed
    // since: once a frame, however often they changed. First, as that can
    // put a stroke on the board.
    refreshPen();
    if (m_layersStale)
        relayerAll();
    if (m_board && !m_dirty.isEmpty()) {
        prepareVisible(true);
        emit pendingItemsChanged();
        if (!m_dirty.isEmpty())
            QMetaObject::invokeMethod(this, &CanvasView::requestFrame, Qt::QueuedConnection);
    }
    if (m_dirty.isEmpty() && m_settledPending) {
        m_settledPending = false;
        // After this frame is on screen.
        QMetaObject::invokeMethod(this, &CanvasView::settled, Qt::QueuedConnection);
    }
    if (m_chromeDirty) {
        m_chromeDirty = false;
        m_chromeReady = true;
        prepareDecorations(m_chromeShapes);
        prepareDraft(m_draftShapes, m_draftStrokes);
    }
    // The window is told what measuring did to the selection, and anything
    // else of it not told yet, before the frame is drawn: what sits by the
    // selection is drawn by it as it is in this frame, not the last.
    tellSelection();
}

// --- Synchronizing ----------------------------------------------------------------

void CanvasView::buildItem(SceneNodes *scene, quint32 id, const Prepared &prepared) {
    Home &home = scene->homes[id];
    scene->destroy(id, home);
    home.position = prepared.position;
    home.bounds = prepared.bounds;
    auto own = [](QSGNode *node) {
        node->setFlag(QSGNode::OwnedByParent);
        return node;
    };

    if (prepared.shapes.count() > 0) {
        home.shapes = Nodes::shapes(prepared.shapes, &scene->shapeMaterial, prepared.position);
        own(home.shapes);
        home.vertices += int(prepared.shapes.vertices.size());
        home.fixed += Load::of(prepared.shapes.count() * 2, shapeBytes);
    }
    if (!prepared.image.isNull()) {
        // Handed to the GPU the first time anything shows it, and shared
        // from then on (see SceneNodes::textures).
        std::shared_ptr<QSGTexture> &texture = scene->textures[prepared.image.cacheKey()];
        if (!texture) {
            // Mipmapped, so a photo zoomed far out is smooth, not sparkling.
            texture.reset(window()->createTextureFromImage(prepared.image, QQuickWindow::TextureHasMipmaps));
            if (texture)
                ++m_picturesUploaded;
        }
        if (texture) {
            home.image = window()->createImageNode();
            own(home.image);
            home.image->setTexture(texture.get());
            home.image->setOwnsTexture(false);
            home.imageRect = prepared.imageRect.translated(prepared.position);
            home.image->setRect(home.imageRect);
            home.image->setFiltering(QSGTexture::Linear);
            home.image->setMipmapFiltering(QSGTexture::Linear);
            home.texture = texture;
            home.fixed += Load::of(2, imageBytes);
        }
    }
    // Built, it holds the picture it shows itself: none is kept for it.
    scene->keptFor.remove(id);
    if (!prepared.strokes.indices.isEmpty()) {
        auto node = [&](const Geo::StrokeMesh &mesh) {
            QSGGeometryNode *stroke = Nodes::stroke(mesh, &scene->strokeMaterial, prepared.position);
            own(stroke);
            return stroke;
        };
        auto loadOf = [](const Geo::StrokeMesh &mesh) { return Load::of(mesh.indices.size() / 3, strokeBytes); };
        home.vertices += int(prepared.strokes.vertices.size());
        if (prepared.coarse[0].indices.isEmpty()) {
            home.strokes = node(prepared.strokes);
            home.fixed += loadOf(prepared.strokes);
        } else {
            // A long stroke or part of the diagram, at every level; the
            // bucket lets one in.
            home.diagram = prepared.diagram;
            home.detail[0] = node(prepared.strokes);
            home.detailLoad[0] = loadOf(prepared.strokes);
            for (int level = 1; level < detailLevels; ++level) {
                home.detail[level] = node(prepared.coarse[level - 1]);
                home.detailLoad[level] = loadOf(prepared.coarse[level - 1]);
            }
        }
    }
    if (prepared.text && !prepared.text->isEmpty()) {
        // Glyphs come from the scene graph's distance field cache: one
        // texture for all text, sharp at every zoom. A title and what is
        // under it are nodes of their own, so each can be drawn with the
        // rest of the text set like it.
        const auto &paragraphs = prepared.text->paragraphs();
        QMatrix4x4 matrix;
        matrix.translate(float(prepared.position.x()), float(prepared.position.y()));
        auto run = [&](size_t from, size_t to, const QColor &color, quint64 style) {
            QSGTextNode *text = nullptr;
            qint64 glyphs = 0;
            for (size_t i = from; i < to && i < paragraphs.size(); ++i) {
                const QString line = paragraphs[i].layout->text();
                if (line.isEmpty())
                    continue;
                if (!text) {
                    text = window()->createTextNode();
                    own(text);
                    text->setRenderType(QSGTextNode::QtRendering);
                    text->setColor(color);
                    text->setMatrix(matrix);
                }
                text->addTextLayout(prepared.textOffset + QPointF(0, paragraphs[i].y), paragraphs[i].layout.get());
                glyphs += std::count_if(line.cbegin(), line.cend(), [](QChar c) { return !c.isSpace(); });
            }
            // Two triangles to the letter.
            if (text)
                home.texts.append({text, style, Load::of(glyphs * 2, glyphBytes)});
        };
        if (prepared.titled) {
            run(0, 1, prepared.titleColor, prepared.titleStyle);
            run(1, paragraphs.size(), prepared.textColor, prepared.bodyStyle);
        } else {
            run(0, paragraphs.size(), prepared.textColor, prepared.bodyStyle);
        }
    }
    scene->attach(id, home, m_place.value(id));
}

QSGNode *CanvasView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *scene = static_cast<SceneNodes *>(old);
    if (!scene) {
        scene = new SceneNodes;
        if (!m_sceneReset) {
            // The scene graph was torn down and what had been built went
            // with it. What is on screen is prepared again right here (the
            // GUI thread is waiting on this one, so the board is safe to
            // read), and the rest in the frames that follow.
            m_built.clear();
            if (m_board)
                for (quint32 id : m_board->order())
                    if (!m_ready.contains(id))
                        m_dirty.insert(id);
            prepareVisible(false);
            QMetaObject::invokeMethod(this, &CanvasView::requestFrame, Qt::QueuedConnection);
        }
        m_sceneReset = m_viewDirty = m_chromeReady = true;
    }

    if (m_sceneReset) {
        scene->clear();
        // All of the board is built again, what is on screen in this frame
        // and the rest in the frames after. A picture the GPU has is kept for
        // each thing on it that will show it, until that is built: let go of
        // with this frame, those off screen would be handed to the GPU again
        // as they are built. One that nothing on it shows is let go of with
        // this frame, and so is one read anew, as another board's pictures
        // are. (The GUI thread is waiting on this one, so the board and the
        // pictures read are safe to look at.)
        if (m_board)
            for (quint32 id : m_board->order())
                if (const Item *item = m_board->item(id); item->kind == Item::Kind::Image)
                    if (auto texture = scene->textures.value(m_images->alreadyRead(item->asset).cacheKey()))
                        scene->keptFor.insert(id, std::move(texture));
        m_gone.clear();
        m_moved.clear();
        m_rehome.clear();
        m_sceneReset = false;
    }
    for (quint32 id : std::as_const(m_gone))
        scene->removeItem(id);
    m_gone.clear();

    for (auto it = m_ready.constBegin(); it != m_ready.constEnd(); ++it) {
        buildItem(scene, it.key(), **it);
        m_built.insert(it.key());
    }
    m_ready.clear();
    // A picture nothing shows any more is let go of only now that every item
    // of this frame is built: an item built again lets go of its picture as
    // it starts over, and has it back by the end.
    m_picturesHeld = scene->letGoOfPictures();

    // An item that only moved keeps its nodes: they are slid to where it is.
    for (quint32 id : std::as_const(m_moved))
        if (const Item *item = m_board ? m_board->item(id) : nullptr)
            scene->move(id, item->rect.topLeft());
    m_moved.clear();

    // And one whose layer changed, or that moved to another part of the
    // board, is taken from its bucket to one where it belongs now.
    for (quint32 id : std::as_const(m_rehome))
        scene->rehome(id, m_place.value(id));
    m_rehome.clear();

    // What is being drawn, and the selection's chrome.
    if (m_chromeReady) {
        m_chromeReady = false;
        setMesh(scene->draft, scene->draftShapes, m_draftShapes, &scene->shapeMaterial);
        setMesh(scene->draft, scene->draftStrokes, m_draftStrokes, &scene->strokeMaterial);
        setMesh(scene->chrome, scene->chromeShapes, m_chromeShapes, &scene->shapeMaterial);
        scene->draftLoad = Load::of(m_draftShapes.count() * 2, shapeBytes);
        scene->draftLoad += Load::of(m_draftStrokes.indices.size() / 3, strokeBytes);
        scene->chromeLoad = Load::of(m_chromeShapes.count() * 2, shapeBytes);
    }

    // With the board as it now is: what of it this frame draws. What is
    // being drawn is drawn whole, as is the selection's chrome, and the text
    // being written is drawn by the editor, every letter of it whether in
    // view or not, so what those ask for comes out of what the board may.
    bool waiting = false;
    const bool was = m_load.reduced;
    const Item *written = editing() && m_board ? m_board->item(m_editing) : nullptr;
    const qint64 editor = written ? qint64(written->text.size()) * 2 * glyphBytes : 0;
    const qint64 draft = scene->draftLoad.bytes, chrome = scene->chromeLoad.bytes;
    // A stroke that has gone on the board in pieces keeps all the room one
    // may have until the pen is lifted: the board does not take back what
    // each piece leaves it, only to give it up again as the next one grows,
    // which would show and hide its detail over and over.
    const qint64 drawing =
        m_drag == Drag::Stroke && m_strokeGroup ? qMax(draft, qint64(strokeLimit()) * strokeBytes) : draft;
    m_load = scene->apply(visibleArea(), m_zoom * devicePixels(),
                          qMax<qint64>(0, m_loadBudget - editor - drawing - chrome), m_drawEverything,
                          m_drag == Drag::Stroke ? m_strokePieces : QVector<quint32>(), &waiting);
    // The board never leaves out the part of it nearest the middle of the
    // view, so a view that has just come to a part piled higher than the
    // stroke being drawn was cut to leave room for can leave it less than
    // that. This frame goes without the stroke then, and it is put down
    // before the next (see refreshPen()): it never makes a frame more than
    // it may be.
    const bool fits = m_drag != Drag::Stroke || m_load.bytes + editor + draft + chrome <= m_loadBudget;
    place(scene->content, scene->draft, fits);
    m_load.editor = editor;
    m_load.draft = fits ? draft : 0;
    m_load.chrome = chrome;
    if (waiting)
        QMetaObject::invokeMethod(this, &CanvasView::requestFrame, Qt::QueuedConnection);
    // A stroke being drawn that is more than this frame turned out to leave
    // room for is put down before the next (see refreshPen()), whether or
    // not the pen moves on.
    if (m_drag == Drag::Stroke && full(m_pen))
        QMetaObject::invokeMethod(this, &CanvasView::requestFrame, Qt::QueuedConnection);
    if (m_load.reduced != was)
        QMetaObject::invokeMethod(this, &CanvasView::loadChanged, Qt::QueuedConnection);

    if (m_viewDirty) {
        m_viewDirty = false;
        const QMatrix4x4 matrix = viewMatrix();
        scene->content->setMatrix(matrix);
        scene->content->markDirty(QSGNode::DirtyMatrix);

        if (scene->gridSize != size()) {
            scene->gridSize = size();
            QSGGeometry::Point2D *corners = scene->grid->geometry()->vertexDataAsPoint2D();
            corners[0].set(0, 0);
            corners[1].set(float(width()), 0);
            corners[2].set(0, float(height()));
            corners[3].set(float(width()), float(height()));
            scene->grid->markDirty(QSGNode::DirtyGeometry);
        }

        GridMaterial &grid = scene->gridMaterial;
        grid.follow(matrix, m_zoom, devicePixels());
        grid.color = m_palette.selection;
        scene->grid->markDirty(QSGNode::DirtyMaterial);
    }
    return scene;
}
