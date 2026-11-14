import QtQuick
import "icons.js" as Icons

// What a connector is like, to see and to change: what is at its start, how
// it gets from one end to the other, how its line is drawn, and what is at
// its end. Each is a button showing how it is now, which brings out the
// choices there are. It is the same four for the connectors that are
// selected and for the ones the connector tool is about to draw.
Item {
    id: options

    property var view
    // Of the connectors that are selected, rather than of the tool's.
    property bool ofSelection: false
    // What its buttons are called, to tell one bar's from the other's.
    property string name: "line"
    // The one whose choices are out; none when none are.
    property string open: ""
    readonly property var kinds: ["start", "route", "dash", "end"]
    readonly property var now: ofSelection ? view.selectionLine : view.line
    // Where this is on the canvas, as of when the choices came out.
    property point host

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    function pick(kind, choice) {
        if (ofSelection)
            view.setSelectionLine(kind, choice)
        else
            view.setLine(kind, choice)
        open = ""
    }

    onOpenChanged: {
        if (open.length)
            host = options.mapToItem(view, 0, 0)
    }

    Row {
        id: row
        spacing: 2

        Repeater {
            model: options.kinds

            Item {
                id: button
                required property string modelData
                // Nothing, when the selected connectors differ in it.
                readonly property string current: options.now[modelData] || ""
                readonly property bool out: options.open === modelData
                objectName: options.name + "-" + modelData
                width: 40
                height: 26

                Rectangle {
                    anchors.fill: parent
                    color: button.out ? theme.selectedFill : area.pressed ? theme.pressedFill
                           : area.containsMouse ? theme.hoverFill : "transparent"
                    radius: Math.min(4, theme.radius)
                }

                Icon {
                    anchors.centerIn: parent
                    size: 22
                    path: button.current.length ? options.view.optionIcon(button.modelData, button.current) : Icons.mixed
                    solid: button.current.length ? options.view.optionSolid(button.modelData, button.current) : ""
                    color: button.out ? theme.accent : area.containsMouse ? theme.bright : theme.foreground
                }

                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: options.open = button.out ? "" : button.modelData
                }
            }
        }
    }

    Picker {
        // On the canvas itself, over everything on it: a popout hung from
        // the bar would be outside the bar, where clicks are not the bar's.
        parent: options.view
        z: 10
        view: options.view
        visible: options.open.length > 0 && options.visible
        kind: options.open.length ? options.open : "route"
        name: options.name + "-" + options.open
        columns: 3
        current: options.now[options.open] || ""
        // Over the button it belongs to, clear of the bar this is in: above
        // where there is room, below otherwise, and never off the side.
        x: {
            const over = options.host.x + options.kinds.indexOf(options.open) * 42 + 20 - width / 2
            return Math.round(Math.max(8, Math.min(options.view.width - 8 - width, over)))
        }
        y: Math.round(options.host.y - height - 15 >= 8 ? options.host.y - height - 15
                                                        : options.host.y + options.height + 15)
        onPicked: (choice) => options.pick(options.open, choice)
    }
}
