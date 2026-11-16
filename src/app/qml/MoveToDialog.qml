pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// "Move to…": picks the folder of a saved query (or the top level) from a list, so moving works
// without dragging. Up/Down choose, Enter moves.
QDialog {
    id: root

    property string queryName: ""
    property string currentFolderId: ""
    property var targets: [] // [{id, name}], the top level first
    readonly property alias targetsList: list
    readonly property alias cancelButton: cancelBtn
    readonly property alias moveButton: moveBtn
    readonly property string chosenId: list.currentIndex >= 0 && list.currentIndex < targets.length ? targets[list.currentIndex].id : ""
    readonly property bool changes: list.currentIndex >= 0 && chosenId !== currentFolderId

    signal chosen(string folderId)

    function openFor(name, folders, currentId) {
        queryName = name;
        currentFolderId = currentId;
        targets = [
            {
                "id": "",
                "name": qsTr("Top level")
            }
        ].concat(folders);
        const at = targets.findIndex(t => t.id === currentId);
        list.currentIndex = Math.max(0, at);
        open();
    }

    function submit() {
        if (!changes)
            return;
        const id = chosenId;
        close();
        chosen(id);
    }

    title: qsTr("Move to…")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onOpened: list.forceActiveFocus(Qt.TabFocusReason)

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 2

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 14
            text: qsTr("Move “%1” to:").arg(root.queryName)
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        ListView {
            id: list

            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(Math.max(contentHeight, Theme.controlHeight), Theme.controlHeight * 8)
            clip: true
            model: root.targets
            activeFocusOnTab: true
            keyNavigationEnabled: true
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

            Keys.onReturnPressed: root.submit()
            Keys.onEnterPressed: root.submit()

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Folders")
            Accessible.focusable: true

            delegate: Item {
                id: option

                required property int index
                required property var modelData
                readonly property bool current: ListView.isCurrentItem
                readonly property bool here: modelData.id === root.currentFolderId

                width: ListView.view.width
                implicitHeight: Theme.controlHeight

                Accessible.role: Accessible.ListItem
                Accessible.name: here ? qsTr("%1, current location").arg(modelData.name) : modelData.name
                Accessible.selected: current
                Accessible.focusable: true

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    color: option.current ? Theme.selectionBg : Qt.alpha(Theme.bgRaised, 0)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing * 2
                    anchors.rightMargin: Theme.spacing * 2
                    spacing: Theme.spacing

                    Icon {
                        text: option.modelData.id === "" ? Icons.database : Icons.folder
                        color: option.current ? Theme.selectionFg : Theme.fgMuted
                    }

                    Text {
                        Layout.fillWidth: true
                        text: option.here ? qsTr("%1 (current)").arg(option.modelData.name) : option.modelData.name
                        font.pointSize: Theme.fontSize
                        color: option.current ? Theme.selectionFg : Theme.fgDefault
                        elide: Text.ElideRight
                        Accessible.ignored: true
                    }
                }

                TapHandler {
                    onTapped: list.currentIndex = option.index
                    onDoubleTapped: {
                        list.currentIndex = option.index;
                        root.submit();
                    }
                }
            }

            FocusRing {
                shown: list.activeFocus
                inset: true
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: cancelBtn

                text: qsTr("Cancel")
                onClicked: root.close()
            }

            QButton {
                id: moveBtn

                text: qsTr("Move")
                variant: "primary"
                enabled: root.changes
                onClicked: root.submit()
            }
        }
    }
}
