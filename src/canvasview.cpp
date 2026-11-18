// The half of CanvasView that listens: the view, the selection, the tools,
// and the editor that opens over an item. scene.cpp is the half that draws.
#include "canvasview.h"

#include <QClipboard>
#include <QCursor>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QMimeData>
#include <QNativeGestureEvent>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

#include "imagestore.h"
#include "library.h"

namespace {

const qreal minimumZoom = 0.1, maximumZoom = 8;
// Where + and - stop on their way in and out.
const qreal zoomStops[] = {0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.25, 1.5, 2, 3, 4, 6, 8};
// The sizes text steps through.
const qreal textSizes[] = {11, 12, 13, 16, 20, 28, 40, 56, 80};
// The most points one stroke of the pen is given: every point of a stroke is
// looked at to find it, to simplify it and to draw it. A stroke that goes on
// past that is put on the board and carried on as another, as one is that
// comes to more triangles than it may (see full()).
const int longestStroke = 4096;
const char *const clipboardFormat = "application/x-sprawl-items";
const QStringList toolNames{"select", "pan", "note", "text", "shape", "pen", "connector", "frame"};

QPointF rounded(QPointF point) { return QPointF(std::round(point.x()), std::round(point.y())); }

bool isCorner(int handle) { return handle % 2 == 0; }

} // namespace

// Writes the first line of the editor as the title it will be on the board.
class TitleHighlighter : public QSyntaxHighlighter {
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;

    void setStyle(const TextStyle &style) {
        const bool titled = style.titled;
        QTextCharFormat format;
        format.setFontWeight(QFont::Bold);
        format.setProperty(QTextFormat::FontPixelSize, qMax(1, qRound(style.titleSize)));
        format.setForeground(style.titleColor);
        if (titled == m_titled && format == m_format)
            return;
        m_titled = titled;
        m_format = format;
        rehighlight();
    }

protected:
    void highlightBlock(const QString &text) override {
        if (m_titled && currentBlock().blockNumber() == 0 && !text.isEmpty())
            setFormat(0, int(text.size()), m_format);
    }

private:
    bool m_titled = false;
    QTextCharFormat m_format;
};

CanvasView::CanvasView(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);

    m_images = new ImageStore(this);
    connect(m_images, &ImageStore::loaded, this, &CanvasView::onImageLoaded);

    m_viewAnimation.setDuration(170);
    m_viewAnimation.setEasingCurve(QEasingCurve::OutCubic);
    m_viewAnimation.setStartValue(0.0);
    m_viewAnimation.setEndValue(1.0);
    connect(&m_viewAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        const qreal t = value.toReal();
        const qreal from = m_animationZoomFrom, to = m_animationZoomTo;
        // Zoom is felt in ratios, so it is eased in ratios.
        m_zoom = from * std::pow(to / from, t);
        if (std::abs(1 / from - 1 / to) > 1e-9) {
            // Two views of different zoom share exactly one point of the
            // screen; holding it still is what makes a zoom feel anchored.
            const QPointF fixed = (m_animationTo - m_animationFrom) / (1 / from - 1 / to);
            m_origin = m_animationFrom + fixed / from - fixed / m_zoom;
        } else {
            m_origin = m_animationFrom + (m_animationTo - m_animationFrom) * t;
        }
        viewMoved();
    });
}

CanvasView::~CanvasView() = default;

// --- What it shows --------------------------------------------------------------

void CanvasView::setBoard(Board *board) {
    if (board == m_board)
        return;
    if (m_board)
        m_board->disconnect(this);
    m_board = board;
    if (m_board) {
        connect(m_board, &Board::itemChanged, this, &CanvasView::onItemChanged);
        connect(m_board, &Board::orderChanged, this, &CanvasView::onOrderChanged);
        connect(m_board, &Board::renumbered, this, &CanvasView::onRenumbered);
        connect(m_board, &Board::reset, this, &CanvasView::onReset);
        connect(m_board, &Board::historyChanged, this, &CanvasView::historyChanged);
        // A step is over, an undo or a redo with it: whatever it did to the
        // selection is told now, once.
        connect(m_board, &Board::historyChanged, this, &CanvasView::tellSelection);
    }
    onReset();
    emit boardChanged();
    emit historyChanged();
}

void CanvasView::setTheme(Theme *theme) {
    if (theme == m_theme)
        return;
    if (m_theme)
        m_theme->disconnect(this);
    m_theme = theme;
    if (m_theme)
        connect(m_theme, &Theme::changed, this, &CanvasView::onThemeChanged);
    onThemeChanged();
    emit themeChanged();
}

void CanvasView::setAssetFolder(const QString &folder) {
    if (folder == m_assetFolder)
        return;
    m_assetFolder = folder;
    m_images->setFolder(folder);
    if (m_board)
        for (quint32 id : m_board->order())
            if (m_board->item(id)->kind == Item::Kind::Image)
                markDirty(id);
    emit assetFolderChanged();
}

void CanvasView::onThemeChanged() {
    const Palette palette = m_theme ? m_theme->palette() : Palette::fallback();
    const int radius = m_theme ? m_theme->radius() : 0;
    m_reduceMotion = m_theme && m_theme->reduceMotion();
    if (palette == m_palette && radius == m_radius)
        return;
    m_palette = palette;
    m_radius = radius;
    m_viewDirty = true;
    // Every item carries the theme's colors, so every item is drawn again:
    // those on screen in the next frame, the rest behind them.
    markAllDirty();
    refreshEditor();
}

void CanvasView::setLoadBudget(qint64 bytes) {
    m_loadBudget = qBound<qint64>(0, bytes, largestLoadBudget);
    requestFrame();
}

void CanvasView::setDrawingEverything(bool everything) {
    m_drawEverything = everything;
    requestFrame();
}

bool CanvasView::loadingImages() {
    if (m_board)
        for (quint32 id : m_board->order())
            if (const Item *item = m_board->item(id); item->kind == Item::Kind::Image)
                m_images->image(item->asset);
    return m_images->loading();
}

void CanvasView::onImageLoaded(const QString &asset) {
    if (!m_board)
        return;
    for (quint32 id : m_board->order()) {
        const Item *item = m_board->item(id);
        if (item->kind == Item::Kind::Image && item->asset == asset)
            markDirty(id);
    }
}

void CanvasView::picturesMayHaveChanged() {
    if (std::exchange(m_picturesToTell, true))
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_picturesToTell = false;
            QSet<QString> shown;
            if (m_board)
                for (quint32 id : m_board->order())
                    if (const Item *item = m_board->item(id); item->kind == Item::Kind::Image)
                        shown.insert(item->asset);
            m_images->setInUse(shown);
        },
        Qt::QueuedConnection);
}

void CanvasView::onReset() {
    picturesMayHaveChanged();
    // A different board: nothing of the last one carries over.
    m_drag = Drag::None;
    m_dragStarted = false;
    m_dragItems.clear();
    m_pen = Geo::Pen();
    m_connectTarget = 0;
    if (m_editing) {
        m_editing = 0;
        emit editingChanged();
    }
    m_selection.clear();
    m_selected.clear();
    m_selectionHoles = false;
    m_ready.clear();
    m_built.clear();
    m_gone.clear();
    m_place.clear();
    m_rehome.clear();
    m_layersStale = true;
    m_sceneReset = true;
    markAllDirty();
    m_selectionBoxStale = m_selectionNews = true;
    tellSelection();
    emit busyChanged();
    emit pressedChanged();
}

void CanvasView::onItemChanged(quint32 id, Board::Change change) {
    const bool selected = m_selected.contains(id);
    // What is gone may have been a picture; what is there says whether it is
    // one. Only moved, it shows what it did.
    if (change != Board::Change::Moved)
        if (const Item *item = m_board->item(id); !item || item->kind == Item::Kind::Image)
            picturesMayHaveChanged();
    switch (change) {
    case Board::Change::Added:
        markUnmeasured(id);
        relayer(id);
        markDirty(id);
        break;
    case Board::Change::Removed:
        m_dirty.remove(id);
        m_moved.remove(id);
        m_built.remove(id);
        m_place.remove(id);
        m_rehome.remove(id);
        m_gone.append(id);
        if (m_editing == id) {
            m_editing = 0;
            emit editingChanged();
        }
        // No longer selected; the list of what is catches up when it is
        // next read.
        if (selected) {
            m_selected.remove(id);
            m_selectionHoles = true;
        }
        emit pendingItemsChanged();
        break;
    case Board::Change::Moved:
        // Only its place changed: its nodes move, and nothing is rebuilt.
        if (m_built.contains(id) && !m_dirty.contains(id))
            m_moved.insert(id);
        relayer(id);
        break;
    case Board::Change::Edited:
        // An item being made big enough for its text is being prepared at its
        // new size already. Its connectors, carried to its new edges, are
        // prepared again. So is anything else that changed, which as it now
        // reads has not been measured (see markUnmeasured()).
        if (id != m_measuring) {
            markUnmeasured(id);
            markDirty(id);
        }
        relayer(id);
        break;
    }
    if (selected) {
        m_chromeDirty = true;
        // What the bar by the selection shows of it (its color, its shape)
        // follows every change to it but a move, an undo included, and the
        // box around the selection follows it wherever it goes. Both are
        // worked out, and told, once for all that changed: when the step is
        // over, or once the events of the moment have been handled, as each
        // move of a drag has been. Worked out as each thing changed, a step
        // that changes all of a big selection would go through all of it
        // again for every one. While things are only being moved, a
        // connector that follows them is only moved too, though the board
        // says it changed.
        m_selectionBoxStale = true;
        if (change != Board::Change::Moved && !m_moving)
            m_selectionNews = true;
        tellSelectionLater();
    }
    if (id == m_editing)
        refreshEditor();
    requestFrame();
}

void CanvasView::onOrderChanged() {
    // What is in front of what decides the layers.
    m_layersStale = true;
    requestFrame();
}

void CanvasView::onRenumbered(quint32 from, quint32 to) {
    // An item made here has a new id, its old one taken by an item written
    // from outside. It is still the item that is selected, being written in
    // or dragged: all of that goes over to the new id before the board says
    // the old one is gone, so none of it is let go of.
    auto swap = [from, to](quint32 &id) {
        if (id == from)
            id = to;
    };
    for (quint32 &id : m_selection)
        swap(id);
    for (quint32 &id : m_marqueeBase)
        swap(id);
    swap(m_editing);
    swap(m_connectFrom);
    swap(m_connectTarget);
    if (m_dragItems.contains(from))
        m_dragItems.insert(to, m_dragItems.take(from));
    for (Item &item : m_dragItems) {
        swap(item.id);
        swap(item.from);
        swap(item.to);
    }
    // The window is told once for all of a selection given new ids, as for
    // any other change to it: told for each, a big paste numbered anew would
    // have it go through all of the selection again for every one.
    if (m_selected.remove(from)) {
        m_selected.insert(to);
        m_selectionNews = true;
        tellSelectionLater();
    }
}

// --- The view ---------------------------------------------------------------------

