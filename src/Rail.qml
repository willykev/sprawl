import QtQuick
import "icons.js" as Icons

// The tools, down the left edge.
Rectangle {
    id: rail

    property var view
    property bool keysShowing: false
    signal pickImage()
    signal toggleShapes()
    signal toggleKeys()

    width: 44
    color: theme.panel

    // How far down the shape tool is, for what comes out beside it.
    readonly property real shapeTop: tools.findIndex(entry => entry.tool === "shape") * 40

    readonly property var tools: [
        { tool: "select", key: "V", icon: Icons.select },
        { tool: "pan", key: "H", icon: Icons.pan },
        { tool: "note", key: "N", icon: Icons.note },
        { tool: "text", key: "T", icon: Icons.text },
        { tool: "shape", key: "S", icon: Icons.shape },
        { tool: "pen", key: "P", icon: Icons.pen },
        { tool: "connector", key: "C", icon: Icons.connector },
        { tool: "image", key: "I", icon: Icons.image },
        { tool: "frame", key: "F", icon: Icons.frame }
    ]

    Column {
        Repeater {
            model: rail.tools

            RailButton {
                required property var modelData
                objectName: "tool-" + modelData.tool
                // A board that could not be read is only looked at: the tools
                // that make things are not to be had.
                enabled: !rail.view.readOnly || modelData.tool === "select" || modelData.tool === "pan"
                icon: modelData.icon
                key: modelData.key
                active: rail.view.tool === modelData.tool && !rail.view.editing
                onClicked: {
                    // A picture is put on the board, not a tool to hold; and
                    // the shape tool, which brings the shapes out beside
                    // it, puts them away again when it is already held.
                    if (modelData.tool === "image")
                        rail.pickImage()
                    else if (modelData.tool === "shape" && rail.view.tool === "shape")
                        rail.toggleShapes()
                    else
                        rail.view.tool = modelData.tool
                }
            }
        }
    }

    RailButton {
        anchors.bottom: parent.bottom
        label: "?"
        active: rail.keysShowing
        onClicked: rail.toggleKeys()
    }
}
