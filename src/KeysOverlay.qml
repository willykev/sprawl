import QtQuick

// Every shortcut, on one card.
Card {
    id: overlay
    objectName: "keys"

    cardWidth: columns.implicitWidth + 40
    cardHeight: columns.implicitHeight + 76

    readonly property var sections: [
        {
            title: qsTr("TOOLS"), width: 190, rows: [
                { label: qsTr("Select"), keys: ["V"] },
                { label: qsTr("Pan"), keys: ["H"] },
                { label: qsTr("Note"), keys: ["N"] },
                { label: qsTr("Text"), keys: ["T"] },
                { label: qsTr("Shape, next shape"), keys: ["S", "S"] },
                { label: qsTr("Pen"), keys: ["P"] },
                { label: qsTr("Connector, next route"), keys: ["C", "C"] },
                { label: qsTr("Image"), keys: ["I"] },
                { label: qsTr("Frame"), keys: ["F"] },
                { label: qsTr("Back to select"), keys: ["Esc"] }
            ]
        },
        {
            title: qsTr("BOARD"), width: 290, rows: [
                { label: qsTr("New note under the pointer"), keys: ["Space"] },
                { label: qsTr("Edit text"), keys: ["Enter"] },
                { label: qsTr("Undo"), keys: ["Ctrl", "Z"] },
                { label: qsTr("Redo"), keys: ["Ctrl", "Shift", "Z"] },
                { label: qsTr("Copy, cut, paste"), keys: ["Ctrl", "C", "X", "V"] },
                { label: qsTr("Duplicate"), keys: ["Ctrl", "D"] },
                { label: qsTr("Delete"), keys: ["Del"] },
                { label: qsTr("Select all"), keys: ["Ctrl", "A"] },
                { label: qsTr("Switch board"), keys: ["Ctrl", "O"] },
                { label: qsTr("Export"), keys: ["Ctrl", "E"] }
            ]
        },
        {
            title: qsTr("ARRANGE"), width: 260, rows: [
                { label: qsTr("Move"), keys: ["←", "↑", "↓", "→"] },
                { label: qsTr("Move further"), keys: ["Shift", "←"] },
                { label: qsTr("Group"), keys: ["Ctrl", "G"] },
                { label: qsTr("Ungroup"), keys: ["Ctrl", "Shift", "G"] },
                { label: qsTr("Bring to front"), keys: ["]"] },
                { label: qsTr("Send to back"), keys: ["["] },
                { label: qsTr("Bold text"), keys: ["Ctrl", "B"] },
                { label: qsTr("Bigger text"), keys: ["Ctrl", "+"] },
                { label: qsTr("Smaller text"), keys: ["Ctrl", "-"] },
                { label: qsTr("Pen thinner, thicker"), keys: ["[", "]"] }
            ]
        },
        {
            title: qsTr("VIEW"), width: 220, rows: [
                { label: qsTr("Zoom in"), keys: ["+"] },
                { label: qsTr("Zoom out"), keys: ["-"] },
                { label: qsTr("Actual size"), keys: ["0"] },
                { label: qsTr("Fit board"), keys: ["1"] },
                { label: qsTr("Pan"), keys: [qsTr("Scroll")] },
                { label: qsTr("Zoom"), keys: ["Ctrl", qsTr("Scroll")] }
            ]
        },
        {
            title: qsTr("APP"), width: 220, rows: [
                { label: qsTr("Shortcuts"), keys: ["?"] },
                { label: qsTr("Quit"), keys: ["Q"] }
            ]
        }
    ]

    PlainText {
        x: 20
        y: 18
        text: qsTr("Keys")
        color: theme.bright
        font.pixelSize: theme.sizes.heading
        font.bold: true
    }

    PlainText {
        anchors.right: parent.right
        anchors.rightMargin: 20
        y: 22
        text: qsTr("? or Esc closes")
        color: theme.foreground
        font.pixelSize: theme.sizes.small
    }

    // Sections run down each column and over into the next, so the card is
    // about as wide as it is tall whatever the list grows to.
    Flow {
        id: columns
        x: 20
        y: 56
        flow: Flow.TopToBottom
        height: 28 * 11 + 32
        spacing: 36

        Repeater {
            model: overlay.sections

            Column {
                id: section
                required property var modelData
                // As wide as the section is set, or as the widest thing in it
                // needs at the size the type is now: a label never runs under
                // its keys.
                width: {
                    let widest = modelData.width
                    for (let i = 0; i < children.length; ++i)
                        widest = Math.max(widest, children[i].implicitWidth)
                    return widest
                }
                bottomPadding: 4

                PlainText {
                    text: section.modelData.title
                    color: theme.accent
                    opacity: 0.85
                    font.pixelSize: theme.sizes.small
                    font.bold: true
                    font.letterSpacing: 1
                    height: 28
                }

                Repeater {
                    model: section.modelData.rows

                    Item {
                        id: row
                        required property var modelData
                        // Its label, room to tell them apart, and its keys.
                        implicitWidth: label.implicitWidth + 16 + caps.implicitWidth
                        width: section.width
                        height: 28

                        PlainText {
                            id: label
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.modelData.label
                            color: theme.foreground
                        }

                        Row {
                            id: caps
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 5

                            Repeater {
                                model: row.modelData.keys
                                KeyCap {
                                    required property string modelData
                                    label: modelData
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
