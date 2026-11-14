#pragma once

#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QSet>
#include <QVariantAnimation>
#include <QVariantMap>
#include <memory>

#include "board.h"
#include "geometry.h"
#include "textlayout.h"
#include "theme.h"

class QQuickTextDocument;
class QSGTexture;
class ImageStore;
class TitleHighlighter;
struct SceneNodes;

// Where an item is drawn from: which layer it is in, and which part of the
// board it is on (see scene.cpp).
struct ScenePlace {
    int layer = 0;
    // The square of the board it is in, and which part of that square.
    qint32 column = 0, row = 0;
    quint8 part = 0;
    // Too big to belong to one square of the board.
    bool wide = false;
    bool operator==(const ScenePlace &) const = default;
};
inline size_t qHash(const ScenePlace &place, size_t seed = 0) {
    return qHashMulti(seed, place.layer, place.column, place.row, place.part, place.wide);
}

// What the canvas last asked the GPU to draw (see scene.cpp).
struct SceneLoad {
    // The groups the board is drawn in, and how many of them were in view.
    int buckets = 0, shown = 0;
    // The items in those; the triangles that were drawn of them; and roughly
    // what a GPU has to keep of those triangles to draw one frame, in bytes.
    int items = 0;
    qint64 triangles = 0, bytes = 0;
    // How much the pen's strokes were simplified: 0 is not at all.
    int strokeDetail = 0;
    bool strokesHidden = false;
    // Type smaller than this, in units, was left out.
    qreal smallestText = 0;
    // Something was left out to keep the frame within what is asked of the
    // GPU, not because it was too small to see.
    bool reduced = false;
    // What the editor asks for on top of that, for the text being written:
    // it draws that itself, and the board is allowed that much less.
    qint64 editor = 0;
};

// The board on screen: an endless surface to pan and zoom, and the tools
// that put things on it.
//
// It is one Qt Quick item that builds its own scene graph rather than one
// QML item per thing on the board. Panning and zooming change a single
// matrix; nothing is laid out, tessellated, or uploaded again. Items become
// scene graph nodes the first time they are needed, those on screen first
// and the rest a little each frame, so a board of any size opens at once,
// and they are arranged so that a board of thousands still draws in a few
// dozen calls (see scene.cpp).
class CanvasView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(Board *board READ board WRITE setBoard NOTIFY boardChanged)
    Q_PROPERTY(Theme *theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString assetFolder READ assetFolder WRITE setAssetFolder NOTIFY assetFolderChanged)

    Q_PROPERTY(qreal zoom READ zoom NOTIFY viewChanged)
    Q_PROPERTY(QPointF origin READ origin NOTIFY viewChanged)
    // The canvas point under the mouse, and whether the mouse is over the
    // canvas at all.
    Q_PROPERTY(QPointF pointer READ pointer NOTIFY pointerChanged)
    Q_PROPERTY(bool pointerInside READ pointerInside NOTIFY pointerChanged)

    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QString shape READ shape WRITE setShape NOTIFY toolChanged)
    // Every shape there is, in the order they are offered in.
    Q_PROPERTY(QStringList shapes READ shapes CONSTANT)
    // What the connector tool draws: its route, what is at its start and
    // its end, and its dash, each by name. And the same of the connectors
    // that are selected, where they all agree.
    Q_PROPERTY(QVariantMap line READ line NOTIFY toolChanged)
    Q_PROPERTY(QVariantMap selectionLine READ selectionLine NOTIFY selectionChanged)
    Q_PROPERTY(QString penColor READ penColor WRITE setPenColor NOTIFY penChanged)
    Q_PROPERTY(qreal penWidth READ penWidth WRITE setPenWidth NOTIFY penChanged)

    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    // What the selected items are when they are all one kind, their color
    // when they all share one, and whether that color can be cleared.
    Q_PROPERTY(QString selectionKind READ selectionKind NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionColor READ selectionColor NOTIFY selectionChanged)
    // The shape they all are, when they are all shapes and all the same.
    Q_PROPERTY(QString selectionShape READ selectionShape NOTIFY selectionChanged)
    // The box around the selection in this item's coordinates, for the
    // toolbar and the size badge to sit by.
    Q_PROPERTY(QRectF selectionBox READ selectionBox NOTIFY selectionBoxChanged)
    Q_PROPERTY(QString selectionSize READ selectionSize NOTIFY selectionBoxChanged)
    // Something is being dragged, so floating chrome keeps out of the way.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    // The text editor that opens over an item: where it goes and how its
    // text is set, so what is typed looks exactly like what stays.
    Q_PROPERTY(bool editing READ editing NOTIFY editingChanged)
    Q_PROPERTY(int longestText READ longestText CONSTANT)
    // The board can be looked at and not changed: it could not be read.
    Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly NOTIFY readOnlyChanged)
    Q_PROPERTY(QString editorText READ editorText NOTIFY editingChanged)
    Q_PROPERTY(QPointF editorPosition READ editorPosition NOTIFY editorChanged)
    Q_PROPERTY(qreal editorWidth READ editorWidth NOTIFY editorChanged)
    Q_PROPERTY(QFont editorFont READ editorFont NOTIFY editorChanged)
    Q_PROPERTY(QColor editorColor READ editorColor NOTIFY editorChanged)
    Q_PROPERTY(bool editorCentered READ editorCentered NOTIFY editorChanged)
    Q_PROPERTY(bool editorSingleLine READ editorSingleLine NOTIFY editorChanged)

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    // Frames drawn and items waiting to be built, for the benchmark.
    Q_PROPERTY(int pendingItems READ pendingItems NOTIFY pendingItemsChanged)
    // More is in view than is drawn in full: zooming in brings it back.
    Q_PROPERTY(bool simplified READ simplified NOTIFY loadChanged)

