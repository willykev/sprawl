import QtQuick

// One tool on the rail: its icon, and the key that picks it.
Item {
    id: button

    property string icon
    property string label
    property string key
    property bool active: false
    signal clicked()

    width: 44
    height: 40

    Rectangle {
        anchors.fill: parent
        color: button.active ? theme.selectedFill : area.pressed ? theme.pressedFill
               : area.containsMouse ? theme.hoverFill : "transparent"
    }

    Icon {
        visible: button.icon.length > 0
        anchors.centerIn: parent
        path: button.icon
        color: button.active ? theme.accent : area.containsMouse ? theme.bright : theme.foreground
    }

    PlainText {
        visible: button.label.length > 0
        anchors.centerIn: parent
        text: button.label
        color: button.active ? theme.accent : area.containsMouse ? theme.bright : theme.foreground
        font.pixelSize: theme.size("subtitle")
    }

    PlainText {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 4
        anchors.bottomMargin: 2
        text: button.key
        color: button.active ? theme.accent : theme.faint
        font.pixelSize: Math.max(8, theme.size("caption") - 1)
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
