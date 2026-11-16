pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Asks for one name: a saved query or a folder, new or renamed. Confirm stays disabled while the
// name is empty, with the reason written under the field. Enter in the field confirms.
QDialog {
    id: root

    property string fieldLabel: qsTr("Name")
    property string confirmText: qsTr("Save")
    // Optional context under the field (the SQL being saved), shown muted and cut.
    property string detail: ""
    readonly property alias field: nameField
    readonly property alias cancelButton: cancelBtn
    readonly property alias confirmButton: confirmBtn
    readonly property bool valid: nameField.text.trim() !== ""

    signal named(string name)

    function openFor(heading, initial, confirm, context) {
        title = heading;
        confirmText = confirm;
        detail = context ?? "";
        nameField.text = initial;
        open();
    }

    function submit() {
        if (!valid)
            return;
        const value = nameField.text.trim();
        close();
        named(value);
    }

    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onOpened: {
        nameField.forceActiveFocus(Qt.TabFocusReason);
        nameField.selectAll();
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 2

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 16
            text: root.fieldLabel
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
        }

        QTextField {
            id: nameField

            Layout.fillWidth: true
            accessibleName: root.fieldLabel
            onAccepted: root.submit()
        }

        Text {
            Layout.fillWidth: true
            visible: !root.valid
            text: qsTr("Enter a name to continue.")
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: root.detail !== ""
            text: root.detail
            font.family: Theme.monoFontFamily
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            elide: Text.ElideRight
            maximumLineCount: 3
            wrapMode: Text.WrapAnywhere
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("SQL: %1").arg(text)
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
                id: confirmBtn

                text: root.confirmText
                variant: "primary"
                enabled: root.valid
                onClicked: root.submit()
            }
        }
    }
}
