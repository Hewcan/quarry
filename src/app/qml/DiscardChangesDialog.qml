pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Asks before unsaved changes of a data tab are lost (closing the tab, refreshing, discarding,
// quitting). `action` says what will happen next; the focus starts on Cancel and the destructive
// button is explicit. Saving from here is not offered: save first, then continue.
QDialog {
    id: root

    property string action: ""
    property string tabId: ""
    property string tabTitle: ""
    property int count: 0
    readonly property alias cancelButton: cancelBtn
    readonly property alias discardButton: discardBtn
    readonly property string message: {
        const what = count === 1 ? qsTr("1 unsaved change") : qsTr("%1 unsaved changes").arg(count);
        if (action === "quit")
            return qsTr("%1 would be lost if you quit Quarry now.").arg(what);
        return qsTr("“%1” has %2. They will be lost.").arg(tabTitle).arg(what);
    }

    signal discardConfirmed(string action, string tabId)

    function openFor(what, id, title, pending) {
        action = what;
        tabId = id;
        tabTitle = title;
        count = pending;
        open();
    }

    title: qsTr("Discard unsaved changes?")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onOpened: cancelBtn.forceActiveFocus(Qt.TabFocusReason)

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 16
            text: root.message
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: cancelBtn

                text: qsTr("Cancel")
                Accessible.description: root.message
                onClicked: root.close()
            }

            QButton {
                id: discardBtn

                text: qsTr("Discard changes")
                variant: "danger"
                onClicked: {
                    root.close();
                    root.discardConfirmed(root.action, root.tabId);
                }
            }
        }
    }
}