public:
    enum class Tool { Select, Pan, Note, Text, Shape, Pen, Connector, Frame };

    explicit CanvasView(QQuickItem *parent = nullptr);
    ~CanvasView() override;

    Board *board() const { return m_board; }
    void setBoard(Board *board);
    Theme *theme() const { return m_theme; }
    void setTheme(Theme *theme);
    QString assetFolder() const { return m_assetFolder; }
    void setAssetFolder(const QString &folder);

    qreal zoom() const { return m_zoom; }
    QPointF origin() const { return m_origin; }
    QPointF pointer() const { return m_pointer; }
    bool pointerInside() const { return m_pointerInside; }

    QString tool() const;
    void setTool(const QString &name);
    QString shape() const { return Board::shapeName(m_shape); }
    QStringList shapes() const { return Board::shapes(); }
    // What a shape is called, and its outline as the path of an icon on a
    // 24 unit grid: drawn from the same description the shape itself is.
    Q_INVOKABLE QString shapeTitle(const QString &shape) const;
    Q_INVOKABLE QString shapeIcon(const QString &shape) const;

    // Things there is a choice of, by kind: shape, route, start, end (what
    // is at each end of a connector), dash. What the choices are, what each
    // is called, and each as an icon on a 24 unit grid: the lines of it,
    // and the part of it that is filled, if any is.
    Q_INVOKABLE QStringList options(const QString &kind) const;
    Q_INVOKABLE QString optionTitle(const QString &kind, const QString &name) const;
    Q_INVOKABLE QString optionIcon(const QString &kind, const QString &name) const;
    Q_INVOKABLE QString optionSolid(const QString &kind, const QString &name) const;
    QVariantMap line() const;
    QVariantMap selectionLine() const;
    Q_INVOKABLE void setLine(const QString &kind, const QString &name);
    // Gives the selected connectors a route, an end, or a dash.
    Q_INVOKABLE void setSelectionLine(const QString &kind, const QString &name);
    // The route so many on from the tool's, around the end and back.
    Q_INVOKABLE void stepRoute(int by);
    void setShape(const QString &name);
    QString penColor() const { return m_penColor; }
    void setPenColor(const QString &color);
    qreal penWidth() const { return m_penWidth; }
    void setPenWidth(qreal width);

    int selectionCount() const { return int(m_selection.size()); }
    QString selectionKind() const;
    QString selectionColor() const;
    QString selectionShape() const;
    QRectF selectionBox() const { return m_selectionBox; }
    QString selectionSize() const { return m_selectionSize; }
    bool busy() const { return m_drag != Drag::None && m_dragStarted; }
    const QVector<quint32> &selection() const { return m_selection; }

    bool editing() const { return m_editing != 0; }
    // The most text an item holds, which the editor keeps to as it is typed in.
    int longestText() const { return Item::longestText; }
    QString editorText() const { return m_editorText; }
    QPointF editorPosition() const { return m_editorPosition; }
    qreal editorWidth() const { return m_editorStyle.box.width(); }
    QFont editorFont() const { return m_editorStyle.font; }
    QColor editorColor() const { return m_editorStyle.color; }
    bool editorCentered() const { return m_editorCentered; }
    bool editorSingleLine() const { return m_editorSingleLine; }

    bool canUndo() const { return m_board && m_board->canUndo(); }
    bool canRedo() const { return m_board && m_board->canRedo(); }
    int pendingItems() const { return int(m_dirty.size()); }
    bool simplified() const { return m_load.reduced; }
    bool readOnly() const { return m_readOnly; }
    void setReadOnly(bool readOnly);
    SceneLoad load() const { return m_load; }
    // The most a frame may ask of the GPU, in the bytes SceneLoad counts.
    qint64 loadBudget() const { return m_loadBudget; }
    void setLoadBudget(qint64 bytes);
    // Draws the whole board whatever is in view: only for measuring what
    // that costs (sprawl open --probe).
    void setDrawingEverything(bool everything);

    // --- View ----------------------------------------------------------------

    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void zoomActual();
    Q_INVOKABLE void zoomToFit();
    // The whole board at once, without the glide: for a board just opened.
    Q_INVOKABLE void showAll();
    // Tells the canvas where the mouse is when something else (a drop) knows.
    Q_INVOKABLE void pointTo(QPointF itemPoint);
    // Zooms by a factor, keeping the point under `around` (in this item's
    // coordinates) where it is.
    Q_INVOKABLE void zoomBy(qreal factor, QPointF around);
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    Q_INVOKABLE void setView(QPointF origin, qreal zoom);
    // Where something inserted should land: under the mouse when it is over
    // the canvas, and the middle of what is showing otherwise.
    Q_INVOKABLE QPointF insertPoint() const;
    QPointF toCanvas(QPointF itemPoint) const { return m_origin + itemPoint / m_zoom; }
    QPointF toItem(QPointF canvasPoint) const { return (canvasPoint - m_origin) * m_zoom; }
    QRectF visibleArea() const { return QRectF(m_origin, QSizeF(width(), height()) / m_zoom); }

    // --- Doing things --------------------------------------------------------

    Q_INVOKABLE void newNote();
    // `position` counts the pictures of one batch, fanned out so that none
    // hides another.
    Q_INVOKABLE void insertImage(const QString &asset, QSize naturalSize, int position = 0);
    Q_INVOKABLE QVariantList selectedIds() const;
    Q_INVOKABLE void insertText(const QString &text);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void duplicateSelection();
    Q_INVOKABLE void groupSelection();
    Q_INVOKABLE void ungroupSelection();
    Q_INVOKABLE void bringToFront();
    Q_INVOKABLE void sendToBack();
    Q_INVOKABLE void nudge(qreal dx, qreal dy);
    Q_INVOKABLE void setSelectionColor(const QString &color);
    // Makes the selected shapes rectangles, ellipses or diamonds.
    Q_INVOKABLE void setSelectionShape(const QString &shape);
    Q_INVOKABLE void toggleBold();
    Q_INVOKABLE void resizeText(int direction);
    Q_INVOKABLE void cycleShape();
    // The shape so many on from this one, around the end and back.
    Q_INVOKABLE void stepShape(int by);
    Q_INVOKABLE void copy();
    Q_INVOKABLE void cut();
    // False when the clipboard holds nothing a board takes, in which case
    // the caller may look for an image in it.
    Q_INVOKABLE bool paste();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // Steps back out of whatever is going on: a drag, editing, a tool, the
    // selection. False when there was nothing to step out of.
    Q_INVOKABLE bool escape();

    Q_INVOKABLE void editSelection();
    Q_INVOKABLE void editorEdited(const QString &text);
    Q_INVOKABLE void commitEdit();
    Q_INVOKABLE void attachEditor(QQuickTextDocument *document);

    // Selects exactly these items, for tests and for commands that add some.
    void select(const QVector<quint32> &ids);
    // Starts reading every picture on the board, and says whether any is
    // still being read: for a screenshot that should not be taken early.
    bool loadingImages();

