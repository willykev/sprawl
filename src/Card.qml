import QtQuick

// A panel over the window, as the Omarchy menu draws its own: the board
// dimmed behind a bordered card. Clicking the dimmed board closes it.
Item {
    id: overlay

    default property alias content: card.data
    property alias cardWidth: card.width
    property alias cardHeight: card.height
    signal dismissed()

    anchors.fill: parent
    visible: false

    Rectangle {
        anchors.fill: parent
        color: theme.scrim
    }

    // Nothing behind the panel hears the mouse while it is up.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onClicked: overlay.dismissed()
        onWheel: (wheel) => wheel.accepted = true
    }

    Rectangle {
        id: card
        objectName: "card"
        anchors.centerIn: parent
        // Shrunk to fit a window too small for it, so nothing is ever cut off.
        scale: Math.min(1, (overlay.width - 32) / Math.max(1, width), (overlay.height - 32) / Math.max(1, height))
        color: theme.menuBackground
        border.color: theme.menuBorder
        border.width: theme.borderWidth
        radius: theme.radius

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }
    }
}
