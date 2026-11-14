// The half of CanvasView that listens: the view, the selection, the tools,
// and the editor that opens over an item. scene.cpp is the half that draws.
#include "canvasview.h"

#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QNativeGestureEvent>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSyntaxHighlighter>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

#include "imagestore.h"

namespace {

const qreal minimumZoom = 0.1, maximumZoom = 8;
// Where + and - stop on their way in and out.
const qreal zoomStops[] = {0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.25, 1.5, 2, 3, 4, 6, 8};
// The sizes text steps through.
const qreal textSizes[] = {11, 12, 13, 16, 20, 28, 40, 56, 80};
// The most points one stroke of the pen is given. A stroke that goes on past
// that is put on the board and carried on as another, so that no single
// stroke is ever more than the GPU is asked to draw at once.
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
        connect(m_board, &Board::reset, this, &CanvasView::onReset);
        connect(m_board, &Board::historyChanged, this, &CanvasView::historyChanged);
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
    m_loadBudget = qMax<qint64>(0, bytes);
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

void CanvasView::onReset() {
    // A different board: nothing of the last one carries over.
    m_drag = Drag::None;
    m_dragStarted = false;
    m_dragItems.clear();
    m_draftPoints.clear();
    m_connectTarget = 0;
    if (m_editing) {
        m_editing = 0;
        emit editingChanged();
    }
    m_selection.clear();
    m_selected.clear();
    m_ready.clear();
    m_built.clear();
    m_gone.clear();
    m_place.clear();
    m_rehome.clear();
    m_layersStale = true;
    m_sceneReset = true;
    markAllDirty();
    refreshSelectionBox();
    emit selectionChanged();
    emit busyChanged();
}

void CanvasView::onItemChanged(quint32 id, Board::Change change) {
    switch (change) {
    case Board::Change::Added:
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
        if (m_selected.contains(id)) {
            QVector<quint32> rest = m_selection;
            rest.removeOne(id);
            setSelection(rest);
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
        // Its own measuring is not news to it.
        if (!m_measuring)
            markDirty(id);
        relayer(id);
        break;
    }
    if (m_selected.contains(id)) {
        m_chromeDirty = true;
        refreshSelectionBox();
        // What the bar by the selection shows of it (its color, its shape)
        // follows every change to it, an undo included.
        if (change == Board::Change::Edited)
            emit selectionChanged();
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

// --- The view ---------------------------------------------------------------------

void CanvasView::viewMoved() {
    m_viewDirty = true;
    m_chromeDirty = true;
    const QPointF pointer = toCanvas(m_pointerItem);
    if (pointer != m_pointer) {
        m_pointer = pointer;
        emit pointerChanged();
    }
    refreshSelectionBox();
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
    for (quint32 id : m_selection)
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
    if (ids == m_selection)
        return;
    m_selection = ids;
    m_selected = QSet<quint32>(ids.constBegin(), ids.constEnd());
    m_chromeDirty = true;
    refreshSelectionBox();
    emit selectionChanged();
    requestFrame();
}

QVector<quint32> CanvasView::withGroups(const QVector<quint32> &ids) const {
    QVector<quint32> all;
    QSet<quint32> groups;
    for (quint32 id : ids) {
        const Item *item = m_board->item(id);
        if (!item)
            continue;
        if (item->group == 0) {
            all.append(id);
        } else if (!groups.contains(item->group)) {
            groups.insert(item->group);
            all += m_board->membersOf(item->group);
        }
    }
    return all;
}

const Item *CanvasView::soleSelection() const {
    return m_board && m_selection.size() == 1 ? m_board->item(m_selection.first()) : nullptr;
}

QString CanvasView::selectionKind() const {
    QString kind;
    for (quint32 id : m_selection) {
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
    for (quint32 id : m_selection) {
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
    for (quint32 id : m_selection) {
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

void CanvasView::refreshSelectionBox() {
    QRectF box, area;
    if (m_board)
        for (quint32 id : std::as_const(m_selection)) {
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
    emit selectionBoxChanged();
}

QVector<CanvasView::HandleSpot> CanvasView::handleSpots() const {
    const Item *item = soleSelection();
    if (!item)
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

QString CanvasView::tool() const { return toolNames.value(int(m_tool)); }

void CanvasView::setTool(const QString &name) {
    const int index = toolNames.indexOf(name);
    if (index >= 0)
        setToolTo(Tool(index));
}

void CanvasView::setToolTo(Tool tool) {
    if (tool == m_tool)
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
    for (quint32 id : m_selection) {
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
    if (!m_board || editing() || m_selection.isEmpty() || index < 0)
        return;
    m_board->begin();
    for (quint32 id : std::as_const(m_selection)) {
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
    emit penChanged();
}

void CanvasView::setPenWidth(qreal width) {
    width = qBound(1.0, width, 64.0);
    if (width == m_penWidth)
        return;
    m_penWidth = width;
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
        return;
    }

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
                // Shift adds to the selection, or takes away from it.
                QVector<quint32> next = m_selection;
                const bool had = m_selected.contains(hit);
                for (quint32 id : group) {
                    next.removeOne(id);
                    if (!had)
                        next.append(id);
                }
                setSelection(next);
                if (had)
                    break;
            } else if (!m_selected.contains(hit)) {
                setSelection(group);
            }
            m_drag = Drag::Move;
            m_connectFrom = hit; // remembered for a click that turns out not to be a drag
        } else {
            m_marqueeBase = shift ? m_selection : QVector<quint32>();
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
        m_draftPoints = {canvas};
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
        if (m_drag == Drag::Move)
            startMove();
        else if (m_drag == Drag::Resize || m_drag == Drag::End)
            m_board->begin();
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
    case Drag::Stroke: {
        // Points closer than most of a screen pixel add nothing but weight.
        const QPointF step = (canvas - m_draftPoints.last()) * m_zoom;
        if (std::hypot(step.x(), step.y()) >= 0.75)
            m_draftPoints.append(canvas);
        if (m_draftPoints.size() >= longestStroke) {
            finishStroke();
            m_draftPoints = {canvas};
        }
        break;
    }
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
        } else if (!(event->modifiers() & Qt::ShiftModifier) && m_selection.size() > 1) {
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
    // The second press of a double click starts nothing of its own.
    cancelDrag();
    m_dragButton = Qt::NoButton;
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
    m_dragItems.clear();
    QVector<quint32> moving = m_selection;
    // A frame carries what sits inside it.
    QVector<quint32> inside;
    for (quint32 id : std::as_const(m_selection)) {
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
    for (quint32 id : std::as_const(moving))
        if (const Item *item = m_board->item(id))
            m_dragItems.insert(id, *item);
}

void CanvasView::dragMove(QPointF canvasPoint) {
    // By whole units, so things stay on the pixel grid at 100%.
    const QPointF delta = rounded(canvasPoint - m_pressCanvas);
    for (auto it = m_dragItems.constBegin(); it != m_dragItems.constEnd(); ++it) {
        Item moved = it.value();
        moved.rect.translate(delta);
        if (moved.kind == Item::Kind::Connector) {
            moved.p1 += delta;
            moved.p2 += delta;
        }
        m_board->set(moved);
    }
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
    const qreal least = start.kind == Item::Kind::Stroke ? 1 : Defaults::minimumSize;

    // The edges the handle holds follow the mouse; the others stay.
    qreal l = from.left() + (left ? delta.x() : 0), r = from.right() + (right ? delta.x() : 0);
    qreal t = from.top() + (top ? delta.y() : 0), b = from.bottom() + (bottom ? delta.y() : 0);
    if (left)
        l = qMin(l, r - least);
    if (right)
        r = qMax(r, l + least);
    if (top)
        t = qMin(t, b - least);
    if (bottom)
        b = qMax(b, t + least);
    l = std::round(l), r = std::round(r), t = std::round(t), b = std::round(b);

    // Pictures and type keep their proportions; anything does with Shift.
    const bool proportional = (keepAspect || start.kind == Item::Kind::Image ||
                               (start.kind == Item::Kind::Text && isCorner(handle))) &&
                              from.width() > 0 && from.height() > 0;
    if (proportional) {
        const qreal ratio = from.width() / from.height();
        qreal w = r - l, h = b - t;
        if (isCorner(handle)) {
            if (w / ratio > h)
                h = w / ratio;
            else
                w = h * ratio;
            l = left ? r - w : l;
            r = l + w;
            t = top ? b - h : t;
            b = t + h;
        } else if (left || right) {
            h = w / ratio;
            t = from.center().y() - h / 2;
            b = t + h;
        } else {
            w = h * ratio;
            l = from.center().x() - w / 2;
            r = l + w;
        }
    }
    const QRectF to(QPointF(l, t), QPointF(r, b));

    Item item = start;
    switch (start.kind) {
    case Item::Kind::Text: {
        if (isCorner(handle)) {
            const qreal scale = to.height() / from.height();
            item.fontSize = qBound(6.0, std::round(start.fontSize * scale), 400.0);
            if (start.wrap > 0)
                item.wrap = std::round(start.wrap * item.fontSize / start.fontSize);
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

void CanvasView::finishStroke() {
    if (m_draftPoints.isEmpty())
        return;
    // Keep the points the path needs to within a third of a screen pixel.
    QVector<QPointF> points = Geo::simplify(m_draftPoints, 0.35 / m_zoom);
    const QRectF bounds = Geo::boundsOf(points);
    for (QPointF &point : points)
        point -= bounds.topLeft();
    Item stroke;
    stroke.kind = Item::Kind::Stroke;
    stroke.rect = bounds;
    stroke.points = points;
    stroke.width = m_penWidth;
    stroke.color = m_penColor;
    m_draftPoints.clear();
    m_board->add(stroke);
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
    if (m_dragStarted && m_board && (m_drag == Drag::Move || m_drag == Drag::Resize || m_drag == Drag::End))
        m_board->rollback();
    if (m_drag == Drag::Marquee)
        setSelection(m_marqueeBase);
    endDrag();
}

void CanvasView::endDrag() {
    const bool was = m_dragStarted;
    m_drag = Drag::None;
    m_dragStarted = false;
    m_dragItems.clear();
    m_dragHandle = -1;
    m_draftPoints.clear();
    m_connectFrom = m_connectTarget = 0;
    m_chromeDirty = true;
    if (was) {
        // Dragging things about only ever pushes layers up; with the drag
        // over, they are worked out afresh.
        m_layersStale = true;
        emit busyChanged();
    }
    updateCursor(m_pointerItem);
    refreshSelectionBox();
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
    // Everything typed is one step of undo.
    if (!m_board->isOpen())
        m_board->begin();
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
    if (!m_board || editing())
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
    if (!m_board || asset.isEmpty() || naturalSize.isEmpty())
        return;
    // A screenful of photo is too much: start it at a size to arrange.
    QSizeF size = naturalSize;
    if (size.width() > 420 || size.height() > 320)
        size.scale(420, 320, Qt::KeepAspectRatio);
    Item image;
    image.kind = Item::Kind::Image;
    image.asset = asset;
    size = QSizeF(std::round(size.width()), std::round(size.height()));
    image.rect = QRectF(freeCorner(rounded(insertPoint() - QPointF(size.width(), size.height()) / 2 +
                                           QPointF(24, 24) * position),
                                   Item::Kind::Image),
                        size);
    setToolTo(Tool::Select);
    createItem(image, false);
}

void CanvasView::insertText(const QString &text) {
    if (!m_board || text.trimmed().isEmpty())
        return;
    Item item;
    item.kind = Item::Kind::Text;
    item.fontSize = Defaults::textSize;
    item.text = text.trimmed();
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
    if (!m_board || editing())
        return;
    setToolTo(Tool::Select);
    setSelection(m_board->order());
}

void CanvasView::clearSelection() { setSelection({}); }

void CanvasView::deleteSelection() {
    if (!m_board || editing() || m_selection.isEmpty())
        return;
    const QVector<quint32> ids = m_selection;
    m_board->begin();
    for (quint32 id : ids)
        m_board->remove(id);
    m_board->commit();
}

QByteArray CanvasView::serialize(const QVector<quint32> &ids) const {
    // Back to front, as they are stacked.
    QVector<quint32> ordered = ids;
    std::sort(ordered.begin(), ordered.end(),
              [this](quint32 a, quint32 b) { return m_board->depth(a) < m_board->depth(b); });
    QJsonArray items;
    for (quint32 id : std::as_const(ordered))
        if (const Item *item = m_board->item(id))
            items.append(Board::itemToJson(*item));
    return QJsonDocument(QJsonObject{{"sprawl", Board::formatVersion}, {"items", items}, {"assets", m_assetFolder}})
        .toJson(QJsonDocument::Compact);
}

QVector<quint32> CanvasView::addCopies(const QVector<Item> &items, QPointF offset) {
    QHash<quint32, quint32> ids, groups;
    QVector<quint32> added;
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
        const quint32 id = m_board->add(item);
        ids.insert(original, id);
        added.append(id);
    }
    // A copied connector joins the copies of what it joined. An end whose
    // item was not copied stays attached to the original when it is on this
    // board, and is free otherwise.
    for (quint32 id : std::as_const(added)) {
        const Item *copy = m_board->item(id);
        if (!copy || copy->kind != Item::Kind::Connector)
            continue;
        Item connector = *copy;
        for (quint32 *end : {&connector.from, &connector.to})
            if (*end)
                *end = ids.value(*end, m_board->item(*end) ? *end : 0);
        m_board->set(connector);
    }
    m_board->commit();
    return added;
}

void CanvasView::duplicateSelection() {
    if (!m_board || editing() || m_selection.isEmpty())
        return;
    QVector<quint32> ordered = m_selection;
    std::sort(ordered.begin(), ordered.end(),
              [this](quint32 a, quint32 b) { return m_board->depth(a) < m_board->depth(b); });
    QVector<Item> items;
    for (quint32 id : std::as_const(ordered))
        items.append(*m_board->item(id));
    setSelection(addCopies(items, QPointF(16, 16)));
}

void CanvasView::copy() {
    if (!m_board || editing() || m_selection.isEmpty())
        return;
    auto *data = new QMimeData;
    data->setData(clipboardFormat, serialize(m_selection));
    // And as plain text, for pasting what was written into anything else.
    QStringList texts;
    for (quint32 id : std::as_const(m_selection))
        if (const Item *item = m_board->item(id); item && !item->text.isEmpty())
            texts << item->text;
    if (!texts.isEmpty())
        data->setText(texts.join("\n\n"));
    QGuiApplication::clipboard()->setMimeData(data);
}

void CanvasView::cut() {
    copy();
    deleteSelection();
}

bool CanvasView::paste() {
    if (!m_board || editing())
        return true;
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data || !data->hasFormat(clipboardFormat))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(data->data(clipboardFormat)).object();
    QVector<Item> items;
    if (!Board::parse(root, items).isEmpty() || items.isEmpty())
        return true;

    // Pictures copied from another board come along with their files.
    const QString from = root.value("assets").toString();
    if (!from.isEmpty() && from != m_assetFolder && !m_assetFolder.isEmpty())
        for (const Item &item : std::as_const(items))
            if (item.kind == Item::Kind::Image && !QFile::exists(m_assetFolder + "/" + item.asset)) {
                QDir().mkpath(m_assetFolder);
                QFile::copy(from + "/" + item.asset, m_assetFolder + "/" + item.asset);
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
    return true;
}

void CanvasView::groupSelection() {
    if (!m_board || m_selection.size() < 2)
        return;
    const quint32 group = m_board->newGroup();
    m_board->begin();
    for (quint32 id : std::as_const(m_selection)) {
        Item item = *m_board->item(id);
        item.group = group;
        m_board->set(item);
    }
    m_board->commit();
}

void CanvasView::ungroupSelection() {
    if (!m_board || m_selection.isEmpty())
        return;
    m_board->begin();
    for (quint32 id : std::as_const(m_selection)) {
        Item item = *m_board->item(id);
        item.group = 0;
        m_board->set(item);
    }
    m_board->commit();
}

void CanvasView::bringToFront() {
    if (m_board && !m_selection.isEmpty())
        m_board->restack(m_selection, true);
}

void CanvasView::sendToBack() {
    if (m_board && !m_selection.isEmpty())
        m_board->restack(m_selection, false);
}

void CanvasView::nudge(qreal dx, qreal dy) {
    if (!m_board || editing() || m_selection.isEmpty())
        return;
    m_pressCanvas = QPointF(0, 0);
    startMove();
    dragMove(QPointF(dx, dy));
    m_board->commit();
    m_dragItems.clear();
}

void CanvasView::setSelectionColor(const QString &color) {
    if (!color.isEmpty() && !Item::colorNames().contains(color))
        return;
    if (!m_board || m_selection.isEmpty()) {
        // Nothing selected: it is the pen's color that is being picked.
        setPenColor(color == m_penColor ? QString() : color);
        return;
    }
    // Picking the color everything already has takes it off again, back to
    // the kind's own neutral. Notes are always some color.
    const bool clear = selectionColor() == color;
    m_board->begin();
    for (quint32 id : std::as_const(m_selection)) {
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
    if (!m_board || editing() || m_selection.isEmpty())
        return;
    const int index = int(Board::shapes().indexOf(name));
    if (index < 0)
        return;
    const Item::Shape shape = Item::Shape(index);
    m_board->begin();
    for (quint32 id : std::as_const(m_selection)) {
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
    if (!m_board || m_selection.isEmpty())
        return;
    bool anyPlain = false;
    for (quint32 id : std::as_const(m_selection))
        if (const Item *item = m_board->item(id); item && item->kind == Item::Kind::Text && !item->bold)
            anyPlain = true;
    const bool wasOpen = m_board->isOpen();
    if (!wasOpen)
        m_board->begin();
    for (quint32 id : std::as_const(m_selection)) {
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
    if (!m_board || m_selection.isEmpty())
        return;
    const bool wasOpen = m_board->isOpen();
    if (!wasOpen)
        m_board->begin();
    for (quint32 id : std::as_const(m_selection)) {
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
    if (!m_board)
        return;
    if (editing())
        commitEdit();
    cancelDrag();
    m_board->undo();
}

void CanvasView::redo() {
    if (!m_board)
        return;
    if (editing())
        commitEdit();
    cancelDrag();
    m_board->redo();
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
    if (!m_selection.isEmpty()) {
        setSelection({});
        return true;
    }
    return false;
}
