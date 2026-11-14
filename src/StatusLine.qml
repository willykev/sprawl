import QtQuick
import "icons.js" as Icons

// The line along the bottom: what mode the canvas is in, which board is
// open, what is selected, whether it is saved, and where the view is.
Rectangle {
    id: bar

    property var view
    signal openBoards()

    // As tall as the Omarchy bar across the top of the screen.
    height: Math.round(26 * theme.fontSize / 12)
    color: theme.panel

    readonly property string mode: view.editing ? "edit" : view.tool
    // Like an editor's mode line: one color for looking, one for making,
    // one for the pen, one for typing.
    readonly property color modeColor: mode === "select" ? theme.accent
                                       : mode === "pan" ? theme.swatch("cyan")
                                       : mode === "pen" ? theme.swatch("magenta")
                                       : mode === "edit" ? theme.swatch("yellow")
                                       : theme.swatch("green")
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
            width: modeText.implicitWidth + 28
            height: parent.height
            color: bar.modeColor

            Text {
                id: modeText
                anchors.centerIn: parent
                text: bar.modeName
                color: theme.inkOn(bar.modeColor)
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

                Text {
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

            Text {
                text: backend.problem
                color: theme.danger
            }
        }

        MouseArea {
            anchors.fill: parent
            enabled: backend.problem.length > 0
            cursorShape: Qt.PointingHandCursor
            onClicked: backend.dismissProblem()
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            visible: backend.problem.length === 0

            // The pen's color, beside its width.
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                visible: bar.view.tool === "pen" && !bar.view.editing && backend.notice.length === 0
                width: 8
                height: 8
                radius: Math.min(2, theme.radius)
                color: bar.view.penColor.length ? theme.swatch(bar.view.penColor) : theme.bright
            }

            Text {
                text: backend.notice.length ? backend.notice : bar.context
                color: theme.foreground
            }
        }
    }

    Row {
        id: right
        anchors.right: parent.right
        height: parent.height

        // More is in view than a frame is allowed to hold, so some of it
        // is left out. Said, rather than done quietly.
        Text {
            visible: bar.view.simplified && bar.width >= 760
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            rightPadding: 16
            text: qsTr("zoom in for full detail")
            color: theme.warning
        }

        Row {
            height: parent.height
            spacing: 8
            rightPadding: 14

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6
                height: 6
                color: backend.saveState === "saved" ? theme.good
                       : backend.saveState === "failed" ? theme.danger : theme.warning
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: backend.saveState === "saved" ? qsTr("saved")
                      : backend.saveState === "failed" ? qsTr("not saved") : qsTr("saving")
                color: theme.foreground
            }
        }

        // Where the mouse is on the board. First to go when the window is narrow.
        Rectangle {
            visible: bar.width >= 760
            width: visible ? coordinates.implicitWidth + 28 : 0
            height: parent.height
            color: theme.raised

            Text {
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

            Text {
                id: zoomText
                anchors.centerIn: parent
                text: qsTr("%1%").arg(Math.round(bar.view.zoom * 100))
                color: theme.inkOn(bar.modeColor)
                font.bold: true
            }

            MouseArea {
                anchors.fill: parent
                onClicked: bar.view.zoomActual()
            }
        }
    }
}