void CanvasView::viewMoved() {
    m_viewDirty = true;
    m_chromeDirty = true;
    const QPointF pointer = toCanvas(m_pointerItem);
    if (pointer != m_pointer) {
        m_pointer = pointer;
        emit pointerChanged();
    }
    // The selection is where it was on the board, and somewhere else on screen.
    m_selectionBoxStale = true;
    tellSelection();
    refreshEditor();
    emit viewChanged();
    requestFrame();
}

void CanvasView::setZoomAround(qreal zoom, QPointF around) {
    zoom = qBound(minimumZoom, zoom, maximumZoom);
    const QPointF anchor = toCanvas(around);
    m_zoom = zoom;
    m_origin = anchor - around / zoom;
    viewMoved();
}

void CanvasView::zoomBy(qreal factor, QPointF around) {
    m_viewAnimation.stop();
    setZoomAround(m_zoom * factor, around);
}

void CanvasView::panBy(qreal dx, qreal dy) {
    m_viewAnimation.stop();
    m_origin += QPointF(dx, dy) / m_zoom;
    viewMoved();
}

void CanvasView::setView(QPointF origin, qreal zoom) {
    m_viewAnimation.stop();
    m_origin = origin;
    m_zoom = qBound(minimumZoom, zoom, maximumZoom);
    viewMoved();
}

void CanvasView::animateTo(QPointF origin, qreal zoom) {
    m_viewAnimation.stop();
    if (m_reduceMotion || !window() || !isVisible()) {
        m_origin = origin;
        m_zoom = zoom;
        viewMoved();
        return;
    }
    m_animationFrom = m_origin;
    m_animationTo = origin;
    m_animationZoomFrom = m_zoom;
    m_animationZoomTo = zoom;
    m_viewAnimation.start();
    // The view is on its way from now, and is said to be now: its first step
    // may be a frame away, and until then whatever follows the view (the
    // window, putting it back for a canvas of another size) would take it
    // for one nobody had moved.
    emit viewChanged();
}

QPointF CanvasView::insertPoint() const {
    return m_pointerInside ? m_pointer : toCanvas(QPointF(width() / 2, height() / 2));
}

void CanvasView::zoomIn() {
    const qreal current = m_viewAnimation.state() == QAbstractAnimation::Running ? m_animationZoomTo : m_zoom;
    qreal next = maximumZoom;
    for (qreal stop : zoomStops)
        if (stop > current * 1.001) {
            next = stop;
            break;
        }
    // Toward the mouse when it is over the canvas, the middle otherwise.
    const QPointF around = m_pointerInside ? m_pointerItem : QPointF(width() / 2, height() / 2);
    animateTo(toCanvas(around) - around / next, next);
}

void CanvasView::zoomOut() {
    const qreal current = m_viewAnimation.state() == QAbstractAnimation::Running ? m_animationZoomTo : m_zoom;
    qreal next = minimumZoom;
    for (qreal stop : zoomStops)
        if (stop < current * 0.999)
            next = stop;
    const QPointF around = m_pointerInside ? m_pointerItem : QPointF(width() / 2, height() / 2);
    animateTo(toCanvas(around) - around / next, next);
}

void CanvasView::zoomActual() {
    const QPointF around = m_pointerInside ? m_pointerItem : QPointF(width() / 2, height() / 2);
    animateTo(toCanvas(around) - around, 1);
}

void CanvasView::zoomToFit() {
    const QRectF bounds = m_board ? m_board->contentBounds() : QRectF();
    if (bounds.isNull()) {
        animateTo(QPointF(0, 0), 1);
        return;
    }
    // The whole board, with air around it, never magnified.
    const qreal margin = 48;
    const qreal zoom = qBound(minimumZoom,
                              qMin((width() - 2 * margin) / qMax(bounds.width(), 1.0),
                                   (height() - 2 * margin) / qMax(bounds.height(), 1.0)),
                              1.0);
    animateTo(bounds.center() - QPointF(width(), height()) / (2 * zoom), zoom);
}

void CanvasView::showAll() {
    zoomToFit();
    if (m_viewAnimation.state() == QAbstractAnimation::Running) {
        m_viewAnimation.stop();
        m_origin = m_animationTo;
        m_zoom = m_animationZoomTo;
        viewMoved();
    }
}

void CanvasView::pointTo(QPointF itemPoint) { setPointer(itemPoint, contains(itemPoint)); }

QVariantList CanvasView::selectedIds() const {
    QVariantList ids;
    for (quint32 id : selection())
        ids.append(id);
    return ids;
}

void CanvasView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        m_viewDirty = true;
        m_chromeDirty = true;
        requestFrame();
    }
}

void CanvasView::itemChange(ItemChange change, const ItemChangeData &data) {
    QQuickItem::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged || change == ItemSceneChange) {
        m_viewDirty = true;
        m_chromeDirty = true;
        // The box around the selection is on whole pixels of the screen.
        m_selectionBoxStale = true;
        tellSelectionLater();
        requestFrame();
    }
}

// --- Selection ----------------------------------------------------------------------

void CanvasView::select(const QVector<quint32> &ids) { setSelection(ids); }

void CanvasView::setSelection(QVector<quint32> ids) {
    QSet<quint32> seen;
    ids.removeIf([&](quint32 id) {
        const bool drop = seen.contains(id) || !m_board || !m_board->item(id);
        seen.insert(id);
        return drop;
    });
    if (ids == selection())
        return;
    m_selection = ids;
    m_selected = QSet<quint32>(ids.constBegin(), ids.constEnd());
    m_selectionHoles = false;
    m_selectionBoxStale = m_selectionNews = true;
    m_chromeDirty = true;
    tellSelection();
    requestFrame();
}

const QVector<quint32> &CanvasView::selection() const {
    // What has left the board since the list was last read leaves it now,
    // all in one go.
    if (m_selectionHoles) {
        m_selectionHoles = false;
        ++m_selectionPasses;
        m_selection.removeIf([this](quint32 id) { return !m_selected.contains(id); });
    }
    return m_selection;
}

QVector<quint32> CanvasView::withGroups(const QVector<quint32> &ids) const {
    // The members of every group asked for, found in one look through the
    // board: a look for each group would go through all of it as many times
    // as there are groups, and a marquee asks on every move of the mouse.
    QHash<quint32, QVector<quint32>> members;
    for (quint32 id : ids)
        if (const Item *item = m_board->item(id); item && item->group)
            members.insert(item->group, {});
    if (!members.isEmpty()) {
        ++m_selectionPasses;
        for (quint32 id : m_board->order())
            if (const auto group = members.find(m_board->item(id)->group); group != members.end())
                group->append(id);
    }
    QVector<quint32> all;
    for (quint32 id : ids) {
        const Item *item = m_board->item(id);
        if (!item)
            continue;
        if (item->group == 0) {
            all.append(id);
        } else if (const auto group = members.find(item->group); group != members.end()) {
            // A group comes along once, however many of it were asked for.
            all += *group;
            members.erase(group);
        }
    }
    return all;
}

const Item *CanvasView::soleSelection() const {
    return m_board && selection().size() == 1 ? m_board->item(selection().first()) : nullptr;
}

QString CanvasView::selectionKind() const {
    QString kind;
    for (quint32 id : selection()) {
        const Item *item = m_board ? m_board->item(id) : nullptr;
        if (!item)
            continue;
        const QString name = Board::kindName(item->kind);
        if (!kind.isEmpty() && kind != name)
            return {};
        kind = name;
    }
    return kind;
}

QString CanvasView::selectionColor() const {
    QString color;
    bool first = true;
    for (quint32 id : selection()) {
        const Item *item = m_board ? m_board->item(id) : nullptr;
        if (!item)
            continue;
        if (!first && item->color != color)
            return {};
        color = item->color;
        first = false;
    }
    return color;
}

QString CanvasView::selectionShape() const {
    QString shape;
    for (quint32 id : selection()) {
        const Item *item = m_board ? m_board->item(id) : nullptr;
        if (!item)
            continue;
        if (item->kind != Item::Kind::Shape)
            return {};
        const QString name = Board::shapeName(item->shape);
        if (!shape.isEmpty() && shape != name)
            return {};
        shape = name;
    }
    return shape;
}

QRectF CanvasView::selectionBox() const {
    measureSelection();
    return m_selectionBox;
}

QString CanvasView::selectionSize() const {
    measureSelection();
    return m_selectionSize;
}

void CanvasView::measureSelection() const {
    if (!m_selectionBoxStale)
        return;
    m_selectionBoxStale = false;
    ++m_selectionPasses;
    QRectF box, area;
    if (m_board)
        for (quint32 id : selection()) {
            const Item *item = m_board->item(id);
            if (!item)
                continue;
            const QRectF onScreen = itemBoxOnScreen(*item);
            box = box.isNull() ? onScreen : box | onScreen;
            area = area.isNull() ? item->rect : area | item->rect;
        }
    // Sizes are in canvas units: what the thing measures at 100%.
    const QString size = box.isNull()
                             ? QString()
                             : QStringLiteral("%1 × %2").arg(qRound(area.width())).arg(qRound(area.height()));
    if (box == m_selectionBox && size == m_selectionSize)
        return;
    m_selectionBox = box;
    m_selectionSize = size;
    m_selectionBoxNews = true;
}

void CanvasView::tellSelection() {
    measureSelection();
    const bool box = m_selectionBoxNews, rest = m_selectionNews;
    m_selectionBoxNews = m_selectionNews = false;
    if (box)
        emit selectionBoxChanged();
    if (rest)
        emit selectionChanged();
}

void CanvasView::tellSelectionLater() {
    if (m_tellQueued)
        return;
    m_tellQueued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_tellQueued = false;
            tellSelection();
        },
        Qt::QueuedConnection);
}

QVector<CanvasView::HandleSpot> CanvasView::handleSpots() const {
    const Item *item = soleSelection();
    // Nothing on a board that cannot be changed is there to take hold of.
    if (!item || m_readOnly)
        return {};
    if (item->kind == Item::Kind::Connector)
        return {{0, toItem(item->p1)}, {1, toItem(item->p2)}};
    const QRectF box = itemBoxOnScreen(*item);
    const QPointF middle = box.center();
    QVector<HandleSpot> spots{{TopLeft, box.topLeft()},
                              {TopRight, box.topRight()},
                              {BottomRight, box.bottomRight()},
                              {BottomLeft, box.bottomLeft()}};
    // A thing too small on screen to take by its sides keeps its corners.
    const bool wide = box.width() >= 28, tall = box.height() >= 28;
    if (item->kind == Item::Kind::Text) {
        // Corners scale the type; the sides set where it wraps.
        spots.append({Right, QPointF(box.right(), middle.y())});
        spots.append({Left, QPointF(box.left(), middle.y())});
        return spots;
    }
    if (wide) {
        spots.append({Top, QPointF(middle.x(), box.top())});
        spots.append({Bottom, QPointF(middle.x(), box.bottom())});
    }
    if (tall) {
        spots.append({Right, QPointF(box.right(), middle.y())});
        spots.append({Left, QPointF(box.left(), middle.y())});
    }
    return spots;
}

