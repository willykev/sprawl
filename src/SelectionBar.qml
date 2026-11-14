import QtQuick
import "icons.js" as Icons

// The toolbar that floats by the selection: the theme's colors, and the two
// things most often done to what is selected.
Rectangle {
    id: bar

    property var view
    // Notes are colored with paper; everything else with the theme's own color.
    readonly property bool paper: view.selectionKind === "note"
    // The popout of shapes, for making the selected shapes another, is out.
    property bool shapesOpen: false

    onVisibleChanged: {
        if (!visible) {
            shapesOpen = false
            line.open = ""
        }
    }

    Connections {
        target: bar.view
        // Whatever was picked from it, or whatever else is selected now,
        // the popout has done its work.
        function onSelectionChanged() {
            bar.shapesOpen = false
            line.open = ""
        }
    }

    Picker {
        // On the canvas itself, over everything on it: a popout hung from
        // the bar would be outside the bar, where clicks are not the bar's.
        parent: bar.view
        z: 10
        view: bar.view
        kind: "shape"
        name: "selected"
        visible: bar.shapesOpen && bar.visible && bar.view.selectionKind === "shape"
        current: bar.view.selectionShape
        // Over the bar where there is room for it, and under it otherwise;
        // and never off the side of the canvas.
        x: Math.round(Math.max(8, Math.min(bar.view.width - 8 - width, bar.x + (bar.width - width) / 2)))
        y: Math.round(bar.y - height - 8 >= 8 ? bar.y - height - 8 : bar.y + bar.height + 8)
        onPicked: (shape) => bar.view.setSelectionShape(shape)
    }

    implicitWidth: row.implicitWidth + 24
    implicitHeight: 40
    color: theme.cardBackground
    border.color: theme.cardBorder
    border.width: theme.borderWidth
    radius: theme.radius

    // Clicks on the bar are for the bar.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        Repeater {
            model: theme.swatchNames

            Item {
                id: swatch
                required property string modelData
                readonly property bool current: bar.view.selectionColor === modelData
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16

                // The color everything selected has wears a ring.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    visible: swatch.current
                    color: "transparent"
                    border.color: theme.bright
                    border.width: 1
                    radius: Math.min(4, theme.radius)
                }

                Rectangle {
                    anchors.fill: parent
                    color: bar.paper ? theme.note(swatch.modelData) : theme.swatch(swatch.modelData)
                    radius: Math.min(2, theme.radius)
                    opacity: swatchArea.containsMouse || swatch.current ? 1 : 0.92
                }

                MouseArea {
                    id: swatchArea
                    anchors.fill: parent
                    anchors.margins: -3
                    hoverEnabled: true
                    onClicked: bar.view.setSelectionColor(swatch.modelData)
                }
            }
        }

        // Shapes can be made another shape.
        Item {
            visible: bar.view.selectionKind === "shape"
            width: 15
            height: 22
            anchors.verticalCenter: parent.verticalCenter

            Rectangle {
                anchors.centerIn: parent
                width: 1
                height: parent.height
                color: theme.muted
            }
        }

        // The shape they are, and a way to all the others.
        Item {
            id: shapeButton
            objectName: "selected-shape"
            visible: bar.view.selectionKind === "shape"
            anchors.verticalCenter: parent.verticalCenter
            width: 46
            height: 26

            Rectangle {
                anchors.fill: parent
                color: bar.shapesOpen ? theme.selectedFill : shapeArea.pressed ? theme.pressedFill
                       : shapeArea.containsMouse ? theme.hoverFill : "transparent"
                radius: Math.min(4, theme.radius)
            }

            Icon {
                x: 5
                anchors.verticalCenter: parent.verticalCenter
                size: 20
                // Shapes of several kinds have no one shape to show.
                path: bar.view.selectionShape.length ? bar.view.shapeIcon(bar.view.selectionShape) : Icons.shape
                color: bar.shapesOpen ? theme.accent : shapeArea.containsMouse ? theme.bright : theme.foreground
            }

            Icon {
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                size: 11
                path: Icons.chevronDown
                color: theme.dim
            }

            MouseArea {
                id: shapeArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: bar.shapesOpen = !bar.shapesOpen
            }
        }

        // Connectors: their ends, their route, their line.
        Item {
            visible: bar.view.selectionKind === "connector"
            width: 15
            height: 22
            anchors.verticalCenter: parent.verticalCenter

            Rectangle {
                anchors.centerIn: parent
                width: 1
                height: parent.height
                color: theme.muted
            }
        }

        LineOptions {
            id: line
            visible: bar.view.selectionKind === "connector"
            anchors.verticalCenter: parent.verticalCenter
            view: bar.view
            ofSelection: true
            name: "selected"
        }

        Item {
            width: 15
            height: 22
            anchors.verticalCenter: parent.verticalCenter

            Rectangle {
                anchors.centerIn: parent
                width: 1
                height: parent.height
                color: theme.muted
            }
        }

        Repeater {
            model: [
                { icon: Icons.copy, action: "duplicate" },
                { icon: Icons.trash, action: "delete" }
            ]

            Item {
                id: action
                required property var modelData
                anchors.verticalCenter: parent.verticalCenter
                width: 26
                height: 26

                Rectangle {
                    anchors.fill: parent
                    color: actionArea.pressed ? theme.pressedFill : actionArea.containsMouse ? theme.hoverFill : "transparent"
                    radius: Math.min(4, theme.radius)
                }

                Icon {
                    anchors.centerIn: parent
                    path: action.modelData.icon
                    color: actionArea.containsMouse ? theme.bright : theme.foreground
                }

                MouseArea {
                    id: actionArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: action.modelData.action === "delete" ? bar.view.deleteSelection()
                                                                    : bar.view.duplicateSelection()
                }
            }
        }
    }
}
