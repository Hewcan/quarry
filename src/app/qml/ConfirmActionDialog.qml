pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// A yes/no question about one action (clear the history, delete a saved query...). The focus
// starts on the safe button, never on the one that destroys something.
QDialog {
    id: root

    property string message: ""
    property string confirmText: qsTr("Delete")
    property string cancelText: qsTr("Cancel")
    // The confirm button is styled (icon and text, not only color) as a destructive action.
    property bool destructive: true
    readonly property alias cancelButton: cancelBtn
    readonly property alias confirmButton: confirmBtn

    signal confirmed

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

                text: root.cancelText
                onClicked: root.close()
            }

            QButton {
                id: confirmBtn

                text: root.confirmText
                variant: root.destructive ? "danger" : "primary"
                onClicked: {
                    root.confirmed();
                    root.close();
                }
            }
        }
    }
}
