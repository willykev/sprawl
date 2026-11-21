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
    // The panel over the window: none, or keys, boards, export, replace, quit.
    property string panel: ""
    // The keys are the canvas's while no panel is up, nothing is typed, and
    // the mouse is not down on the canvas: until it lets go, what it is
    // doing has the board to itself.
    readonly property bool free: panel === "" && !canvas.editing && !canvas.pressed
    // Of those, the keys that change the board are not for a board that
    // could not be read: it is there to be looked at.
    readonly property bool writable: free && !canvas.readOnly
    // The view of the open board stays as it is, whatever size the canvas
    // comes to: since it was put where the board was last left, or around
    // all of it, it has been moved, or the board has been worked on or has
    // changed. Until then it is put there again whenever the canvas changes
    // size, as it does when a tiling desktop gives the window its real size
    // a moment after it opens. Only where it is moved to is remembered: a
    // board nobody has moved is put around all of it again, at whatever
    // size the window is then.
    property bool viewKept: false
    // The view is being put there now, which is not moving it.
    property bool puttingView: false
    // Quitting with a board that would not save was agreed to.
    property bool sure: false
    // The popout of shapes by the shape tool is out.
    property bool shapesOpen: false

    function openPanel(name) {
        let query = ""
        if (name.startsWith("boards=")) {
            query = name.slice("boards=".length)
            name = "boards"
        }
        if (canvas.editing)
            canvas.commitEdit()
        closePanel()
        panel = name
        if (name === "keys")
            keys.visible = true
        else if (name === "boards")
            boards.open(query)
        else if (name === "newboard")
            boards.openNew()
        else if (name === "export")
            exporter.open(canvas.selectedIds())
        else if (name === "replace")
            replacing.open()
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
        keys.visible = boards.visible = exporter.visible = replacing.visible = quitting.visible = false
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
        // Nothing has been done to a board just opened, or to its view, even
        // before there is a canvas to put it in.
        viewKept = false
        if (canvas.width <= 0 || canvas.height <= 0)
            return
        const view = backend.rememberedView()
        puttingView = true
        // Where a board was left is read from a file that may have been
        // edited by hand, and what is not a place and a zoom is no view.
        if ([view.x, view.y, view.zoom].every(Number.isFinite) && view.zoom > 0)
            canvas.setView(Qt.point(view.x, view.y), view.zoom)
        else
            canvas.showAll()
        puttingView = false
    }

    // Arrows move what is selected, or the view when nothing is (or nothing
    // may be moved); and while the shapes are out, they move among those.
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
        if (canvas.selectionCount > 0 && !canvas.readOnly)
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
        // A stroke being drawn is kept as far as it got. What is being typed
        // is finished, as Esc would finish it, so that is what is kept: text
        // left empty leaves no trace.
        canvas.keepStroke()
        if (canvas.editing)
            canvas.commitEdit()
        if (!sure && !backend.flush()) {
            close.accepted = false
            openPanel("quit")
        }
    }

    Component.onCompleted: {
        backend.attachCanvas(canvas)
        restoreView()
    }

    Connections {
        target: canvas
        // Taking the shape tool, or stepping to another shape, brings the
        // shapes out; starting to draw, or any other tool, puts them away.
        function onToolChanged() { win.shapesOpen = canvas.tool === "shape" }
        function onBusyChanged() {
            if (canvas.busy)
                win.shapesOpen = false
        }
        function onTextLeftOut() {
            backend.notify(qsTr("One item holds %1 characters. The rest was left out.").arg(canvas.longestText))
        }
        function onItemsLeftOut() {
            backend.notify(qsTr("Not all of what was copied could be pasted."))
        }
    }

    Connections {
        target: backend.board
        // Anything done to the board, here or from outside, keeps the view
        // as it is: nothing on it moves on screen when the window is next
        // given another size. (Opening a board replaces it too, but before
        // its view is put, which starts it afresh.)
        function onModified() { win.viewKept = true }
        function onReset() { win.viewKept = true }
    }

    Connections {
        target: backend
        function onBoardOpened() { win.restoreView() }
        function onTextPasted(text) { canvas.insertText(text) }
    }

    // --- Tools ---
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.tool = "select" }
    Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.tool = "pan" }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.tool = "note" }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.tool = "text" }
    Shortcut {
        sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.writable
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
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.tool = "pen" }
    Shortcut {
        sequence: "C"; context: Qt.ApplicationShortcut; enabled: win.writable
        // Pressed again, it is the next route: straight, curved, elbow.
        onActivated: {
            if (canvas.tool === "connector")
                canvas.stepRoute(1)
            else
                canvas.tool = "connector"
        }
    }
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: backend.pickImages() }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.tool = "frame" }

    // --- Board ---
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.newNote() }
    Shortcut {
        sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut
        enabled: win.writable && canvas.selectionCount === 1
        onActivated: canvas.editSelection()
    }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.undo() }
    Shortcut {
        // The desktop's own redo, Ctrl+Shift+Z, and Ctrl+Y too, each named
        // once: a key named twice is ambiguous, and an ambiguous shortcut
        // never fires.
        sequences: canvas.redoKeys; context: Qt.ApplicationShortcut; enabled: win.writable
        onActivated: canvas.redo()
    }
    Shortcut { sequences: [StandardKey.Copy]; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.copy() }
    Shortcut { sequences: [StandardKey.Cut]; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.cut() }
    Shortcut {
        sequences: [StandardKey.Paste]; context: Qt.ApplicationShortcut; enabled: win.writable
        // Items copied from a board first; failing that, whatever picture
        // or text the clipboard holds.
        onActivated: {
            if (!canvas.paste())
                backend.pasteClipboard()
        }
    }
    Shortcut { sequence: "Ctrl+D"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.duplicateSelection() }
    Shortcut {
        sequences: ["Delete", "Backspace"]; context: Qt.ApplicationShortcut; enabled: win.writable
        onActivated: canvas.deleteSelection()
    }
    Shortcut { sequence: "Ctrl+A"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: canvas.selectAll() }
    // The panels come up from the middle of writing too, but not from the
    // middle of a drag.
    Shortcut {
        sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; enabled: win.panel === "" && !canvas.pressed
        onActivated: win.openPanel("boards")
    }
    Shortcut {
        sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; enabled: win.panel === "" && !canvas.pressed
        onActivated: win.openPanel("newboard")
    }
    Shortcut {
        sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; enabled: win.panel === "" && !canvas.pressed
        onActivated: win.openPanel("export")
    }

    // --- Arrange ---
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(-1, 0, false) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(1, 0, false) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(0, -1, false) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(0, 1, false) }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(-1, 0, true) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(1, 0, true) }
    Shortcut { sequence: "Shift+Up"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(0, -1, true) }
    Shortcut { sequence: "Shift+Down"; context: Qt.ApplicationShortcut; enabled: win.free; onActivated: win.arrow(0, 1, true) }
    Shortcut { sequence: "Ctrl+G"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.groupSelection() }
    Shortcut { sequence: "Ctrl+Shift+G"; context: Qt.ApplicationShortcut; enabled: win.writable; onActivated: canvas.ungroupSelection() }
    Shortcut {
        sequence: "]"; context: Qt.ApplicationShortcut; enabled: win.writable
        // The pen's width while the pen is held; the stacking order otherwise.
        onActivated: {
            if (canvas.tool === "pen")
                win.stepPen(1)
            else
                canvas.bringToFront()
        }
    }
    Shortcut {
        sequence: "["; context: Qt.ApplicationShortcut; enabled: win.writable
        onActivated: {
            if (canvas.tool === "pen")
                win.stepPen(-1)
            else
                canvas.sendToBack()
        }
    }
    // Text is made bold, bigger and smaller while it is being written too.
    Shortcut {
        sequence: "Ctrl+B"; context: Qt.ApplicationShortcut; enabled: win.panel === "" && !canvas.pressed && !canvas.readOnly
        onActivated: canvas.toggleBold()
    }
    Shortcut {
        // + is Shift and = on most keyboards, and whether Shift still counts
        // once it has made the + depends on the layout: say it both ways.
        sequences: ["Ctrl++", "Ctrl+Shift++", "Ctrl+="]; context: Qt.ApplicationShortcut
        enabled: win.panel === "" && !canvas.pressed && !canvas.readOnly
        onActivated: canvas.resizeText(1)
    }
    Shortcut {
        sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; enabled: win.panel === "" && !canvas.pressed && !canvas.readOnly
        onActivated: canvas.resizeText(-1)
    }

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
            readOnly: backend.readOnly

            onViewChanged: {
                if (win.puttingView)
                    return
                win.viewKept = true
                backend.rememberView(origin, zoom)
            }
            onWidthChanged: {
                if (!win.viewKept)
                    win.restoreView()
            }
            onHeightChanged: {
                if (!win.viewKept)
                    win.restoreView()
            }
            // A press on the board, or writing on it, is working on it.
            onPressedChanged: {
                if (pressed)
                    win.viewKept = true
            }
            onEditingChanged: {
                if (editing) {
                    win.viewKept = true
                    editor.text = editorText
                    editor.forceActiveFocus()
                    editor.cursorPosition = editor.length
                } else {
                    forceActiveFocus()
                }
            }

            // Pictures dropped on the board land where they are dropped. A
            // board that could not be read takes none.
            DropArea {
                anchors.fill: parent
                enabled: !canvas.readOnly
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
                objectName: "editor"
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
                    // Never more in it than an item holds. What goes in
                    // over that comes straight back out, from the end of
                    // what was just put in (where the cursor is), before a
                    // frame is drawn of it: the editor draws all it holds.
                    if (length > canvas.longestText) {
                        const over = length - canvas.longestText
                        const end = Math.max(over, cursorPosition)
                        const start = end - over
                        const splitsCharacter = (at) => {
                            const before = text.charCodeAt(at - 1)
                            const after = text.charCodeAt(at)
                            return before >= 0xd800 && before <= 0xdbff && after >= 0xdc00 && after <= 0xdfff
                        }
                        remove(splitsCharacter(start) ? start - 1 : start, splitsCharacter(end) ? end + 1 : end)
                        canvas.textLeftOut()
                        return
                    }
                    if (canvas.editing && activeFocus)
                        canvas.editorEdited(text)
                }
                onActiveFocusChanged: {
                    // The keys going to anything else in the window finish
                    // what is being written. The window losing them to
                    // another (a terminal, a notification, another
                    // workspace) does not: they come back to the editor
                    // with the window.
                    if (!activeFocus && canvas.editing && win.active)
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

                PlainText {
                    id: sizeText
                    anchors.centerIn: parent
                    text: canvas.selectionSize
                    color: theme.accentInk
                    font.pixelSize: theme.sizes.small
                    font.bold: true
                }
            }

            SelectionBar {
                view: canvas
                readonly property rect box: canvas.selectionBox
                // By the selection while it is at rest and on screen. All it
                // offers are changes, so a board that could not be read has
                // none of it.
                visible: canvas.selectionCount > 0 && !canvas.busy && !canvas.editing && canvas.tool === "select"
                         && !canvas.readOnly
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
                backend.exportBoard(format, exporter.ids)
            }
        }

        // An export named without its ending is given it, and the save dialog
        // never asked about a file that already has that name.
        ConfirmDialog {
            id: replacing
            property string name
            property string folder
            title: qsTr("Replace %1?").arg(name)
            detail: qsTr("There is a file of that name in %1 already.").arg(folder)
            hint: qsTr("Enter replaces it · Esc keeps it")
            onDismissed: {
                win.closePanel()
                backend.replaceWithExport(false)
            }
            onAccepted: {
                win.closePanel()
                backend.replaceWithExport(true)
            }

            Connections {
                target: backend
                function onReplaceAsked(name, folder) {
                    replacing.name = name
                    replacing.folder = folder
                    win.openPanel("replace")
                }
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
