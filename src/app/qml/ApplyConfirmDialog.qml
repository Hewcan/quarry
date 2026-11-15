pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Reinforced confirmation (UX.md 1.5): saving changes to a production connection needs the
// connection name typed exactly. The focus starts in the field; the destructive button stays
// disabled until the text matches; Esc or Cancel saves nothing.
QDialog {
    id: root

    property string tabId: ""
    property string connectionName: ""
    property int count: 0
    // Set when the user confirmed, so closing does not count as declining.
    property bool answered: false
    readonly property alias nameField: nameInput
    readonly property alias cancelButton: cancelBtn
    readonly property alias saveButton: saveBtn
    readonly property bool matches: nameInput.text === connectionName && connectionName !== ""

    signal confirmed(string tabId, string typedName)
    signal declined(string tabId)

    function openFor(request) {
        tabId = request.tabId;
        connectionName = request.connectionName;
        count = request.count;
        open();
    }

    function confirm() {
        if (!matches)
            return;
        answered = true;
        confirmed(tabId, nameInput.text);
        close();
    }

    title: qsTr("Save changes to production?")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onAboutToShow: {
        answered = false;
        nameInput.text = "";
    }
    onOpened: nameInput.forceActiveFocus(Qt.TabFocusReason)
    onClosed: {
        if (!answered)
            declined(tabId);
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing * 2

            Text {
                text: qsTr("Connection: %1").arg(root.connectionName)
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
            }

            ProductionBadge {}
        }

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 18
            text: (root.count === 1 ? qsTr("1 change will be written to the production database “%1”.") : qsTr("%1 changes will be written to the production database “%2”.").arg(root.count)).arg(root.connectionName) + " " + qsTr("Type the connection name to confirm.")
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        QTextField {
            id: nameInput

            Layout.fillWidth: true
            accessibleName: qsTr("Type the connection name “%1” to confirm").arg(root.connectionName)
            placeholderText: root.connectionName
            onAccepted: root.confirm()
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
                id: saveBtn

                text: qsTr("Save to production")
                variant: "danger"
                enabled: root.matches
                onClicked: root.confirm()
            }
        }
    }
}