signals:
    void boardChanged();
    void themeChanged();
    void assetFolderChanged();
    void viewChanged();
    void pointerChanged();
    void toolChanged();
    void penChanged();
    void selectionChanged();
    void selectionBoxChanged();
    void busyChanged();
    void editingChanged();
    void editorChanged();
    void historyChanged();
    void pendingItemsChanged();
    void loadChanged();
    // More text was given than an item holds, and what was over was left out.
    void textLeftOut();
    void readOnlyChanged();
    // The board is all on screen for the first time (or again, after a
    // change of board or theme).
    void settled();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *) override;

private:
    enum class Drag { None, Pan, Marquee, Move, Resize, End, Stroke, Box, Connect };
    // The eight handles of a box, clockwise from the top left; -1 is none.
    enum Handle { TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };

    // Everything an item is drawn with, worked out on the GUI thread so the
    // render thread only has to hand it to the GPU.
    struct Prepared {
        QPointF position;
        // What it covers when drawn.
        QRectF bounds;
        Geo::ShapeMesh shapes;
        Geo::StrokeMesh strokes;
        // A long stroke again, in less detail, for when it is small on
        // screen. Empty for anything else.
        Geo::StrokeMesh coarse[2];
        std::unique_ptr<TextBlock> text;
        QColor textColor, titleColor;
        QPointF textOffset;
        // The first line is a title, in type of its own.
        bool titled = false;
        // What the title and the rest are set in: text that shares a style
        // can be drawn together.
        quint64 titleStyle = 0, bodyStyle = 0;
        QString asset;
        QImage image;
        QRectF imageRect;
    };

    // --- Scene (scene.cpp) ---------------------------------------------------
    std::unique_ptr<Prepared> prepare(const Item &item);
    void prepareVisible(bool measure);
    void prepareDecorations(Geo::ShapeMesh &shapes);
    void prepareDraft(Geo::ShapeMesh &shapes, Geo::StrokeMesh &strokes);
    void buildItem(SceneNodes *scene, quint32 id, const Prepared &prepared);
    // Which layer each item is drawn in, and where: see scene.cpp.
    void relayer(quint32 id);
    void raiseLayer(quint32 id, int layer);
    void relayerAll();
    void setLayer(quint32 id, int layer);
    ScenePlace placeFor(quint32 id, int layer) const;
    QMatrix4x4 viewMatrix() const;
    qreal devicePixels() const;
    void markDirty(quint32 id);
    void markAllDirty();
    void requestFrame();

    // --- Board changes -------------------------------------------------------
    void onItemChanged(quint32 id, Board::Change change);
    void onOrderChanged();
    void onReset();
    void onThemeChanged();
    void onImageLoaded(const QString &asset);

    // --- View ----------------------------------------------------------------
    void setZoomAround(qreal zoom, QPointF around);
    void moveOrigin(QPointF origin);
    void animateTo(QPointF origin, qreal zoom);
    void viewMoved();

    // --- Selection -----------------------------------------------------------
    void setSelection(QVector<quint32> ids);
    QVector<quint32> withGroups(const QVector<quint32> &ids) const;
    void refreshSelectionBox();
    QRectF itemBoxOnScreen(const Item &item) const;
    struct HandleSpot {
        int handle;
        QPointF at;
    };
    QVector<HandleSpot> handleSpots() const;
    int handleAt(QPointF itemPoint) const;
    const Item *soleSelection() const;

    // --- Tools ---------------------------------------------------------------
    void setToolTo(Tool tool);
    void updateCursor(QPointF itemPoint);
    void setPointer(QPointF itemPoint, bool inside);
    void startMove();
    void dragMove(QPointF canvasPoint);
    void dragResize(QPointF canvasPoint, bool keepAspect);
    void dragEnd(QPointF canvasPoint);
    void dragMarquee(QPointF itemPoint);
    void finishStroke();
    void finishBox(QPointF canvasPoint);
    void finishConnect(QPointF canvasPoint);
    void cancelDrag();
    void endDrag();
    quint32 attachableAt(QPointF canvasPoint, quint32 except = 0) const;
    // A connector as the tool makes one: nowhere yet, and joined to nothing.
    Item connectorInHand() const;
    QPointF freeCorner(QPointF corner, Item::Kind kind) const;
    quint32 createItem(Item item, bool edit);
    void beginEdit(quint32 id, bool isNew);
    void refreshEditor();
    QColor colorFor(const QString &name, const QColor &neutral) const;
    QByteArray serialize(const QVector<quint32> &ids) const;
    QVector<quint32> addCopies(const QVector<Item> &items, QPointF offset);
    qreal slack() const { return 5 / m_zoom; }

    QPointer<Board> m_board;
    QPointer<Theme> m_theme;
    Palette m_palette = Palette::fallback();
    int m_radius = 0;
    bool m_reduceMotion = false;
    QString m_assetFolder;
    ImageStore *m_images = nullptr;

    // View
    qreal m_zoom = 1;
    QPointF m_origin;
    QVariantAnimation m_viewAnimation;
    QPointF m_animationFrom, m_animationTo;
    qreal m_animationZoomFrom = 1, m_animationZoomTo = 1;
    QPointF m_pointer, m_pointerItem;
    bool m_pointerInside = false;

    // Tools
    Tool m_tool = Tool::Select;
    Item::Shape m_shape = Item::Shape::Rect;
    Item::Route m_route = Item::Route::Straight;
    Item::Head m_lineStart = Item::Head::None, m_lineEnd = Item::Head::Arrow;
    Item::Dash m_dash = Item::Dash::Solid;
    QString m_penColor;
    qreal m_penWidth = Defaults::penWidth;
    QString m_noteColor = QStringLiteral("yellow");

    // Selection
    QVector<quint32> m_selection;
    QSet<quint32> m_selected;
    QRectF m_selectionBox;
    QString m_selectionSize;

    // Dragging
    Drag m_drag = Drag::None;
    bool m_dragStarted = false;
    Qt::MouseButton m_dragButton = Qt::NoButton;
    QPointF m_pressItem, m_pressCanvas, m_lastItem;
    QPointF m_panOrigin;
    QHash<quint32, Item> m_dragItems;
    int m_dragHandle = -1;
    QRectF m_marquee;
    QVector<quint32> m_marqueeBase;
    QVector<QPointF> m_draftPoints;
    QRectF m_draftBox;
    quint32 m_connectFrom = 0, m_connectTarget = 0;
    QPointF m_connectStart, m_connectEnd;
    bool m_connectSideways = true;

    // Editing
    quint32 m_editing = 0;
    bool m_editingNew = false;
    QString m_editorText;
    QPointF m_editorPosition;
    TextStyle m_editorStyle;
    bool m_editorCentered = false, m_editorSingleLine = false;
    TitleHighlighter *m_highlighter = nullptr;

    // What the scene still has to catch up on. Written on the GUI thread,
    // read while it is blocked for the render thread to synchronize.
    QSet<quint32> m_dirty, m_moved;
    QVector<quint32> m_gone;
    QHash<quint32, std::shared_ptr<Prepared>> m_ready;
    QSet<quint32> m_built;
    // Where each item is drawn from, and the items for which that changed
    // since the scene last looked.
    QHash<quint32, ScenePlace> m_place;
    QSet<quint32> m_rehome;
    bool m_layersStale = true;
    bool m_sceneReset = true, m_viewDirty = true;
    // What the last frame came to. Written while the scene synchronizes,
    // when this thread is waiting on it.
    SceneLoad m_load;
    // Eight megabytes is about sixty-five thousand small triangles. A
    // tile-based GPU keeps every triangle of a frame in a buffer of fixed
    // size until the frame is drawn, and the Apple GPU driver on Asahi does
    // not survive that buffer filling up: it takes the whole desktop with
    // it. Measured on an M3 Max with `sprawl open --probe`: frames of this
    // much (strokes in full, type at its smallest, strokes from far off) do
    // not make the buffer grow at all, which is where every other window on
    // the desktop is; frames of five times this had it half full; and
    // somewhere past seven times this it filled.
    qint64 m_loadBudget = 8'000'000;
    bool m_drawEverything = false;
    bool m_readOnly = false;
    // The selection's chrome and whatever is being drawn: dirty until
    // prepared, then ready until the scene has taken it.
    bool m_chromeDirty = true, m_chromeReady = false;
    bool m_settledPending = true;
    Geo::ShapeMesh m_chromeShapes, m_draftShapes;
    Geo::StrokeMesh m_draftStrokes;
    QElapsedTimer m_polishTimer;
    bool m_measuring = false;
};
