import QtQuick

// The choices there are of something, in a grid, to pick one from: the
// shapes, out by the shape tool and from the bar by a selected shape; and
// what a connector's ends, route and line can be. It is drawn from the
// canvas's own lists, so a shape added there is here without a word.
Rectangle {
    id: picker

    property var view
    // What it is a choice of: shape, route, start, end, dash.
    property string kind: "shape"
    // The one that is picked now; none when the selection is mixed.
    property string current
    // What its buttons are called, to tell one popout's from the other's.
    property string name: "shape"
    property int columns: 5
    // Whether to say which key steps through them.
    property bool hint: false
    signal picked(string choice)

    // The one the mouse is over, whose name is shown in place of the
    // current one's.
    property string hovered

    implicitWidth: grid.implicitWidth + 20
    implicitHeight: grid.implicitHeight + foot.height + 21
    color: theme.cardBackground
    border.color: theme.cardBorder
    border.width: theme.borderWidth
    radius: theme.radius

    // Clicks on the popout are for the popout.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Grid {
        id: grid
        x: 10
        y: 10
        columns: picker.columns

        Repeater {
            model: picker.view ? picker.view.options(picker.kind) : []

            Item {
                id: option
                required property string modelData
                readonly property bool chosen: picker.current === modelData
                objectName: picker.name + "-" + modelData
                width: 38
                height: 34

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    color: option.chosen ? theme.selectedFill : area.pressed ? theme.pressedFill
                           : area.containsMouse ? theme.hoverFill : "transparent"
                    border.color: option.chosen ? theme.focusBorder : "transparent"
                    border.width: theme.controlBorderWidth
                    radius: Math.min(4, theme.radius)
                }

                Icon {
                    anchors.centerIn: parent
                    size: 22
                    path: picker.view.optionIcon(picker.kind, option.modelData)
                    solid: picker.view.optionSolid(picker.kind, option.modelData)
                    color: option.chosen ? theme.accent : area.containsMouse ? theme.bright : theme.foreground
                }

                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: picker.hovered = option.modelData
                    onExited: {
                        if (picker.hovered === option.modelData)
                            picker.hovered = ""
                    }
                    onClicked: picker.picked(option.modelData)
                }
            }
        }
    }

    Rectangle {
        anchors.left: grid.left
        anchors.right: grid.right
        anchors.top: grid.bottom
        anchors.topMargin: 5
        height: 1
        color: theme.muted
        opacity: 0.7
    }

    Item {
        id: foot
        anchors.left: grid.left
        anchors.right: grid.right
        anchors.top: grid.bottom
        anchors.topMargin: 6
        height: 28

        // What the one under the mouse is called, or the one picked.
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 4
            // Up to the keys, where they are shown, and to the edge otherwise.
            anchors.right: parent.right
            anchors.rightMargin: picker.hint ? keys.width + 8 : 2
            anchors.verticalCenter: parent.verticalCenter
            // (Read again when the language changes.)
            text: {
                const choice = picker.hovered.length ? picker.hovered : picker.current
                return translator.language && choice.length ? picker.view.optionTitle(picker.kind, choice) : ""
            }
            color: theme.foreground
            font.pixelSize: theme.size("small")
            elide: Text.ElideRight
        }

        Row {
            id: keys
            visible: picker.hint
            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            KeyCap { label: "S" }
            KeyCap { label: "←" }
            KeyCap { label: "→" }
        }
    }
}
