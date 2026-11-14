import QtQuick

// A key as the shortcuts overlay shows one.
Rectangle {
    id: cap

    property string label

    implicitWidth: Math.max(20, text.implicitWidth + 14)
    // Taller once its letters need it, as they do at the shell's larger
    // sizes, so they stay clear of its border.
    implicitHeight: Math.max(20, text.implicitHeight + 2 * border.width)
    color: "transparent"
    border.color: theme.controlBorder
    border.width: theme.controlBorderWidth
    radius: Math.min(4, theme.radius)

    PlainText {
        id: text
        anchors.centerIn: parent
        text: cap.label
        color: theme.foreground
        font.pixelSize: theme.sizes.small
    }
}
