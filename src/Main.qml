import QtQuick
import QtQuick.Controls
import Sprawl

ApplicationWindow {
    id: win

    width: startSize.width
    height: startSize.height
    minimumWidth: 560
    minimumHeight: 460
    visible: !startHidden
    title: backend.boardName.length > 0 ? qsTr("%1 · Sprawl").arg(backend.boardName) : "Sprawl"
    color: theme.background

    // The desktop's theme, under a name the canvas's own `theme` property
    // does not hide where it is handed over.
    readonly property var desktop: theme
    // The panel over the window: none, or keys, boards, export, quit.
    property string panel: ""
    // The keys are the canvas's while no panel is up and nothing is typed.
    readonly property bool free: panel === "" && !canvas.editing
    // The view has been put where the board was last left, so from now on
    // where it goes is worth remembering.
    property bool viewRestored: false
    // Quitting with a board that would not save was agreed to.
    property bool sure: false
    // The popout of shapes by the shape tool is out.
    property bool shapesOpen: false

    function openPanel(name) {
        if (canvas.editing)
            canvas.commitEdit()
        closePanel()
        panel = name
        if (name === "keys")
            keys.visible = true
        else if (name === "boards")
            boards.open()
        else if (name === "newboard")
            boards.openNew()
        else if (name === "export")
            exporter.open(canvas.selectionCount > 0)
        else if (name === "quit")
            quitting.open()
        else {
            panel = ""
            // Not panels, but looked at the same way (sprawl open --show):
            // the choices of what the connector tool draws.
            if (["start", "route", "dash", "end"].includes(name))
                connectorBar.open = name
        }
    }

    function closePanel() {
        panel = ""
        keys.visible = boards.visible = exporter.visible = quitting.visible = false
        canvas.forceActiveFocus()
    }

    // Everything is saved as it changes, so quitting only has to ask when
    // the board could not be written.
    function quit() {
        if (backend.flush())
            Qt.quit()
        else
            openPanel("quit")
    }

    function restoreView() {
        if (canvas.width <= 0 || canvas.height <= 0)
            return
        const view = backend.rememberedView()
        if (view.zoom !== undefined)
            canvas.setView(Qt.point(view.x, view.y), view.zoom)
        else
            canvas.showAll()
        viewRestored = true
    }

    // Arrows move what is selected, or the view when nothing is; and while
    // the shapes are out, they move among those.
    function arrow(dx, dy, far) {
        if (shapesOpen) {
            const shapes = canvas.shapes
            let to = shapes.indexOf(canvas.shape) + dx + dy * shapePicker.columns
            // Off the top or the bottom of the grid is nowhere; off either
            // end of a row is the row before or after.
            if (dy !== 0 && (to < 0 || to >= shapes.length))
                return
            canvas.shape = shapes[(to + shapes.length) % shapes.length]
            return
        }
        if (canvas.selectionCount > 0)
            canvas.nudge(dx * (far ? 10 : 1), dy * (far ? 10 : 1))
        else
            canvas.panBy(dx * (far ? 240 : 48), dy * (far ? 240 : 48))
    }

    function stepPen(direction) {
        const widths = [1, 2, 4, 8]
        const at = widths.indexOf(canvas.penWidth)
        canvas.penWidth = widths[Math.max(0, Math.min(widths.length - 1, (at < 0 ? 1 : at) + direction))]
    }

    onClosing: (close) => {
        if (!sure && !backend.flush()) {
            close.accepted = false
            openPanel("quit")
        }
    }

    Component.onCompleted: restoreView()

    Connections {
        target: canvas
        // Taking the shape tool, or stepping to another shape, brings the
        // shapes out; starting to draw, or any other tool, puts them away.
        function onToolChanged() { win.shapesOpen = canvas.tool === "shape" }
        function onBusyChanged() {
            if (canvas.busy)
                win.shapesOpen = false
        }
    }

    Connections {
        target: backend
        function onBoardOpened() { win.restoreView() }
        function onImageAdded(asset, size, position) { canvas.insertImage(asset, size, position) }
        function onTextPasted(text) { canvas.insertText(text) }
    }

    // --- Tools ---
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.tool = "select" }
    Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.tool = "pan" }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.tool = "note" }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.tool = "text" }
    Shortcut {
        sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.free
        // It takes the tool and brings the shapes out; pressed again, it is
        // the next shape.
        onActivated: {
            if (canvas.tool !== "shape")
                canvas.tool = "shape"
            else if (!win.shapesOpen)
                win.shapesOpen = true
            else
                canvas.stepShape(1)
        }
    }
    Shortcut {
        sequence: "Shift+S"; context: Qt.ApplicationShortcut; enabled: win.free && canvas.tool === "shape"
        onActivated: canvas.stepShape(-1)
    }
    Shortcut {
        // With the shapes out, Enter is "this one": they are put away, and
        // the tool is still in hand.
        sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: win.free && win.shapesOpen
        onActivated: win.shapesOpen = false
    }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.tool = "pen" }
    Shortcut {
        sequence: "C"; context: Qt.ApplicationShortcut; enabled: win.free
        // Pressed again, it is the next route: straight, curved, elbow.
        onActivated: {
            if (canvas.tool === "connector")
                canvas.stepRoute(1)
            else
                canvas.tool = "connector"
        }
    }
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: backend.pickImages() }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.tool = "frame" }

    // --- Board ---
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.newNote() }
    Shortcut {
        sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut
        enabled: win.free && canvas.selectionCount === 1
        onActivated: canvas.editSelection()
    }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.undo() }
    Shortcut {
        // The desktop's own redo is Ctrl+Shift+Z. Naming it a second time
        // would make the two ambiguous, and an ambiguous shortcut never fires.
        sequences: [StandardKey.Redo, "Ctrl+Y"]; context: Qt.ApplicationShortcut; enabled: win.free
        onActivated: canvas.redo()
    }
    Shortcut { sequences: [StandardKey.Copy]; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.copy() }
    Shortcut { sequences: [StandardKey.Cut]; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.cut() }
    Shortcut {
        sequences: [StandardKey.Paste]; context: Qt.ApplicationShortcut; enabled: win.free
        // Items copied from a board first; failing that, whatever picture
        // or text the clipboard holds.
        onActivated: {
            if (!canvas.paste())
                backend.pasteClipboard()
        }
    }
    Shortcut { sequence: "Ctrl+D"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.duplicateSelection() }
    Shortcut {
        sequences: ["Delete", "Backspace"]; context: Qt.ApplicationShortcut; enabled: win.free
        onActivated: canvas.deleteSelection()
    }
    Shortcut { sequence: "Ctrl+A"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.selectAll() }
    Shortcut { sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; enabled: win.panel === ""; onActivated: win.openPanel("boards") }
    Shortcut { sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; enabled: win.panel === ""; onActivated: win.openPanel("newboard") }
    Shortcut { sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; enabled: win.panel === ""; onActivated: win.openPanel("export") }

    // --- Arrange ---
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(-1, 0, false) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(1, 0, false) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(0, -1, false) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(0, 1, false) }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(-1, 0, true) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(1, 0, true) }
    Shortcut { sequence: "Shift+Up"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(0, -1, true) }
    Shortcut { sequence: "Shift+Down"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(0, 1, true) }
    Shortcut { sequence: "Ctrl+G"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.groupSelection() }
    Shortcut { sequence: "Ctrl+Shift+G"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.ungroupSelection() }
    Shortcut {
        sequence: "]"; context: Qt.ApplicationShortcut; enabled: win.free
        // The pen's width while the pen is held; the stacking order otherwise.
        onActivated: {
            if (canvas.tool === "pen")
                win.stepPen(1)
            else
                canvas.bringToFront()
        }
    }
    Shortcut {
        sequence: "["; context: Qt.ApplicationShortcut; enabled: win.free
        onActivated: {
            if (canvas.tool === "pen")
                win.stepPen(-1)
            else
                canvas.sendToBack()
        }
    }
    Shortcut { sequence: "Ctrl+B"; context: Qt.ApplicationShortcut; enabled: win.panel === ""; onActivated: canvas.toggleBold() }
    Shortcut {
        // + is Shift and = on most keyboards, and whether Shift still counts
        // once it has made the + depends on the layout: say it both ways.
        sequences: ["Ctrl++", "Ctrl+Shift++", "Ctrl+="]; context: Qt.ApplicationShortcut; enabled: win.panel === ""
        onActivated: canvas.resizeText(1)
    }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; enabled: win.panel === ""; onActivated: canvas.resizeText(-1) }

    // --- View ---
    Shortcut {
        sequences: ["+", "Shift++", "="]; context: Qt.ApplicationShortcut; enabled: win.free
        onActivated: canvas.zoomIn()
    }
    Shortcut { sequence: "-"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.zoomOut() }
    Shortcut { sequence: "0"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.zoomActual() }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.zoomToFit() }

    // --- App ---
    Shortcut {
        sequences: ["?", "Shift+?", "F1"]; context: Qt.ApplicationShortcut; enabled: win.free || win.panel === "keys"
        onActivated: win.panel === "keys" ? win.closePanel() : win.openPanel("keys")
    }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.quit() }
    Shortcut {
        sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.panel === "" || win.panel === "keys"
        // Out of the panel, or one step back out of whatever the canvas is doing.
        onActivated: win.panel === "keys" ? win.closePanel() : canvas.escape()
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        CanvasView {
            id: canvas
            anchors.left: rail.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: status.top
            focus: true
            board: backend.board
            theme: win.desktop
            assetFolder: backend.assetFolder

            onViewChanged: {
                if (win.viewRestored)
                    backend.rememberView(origin, zoom)
            }
            onWidthChanged: {
                if (!win.viewRestored)
                    win.restoreView()
            }
            onEditingChanged: {
                if (editing) {
                    editor.text = editorText
                    editor.forceActiveFocus()
                    editor.cursorPosition = editor.length
                } else {
                    forceActiveFocus()
                }
            }

            // Pictures dropped on the board land where they are dropped.
            DropArea {
                anchors.fill: parent
                onDropped: (drop) => {
                    if (!drop.hasUrls)
                        return
                    canvas.pointTo(Qt.point(drop.x, drop.y))
                    backend.addFiles(drop.urls)
                    drop.accept()
                }
            }

            // The editor that opens over an item. It is the same type, in
            // the same place, wrapped at the same width as the text it
            // stands in for, so starting to type moves nothing.
            TextEdit {
                id: editor
                visible: canvas.editing
                x: canvas.editorPosition.x
                y: canvas.editorPosition.y
                width: canvas.editorWidth > 0 ? canvas.editorWidth : implicitWidth
                transformOrigin: Item.TopLeft
                scale: canvas.zoom
                font: canvas.editorFont
                color: canvas.editorColor
                wrapMode: canvas.editorWidth > 0 ? TextEdit.Wrap : TextEdit.NoWrap
                horizontalAlignment: canvas.editorCentered ? TextEdit.AlignHCenter : TextEdit.AlignLeft
                textFormat: TextEdit.PlainText
                selectByMouse: true
                selectionColor: Qt.alpha(theme.accent, 0.4)
                selectedTextColor: color
                Component.onCompleted: canvas.attachEditor(textDocument)
                onTextChanged: {
                    if (canvas.editing && activeFocus)
                        canvas.editorEdited(text)
                }
                onActiveFocusChanged: {
                    if (!activeFocus && canvas.editing)
                        canvas.commitEdit()
                }
                Keys.onPressed: (event) => {
                    const enter = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                    // A title is one line, so Enter finishes it; anywhere
                    // else Enter is a new line and Ctrl+Enter finishes.
                    if (enter && (canvas.editorSingleLine || (event.modifiers & Qt.ControlModifier))) {
                        canvas.commitEdit()
                        event.accepted = true
                    }
                }
            }

            // How big the selection is, in the board's own units.
            Rectangle {
                readonly property rect box: canvas.selectionBox
                visible: canvas.selectionCount > 0 && !canvas.editing && canvas.tool === "select"
                         && canvas.selectionKind !== "connector" && box.width > 0
                x: Math.round(box.x + box.width / 2 - width / 2)
                y: Math.round(box.y + box.height + 10)
                width: sizeText.implicitWidth + 18
                height: 16
                color: theme.accent
                radius: Math.min(4, theme.radius)

                Text {
                    id: sizeText
                    anchors.centerIn: parent
                    text: canvas.selectionSize
                    color: theme.accentInk
                    font.pixelSize: theme.size("small")
                    font.bold: true
                }
            }

            SelectionBar {
                view: canvas
                readonly property rect box: canvas.selectionBox
                // By the selection while it is at rest and on screen.
                visible: canvas.selectionCount > 0 && !canvas.busy && !canvas.editing && canvas.tool === "select"
                         && box.x < canvas.width && box.x + box.width > 0 && box.y < canvas.height && box.y + box.height > 0
                x: Math.round(Math.max(8, Math.min(canvas.width - width - 8, box.x + box.width / 2 - width / 2)))
                y: {
                    // Above the selection, or below it when there is no room above.
                    const above = box.y - 16 - height
                    if (above >= 8)
                        return Math.round(above)
                    return Math.round(Math.max(8, Math.min(canvas.height - height - 8, box.y + box.height + 42)))
                }
            }

            PenBar {
                view: canvas
                visible: canvas.tool === "pen" && !canvas.busy
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
            }

            ConnectorBar {
                id: connectorBar
                view: canvas
                visible: canvas.tool === "connector" && !canvas.busy
                onVisibleChanged: open = ""
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
            }
        }

        Rail {
            id: rail
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: status.top
            view: canvas
            keysShowing: win.panel === "keys"
            onPickImage: backend.pickImages()
            onToggleShapes: win.shapesOpen = !win.shapesOpen
            onToggleKeys: win.panel === "keys" ? win.closePanel() : win.openPanel("keys")
        }

        // The shapes, out beside the shape tool while it is held: to see
        // them all and click one, where S only steps through them.
        Picker {
            id: shapePicker
            view: canvas
            kind: "shape"
            name: "draw"
            hint: true
            visible: win.shapesOpen && canvas.tool === "shape" && !canvas.busy
            current: canvas.shape
            anchors.left: rail.right
            anchors.leftMargin: 8
            y: Math.max(8, Math.min(rail.shapeTop - 8, status.y - height - 8))
            onPicked: (shape) => {
                canvas.shape = shape
                win.shapesOpen = false
            }
        }

        StatusLine {
            id: status
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            view: canvas
            onOpenBoards: win.openPanel("boards")
        }

        KeysOverlay {
            id: keys
            onDismissed: win.closePanel()
        }

        BoardSwitcher {
            id: boards
            onDismissed: win.closePanel()
        }

        ExportDialog {
            id: exporter
            onDismissed: win.closePanel()
            onChosen: (format) => {
                win.closePanel()
                backend.exportBoard(format, canvas.selectedIds())
            }
        }

        ConfirmDialog {
            id: quitting
            title: qsTr("The board is not saved")
            detail: backend.problem
            hint: qsTr("Enter quits anyway · Esc goes back")
            onDismissed: win.closePanel()
            onAccepted: {
                win.sure = true
                Qt.quit()
            }
        }
    }
}