int CanvasView::handleAt(QPointF itemPoint) const {
    if (editing())
        return -1;
    const auto spots = handleSpots();
    for (const HandleSpot &spot : spots)
        if (std::abs(itemPoint.x() - spot.at.x()) <= 7 && std::abs(itemPoint.y() - spot.at.y()) <= 7)
            return spot.handle;
    return -1;
}

// --- Tools ----------------------------------------------------------------------------

// What everything that would change the board asks first. A board that could
// not be read is there to be looked at, and nothing changes it. And from the
// moment the mouse is pressed on the canvas until it lets go, the board is
// that drag's: it holds on to what it took hold of and keeps all it does in
// one step of undo, so nothing else changes the board, or opens or closes a
// step, under it.
bool CanvasView::mayChange() const { return m_board && !m_readOnly && m_drag == Drag::None; }

QString CanvasView::tool() const { return toolNames.value(int(m_tool)); }

void CanvasView::setTool(const QString &name) {
    const int index = toolNames.indexOf(name);
    if (index >= 0)
        setToolTo(Tool(index));
}

void CanvasView::setToolTo(Tool tool) {
    if (tool == m_tool)
        return;
    // A tool that makes things is taken up only when things may be made.
    // Select and pan, which only look, are always to hand.
    if (tool != Tool::Select && tool != Tool::Pan && !mayChange())
        return;
    if (editing())
        commitEdit();
    cancelDrag();
    m_tool = tool;
    // A tool that makes things starts from nothing selected.
    if (tool != Tool::Select && tool != Tool::Pan)
        setSelection({});
    updateCursor(m_pointerItem);
    emit toolChanged();
}

void CanvasView::setShape(const QString &name) {
    const int index = int(Board::shapes().indexOf(name));
    if (index < 0 || Item::Shape(index) == m_shape)
        return;
    m_shape = Item::Shape(index);
    emit toolChanged();
}

void CanvasView::cycleShape() { stepShape(1); }

void CanvasView::stepShape(int by) {
    const int count = Item::shapeCount;
    m_shape = Item::Shape(((int(m_shape) + by) % count + count) % count);
    emit toolChanged();
}

Item CanvasView::connectorInHand() const {
    Item connector;
    connector.kind = Item::Kind::Connector;
    connector.width = Defaults::connectorWidth;
    connector.route = m_route;
    connector.start = m_lineStart;
    connector.end = m_lineEnd;
    connector.dash = m_dash;
    return connector;
}

QStringList CanvasView::options(const QString &kind) const {
    return kind == "shape"   ? Board::shapes()
           : kind == "route" ? Board::routes()
           : kind == "dash"  ? Board::dashes()
           : kind == "start" || kind == "end" ? Board::heads() : QStringList();
}

QString CanvasView::optionTitle(const QString &kind, const QString &name) const {
    const int index = int(options(kind).indexOf(name));
    if (index < 0)
        return name;
    if (kind == "shape")
        return shapeTitle(name);
    if (kind == "route")
        return QStringList{tr("Straight"), tr("Curved"), tr("Elbow")}.value(index);
    if (kind == "dash")
        return QStringList{tr("Solid"), tr("Dashed"), tr("Dotted")}.value(index);
    return QStringList{tr("None"),    tr("Arrow"),  tr("Open arrow"), tr("Triangle"), tr("Dot"),
                       tr("Circle"),  tr("Diamond"), tr("Open diamond"), tr("Bar")}
        .value(index);
}

namespace {

// What is at an end of a connector, as an icon: the last of a line, with
// the end at its right, or, for a start, the same in a mirror. (Only the
// last of it, so that an end with nothing at it does not look like a line
// that is solid.) `solid` asks for the part that is filled rather than for
// the lines.
QString headIcon(Item::Head head, bool start, bool solid) {
    auto x = [start](qreal value) { return QString::number(start ? 24 - value : value, 'f', 2); };
    auto run = [&](std::initializer_list<QPointF> points, bool closed) {
        QString path;
        int count = 0;
        for (const QPointF &point : points)
            path += QString("%1%2 %3 ").arg(count++ ? "L" : "M", x(point.x()), QString::number(point.y(), 'f', 2));
        return path + (closed ? "Z " : "");
    };
    auto circle = [&] {
        QString path;
        for (int i = 0; i < 20; ++i) {
            const qreal angle = 2 * M_PI * i / 20;
            path += QString("%1%2 %3 ").arg(i ? "L" : "M", x(17.5 + 3.5 * std::cos(angle)),
                                           QString::number(12 + 3.5 * std::sin(angle), 'f', 2));
        }
        return path + "Z ";
    };
    QString lines, filled;
    switch (head) {
    case Item::Head::None:
        lines = run({{8, 12}, {21, 12}}, false);
        break;
    case Item::Head::Arrow:
        lines = run({{8, 12}, {14, 12}}, false);
        filled = run({{21, 12}, {13, 7.5}, {13, 16.5}}, true);
        break;
    case Item::Head::Open:
        lines = run({{8, 12}, {20.5, 12}}, false) + run({{14, 7}, {21, 12}, {14, 17}}, false);
        break;
    case Item::Head::Triangle:
        lines = run({{8, 12}, {13, 12}}, false) + run({{13, 7}, {21, 12}, {13, 17}}, true);
        break;
    case Item::Head::Dot:
        lines = run({{8, 12}, {14, 12}}, false);
        filled = circle();
        break;
    case Item::Head::Circle:
        lines = run({{8, 12}, {14, 12}}, false) + circle();
        break;
    case Item::Head::Diamond:
        lines = run({{7, 12}, {11, 12}}, false);
        filled = run({{11, 12}, {16, 8}, {21, 12}, {16, 16}}, true);
        break;
    case Item::Head::OpenDiamond:
        lines = run({{7, 12}, {11, 12}}, false) + run({{11, 12}, {16, 8}, {21, 12}, {16, 16}}, true);
        break;
    case Item::Head::Bar:
        lines = run({{8, 12}, {21, 12}}, false) + run({{21, 6}, {21, 18}}, false);
        break;
    }
    return (solid ? filled : lines).trimmed();
}

} // namespace

QString CanvasView::optionIcon(const QString &kind, const QString &name) const {
    const int index = int(options(kind).indexOf(name));
    if (index < 0)
        return {};
    if (kind == "shape")
        return shapeIcon(name);
    if (kind == "route")
        return QStringList{"M4 18 L20 6", "M4 18 C13 18 11 6 20 6", "M4 18 H12 V6 H20"}.value(index);
    if (kind == "dash")
        return QStringList{"M3 12 H21", "M3 12 H8 M10.5 12 H15.5 M18 12 H21",
                           "M4 12 h0.01 M8 12 h0.01 M12 12 h0.01 M16 12 h0.01 M20 12 h0.01"}
            .value(index);
    return headIcon(Item::Head(index), kind == "start", false);
}

QString CanvasView::optionSolid(const QString &kind, const QString &name) const {
    const int index = int(options(kind).indexOf(name));
    if (index < 0 || (kind != "start" && kind != "end"))
        return {};
    return headIcon(Item::Head(index), kind == "start", true);
}

QVariantMap CanvasView::line() const {
    return {{"route", Board::routes().value(int(m_route))},
            {"start", Board::heads().value(int(m_lineStart))},
            {"end", Board::heads().value(int(m_lineEnd))},
            {"dash", Board::dashes().value(int(m_dash))}};
}

QVariantMap CanvasView::selectionLine() const {
    // For each, the one they all have, or nothing when they differ.
    QVariantMap common;
    bool first = true;
    for (quint32 id : selection()) {
        const Item *item = m_board ? m_board->item(id) : nullptr;
        if (!item || item->kind != Item::Kind::Connector)
            continue;
        const QVariantMap own{{"route", Board::routes().value(int(item->route))},
                              {"start", Board::heads().value(int(item->start))},
                              {"end", Board::heads().value(int(item->end))},
                              {"dash", Board::dashes().value(int(item->dash))}};
        if (first) {
            common = own;
        } else {
            for (auto it = common.begin(); it != common.end(); ++it)
                if (*it != own.value(it.key()))
                    *it = QString();
        }
        first = false;
    }
    if (first)
        common = {{"route", QString()}, {"start", QString()}, {"end", QString()}, {"dash", QString()}};
    return common;
}

namespace {

// Sets the part of a connector that `kind` names. False when there is no
// such part, or no such choice of it.
bool setPart(Item &connector, const QString &kind, int index) {
    if (index < 0)
        return false;
    if (kind == "route")
        connector.route = Item::Route(index);
    else if (kind == "start")
        connector.start = Item::Head(index);
    else if (kind == "end")
        connector.end = Item::Head(index);
    else if (kind == "dash")
        connector.dash = Item::Dash(index);
    else
        return false;
    return true;
}

} // namespace

void CanvasView::setLine(const QString &kind, const QString &name) {
    Item connector = connectorInHand();
    if (kind == "shape" || !setPart(connector, kind, int(options(kind).indexOf(name))))
        return;
    if (connector == connectorInHand())
        return;
    m_route = connector.route;
    m_lineStart = connector.start;
    m_lineEnd = connector.end;
    m_dash = connector.dash;
    emit toolChanged();
}

void CanvasView::setSelectionLine(const QString &kind, const QString &name) {
    const int index = kind == "shape" ? -1 : int(options(kind).indexOf(name));
    if (!mayChange() || editing() || selection().isEmpty() || index < 0)
        return;
    m_board->begin();
    for (quint32 id : selection()) {
        Item item = *m_board->item(id);
        if (item.kind == Item::Kind::Connector && setPart(item, kind, index))
            m_board->set(item);
    }
    m_board->commit();
    // The next one drawn is like these too.
    setLine(kind, name);
    emit selectionChanged();
}

void CanvasView::stepRoute(int by) {
    const int count = Item::routeCount;
    m_route = Item::Route(((int(m_route) + by) % count + count) % count);
    emit toolChanged();
}

QString CanvasView::shapeTitle(const QString &name) const {
    switch (Item::Shape(qMax(0, int(Board::shapes().indexOf(name))))) {
    case Item::Shape::Rect:
        return tr("Rectangle");
    case Item::Shape::Rounded:
        return tr("Rounded rectangle");
    case Item::Shape::Pill:
        return tr("Pill");
    case Item::Shape::Ellipse:
        return tr("Ellipse");
    case Item::Shape::Diamond:
        return tr("Diamond");
    case Item::Shape::Triangle:
        return tr("Triangle");
    case Item::Shape::RightTriangle:
        return tr("Right triangle");
    case Item::Shape::Parallelogram:
        return tr("Parallelogram");
    case Item::Shape::Trapezoid:
        return tr("Trapezoid");
    case Item::Shape::Pentagon:
        return tr("Pentagon");
    case Item::Shape::Hexagon:
        return tr("Hexagon");
    case Item::Shape::Octagon:
        return tr("Octagon");
    case Item::Shape::Star:
        return tr("Star");
    case Item::Shape::Cross:
        return tr("Cross");
    case Item::Shape::Arrow:
        return tr("Arrow");
    case Item::Shape::ArrowLeft:
        return tr("Arrow left");
    case Item::Shape::DoubleArrow:
        return tr("Double arrow");
    case Item::Shape::Bubble:
        return tr("Speech bubble");
    case Item::Shape::Cylinder:
        return tr("Cylinder");
    }
    return name;
}

