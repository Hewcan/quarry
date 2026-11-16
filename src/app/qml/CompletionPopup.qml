pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The suggestions at the cursor of the SQL editor. It is not a Popup and never takes the focus:
// the editor keeps it and drives the list (move, accept), so typing is never interrupted.
// Below the caret; above it when there is no room below and more above.
Item {
    id: root

    required property var suggestions // CompletionModel
    // The caret in the coordinates of the parent (the editor).
    property rect caret: Qt.rect(0, 0, 0, 0)
    // "Connect to suggest tables" and the like: shown above the rows, or alone.
    property string notice: ""
    property bool shown: false

    readonly property alias listView: list
    readonly property alias currentIndex: list.currentIndex
    readonly property int count: list.count
    readonly property int visibleRows: 8
    readonly property real wanted: (notice !== "" ? noticeText.implicitHeight + Theme.spacing * 2 : 0) + Math.min(list.count, visibleRows) * Theme.controlHeight + 2
    readonly property real below: parent.height - (caret.y + caret.height)
    readonly property real above: caret.y
    readonly property bool flipped: wanted > below && above > below

    signal accepted(int index)

    function select(index) {
        list.currentIndex = list.count > 0 ? Math.max(0, Math.min(index, list.count - 1)) : -1;
    }

    function move(step) {
        select(list.currentIndex + step);
    }

    function movePage(down) {
        const rows = Math.max(1, Math.floor(list.height / Theme.controlHeight) - 1);
        move(down ? rows : -rows);
    }

    visible: shown && (list.count > 0 || notice !== "")
    z: 10
    width: Math.min(Theme.controlHeight * 14, parent.width)
    height: Math.max(Theme.controlHeight, Math.min(wanted, flipped ? above : below))
    x: Math.max(0, Math.min(caret.x, parent.width - width))
    y: flipped ? caret.y - height : caret.y + caret.height

    Rectangle {
        anchors.fill: parent
        color: Theme.bgRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0

        Text {
            id: noticeText

            Layout.fillWidth: true
            Layout.margins: Theme.spacing
            visible: root.notice !== ""
            text: root.notice
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WordWrap

            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        ListView {
            id: list

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: count > 0
            clip: true
            model: root.suggestions
            currentIndex: 0
            focus: false
            activeFocusOnTab: false
            keyNavigationEnabled: false
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Suggestions")

            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

            delegate: SuggestionRow {
                onActivated: index => {
                    list.currentIndex = index;
                    root.accepted(index);
                }
            }
        }
    }
}
