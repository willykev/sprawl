import QtQuick
import QtQuick.Shapes

// A line icon, drawn from a path on a 24 unit grid in whatever color it is
// given, so it follows the theme like text does.
Item {
    id: icon

    property string path
    // A part of it that is filled in rather than drawn around, if any.
    property string solid
    property color color: theme.foreground
    property real size: 16

    implicitWidth: size
    implicitHeight: size

    Shape {
        anchors.centerIn: parent
        width: 24
        height: 24
        scale: icon.size / 24
        // Curves drawn on the GPU: smooth at any size with no multisampling.
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: icon.color
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.path }
        }

        ShapePath {
            strokeColor: "transparent"
            strokeWidth: 0
            fillColor: icon.solid.length ? icon.color : "transparent"
            PathSvg { path: icon.solid }
        }
    }
}
