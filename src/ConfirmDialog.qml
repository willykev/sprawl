import QtQuick

// Asks before something that cannot be taken back.
Card {
    id: dialog

    property string title
    property string detail
    property string hint
    signal accepted()

    cardWidth: 460
    cardHeight: column.implicitHeight + 36

    function open() {
        visible = true
        keys.forceActiveFocus()
    }

    Item {
        id: keys
        focus: true
        Keys.onPressed: (event) => {
            event.accepted = true
            if (event.key === Qt.Key_Escape)
                dialog.dismissed()
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                dialog.accepted()
        }
    }

    Column {
        id: column
        x: 20
        y: 18
        width: parent.width - 40
        spacing: 8

        PlainText {
            width: parent.width
            text: dialog.title
            color: theme.bright
            font.pixelSize: theme.sizes.heading
            font.bold: true
            wrapMode: Text.Wrap
        }

        PlainText {
            width: parent.width
            visible: text.length > 0
            text: dialog.detail
            color: theme.foreground
            wrapMode: Text.Wrap
        }

        PlainText {
            width: parent.width
            topPadding: 4
            text: dialog.hint
            color: theme.dim
            font.pixelSize: theme.sizes.small
            wrapMode: Text.Wrap
        }
    }
}
