import QtQuick

// Ctrl+E: what kind of file to make of the board, or of what is selected.
Card {
    id: dialog
    objectName: "export"

    // What was selected as it opened: what is exported, whatever is
    // selected by the time a format is chosen. None is all of the board.
    property var ids: []
    readonly property bool selection: ids.length > 0
    property int current: 0
    signal chosen(string format)

    readonly property var formats: [
        { format: "png", label: qsTr("PNG picture"), key: "P" },
        { format: "svg", label: qsTr("SVG drawing"), key: "S" },
        { format: "pdf", label: qsTr("PDF document"), key: "D" }
    ]

    // Wide enough for its title and its hint, with room on either side, at
    // whatever size the type is.
    cardWidth: Math.max(380, title.implicitWidth + 40, hint.implicitWidth + 56)
    cardHeight: column.implicitHeight + 32

    function open(selected) {
        ids = selected
        current = 0
        visible = true
        keys.forceActiveFocus()
    }

    Item {
        id: keys
        focus: true
        Keys.onPressed: (event) => {
            event.accepted = true
            if (event.key === Qt.Key_Escape) {
                dialog.dismissed()
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                dialog.chosen(dialog.formats[dialog.current].format)
            } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Tab) {
                dialog.current = Math.min(dialog.formats.length - 1, dialog.current + 1)
            } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Backtab) {
                dialog.current = Math.max(0, dialog.current - 1)
            } else {
                const picked = dialog.formats.findIndex(entry => entry.key === event.text.toUpperCase())
                if (picked >= 0)
                    dialog.chosen(dialog.formats[picked].format)
                else
                    event.accepted = false
            }
        }
    }

    Column {
        id: column
        x: 16
        y: 14
        width: parent.width - 32

        PlainText {
            id: title
            x: 4
            height: 34
            text: dialog.selection ? qsTr("Export what is selected") : qsTr("Export the board")
            color: theme.bright
            font.pixelSize: theme.sizes.heading
            font.bold: true
        }

        Repeater {
            model: dialog.formats

            Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool selected: index === dialog.current
                width: column.width
                height: 28
                color: selected ? theme.hoverFill : rowArea.containsMouse ? theme.controlFill : "transparent"
                border.color: selected ? theme.focusBorder : "transparent"
                border.width: theme.controlBorderWidth
                radius: Math.min(4, theme.radius)

                PlainText {
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.label
                    color: row.selected ? theme.accent : theme.foreground
                }

                KeyCap {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    label: row.modelData.key
                }

                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: dialog.chosen(row.modelData.format)
                }
            }
        }

        PlainText {
            id: hint
            x: 12
            height: 32
            verticalAlignment: Text.AlignVCenter
            text: qsTr("Enter chooses where to save it · Esc closes")
            color: theme.dim
            font.pixelSize: theme.sizes.small
        }
    }
}
