import QtQuick

// The interface's text: exactly the characters it is given, and nothing
// made of them. Left to itself, Text reads anything that looks like markup
// as markup, so the name of a board or of a file, or the words of a file
// that would not open, could set themselves in bold or have a picture
// fetched from wherever they said. Everything written in the window is
// one of these.
Text {
    textFormat: Text.PlainText
}
