pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// One row of a suggestion list (the editor popup and the palette's "@" search): an icon, the
// name, the kind as a word (the icon is never the only cue) and the detail (type or schema).
// A delegate of a ListView; the list owns the current row.
Item {
    id: row

    required property int index
    required property string label
    required property string kind
    required property string kindText
    required property string detail

    readonly property bool isCurrent: ListView.isCurrentItem

    signal activated(int index)

    function iconFor(k) {
        switch (k) {
        case "schema":
            return Icons.layers;
        case "table":
            return Icons.table;
        case "view":
        case "materializedView":
            return Icons.eye;
        case "column":
            return Icons.columns3;
        case "function":
            return Icons.squareFunction;
        default:
            return Icons.fileCode;
        }
    }

    width: ListView.view.width
    height: Math.max(Theme.controlHeight, line.implicitHeight + Theme.spacing * 2)

    Accessible.role: Accessible.ListItem
    Accessible.name: label + ", " + kindText + (detail !== "" ? ", " + detail : "")
    Accessible.selected: isCurrent

    Rectangle {
        anchors.fill: parent
        color: row.isCurrent || hover.hovered ? Theme.bgHover : Qt.alpha(Theme.bgRaised, 0)

        // The current row is marked by a bar, not only by its fill.
        Rectangle {
            visible: row.isCurrent
            width: Theme.focusRingWidth
            height: parent.height
            color: Theme.accent
        }
    }

    RowLayout {
        id: line

        anchors.fill: parent
        anchors.leftMargin: Theme.spacing * 2
        anchors.rightMargin: Theme.spacing * 2
        spacing: Theme.spacing * 2

        Icon {
            text: row.iconFor(row.kind)
            color: Theme.fgMuted
        }

        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: Theme.controlHeight
            text: row.label
            font.family: Theme.monoFontFamily
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            elide: Text.ElideRight
        }

        Text {
            text: row.kindText
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
        }

        Text {
            Layout.maximumWidth: Theme.controlHeight * 5
            visible: row.detail !== ""
            text: row.detail
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            elide: Text.ElideRight
        }
    }

    HoverHandler {
        id: hover
    }

    TapHandler {
        onTapped: row.activated(row.index)
    }
}
