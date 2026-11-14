import QtQuick

// What the connector tool draws: there while the tool is held.
Rectangle {
    id: bar

    property var view
    property alias open: choices.open

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
        spacing: 10

        LineOptions {
            id: choices
            anchors.verticalCenter: parent.verticalCenter
            view: bar.view
            name: "draw"
        }

        // The key that steps to the next route.
        KeyCap {
            anchors.verticalCenter: parent.verticalCenter
            label: "C"
        }
    }
}
