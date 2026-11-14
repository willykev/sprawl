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
        id: symbol
        visible: button.icon.length > 0
        anchors.centerIn: parent
        path: button.icon
        // Faint when it is not to be had.
        color: button.active ? theme.accent : !button.enabled ? theme.faint
               : area.containsMouse ? theme.bright : theme.foreground
    }

    PlainText {
        visible: button.label.length > 0
        anchors.centerIn: parent
        text: button.label
        color: button.active ? theme.accent : area.containsMouse ? theme.bright : theme.foreground
        font.pixelSize: theme.sizes.subtitle
    }

    // The key, in the corner beside the icon: no bigger than there is room
    // for between the icon and the edge, however big the type, so it never
    // runs into the icon.
    PlainText {
        anchors.left: symbol.right
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 2
        anchors.rightMargin: 4
        anchors.bottomMargin: 2
        horizontalAlignment: Text.AlignRight
        verticalAlignment: Text.AlignBottom
        fontSizeMode: Text.HorizontalFit
        minimumPixelSize: 8
        text: button.key
        color: button.active ? theme.accent : theme.faint
        font.pixelSize: Math.max(8, theme.sizes.caption - 1)
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
