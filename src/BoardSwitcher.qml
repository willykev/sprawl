import QtQuick
import "icons.js" as Icons

// Ctrl+O: the boards, found by typing a few letters of a name. Also where a
// board is made, renamed, or thrown away.
Card {
    id: switcher

    // switch: pick a board. create: a new board gets the name typed.
    // rename: the open board does. remove: asking before the open board
    // goes to the trash.
    property string mode: "switch"
    property var matches: []
    property int current: 0

    readonly property string typed: backend.tidyName(input.text)
    readonly property bool fresh: mode === "switch" && typed.length > 0 && !matches.some(match => match.name === typed)

    cardWidth: 520
    cardHeight: column.implicitHeight + 32

    function open() {
        mode = "switch"
        input.text = ""
        refresh()
        visible = true
        input.forceActiveFocus()
    }

    function openNew() {
        open()
        mode = "create"
        refresh()
    }

    function refresh() {
        matches = mode === "switch" ? backend.findBoards(input.text) : []
        current = 0
    }

    function accept() {
        if (mode === "create") {
            create()
        } else if (mode === "rename") {
            if (backend.renameBoard(input.text))
                dismissed()
        } else if (mode === "remove") {
            if (backend.deleteBoard())
                dismissed()
        } else if (matches.length > 0) {
            if (backend.openBoard(matches[current].name))
                dismissed()
        } else if (fresh) {
            create()
        }
    }

    function create() {
        if (backend.newBoard(typed))
            dismissed()
    }

    function back() {
        if (mode === "switch" || mode === "create") {
            dismissed()
            return
        }
        mode = "switch"
        input.text = ""
        refresh()
    }

    Column {
        id: column
        x: 16
        y: 16
        width: parent.width - 32

        Rectangle {
            width: parent.width
            height: 32
            color: theme.controlFill
            border.color: theme.controlBorder
            border.width: theme.controlBorderWidth
            radius: Math.min(4, theme.radius)

            Icon {
                id: prompt
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                path: Icons.chevronRight
                size: 14
                color: switcher.mode === "remove" ? theme.danger : theme.accent
            }

            Text {
                anchors.left: prompt.right
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: input.text.length === 0 || switcher.mode === "remove"
                text: switcher.mode === "create" ? qsTr("Name the new board")
                      : switcher.mode === "rename" ? qsTr("A new name for %1").arg(backend.boardName)
                      : switcher.mode === "remove" ? qsTr("Move %1 to the trash?").arg(backend.boardName)
                      : qsTr("Switch board")
                color: switcher.mode === "remove" ? theme.foreground : theme.dim
            }

            TextInput {
                id: input
                anchors.left: prompt.right
                anchors.right: parent.right
                anchors.leftMargin: 8
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                // Asking about the trash takes a yes or a no, not a name.
                readOnly: switcher.mode === "remove"
                opacity: switcher.mode === "remove" ? 0 : 1
                color: theme.bright
                selectionColor: Qt.alpha(theme.accent, 0.35)
                selectedTextColor: theme.bright
                clip: true
                onTextEdited: switcher.refresh()

                Keys.onPressed: (event) => {
                    event.accepted = true
                    const control = event.modifiers & Qt.ControlModifier
                    if (event.key === Qt.Key_Escape) {
                        switcher.back()
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        switcher.accept()
                    } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Tab) {
                        switcher.current = Math.min(switcher.matches.length - 1, switcher.current + 1)
                    } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Backtab) {
                        switcher.current = Math.max(0, switcher.current - 1)
                    } else if (control && event.key === Qt.Key_N && switcher.mode === "switch") {
                        // With a name typed it is made at once; without, it asks for one.
                        if (switcher.typed.length > 0)
                            switcher.create()
                        else
                            switcher.mode = "create"
                    } else if (event.key === Qt.Key_F2 && switcher.mode === "switch") {
                        switcher.mode = "rename"
                        input.text = backend.boardName
                        input.selectAll()
                        switcher.refresh()
                    } else if (control && event.key === Qt.Key_Delete && switcher.mode === "switch") {
                        switcher.mode = "remove"
                        input.text = ""
                        switcher.refresh()
                    } else {
                        event.accepted = false
                    }
                }
            }
        }

        Item { width: 1; height: 8 }

        Repeater {
            // As many as fit comfortably; typing narrows the rest down.
            model: switcher.matches.slice(0, 9)

            Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool selected: index === switcher.current
                width: column.width
                height: 28
                color: selected ? theme.hoverFill : rowArea.containsMouse ? theme.controlFill : "transparent"
                border.color: selected ? theme.focusBorder : "transparent"
                border.width: theme.controlBorderWidth
                radius: Math.min(4, theme.radius)

                Text {
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.name
                    color: row.selected ? theme.accent : theme.foreground
                    elide: Text.ElideRight
                    width: parent.width - 120
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.modelData.current
                    text: qsTr("open now")
                    color: row.selected ? theme.accent : theme.dim
                    font.pixelSize: theme.size("small")
                }

                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        switcher.current = row.index
                        switcher.accept()
                    }
                }
            }
        }

        Text {
            visible: switcher.mode === "switch" && switcher.matches.length === 0
            x: 12
            height: 28
            verticalAlignment: Text.AlignVCenter
            text: switcher.fresh ? qsTr("No board is called that. Enter makes it.") : qsTr("There are no boards yet.")
            color: theme.dim
        }

        Item {
            width: column.width
            height: 21
            visible: switcher.mode === "switch"

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 1
                color: theme.muted
                opacity: 0.7
            }
        }

        Repeater {
            model: switcher.mode !== "switch" ? [] : [
                { label: switcher.fresh ? qsTr("New board %1").arg(switcher.typed) : qsTr("New board"), keys: ["Ctrl", "N"] },
                { label: qsTr("Rename %1").arg(backend.boardName), keys: ["F2"] },
                { label: qsTr("Delete %1").arg(backend.boardName), keys: ["Ctrl", "Del"] }
            ]

            Item {
                id: action
                required property var modelData
                width: column.width
                height: 28

                Text {
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: action.modelData.label
                    color: theme.foreground
                    elide: Text.ElideRight
                    width: parent.width - 130
                }

                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5

                    Repeater {
                        model: action.modelData.keys
                        KeyCap {
                            required property string modelData
                            label: modelData
                        }
                    }
                }
            }
        }

        Text {
            x: 12
            height: 30
            verticalAlignment: Text.AlignVCenter
            text: switcher.mode === "create" ? qsTr("Enter makes it · Esc closes")
                  : switcher.mode === "rename" ? qsTr("Enter renames · Esc goes back")
                  : switcher.mode === "remove" ? qsTr("Enter moves it to the trash · Esc keeps it")
                  : qsTr("Enter opens · Esc closes")
            color: theme.dim
            font.pixelSize: theme.size("small")
        }
    }
}
