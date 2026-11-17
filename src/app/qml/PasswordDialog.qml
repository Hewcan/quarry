pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Asks for the password of an "Ask every time" connection when it is expanded. Enter connects,
// Esc or Cancel leaves the connection closed. The password is never shown or stored on disk.
// `kind` says which secret it is: the database password, or the SSH tunnel's password or key
// passphrase.
QDialog {
    id: root

    property string connectionId: ""
    property string connectionName: ""
    property bool retry: false
    // "database" | "sshPassword" | "sshPassphrase"
    property string kind: "database"
    // Set when the user chose Connect, so closing does not count as dismissing.
    property bool answered: false
    readonly property alias passwordField: field
    readonly property string fieldLabel: kind === "sshPassword" ? qsTr("SSH password") : kind === "sshPassphrase" ? qsTr("Key passphrase") : qsTr("Password")
    readonly property string explanation: kind === "sshPassword" ? qsTr("Enter the SSH password to open the tunnel of “%1”. Quarry keeps it in memory until you close the application.").arg(connectionName) : kind === "sshPassphrase" ? qsTr("Enter the passphrase of the SSH key for “%1”. Leave it empty if the key has none. Quarry keeps it in memory until you close the application.").arg(connectionName) : qsTr("Enter the password for “%1”. Quarry keeps it in memory until you close the application.").arg(connectionName)
    readonly property string retryText: kind === "sshPassword" ? qsTr("The SSH password was not accepted. Try again.") : kind === "sshPassphrase" ? qsTr("The key passphrase was not accepted. Try again.") : qsTr("The password was not accepted. Try again.")

    signal submitted(string id, string password)
    signal dismissed(string id)

    function openFor(id, name, isRetry, secretKind) {
        connectionId = id;
        connectionName = name;
        retry = isRetry;
        kind = secretKind ?? "database";
        open();
    }

    function submit() {
        answered = true;
        submitted(connectionId, field.text);
        close();
    }

    title: kind === "sshPassword" ? qsTr("SSH password required") : kind === "sshPassphrase" ? qsTr("Key passphrase required") : qsTr("Password required")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onAboutToShow: {
        field.text = "";
        answered = false;
    }
    onOpened: field.forceActiveFocus(Qt.TabFocusReason)
    onClosed: {
        field.text = "";
        if (!answered)
            dismissed(connectionId);
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 14
            text: root.explanation
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.retry
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: Icons.circleAlert
                color: Theme.danger
            }

            Text {
                Layout.fillWidth: true
                text: root.retryText
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.fieldLabel
            font.pointSize: Theme.fontSize
            font.bold: true
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        QTextField {
            id: field

            Layout.fillWidth: true
            echoMode: TextInput.Password
            accessibleName: root.fieldLabel
            // Qt returns no Accessible.name for a password edit; the description carries the label.
            Accessible.description: root.fieldLabel
            Keys.onReturnPressed: root.submit()
            Keys.onEnterPressed: root.submit()
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            QButton {
                text: qsTr("Connect")
                variant: "primary"
                onClicked: root.submit()
            }
        }
    }
}