QString CanvasView::shapeIcon(const QString &name) const {
    const int index = int(Board::shapes().indexOf(name));
    if (index < 0)
        return {};
    const Item::Shape shape = Item::Shape(index);
    // As it is when dropped on the board, only not so wide that an icon of
    // it would be a sliver, and fitted to the middle of the grid.
    QSizeF size = Defaults::shapeSize(shape);
    size.setHeight(qMax(size.height(), size.width() / 1.5));
    size.scale(QSizeF(19, 15), Qt::KeepAspectRatio);
    const QPointF corner((24 - size.width()) / 2, (24 - size.height()) / 2);
    const Geo::Figure figure = Geo::figure(shape, size, 0.1);
    QString path;
    auto trace = [&](const QVector<QPointF> &points, bool closed) {
        for (int i = 0; i < points.size(); ++i) {
            const QPointF at = points[i] + corner;
            path += QString("%1%2 %3 ").arg(i ? "L" : "M").arg(at.x(), 0, 'f', 2).arg(at.y(), 0, 'f', 2);
        }
        if (closed && !points.isEmpty())
            path += "Z ";
    };
    trace(figure.edge, true);
    trace(figure.line, false);
    return path.trimmed();
}

void CanvasView::setPenColor(const QString &color) {
    if (color == m_penColor)
        return;
    m_penColor = color;
    // A stroke being drawn takes it on in the next frame (see refreshPen()).
    if (m_drag == Drag::Stroke)
        requestFrame();
    emit penChanged();
}

void CanvasView::setPenWidth(qreal width) {
    width = qBound(1.0, width, 64.0);
    if (width == m_penWidth)
        return;
    m_penWidth = width;
    if (m_drag == Drag::Stroke)
        requestFrame();
    emit penChanged();
}

void CanvasView::updateCursor(QPointF itemPoint) {
    Qt::CursorShape shape = Qt::ArrowCursor;
    switch (m_tool) {
    case Tool::Pan:
        shape = m_drag == Drag::Pan ? Qt::ClosedHandCursor : Qt::OpenHandCursor;
        break;
    case Tool::Text:
        shape = Qt::IBeamCursor;
        break;
    case Tool::Note:
    case Tool::Shape:
    case Tool::Pen:
    case Tool::Connector:
    case Tool::Frame:
        shape = Qt::CrossCursor;
        break;
    case Tool::Select: {
        const int handle = m_drag == Drag::Resize || m_drag == Drag::End ? m_dragHandle : handleAt(itemPoint);
        const Item *sole = soleSelection();
        if (handle < 0)
            break;
        if (sole && sole->kind == Item::Kind::Connector)
            shape = Qt::CrossCursor;
        else if (handle == TopLeft || handle == BottomRight)
            shape = Qt::SizeFDiagCursor;
        else if (handle == TopRight || handle == BottomLeft)
            shape = Qt::SizeBDiagCursor;
        else
            shape = handle == Top || handle == Bottom ? Qt::SizeVerCursor : Qt::SizeHorCursor;
        break;
    }
    }
    if (m_drag == Drag::Pan)
        shape = Qt::ClosedHandCursor;
    if (cursor().shape() != shape)
        setCursor(shape);
}

void CanvasView::setPointer(QPointF itemPoint, bool inside) {
    const QPointF canvas = toCanvas(itemPoint);
    m_pointerItem = itemPoint;
    if (canvas == m_pointer && inside == m_pointerInside)
        return;
    m_pointer = canvas;
    m_pointerInside = inside;
    emit pointerChanged();
}

quint32 CanvasView::attachableAt(QPointF canvasPoint, quint32 except) const {
    return m_board->itemAt(canvasPoint, slack(),
                           [except](const Item &item) { return Board::canAttach(item) && item.id != except; });
}

// Where to put a new thing so that it does not sit exactly on one of its
// own kind: a second note made without moving the mouse, a picture pasted
// twice. Each steps down and to the right of the last, like a hand of cards.
QPointF CanvasView::freeCorner(QPointF corner, Item::Kind kind) const {
    QVector<quint32> near;
    for (int step = 0; step < 64; ++step) {
        m_board->itemsIn(QRectF(corner, QSizeF(1, 1)), near);
        const bool taken = std::any_of(near.constBegin(), near.constEnd(), [&](quint32 id) {
            const Item *item = m_board->item(id);
            return item && item->kind == kind && item->rect.topLeft() == corner;
        });
        if (!taken)
            break;
        corner += QPointF(16, 16);
    }
    return corner;
}

quint32 CanvasView::createItem(Item item, bool edit) {
    // Whatever was being written is finished first, as a step of its own:
    // a picture dropped on the board meanwhile does not take it along.
    if (editing())
        commitEdit();
    m_board->begin();
    const quint32 id = m_board->add(item);
    setSelection({id});
    if (edit)
        beginEdit(id, true);
    else
        m_board->commit();
    return id;
}

// --- The mouse -------------------------------------------------------------------------

void CanvasView::mousePressEvent(QMouseEvent *event) {
    event->accept();
    forceActiveFocus(Qt::MouseFocusReason);
    if (editing())
        commitEdit();
    if (!m_board || m_drag != Drag::None)
        return;

    const QPointF at = event->position(), canvas = toCanvas(at);
    setPointer(at, true);
    m_viewAnimation.stop();
    m_pressItem = m_lastItem = at;
    m_pressCanvas = canvas;
    m_dragStarted = false;
    m_dragButton = event->button();
    const bool shift = event->modifiers() & Qt::ShiftModifier;

    if (event->button() == Qt::MiddleButton || m_tool == Tool::Pan) {
        m_drag = Drag::Pan;
        m_panOrigin = m_origin;
        updateCursor(at);
        emit pressedChanged();
        return;
    }
    // Every other tool makes something, and nothing is made on a board that
    // may not be changed.
    if (m_tool != Tool::Select && !mayChange())
        return;

    switch (m_tool) {
    case Tool::Select: {
        const int handle = handleAt(at);
        const Item *sole = soleSelection();
        if (handle >= 0 && sole) {
            m_dragHandle = handle;
            m_dragItems = {{sole->id, *sole}};
            m_drag = sole->kind == Item::Kind::Connector ? Drag::End : Drag::Resize;
            break;
        }
        const quint32 hit = m_board->itemAt(canvas, slack());
        if (hit) {
            const QVector<quint32> group = withGroups({hit});
            if (shift) {
                // Shift adds to the selection, or takes away from it: a
                // group all in one go, not a look through the selection for
                // each thing in the group.
                const bool had = m_selected.contains(hit);
                const QSet<quint32> members(group.constBegin(), group.constEnd());
                QVector<quint32> next = selection();
                next.removeIf([&members](quint32 id) { return members.contains(id); });
                if (!had)
                    next += group;
                setSelection(next);
                if (had)
                    break;
            } else if (!m_selected.contains(hit)) {
                setSelection(group);
            }
            if (!mayChange()) {
                // Nothing is moved on a board that may not be changed, so a
                // press on something is all the click there will be: it
                // picks just that, as letting go would.
                if (!shift)
                    setSelection(group);
                break;
            }
            m_drag = Drag::Move;
            m_connectFrom = hit; // remembered for a click that turns out not to be a drag
        } else {
            m_marqueeBase = shift ? selection() : QVector<quint32>();
            if (!shift)
                setSelection({});
            m_marquee = QRectF(at, QSizeF(0, 0));
            m_drag = Drag::Marquee;
        }
        break;
    }
    case Tool::Note: {
        Item note;
        note.kind = Item::Kind::Note;
        note.color = m_noteColor;
        note.rect = QRectF(rounded(canvas - QPointF(Defaults::noteSize, Defaults::noteSize) / 2),
                           QSizeF(Defaults::noteSize, Defaults::noteSize));
        setToolTo(Tool::Select);
        createItem(note, true);
        break;
    }
    case Tool::Text: {
        Item text;
        text.kind = Item::Kind::Text;
        text.fontSize = Defaults::textSize;
        // The line of type is level with the click.
        text.rect = QRectF(rounded(canvas - QPointF(0, 8)), QSizeF(2, 16));
        setToolTo(Tool::Select);
        createItem(text, true);
        break;
    }
    case Tool::Shape:
    case Tool::Frame:
        m_draftBox = QRectF(canvas, QSizeF(0, 0));
        m_drag = Drag::Box;
        break;
    case Tool::Pen:
        // However many strokes it comes to (see carryOn()), what the pen
        // draws before it is lifted is one step of undo, which endDrag()
        // ends, and backing out of it takes all of it back.
        m_board->begin();
        m_pen = penInHand();
        m_pen.add(canvas);
        m_strokeGroup = 0;
        m_strokePieces.clear();
        m_drag = Drag::Stroke;
        m_dragStarted = true;
        m_chromeDirty = true;
        emit busyChanged();
        requestFrame();
        break;
    case Tool::Connector:
        m_connectFrom = attachableAt(canvas);
        m_connectTarget = 0;
        m_connectStart = m_connectEnd = canvas;
        m_drag = Drag::Connect;
        break;
    case Tool::Pan:
        break;
    }
    if (m_drag != Drag::None)
        emit pressedChanged();
}

void CanvasView::mouseMoveEvent(QMouseEvent *event) {
    event->accept();
    const QPointF at = event->position(), canvas = toCanvas(at);
    setPointer(at, contains(at));
    if (m_drag == Drag::None || !m_board)
        return;

    if (!m_dragStarted) {
        // A press becomes a drag only once the mouse has clearly moved, so a
        // click never nudges what it clicked.
        if (m_drag != Drag::Pan && (at - m_pressItem).manhattanLength() < 4)
            return;
        m_dragStarted = true;
        if (m_drag == Drag::Move) {
            startMove();
        } else if (m_drag == Drag::Resize || m_drag == Drag::End) {
            m_board->begin();
            // From the item as it is now, which may have come in from outside
            // since the press; and the step holds it from the start, whether
            // or not the first step of the mouse changes it (see
            // Board::hold()).
            for (auto it = m_dragItems.begin(); it != m_dragItems.end(); ++it)
                if (const Item *item = m_board->item(it.key())) {
                    it.value() = *item;
                    m_board->hold(it.key());
                }
        }
        m_chromeDirty = true;
        emit busyChanged();
    }

    switch (m_drag) {
    case Drag::Pan:
        m_origin = m_panOrigin - (at - m_pressItem) / m_zoom;
        viewMoved();
        break;
    case Drag::Marquee:
        dragMarquee(at);
        break;
    case Drag::Move:
        dragMove(canvas);
        break;
    case Drag::Resize:
        dragResize(canvas, event->modifiers() & Qt::ShiftModifier);
        break;
    case Drag::End:
        dragEnd(canvas);
        break;
    case Drag::Stroke:
        drawTo(canvas);
        break;
    case Drag::Box:
        m_draftBox.setBottomRight(canvas);
        break;
    case Drag::Connect: {
        m_connectTarget = attachableAt(canvas, m_connectFrom);
        Item preview = connectorInHand();
        preview.from = m_connectFrom;
        preview.to = m_connectTarget;
        preview.p1 = m_pressCanvas;
        preview.p2 = canvas;
        const Board::Ends ends = m_board->connectorEnds(preview);
        m_connectStart = ends.from;
        m_connectEnd = ends.to;
        m_connectSideways = ends.sideways;
        break;
    }
    case Drag::None:
        break;
    }
    m_lastItem = at;
    m_chromeDirty = true;
    requestFrame();
}

