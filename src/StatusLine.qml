import QtQuick
import "icons.js" as Icons

// The line along the bottom: what mode the canvas is in, which board is
// open, what is selected, whether it is saved, and where the view is.
Rectangle {
    id: bar

    property var view
    signal openBoards()

    // As tall as the Omarchy bar across the top of the screen.
    height: theme.barHeight
    color: theme.panel

    readonly property string mode: view.editing ? "edit" : view.tool
    // Like an editor's mode line: one color for looking, one for making,
    // one for the pen, one for typing.
    readonly property color modeColor: mode === "select" ? theme.accent
                                       : mode === "pan" ? theme.swatches.cyan
                                       : mode === "pen" ? theme.swatches.magenta
                                       : mode === "edit" ? theme.swatches.yellow
                                       : theme.swatches.green
    // The theme's dark or light, whichever reads on it. (Handed the theme's
    // revision, so it is asked again when the theme changes.)
    readonly property color modeInk: theme.inkOn(modeColor, theme.revision)
    readonly property string modeName: mode === "select" ? qsTr("SELECT")
                                       : mode === "pan" ? qsTr("PAN")
                                       : mode === "note" ? qsTr("NOTE")
                                       : mode === "text" ? qsTr("TEXT")
                                       : mode === "shape" ? qsTr("SHAPE")
                                       : mode === "pen" ? qsTr("PEN")
                                       : mode === "connector" ? qsTr("CONNECT")
                                       : mode === "frame" ? qsTr("FRAME")
                                       : qsTr("EDIT")

    function kindName(kind, count) {
        switch (kind) {
        case "note": return qsTr("%n notes", "", count)
        case "text": return qsTr("%n texts", "", count)
        case "shape": return qsTr("%n shapes", "", count)
        case "stroke": return qsTr("%n strokes", "", count)
        case "connector": return qsTr("%n connectors", "", count)
        case "image": return qsTr("%n pictures", "", count)
        case "frame": return qsTr("%n frames", "", count)
        }
        return ""
    }

    // What the line says about what is going on right now.
    readonly property string context: {
        if (view.editing)
            return qsTr("Esc when done")
        switch (view.tool) {
        case "pan": return qsTr("drag to move the view")
        case "note": return qsTr("click to place a note")
        case "text": return qsTr("click to write")
        case "shape":
            // (Read again when the language changes.)
            return translator.language
                   ? qsTr("%1 · drag to draw, click to place, S for the next shape").arg(view.shapeTitle(view.shape)) : ""
        case "pen": return qsTr("stroke %1 px").arg(view.penWidth.toLocaleString(Qt.locale(translator.language), "f", 0))
        case "connector":
            return translator.language
                   ? qsTr("%1 · drag from one thing to another, C for the next route")
                         .arg(view.optionTitle("route", view.line.route)) : ""
        case "frame": return qsTr("drag around the things to frame")
        }
        if (view.selectionCount === 1)
            return qsTr("1 selected · %1").arg(kindSingular(view.selectionKind))
        if (view.selectionCount > 1)
            return view.selectionKind.length ? qsTr("%1 selected").arg(kindName(view.selectionKind, view.selectionCount))
                                               : qsTr("%n selected", "", view.selectionCount)
        return ""
    }

    function kindSingular(kind) {
        switch (kind) {
        case "note": return qsTr("note")
        case "text": return qsTr("text")
        case "shape": return qsTr("shape")
        case "stroke": return qsTr("stroke")
        case "connector": return qsTr("connector")
        case "image": return qsTr("picture")
        case "frame": return qsTr("frame")
        }
        return ""
    }

    Row {
        id: left
        anchors.left: parent.left
        height: parent.height

        Rectangle {
            objectName: "mode"
            width: modeText.implicitWidth + 28
            height: parent.height
            color: bar.modeColor

            PlainText {
                id: modeText
                objectName: "mode-name"
                anchors.centerIn: parent
                text: bar.modeName
                color: bar.modeInk
                font.bold: true
                font.letterSpacing: 0.6
            }
        }

        Rectangle {
            width: boardRow.implicitWidth + 24
            height: parent.height
            color: boardArea.containsMouse ? Qt.tint(theme.raised, theme.hoverFill) : theme.raised

            Row {
                id: boardRow
                anchors.centerIn: parent
                spacing: 6

                PlainText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: backend.boardName
                    color: theme.foreground
                    elide: Text.ElideMiddle
                    width: Math.min(implicitWidth, Math.max(80, bar.width * 0.25))
                }

                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.chevronDown
                    size: 12
                }
            }

            MouseArea {
                id: boardArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: bar.openBoards()
            }
        }
    }

    // The middle: something wrong, something done, or what is selected.
    Item {
        id: middle
        anchors.left: left.right
        anchors.right: right.left
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        height: parent.height
        clip: true

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            visible: backend.problem.length > 0

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                path: Icons.close
                size: 12
                color: theme.danger
            }

            PlainText {
                id: problemLine
                objectName: "problem"
                text: backend.problem
                color: theme.danger
                // As much of it as there is room for, and a sign that there
                // is more: the whole of it is over the board.
                width: Math.min(implicitWidth, middle.width - 20)
                elide: Text.ElideRight
            }
        }

        MouseArea {
            anchors.fill: parent
            enabled: backend.problem.length > 0
            // A hand only while a click does something: one that is not
            // enabled shows its cursor all the same.
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: backend.dismissProblem()
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            visible: backend.problem.length === 0

            // The pen's color, beside its width.
            Rectangle {
                objectName: "pen-color"
                anchors.verticalCenter: parent.verticalCenter
                visible: bar.view.tool === "pen" && !bar.view.editing && backend.notice.length === 0
                width: 8
                height: 8
                radius: Math.min(2, theme.radius)
                // As the board draws what the pen makes: in the swatch it
                // names, and in the bright when there is no such swatch.
                color: theme.swatches[bar.view.penColor] || theme.bright
            }

            PlainText {
                text: backend.notice.length ? backend.notice : bar.context
                color: theme.foreground
                width: Math.min(implicitWidth, middle.width - 16)
                elide: Text.ElideRight
            }
        }
    }

    Row {
        id: right
        anchors.right: parent.right
        height: parent.height

        // More is in view than a frame is allowed to hold, so some of it
        // is left out. Said, rather than done quietly.
        PlainText {
            visible: bar.view.simplified && bar.width >= 760
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            rightPadding: 16
            text: qsTr("zoom in for full detail")
            color: theme.warning
        }

        Item {
            width: saving.width
            height: parent.height

            Row {
                id: saving
                height: parent.height
                spacing: 8
                rightPadding: 14

                // The board could not be written, or is waiting to be: nothing
                // is written over a file that cannot be read until it can be.
                // Or it could not be read at all, and nothing of it ever is.
                readonly property bool notSaved: backend.saveState === "failed" || backend.saveState === "paused"
                                                 || backend.saveState === "readonly"

                Rectangle {
                    objectName: "saveMark"
                    anchors.verticalCenter: parent.verticalCenter
                    width: 6
                    height: 6
                    color: backend.saveState === "saved" ? theme.good
                           : parent.notSaved ? theme.danger : theme.warning
                }

                PlainText {
                    objectName: "saveState"
                    anchors.verticalCenter: parent.verticalCenter
                    text: backend.saveState === "saved" ? qsTr("saved")
                          : backend.saveState === "readonly" ? qsTr("read only")
                          : parent.notSaved ? qsTr("not saved") : qsTr("saving")
                    color: theme.foreground
                }
            }

            // Read only, or waiting for a file that cannot be read: a click
            // says why again, once that has been put away.
            MouseArea {
                anchors.fill: parent
                enabled: backend.saveState === "readonly" || backend.saveState === "paused"
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: backend.sayWhy()
            }
        }

        // Where the mouse is on the board. First to go when the window is narrow.
        Rectangle {
            visible: bar.width >= 760
            width: visible ? coordinates.implicitWidth + 28 : 0
            height: parent.height
            color: theme.raised

            PlainText {
                id: coordinates
                anchors.centerIn: parent
                text: qsTr("x %1  y %2").arg(Math.round(bar.view.pointer.x)).arg(Math.round(bar.view.pointer.y))
                color: theme.foreground
            }
        }

        Rectangle {
            width: zoomText.implicitWidth + 28
            height: parent.height
            color: bar.modeColor

            PlainText {
                id: zoomText
                objectName: "zoom"
                anchors.centerIn: parent
                text: qsTr("%1%").arg(Math.round(bar.view.zoom * 100))
                color: bar.modeInk
                font.bold: true
            }

            MouseArea {
                anchors.fill: parent
                onClicked: bar.view.zoomActual()
            }
        }
    }

    // A problem too long for the line is shown whole over the board, where
    // there is room for all of it; or, in a window too small even for that,
    // as much as the board has room for, and the wheel brings in the rest.
    // A click on either puts it away.
    Rectangle {
        id: whole
        objectName: "wholeProblem"
        visible: backend.problem.length > 0 && problemLine.truncated
        // At the top of the board, clear of the bars along its bottom, and
        // never past the board onto the line.
        x: Math.round(bar.view.x - bar.x + (bar.view.width - width) / 2)
        y: Math.round(bar.view.y - bar.y + 16)
        width: Math.min(560, bar.view.width - 32)
        height: Math.min(wholeRow.implicitHeight + 24, bar.view.height - 32)
        color: theme.cardBackground
        border.color: theme.cardBorder
        border.width: theme.borderWidth
        radius: theme.radius

        // Clicks on it are for it: one puts it away. So is the wheel, which
        // goes through what does not fit as it would pan the board.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            cursorShape: Qt.PointingHandCursor
            onClicked: backend.dismissProblem()
            onWheel: (wheel) => {
                // A touchpad says how far the fingers moved; a wheel, notches.
                const by = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y / 120 * 48
                words.down = Math.max(0, Math.min(words.more, words.down - by))
                wheel.accepted = true
            }
        }

        // As much of it as there is room for.
        Item {
            id: words
            x: 14
            y: 12
            width: whole.width - 28
            height: whole.height - 24
            clip: true

            // How far down it has been read, and how much further it goes.
            property real down: 0
            readonly property real more: Math.max(0, wholeRow.implicitHeight - height)

            Row {
                id: wholeRow
                y: -Math.min(words.down, words.more)
                spacing: 8

                Icon {
                    // Level with the first line.
                    y: Math.round((wholeText.contentHeight / Math.max(1, wholeText.lineCount) - height) / 2)
                    path: Icons.close
                    size: 12
                    color: theme.danger
                }

                PlainText {
                    id: wholeText
                    width: whole.width - 48
                    text: backend.problem
                    color: theme.foreground
                    wrapMode: Text.Wrap
                    // Another problem is read from its start.
                    onTextChanged: words.down = 0
                }
            }
        }

        // Beside it, while not all of it is in view: how much is, and where.
        Rectangle {
            objectName: "wholeProblemInView"
            readonly property real all: Math.max(1, wholeRow.implicitHeight)

            visible: words.more > 0
            x: whole.width - 9
            y: words.y + Math.round(words.height * Math.min(words.down, words.more) / all)
            width: 3
            height: Math.round(words.height * words.height / all)
            radius: 1
            color: theme.muted
        }
    }
}
