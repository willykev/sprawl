import QtQuick

// What the pen draws with: its color and its width.
Rectangle {
    id: bar

    property var view

    implicitWidth: row.implicitWidth + 24
    implicitHeight: 40
    color: theme.cardBackground
    border.color: theme.cardBorder
    border.width: theme.borderWidth
    radius: theme.radius

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
            // The pen's own color first: ink that reads on the view.
            model: [""].concat(theme.swatchNames)

            Item {
                id: swatch
                required property string modelData
                readonly property bool current: bar.view.penColor === modelData
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16

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
                    color: swatch.modelData.length ? theme.swatch(swatch.modelData) : theme.bright
                    radius: Math.min(2, theme.radius)
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -3
                    onClicked: bar.view.penColor = swatch.modelData
                }
            }
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
            model: [1, 2, 4, 8]

            Item {
                id: step
                required property int modelData
                readonly property bool current: bar.view.penWidth === modelData
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 26

                Rectangle {
                    anchors.fill: parent
                    color: step.current ? theme.selectedFill : stepArea.containsMouse ? theme.hoverFill : "transparent"
                    radius: Math.min(4, theme.radius)
                }

                // A dot as wide as the stroke.
                Rectangle {
                    anchors.centerIn: parent
                    width: step.modelData + 2
                    height: width
                    radius: width / 2
                    color: step.current ? theme.accent : theme.foreground
                }

                MouseArea {
                    id: stepArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: bar.view.penWidth = step.modelData
                }
            }
        }
    }
}