void CanvasView::mouseReleaseEvent(QMouseEvent *event) {
    event->accept();
    if (m_drag == Drag::None || event->button() != m_dragButton || !m_board)
        return;
    const QPointF canvas = toCanvas(event->position());
    switch (m_drag) {
    case Drag::Move:
        if (m_dragStarted) {
            m_board->commit();
        } else if (!(event->modifiers() & Qt::ShiftModifier) && selection().size() > 1) {
            // A click on one of several selected items selects just that one.
            setSelection(withGroups({m_connectFrom}));
        }
        break;
    case Drag::Resize:
    case Drag::End:
        if (m_dragStarted)
            m_board->commit();
        break;
    case Drag::Stroke:
        finishStroke();
        break;
    case Drag::Box:
        finishBox(canvas);
        break;
    case Drag::Connect:
        finishConnect(canvas);
        break;
    case Drag::Pan:
    case Drag::Marquee:
    case Drag::None:
        break;
    }
    endDrag();
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent *event) {
    event->accept();
    if (!m_board || event->button() != Qt::LeftButton || m_tool != Tool::Select)
        return;
    // A drag another button began (the middle one, moving the view) is not
    // this double click's to end: like a click meanwhile, it does nothing.
    if (m_drag != Drag::None && m_dragButton != event->button())
        return;
    // The second press of a double click starts nothing of its own.
    cancelDrag();
    m_dragButton = Qt::NoButton;
    // And what it does start, writing, is a change.
    if (!mayChange())
        return;
    const QPointF canvas = toCanvas(event->position());
    const quint32 hit = m_board->itemAt(canvas, slack());
    if (hit) {
        setSelection({hit});
        beginEdit(hit, false);
        return;
    }
    // A double click on the empty canvas starts writing there.
    Item text;
    text.kind = Item::Kind::Text;
    text.fontSize = Defaults::textSize;
    text.rect = QRectF(rounded(canvas - QPointF(0, 8)), QSizeF(2, 16));
    createItem(text, true);
}

void CanvasView::mouseUngrabEvent() {
    // The mouse was taken away mid-drag (the window lost it): keep a stroke
    // as far as it got, and put anything else back.
    if (m_drag == Drag::Stroke && m_board) {
        finishStroke();
        endDrag();
    } else {
        cancelDrag();
    }
}

void CanvasView::keepStroke() {
    if (m_drag != Drag::Stroke || !m_board)
        return;
    finishStroke();
    endDrag();
}

void CanvasView::hoverEnterEvent(QHoverEvent *event) {
    setPointer(event->position(), true);
    updateCursor(event->position());
}

void CanvasView::hoverMoveEvent(QHoverEvent *event) {
    setPointer(event->position(), true);
    updateCursor(event->position());
}

void CanvasView::hoverLeaveEvent(QHoverEvent *) { setPointer(m_pointerItem, false); }

void CanvasView::wheelEvent(QWheelEvent *event) {
    event->accept();
    const QPointF pixels = event->pixelDelta();
    const QPointF notches = QPointF(event->angleDelta()) / 120.0;
    // A touchpad reports how far the fingers moved; a wheel reports notches.
    const bool touchpad = !event->pixelDelta().isNull();
    setPointer(event->position(), true);
    if (event->modifiers() & Qt::ControlModifier) {
        const qreal factor = touchpad ? std::exp(pixels.y() * 0.005) : std::pow(1.2, notches.y());
        zoomBy(factor, event->position());
        return;
    }
    QPointF delta = touchpad ? pixels : notches * 48;
    // A wheel only goes up and down; Shift turns it sideways.
    if (!touchpad && (event->modifiers() & Qt::ShiftModifier))
        delta = delta.transposed();
    panBy(-delta.x(), -delta.y());
}

bool CanvasView::event(QEvent *event) {
    if (event->type() == QEvent::NativeGesture) {
        // A pinch on the touchpad.
        auto *gesture = static_cast<QNativeGestureEvent *>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            setPointer(gesture->position(), true);
            zoomBy(1 + gesture->value(), gesture->position());
        }
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

// --- Dragging ---------------------------------------------------------------------------

void CanvasView::startMove() {
    m_board->begin();
    m_dragItems = carried();
    // The step holds what is carried from the start, whether or not the
    // first step of the mouse moves it (see Board::hold()).
    for (auto it = m_dragItems.constBegin(); it != m_dragItems.constEnd(); ++it)
        m_board->hold(it.key());
}

QHash<quint32, Item> CanvasView::carried() const {
    QVector<quint32> moving = selection();
    // A frame carries what sits inside it.
    QVector<quint32> inside;
    for (quint32 id : selection()) {
        const Item *frame = m_board->item(id);
        if (!frame || frame->kind != Item::Kind::Frame)
            continue;
        m_board->itemsIn(frame->rect, inside);
        for (quint32 other : std::as_const(inside)) {
            const Item *item = m_board->item(other);
            if (item && other != id && frame->rect.contains(item->rect))
                moving.append(other);
        }
    }
    QHash<quint32, Item> items;
    for (quint32 id : std::as_const(moving))
        if (const Item *item = m_board->item(id))
            items.insert(id, *item);
    return items;
}

void CanvasView::moveBy(const QHash<quint32, Item> &items, QPointF delta) {
    // Only where things are changes, and where the connectors that follow
    // them are: nothing the bar by the selection shows, which is not told of
    // it on every move of a drag.
    m_moving = true;
    for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
        Item moved = it.value();
        moved.rect.translate(delta);
        if (moved.kind == Item::Kind::Connector) {
            moved.p1 += delta;
            moved.p2 += delta;
        }
        m_board->set(moved);
    }
    m_moving = false;
}

void CanvasView::dragMove(QPointF canvasPoint) {
    // By whole units, so things stay on the pixel grid at 100%.
    moveBy(m_dragItems, rounded(canvasPoint - m_pressCanvas));
}

void CanvasView::dragResize(QPointF canvasPoint, bool keepAspect) {
    const Item start = m_dragItems.constBegin().value();
    const QRectF from = start.rect;
    const QPointF delta = canvasPoint - m_pressCanvas;
    const int handle = m_dragHandle;
    const bool left = handle == TopLeft || handle == Left || handle == BottomLeft;
    const bool right = handle == TopRight || handle == Right || handle == BottomRight;
    const bool top = handle == TopLeft || handle == Top || handle == TopRight;
    const bool bottom = handle == BottomLeft || handle == Bottom || handle == BottomRight;
    const bool stroke = start.kind == Item::Kind::Stroke;
    const qreal farthest = Item::farthest;

    // Pictures and type keep their proportions; anything does with Shift.
    // Type only by a corner: its sides set where it wraps, and its height is
    // what its lines come to.
    const bool proportional = (start.kind == Item::Kind::Text ? isCorner(handle)
                                                               : keepAspect || start.kind == Item::Kind::Image) &&
                              from.width() > 0 && from.height() > 0;

    // The least it is made across and down: as little as anything can be
    // made, and a unit for a stroke. In proportion its width and height go
    // together, and neither leaps to bring the other up to its least: what
    // is already narrower or lower than that is made no less than it was,
    // and a stroke, as often as not nearly level, keeps only its longer side
    // to a unit, and that only if it was as long.
    const qreal least = stroke ? 1 : Defaults::minimumSize;
    QSizeF leastSize(least, least);
    if (proportional && stroke)
        leastSize = from.size() * qMin(1.0, least / qMax(from.width(), from.height()));
    else if (proportional)
        leastSize = QSizeF(qMin(least, from.width()), qMin(least, from.height()));

    // The edges the handle holds follow the mouse; the others stay. All on
    // whole units, so things stay on the pixel grid at 100%, but for a
    // stroke: it is where the hand drew it, and rounded, one nearly level
    // would come out flat.
    qreal l = from.left() + (left ? delta.x() : 0), r = from.right() + (right ? delta.x() : 0);
    qreal t = from.top() + (top ? delta.y() : 0), b = from.bottom() + (bottom ? delta.y() : 0);
    if (!stroke)
        l = std::round(l), r = std::round(r), t = std::round(t), b = std::round(b);

    // The most is what a file holds across and down; and as a file holds no
    // left or top edge farther out than that, either way, the one a handle
    // holds goes no farther, and in proportion a side no handle holds grows
    // or shrinks about its middle only as far as keeps its edge in. Where a
    // side cannot have its least as well, at the very edge of the board,
    // what a file holds comes first.
    const QPointF middle = from.center();
    qreal leastW = leastSize.width(), mostW = farthest, leastH = leastSize.height(), mostH = farthest;
    if (left)
        leastW = qMax(leastW, r - farthest), mostW = qMin(mostW, r + farthest);
    else if (proportional && !right)
        leastW = qMax(leastW, 2 * (middle.x() - farthest)), mostW = qMin(mostW, 2 * (middle.x() + farthest));
    if (top)
        leastH = qMax(leastH, b - farthest), mostH = qMin(mostH, b + farthest);
    else if (proportional && !bottom)
        leastH = qMax(leastH, 2 * (middle.y() - farthest)), mostH = qMin(mostH, 2 * (middle.y() + farthest));

    qreal w = r - l, h = b - t;
    if (proportional) {
        // One scale across and down: at a corner the larger side leads, at a
        // side the side in hand, and only as far as both have room for.
        const qreal across = w / from.width(), down = h / from.height();
        const qreal scale = isCorner(handle) ? qMax(across, down) : left || right ? across : down;
        const qreal smallest = qMax(leastW / from.width(), leastH / from.height());
        const qreal largest = qMin(mostW / from.width(), mostH / from.height());
        w = from.width() * qMin(largest, qMax(smallest, scale));
        h = from.height() * qMin(largest, qMax(smallest, scale));
    }
    // Each side that changes within its room, and not a hair out of it for
    // how the proportion fell. Without proportion, a side no handle holds
    // stays as it was.
    if (proportional || left || right)
        w = qMin(mostW, qMax(leastW, w));
    if (proportional || top || bottom)
        h = qMin(mostH, qMax(leastH, h));
    // The edge opposite the handle stays where it was, and so does the
    // middle of a side that follows in proportion.
    l = left ? r - w : right || !proportional ? l : middle.x() - w / 2;
    t = top ? b - h : bottom || !proportional ? t : middle.y() - h / 2;
    // From its corner and its size, so that a side kept to the least is the
    // least exactly, and not a hair under for where its middle fell.
    const QRectF to(l, t, w, h);

    Item item = start;
    switch (start.kind) {
    case Item::Kind::Text: {
        if (isCorner(handle)) {
            const qreal scale = to.height() / from.height();
            item.fontSize = qBound(6.0, std::round(start.fontSize * scale), 400.0);
            // It wraps no wider than a file holds, though the type may grow
            // more than the corner did: to whole points, and to 6 at least.
            if (start.wrap > 0)
                item.wrap = qMin(std::round(start.wrap * item.fontSize / start.fontSize), farthest);
        } else {
            item.wrap = qMax(24.0, to.width());
        }
        TextMetrics::fit(item);
        // The corner or side opposite the handle stays where it was.
        item.rect.moveTo(left ? from.right() - item.rect.width() : from.left(),
                         top ? from.bottom() - item.rect.height() : from.top());
        break;
    }
    case Item::Kind::Stroke: {
        // A dead straight stroke has no thickness to scale.
        const qreal sx = from.width() > 0 ? to.width() / from.width() : 1;
        const qreal sy = from.height() > 0 ? to.height() / from.height() : 1;
        for (QPointF &point : item.points)
            point = QPointF(point.x() * sx, point.y() * sy);
        item.rect = QRectF(to.topLeft(), QSizeF(from.width() * sx, from.height() * sy));
        break;
    }
    default:
        item.rect = to;
        // A note cannot be made smaller than what is written on it.
        if (TextMetrics::fit(item) && top && item.kind == Item::Kind::Note)
            item.rect.moveBottom(from.bottom());
        break;
    }
    // And its corner where a file can have it, whatever its writing made of
    // its size: at the very edge of the board, what grows to fit its text is
    // pushed back in.
    item.rect.moveTo(qBound(-farthest, item.rect.x(), farthest), qBound(-farthest, item.rect.y(), farthest));
    m_board->set(item);
}

