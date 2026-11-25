pragma ComponentBehavior: Bound
import QtQuick
import Quarry.Controls

// Compact column type badge shown before a column name in table headers and the transposed view:
//   "text"   -> A-Z
//   "number" -> 123
//   "time"   -> clock glyph
//   "binary" -> 01 / 10 stacked tightly in one single-line glyph
//   "json"   -> { }
// Decorative: ignored by the accessibility tree (the column name and type live on the header).
Item {
    id: root

    property string kind: ""
    property color badgeColor: Theme.fgMuted

    readonly property string labelText: kind === "text" ? "A-Z" : kind === "number" ? "123" : kind === "json" ? "{ }" : ""

    visible: kind !== ""
    Accessible.ignored: true

    implicitWidth: kind === "time" ? clockIcon.implicitWidth : kind === "binary" ? binaryBox.implicitWidth : labelText !== "" ? labelItem.implicitWidth : 0
    implicitHeight: kind === "time" ? clockIcon.implicitHeight : kind === "binary" ? binaryBox.implicitHeight : labelText !== "" ? labelItem.implicitHeight : 0

    Text {
        id: labelItem

        anchors.centerIn: parent
        visible: root.labelText !== ""
        Accessible.ignored: true
        textFormat: Text.PlainText
        text: root.labelText
        font.family: Theme.monoFontFamily
        font.pointSize: Math.max(6, Theme.fontSize * 0.72)
        font.bold: true
        color: root.badgeColor
    }

    Icon {
        id: clockIcon

        anchors.centerIn: parent
        visible: root.kind === "time"
        Accessible.ignored: true
        text: Icons.clock
        font.pointSize: Theme.fontSize * 0.95
        color: root.badgeColor
    }

    // Two micro-lines ("01" over "10") stacked tightly so the whole symbol fits in a single line.
    Column {
        id: binaryBox

        anchors.centerIn: parent
        visible: root.kind === "binary"
        spacing: -Math.max(1, Math.round(Theme.fontSize * 0.18))
        Accessible.ignored: true

        Text {
            Accessible.ignored: true
            textFormat: Text.PlainText
            text: "01"
            font.family: Theme.monoFontFamily
            font.pointSize: Math.max(5, Theme.fontSize * 0.52)
            font.bold: true
            lineHeight: 0.85
            color: root.badgeColor
        }

        Text {
            Accessible.ignored: true
            textFormat: Text.PlainText
            text: "10"
            font.family: Theme.monoFontFamily
            font.pointSize: Math.max(5, Theme.fontSize * 0.52)
            font.bold: true
            lineHeight: 0.85
            color: root.badgeColor
        }
    }
}