void CanvasView::dragEnd(QPointF canvasPoint) {
    const Item *current = m_board->item(m_dragItems.constBegin().key());
    if (!current)
        return;
    Item item = *current;
    // An end dropped on an item attaches to it; anywhere else it is free.
    const bool first = m_dragHandle == 0;
    m_connectTarget = attachableAt(canvasPoint, first ? item.to : item.from);
    (first ? item.from : item.to) = m_connectTarget;
    (first ? item.p1 : item.p2) = canvasPoint;
    m_board->set(item);
}

void CanvasView::dragMarquee(QPointF itemPoint) {
    m_marquee = QRectF(m_pressItem, itemPoint).normalized();
    const QRectF area(toCanvas(m_marquee.topLeft()), toCanvas(m_marquee.bottomRight()));
    QVector<quint32> found, touched = m_marqueeBase;
    m_board->itemsIn(area, found);
    for (quint32 id : std::as_const(found)) {
        const Item *item = m_board->item(id);
        if (item && m_board->touches(*item, area))
            touched.append(id);
    }
    setSelection(withGroups(touched));
}

// The pen as it is set, for the view as it is: how wide it draws, in what
// color, and how many screen pixels a unit is, which says how finely what it
// draws is cut.
Geo::Pen CanvasView::penInHand() const {
    return Geo::Pen(m_penWidth / 2, Geo::packColor(colorFor(m_penColor, m_palette.bright)), m_zoom * devicePixels());
}

void CanvasView::drawTo(QPointF canvasPoint) {
    refreshPen();
    // Points closer than most of a screen pixel add nothing but weight.
    const QPointF step = (canvasPoint - m_pen.points().last()) * m_zoom;
    if (std::hypot(step.x(), step.y()) < 0.75)
        return;
    // Where the pen turned sharply it made a corner, which the line comes to
    // a point at rather than rounding it off: that point is given twice (see
    // Geo::smooth()). It can take a point more to tell where it was.
    if (const Geo::Corner corner = Geo::cornerBefore(m_pen.points(), canvasPoint, 1 / m_zoom); !corner.points.isEmpty())
        m_pen.redraw(corner.back, corner.points);
    m_pen.add(canvasPoint);
    if (full(m_pen))
        carryOn();
}

// The stroke being drawn is cut for the pen and the view as they were when
// it was: if either has changed since (a zoom, a wider pen, another color),
// it is cut again, which can make it more triangles than it was. That waits
// for the next point or the next frame, whichever comes first, so it is done
// once however many times they changed in between. Either way a frame is on
// its way (a point is drawn in one, and this is the start of making one
// ready), so neither this nor carryOn() asks for one.
void CanvasView::refreshPen() {
    if (m_drag != Drag::Stroke)
        return;
    if (Geo::Pen pen = penInHand(); !pen.cutAlike(m_pen)) {
        // Once it is past what it may be, it is put down as it is: the rest
        // need not be cut to know that.
        for (qsizetype i = 0; i < m_pen.points().size() && !full(pen); ++i)
            pen.add(m_pen.points()[i]);
        if (full(pen)) {
            carryOn();
            return;
        }
        m_pen = std::move(pen);
        m_chromeDirty = true;
    }
    // How much it may be can have changed even so (see strokeLimit()).
    if (full(m_pen))
        carryOn();
}

// The stroke being drawn is drawn whole, however much of it is in view, so
// it may only come to so many triangles, its share of a frame; and to only
// so many points, as any stroke.
bool CanvasView::full(const Geo::Pen &pen) const {
    const qsizetype points = pen.points().size();
    return points >= 2 && (points >= longestStroke || pen.triangles() > strokeLimit());
}

// Puts the stroke being drawn on the board as far as it has got, where it is
// drawn as the board is, and carries on drawing from its last point as a
// stroke of its own.
void CanvasView::carryOn() {
    // The pieces are one stroke to whoever picks it up: a group.
    if (!m_strokeGroup)
        m_strokeGroup = m_board->newGroup();
    const QPointF last = m_pen.points().last();
    m_strokePieces.append(finishStroke());
    m_pen = penInHand();
    m_pen.add(last);
    m_chromeDirty = true;
}

// Puts the stroke being drawn on the board as far as it has got, and says
// which item it is there: none, when there was nothing to put down.
quint32 CanvasView::finishStroke() {
    // One carried on from another and let go of before it went anywhere is
    // only the end of that one, which is drawn already: not a dot of its own.
    if (m_pen.points().isEmpty() || (m_strokeGroup && m_pen.points().size() == 1)) {
        m_pen = Geo::Pen();
        return 0;
    }
    // Keep the points its curve needs to stay within a third of a screen
    // pixel of the curve that was drawn, corners and all: letting go of it
    // changes nothing that shows.
    QVector<QPointF> points = Geo::simplifyCurve(m_pen.points(), 0.35 / m_zoom);
    const QRectF bounds = Geo::boundsOf(points);
    for (QPointF &point : points)
        point -= bounds.topLeft();
    Item stroke;
    stroke.kind = Item::Kind::Stroke;
    stroke.rect = bounds;
    stroke.points = points;
    stroke.width = m_penWidth;
    stroke.color = m_penColor;
    stroke.group = m_strokeGroup;
    m_pen = Geo::Pen();
    return m_board->add(stroke);
}

void CanvasView::finishBox(QPointF canvasPoint) {
    m_draftBox.setBottomRight(canvasPoint);
    const QRectF dragged = m_draftBox.normalized();
    const bool frame = m_tool == Tool::Frame;
    Item item;
    item.kind = frame ? Item::Kind::Frame : Item::Kind::Shape;
    item.shape = m_shape;
    if (!m_dragStarted || (dragged.width() < 8 && dragged.height() < 8)) {
        // A click drops one of the usual size, centered where it was clicked.
        const QSizeF size = frame ? QSizeF(Defaults::frameWidth, Defaults::frameHeight) : Defaults::shapeSize(m_shape);
        item.rect = QRectF(rounded(m_pressCanvas - QPointF(size.width(), size.height()) / 2), size);
    } else {
        item.rect = QRectF(rounded(dragged.topLeft()), rounded(dragged.bottomRight()));
        item.rect.setWidth(qMax(item.rect.width(), Defaults::minimumSize));
        item.rect.setHeight(qMax(item.rect.height(), Defaults::minimumSize));
    }
    m_drag = Drag::None;
    setToolTo(Tool::Select);
    createItem(item, true);
}

void CanvasView::finishConnect(QPointF canvasPoint) {
    if (!m_dragStarted)
        return;
    const quint32 to = attachableAt(canvasPoint, m_connectFrom);
    const QPointF span = (canvasPoint - m_pressCanvas) * m_zoom;
    // Too short to be meant, when it joins nothing.
    if (!m_connectFrom && !to && std::hypot(span.x(), span.y()) < 8)
        return;
    Item connector = connectorInHand();
    connector.from = m_connectFrom;
    connector.to = to;
    connector.p1 = m_pressCanvas;
    connector.p2 = canvasPoint;
    m_drag = Drag::None;
    setToolTo(Tool::Select);
    setSelection({m_board->add(connector)});
}

void CanvasView::cancelDrag() {
    if (m_drag == Drag::None)
        return;
    if (m_dragStarted && m_board &&
        (m_drag == Drag::Move || m_drag == Drag::Resize || m_drag == Drag::End || m_drag == Drag::Stroke))
        m_board->rollback();
    if (m_drag == Drag::Marquee)
        setSelection(m_marqueeBase);
    endDrag();
}

void CanvasView::endDrag() {
    // What the pen drew is kept, as the one step it began, however it came
    // to be lifted. Backing out of it has rolled that back already.
    if (m_drag == Drag::Stroke && m_board)
        m_board->commit();
    const bool was = m_dragStarted;
    m_drag = Drag::None;
    m_dragStarted = false;
    m_dragItems.clear();
    m_dragHandle = -1;
    m_pen = Geo::Pen();
    m_connectFrom = m_connectTarget = 0;
    m_chromeDirty = true;
    if (was) {
        // Dragging things about only ever pushes layers up; with the drag
        // over, they are worked out afresh.
        m_layersStale = true;
        emit busyChanged();
    }
    // Every press that was followed ends here, whatever it came to.
    emit pressedChanged();
    updateCursor(m_pointerItem);
    requestFrame();
}

// --- Editing text -------------------------------------------------------------------------

void CanvasView::attachEditor(QQuickTextDocument *document) {
    if (!document || !document->textDocument())
        return;
    delete m_highlighter;
    m_highlighter = new TitleHighlighter(document->textDocument());
    m_highlighter->setStyle(m_editorStyle);
}

void CanvasView::editSelection() {
    if (!mayChange())
        return;
    if (const Item *item = soleSelection())
        beginEdit(item->id, false);
}

void CanvasView::beginEdit(quint32 id, bool isNew) {
    const Item *item = m_board ? m_board->item(id) : nullptr;
    if (!item || !item->hasText()) {
        if (m_board && m_board->isOpen())
            m_board->commit();
        return;
    }
    if (editing())
        commitEdit();
    // Everything typed is one step of undo, and is about this item from the
    // start, typed in yet or not.
    if (!m_board->isOpen())
        m_board->begin();
    m_board->hold(id);
    m_editing = id;
    m_editingNew = isNew;
    m_editorText = item->text;
    m_chromeDirty = true;
    // Its text moves into the editor.
    markDirty(id);
    refreshEditor();
    emit editingChanged();
}

void CanvasView::refreshEditor() {
    const Item *item = editing() && m_board ? m_board->item(m_editing) : nullptr;
    if (!item)
        return;
    const TextStyle style = TextMetrics::styleFor(*item, m_palette);
    // Laid out as the board will lay it out, so the editor sits exactly
    // where the text will be. Empty text still has a line for the cursor.
    const TextBlock block(item->text.isEmpty() ? QStringLiteral(" ") : item->text, style);
    QPointF corner = item->rect.topLeft() + block.origin();
    if (item->kind == Item::Kind::Connector)
        corner = TextMetrics::connectorLabelCorner(*item, block.size());
    m_editorStyle = style;
    m_editorPosition = toItem(corner);
    m_editorCentered = style.wraps() && (style.alignment & Qt::AlignHCenter);
    m_editorSingleLine = item->kind == Item::Kind::Frame || item->kind == Item::Kind::Connector;
    if (m_highlighter)
        m_highlighter->setStyle(style);
    emit editorChanged();
}

void CanvasView::editorEdited(const QString &text) {
    const Item *current = editing() && m_board ? m_board->item(m_editing) : nullptr;
    if (!current)
        return;
    Item item = *current;
    item.text = m_editorSingleLine ? QString(text).replace('\n', ' ') : text;
    if (item.text == current->text)
        return;
    // Notes and shapes grow with what is typed; text is as big as its type.
    TextMetrics::fit(item);
    m_board->set(item);
    refreshEditor();
}

void CanvasView::commitEdit() {
    if (!editing() || !m_board)
        return;
    const quint32 id = m_editing;
    const bool isNew = m_editingNew;
    m_editing = 0;
    const Item *item = m_board->item(id);
    if (item && item->kind == Item::Kind::Text && item->text.trimmed().isEmpty()) {
        // Text with nothing in it is nothing: it leaves no trace.
        if (isNew) {
            m_board->rollback();
        } else {
            m_board->remove(id);
            m_board->commit();
        }
    } else {
        m_board->commit();
        if (item)
            markDirty(id);
    }
    m_chromeDirty = true;
    emit editingChanged();
    requestFrame();
}

// --- Doing things --------------------------------------------------------------------------

void CanvasView::newNote() {
    if (!mayChange() || editing())
        return;
    Item note;
    note.kind = Item::Kind::Note;
    note.color = m_noteColor;
    note.rect = QRectF(freeCorner(rounded(insertPoint() - QPointF(Defaults::noteSize, Defaults::noteSize) / 2),
                                  Item::Kind::Note),
                       QSizeF(Defaults::noteSize, Defaults::noteSize));
    setToolTo(Tool::Select);
    createItem(note, true);
}

void CanvasView::insertImage(const QString &asset, QSize naturalSize, int position) {
    if (!mayChange() || asset.isEmpty() || naturalSize.isEmpty())
        return;
    // Neither a screenful nor a sliver: a size to arrange and to see.
    const QSizeF size = Defaults::pictureSize(naturalSize);
    Item image;
    image.kind = Item::Kind::Image;
    image.asset = asset;
    image.rect = QRectF(freeCorner(rounded(insertPoint() - QPointF(size.width(), size.height()) / 2 +
                                           QPointF(24, 24) * position),
                                   Item::Kind::Image),
                        size);
    setToolTo(Tool::Select);
    createItem(image, false);
}

void CanvasView::insertText(const QString &text) {
    if (!mayChange() || text.trimmed().isEmpty())
        return;
    Item item;
    item.kind = Item::Kind::Text;
    item.fontSize = Defaults::textSize;
    item.text = Item::fitted(text.trimmed());
    if (item.text.size() < text.trimmed().size())
        emit textLeftOut();
    // Pasted prose wraps at a readable width rather than running on.
    QSizeF natural = QSizeF();
    TextMetrics::fit(item);
    natural = item.rect.size();
    if (natural.width() > 480) {
        item.wrap = 480;
        TextMetrics::fit(item);
    }
    item.rect.moveTo(freeCorner(rounded(insertPoint() - QPointF(item.rect.width(), item.rect.height()) / 2),
                                Item::Kind::Text));
    setToolTo(Tool::Select);
    createItem(item, false);
}

void CanvasView::selectAll() {
    // Selecting changes nothing on the board, but while the mouse is down
    // what is selected is what the press moves, and the tool in hand is what
    // it draws with: neither changes under it.
    if (!m_board || editing() || pressed())
        return;
    setToolTo(Tool::Select);
    setSelection(m_board->order());
}

void CanvasView::clearSelection() {
    // Nor is the selection emptied under a press.
    if (!pressed())
        setSelection({});
}

void CanvasView::setReadOnly(bool readOnly) {
    if (readOnly == m_readOnly)
        return;
    m_readOnly = readOnly;
    if (m_readOnly) {
        // From now on the board is only looked at. What was being written
        // stays as it was written, what the mouse was in the middle of is
        // put back, and a tool that makes things is put down.
        if (editing())
            commitEdit();
        cancelDrag();
        if (m_tool != Tool::Select && m_tool != Tool::Pan)
            setToolTo(Tool::Select);
    }
    // The handles come and go with it.
    m_chromeDirty = true;
    updateCursor(m_pointerItem);
    requestFrame();
    emit readOnlyChanged();
}

void CanvasView::deleteSelection() {
    if (!mayChange() || editing() || selection().isEmpty())
        return;
    const QVector<quint32> ids = selection();
    m_board->begin();
    m_board->remove(ids);
    m_board->commit();
}

QByteArray CanvasView::serialize(const QVector<quint32> &ids) const {
    // Back to front, as they are stacked.
    QVector<quint32> ordered = ids;
    std::sort(ordered.begin(), ordered.end(),
              [this](quint32 a, quint32 b) { return m_board->depth(a) < m_board->depth(b); });
    // What is copied stands on its own, since it may be pasted anywhere: a
    // connector's end that was on something left behind is where it was,
    // and on nothing.
    const QSet<quint32> copied(ids.constBegin(), ids.constEnd());
    QJsonArray items;
    for (quint32 id : std::as_const(ordered))
        if (const Item *item = m_board->item(id)) {
            Item copy = *item;
            for (quint32 *end : {&copy.from, &copy.to})
                if (!copied.contains(*end))
                    *end = 0;
            items.append(Board::itemToJson(copy));
        }
    return QJsonDocument(QJsonObject{{"sprawl", Board::formatVersion}, {"items", items}, {"assets", m_assetFolder}})
        .toJson(QJsonDocument::Compact);
}

QVector<quint32> CanvasView::addCopies(const QVector<Item> &items, QPointF offset) {
    QHash<quint32, quint32> ids, groups;
    QVector<quint32> added;
    // What is being copied, by the ids it has where it came from.
    QSet<quint32> originals;
    for (const Item &item : items)
        if (item.id)
            originals.insert(item.id);
    // A copied connector, and what its ends were on that is copied after it.
    struct Join {
        quint32 connector, from, to;
    };
    QVector<Join> joins;
    m_board->begin();
    for (Item item : items) {
        const quint32 original = item.id;
        item.rect.translate(offset);
        item.p1 += offset;
        item.p2 += offset;
        // Copies of grouped items are a group of their own.
        if (item.group) {
            if (!groups.contains(item.group))
                groups.insert(item.group, m_board->newGroup());
            item.group = groups.value(item.group);
        }
        Join join{0, 0, 0};
        if (item.kind == Item::Kind::Connector) {
            // A copied connector joins the copies of what it joined. It goes
            // down joined to those already down, under it: drawn after what
            // it joins, as most are, it goes down as it is to be, and is not
            // changed again to be joined, one more change for the window to
            // catch up with. An end whose copy is not down yet is on nothing
            // until they all are, as the board would have it of an end whose
            // item it cannot find, and is joined then. One that came along
            // with something it joins keeps its other end on the original.
            // One copied on its own is on nothing: a line where it was put,
            // not a second one exactly over the first.
            const bool cameAlong = originals.contains(item.from) || originals.contains(item.to);
            for (const auto &[end, copied] : {std::pair{&item.from, &join.from}, std::pair{&item.to, &join.to}}) {
                if (originals.contains(*end) && ids.contains(*end)) {
                    *end = ids.value(*end);
                } else if (originals.contains(*end)) {
                    *copied = *end;
                    *end = 0;
                } else if (!(cameAlong && m_board->item(*end))) {
                    *end = 0;
                }
            }
        }
        const quint32 id = m_board->add(item);
        ids.insert(original, id);
        added.append(id);
        if (join.from || join.to) {
            join.connector = id;
            joins.append(join);
        }
    }
    for (const Join &join : std::as_const(joins)) {
        Item connector = *m_board->item(join.connector);
        if (join.from)
            connector.from = ids.value(join.from);
        if (join.to)
            connector.to = ids.value(join.to);
        m_board->set(connector);
    }
    m_board->commit();
    return added;
}

void CanvasView::duplicateSelection() {
    if (!mayChange() || editing() || selection().isEmpty())
        return;
    QVector<quint32> ordered = selection();
    std::sort(ordered.begin(), ordered.end(),
              [this](quint32 a, quint32 b) { return m_board->depth(a) < m_board->depth(b); });
    QVector<Item> items;
    for (quint32 id : std::as_const(ordered))
        items.append(*m_board->item(id));
    setSelection(addCopies(items, QPointF(16, 16)));
}

void CanvasView::copy() {
    if (!m_board || editing() || selection().isEmpty())
        return;
    auto *data = new QMimeData;
    data->setData(clipboardFormat, serialize(selection()));
    // And as plain text, for pasting what was written into anything else.
    QStringList texts;
    for (quint32 id : selection())
        if (const Item *item = m_board->item(id); item && !item->text.isEmpty())
            texts << item->text;
    if (!texts.isEmpty())
        data->setText(texts.join("\n\n"));
    QGuiApplication::clipboard()->setMimeData(data);
}

void CanvasView::cut() {
    // What may not be taken away is not copied either.
    if (!mayChange())
        return;
    copy();
    deleteSelection();
}

namespace {

// What can be pasted of the items on the clipboard, back to front. They are
// read as a board's file is, but anything could have put them there (Sprawl
// 0.1.0, for one), and what is wrong with one item leaves out that item, not
// the rest. A connector's end with nothing among them to be on is where it
// was, and on nothing, as copying it without what it is on leaves it; and a
// connector that does not say where that is has nowhere to be, so is left
// out. Text from before an item held only so much is cut to what one holds,
// and `shortened` says so; `leftOut` says that something was left out, and
// it is all of it when the clipboard is not one this Sprawl reads.
QVector<Item> pasteable(const QJsonObject &root, bool &shortened, bool &leftOut) {
    // As in a file: a format this Sprawl reads, and the items in a list.
    const QJsonValue version = root.value("sprawl"), list = root.value("items");
    if (!version.isDouble() || version.toInt() > Board::formatVersion || (!list.isUndefined() && !list.isArray())) {
        leftOut = true;
        return {};
    }
    // Each item that could be read, and what it was read from.
    struct Read {
        Item item;
        QJsonObject object;
        bool shortened;
    };
    QVector<Read> read;
    // The ids given, and those of the things a connector can be on.
    QSet<quint32> ids, attachable;
    const QJsonArray array = list.toArray();
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        QJsonObject object = value.toObject();
        const QString text = object.value("text").toString();
        if (text.size() > Item::longestText)
            object["text"] = Item::fitted(text);
        Item item;
        if (!Board::itemFromJson(object, item).isEmpty())
            continue;
        // An id given twice is the first one's: the second is wrong, as it
        // would be in a file.
        if (item.id) {
            if (ids.contains(item.id))
                continue;
            ids.insert(item.id);
            if (Board::canAttach(item))
                attachable.insert(item.id);
        }
        const bool fitted = item.hasText() && text.size() > Item::longestText;
        read.append({std::move(item), std::move(object), fitted});
    }
    QVector<Item> items;
    items.reserve(read.size());
    for (Read &each : read) {
        if (each.item.kind == Item::Kind::Connector) {
            // Its ends with nothing to be on come off, and it is read again
            // as what it is then: a free end has to say where it is.
            const bool from = each.item.from && !attachable.contains(each.item.from);
            const bool to = each.item.to && !attachable.contains(each.item.to);
            if (from)
                each.object.remove("from");
            if (to)
                each.object.remove("to");
            if ((from || to) && !Board::itemFromJson(each.object, each.item).isEmpty())
                continue;
        }
        shortened = shortened || each.shortened;
        items.append(std::move(each.item));
    }
    // Each on the clipboard is pasted, or left out.
    leftOut = items.size() < array.size();
    return items;
}

} // namespace

bool CanvasView::paste() {
    if (!mayChange() || editing())
        return true;
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data || !data->hasFormat(clipboardFormat))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(data->data(clipboardFormat)).object();
    bool shortened = false, leftOut = false;
    QVector<Item> items = pasteable(root, shortened, leftOut);
    if (items.isEmpty()) {
        // With nothing at all to paste, the window pastes the clipboard as
        // anything else would: as the text of what was copied, if there is
        // any. That none of it could be is said once that is done, so that
        // nothing said of the text is said over it.
        if (leftOut)
            QMetaObject::invokeMethod(this, &CanvasView::itemsLeftOut, Qt::QueuedConnection);
        return false;
    }

    // Pictures copied from another board come along with their files. The
    // clipboard only says where to look for them, and anything running can
    // put something on the clipboard: a picture is taken in as one dropped
    // on the board would be, and what is not a picture is not taken at all.
    // Each is taken in whatever this board has already: a picture here with
    // the same name may be another picture altogether, and what is taken in
    // is named for what it holds. One that cannot be had from there is
    // pasted as a picture that is missing, but not as this board's picture
    // of the same name, unless that name is one of what a picture holds.
    const QString from = root.value("assets").toString();
    if (!from.isEmpty() && from != m_assetFolder && !m_assetFolder.isEmpty()) {
        static const QRegularExpression namedForContents(QStringLiteral("^[0-9a-f]{16}(\\.[0-9a-z]+)?$"));
        QHash<QString, QString> taken;
        QVector<bool> dropped(items.size(), false);
        QSet<quint32> droppedIds;
        for (qsizetype i = 0; i < items.size(); ++i) {
            Item &item = items[i];
            if (item.kind != Item::Kind::Image)
                continue;
            auto found = taken.constFind(item.asset);
            if (found == taken.constEnd())
                found = taken.insert(item.asset, Library::importAsset(m_assetFolder, from, item.asset));
            if (!found->isEmpty()) {
                item.asset = *found;
            } else if (QFile::exists(m_assetFolder + "/" + item.asset) &&
                       !namedForContents.match(item.asset).hasMatch()) {
                dropped[i] = true;
                if (item.id)
                    droppedIds.insert(item.id);
            }
        }
        // A connector on a picture left out goes with it, as one on an item
        // taken off the board does.
        for (qsizetype i = 0; i < items.size(); ++i)
            if (items[i].kind == Item::Kind::Connector &&
                (droppedIds.contains(items[i].from) || droppedIds.contains(items[i].to)))
                dropped[i] = true;
        if (dropped.contains(true)) {
            QVector<Item> kept;
            for (qsizetype i = 0; i < items.size(); ++i)
                if (!dropped[i])
                    kept.append(std::move(items[i]));
            items = std::move(kept);
            leftOut = true;
        }
        if (items.isEmpty()) {
            QMetaObject::invokeMethod(this, &CanvasView::itemsLeftOut, Qt::QueuedConnection);
            return true;
        }
    }

    // Centered where the mouse is, as anything inserted is.
    QRectF bounds;
    for (const Item &item : std::as_const(items))
        bounds = bounds.isNull() ? item.rect : bounds | item.rect;
    // Pasted again in the same place, they step aside from the last ones.
    QPointF offset = rounded(insertPoint() - bounds.center());
    const Item &first = items.first();
    offset += freeCorner(first.rect.topLeft() + offset, first.kind) - (first.rect.topLeft() + offset);
    setToolTo(Tool::Select);
    setSelection(addCopies(items, offset));
    // What was left out is said, or else text that was cut short, once all
    // the rest is down: a notice is up for only so long, and a big paste
    // takes a while.
    if (leftOut)
        emit itemsLeftOut();
    else if (shortened)
        emit textLeftOut();
    return true;
}

void CanvasView::groupSelection() {
    if (!mayChange() || editing() || selection().size() < 2)
        return;
    const quint32 group = m_board->newGroup();
    m_board->begin();
    for (quint32 id : selection()) {
        Item item = *m_board->item(id);
        item.group = group;
        m_board->set(item);
    }
    m_board->commit();
}

void CanvasView::ungroupSelection() {
    if (!mayChange() || editing() || selection().isEmpty())
        return;
    m_board->begin();
    for (quint32 id : selection()) {
        Item item = *m_board->item(id);
        item.group = 0;
        m_board->set(item);
    }
    m_board->commit();
}

void CanvasView::bringToFront() {
    if (mayChange() && !selection().isEmpty())
        m_board->restack(selection(), true);
}

void CanvasView::sendToBack() {
    if (mayChange() && !selection().isEmpty())
        m_board->restack(selection(), false);
}

void CanvasView::nudge(qreal dx, qreal dy) {
    if (!mayChange() || editing() || selection().isEmpty())
        return;
    // What a drag would take along, moved as a drag moves it, in a step of
    // its own; nothing of a drag's own is borrowed for it.
    m_board->begin();
    moveBy(carried(), rounded(QPointF(dx, dy)));
    m_board->commit();
}

void CanvasView::setSelectionColor(const QString &color) {
    if (!color.isEmpty() && !Item::colorNames().contains(color))
        return;
    if (!m_board || selection().isEmpty()) {
        // Nothing selected: it is the pen's color that is being picked.
        setPenColor(color == m_penColor ? QString() : color);
        return;
    }
    if (!mayChange() || editing())
        return;
    // Picking the color everything already has takes it off again, back to
    // the kind's own neutral. Notes are always some color.
    const bool clear = selectionColor() == color;
    m_board->begin();
    for (quint32 id : selection()) {
        Item item = *m_board->item(id);
        if (item.kind == Item::Kind::Image)
            continue;
        if (item.kind == Item::Kind::Note) {
            if (color.isEmpty())
                continue;
            item.color = color;
            m_noteColor = color;
        } else {
            item.color = clear ? QString() : color;
        }
        m_board->set(item);
    }
    m_board->commit();
    emit selectionChanged();
}

void CanvasView::setSelectionShape(const QString &name) {
    if (!mayChange() || editing() || selection().isEmpty())
        return;
    const int index = int(Board::shapes().indexOf(name));
    if (index < 0)
        return;
    const Item::Shape shape = Item::Shape(index);
    m_board->begin();
    for (quint32 id : selection()) {
        Item item = *m_board->item(id);
        if (item.kind != Item::Kind::Shape || item.shape == shape)
            continue;
        item.shape = shape;
        // Some outlines have less room for what is written on them.
        TextMetrics::fit(item);
        m_board->set(item);
    }
    m_board->commit();
    // The next shape drawn is this one too.
    setShape(name);
}

void CanvasView::toggleBold() {
    if (!mayChange() || selection().isEmpty())
        return;
    bool anyPlain = false;
    for (quint32 id : selection())
        if (const Item *item = m_board->item(id); item && item->kind == Item::Kind::Text && !item->bold)
            anyPlain = true;
    const bool wasOpen = m_board->isOpen();
    if (!wasOpen)
        m_board->begin();
    for (quint32 id : selection()) {
        Item item = *m_board->item(id);
        if (item.kind != Item::Kind::Text)
            continue;
        item.bold = anyPlain;
        TextMetrics::fit(item);
        m_board->set(item);
    }
    if (!wasOpen)
        m_board->commit();
}

void CanvasView::resizeText(int direction) {
    if (!mayChange() || selection().isEmpty())
        return;
    const bool wasOpen = m_board->isOpen();
    if (!wasOpen)
        m_board->begin();
    for (quint32 id : selection()) {
        Item item = *m_board->item(id);
        if (item.kind != Item::Kind::Text)
            continue;
        // The next size up or down the scale from wherever it is.
        qreal size = item.fontSize;
        if (direction > 0) {
            for (qreal step : textSizes)
                if (step > item.fontSize + 0.5) {
                    size = step;
                    break;
                }
        } else {
            for (qreal step : textSizes)
                if (step < item.fontSize - 0.5)
                    size = step;
        }
        if (item.wrap > 0)
            item.wrap = std::round(item.wrap * size / item.fontSize);
        item.fontSize = size;
        TextMetrics::fit(item);
        m_board->set(item);
    }
    if (!wasOpen)
        m_board->commit();
}

void CanvasView::undo() {
    if (!mayChange())
        return;
    if (editing())
        commitEdit();
    m_board->undo();
}

void CanvasView::redo() {
    if (!mayChange())
        return;
    if (editing())
        commitEdit();
    m_board->redo();
}

QVariantList CanvasView::redoKeys() const { return redoKeysWith(QKeySequence::keyBindings(QKeySequence::Redo)); }

QVariantList CanvasView::redoKeysWith(const QList<QKeySequence> &desktop) {
    // Some desktops count Ctrl+Y among their own. Named again, it would be
    // ambiguous, and an ambiguous shortcut never fires.
    QVariantList keys{int(QKeySequence::Redo)};
    if (!desktop.contains(QKeySequence(Qt::CTRL | Qt::Key_Y)))
        keys.append(QStringLiteral("Ctrl+Y"));
    return keys;
}

bool CanvasView::escape() {
    if (m_drag != Drag::None) {
        cancelDrag();
        return true;
    }
    if (editing()) {
        commitEdit();
        return true;
    }
    if (m_tool != Tool::Select) {
        setToolTo(Tool::Select);
        return true;
    }
    if (!selection().isEmpty()) {
        setSelection({});
        return true;
    }
    return false;
}
