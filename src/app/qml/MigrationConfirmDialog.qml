pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The question before a migration runs or is rolled back (H12 phase C, UX.md). It says which
// migration, on which database and server of which connection (with the PRODUCTION mark), that this
// runs the project's PHP code and changes the database, and shows the exact command in a field that
// can be selected and copied. The focus starts on Cancel, so Enter never confirms by accident. On a
// production connection the database name has to be typed (exactly) before the button turns on.
// Closing the dialog any other way (Esc, Cancel) runs nothing.
QDialog {
    id: root

    property string connectionId: ""
    // The migration settings' database key ("" = the connection's own database).
    property string database: ""
    property string migrationName: ""
    // MigrationController.confirmationOf(): {title, confirmText, name, connectionName, production,
    // server, database, host, typeToConfirm, phrase, rollbackNote, command, workingDirectory}
    property var info: ({})
    // Set when the user confirmed, so closing does not count as declining.
    property bool answered: false
    readonly property alias cancelButton: cancelBtn
    readonly property alias confirmButton: confirmBtn
    readonly property alias nameField: nameInput
    readonly property alias commandField: commandInput
    readonly property bool production: info.production === true
    readonly property bool matches: !production || (nameInput.text === info.typeToConfirm && nameInput.text !== "")

    signal confirmed(string connectionId, string database, string name)
    signal declined(string connectionId, string database, string name)

    function openFor(id, db, name, details) {
        connectionId = id;
        database = db;
        migrationName = name;
        info = details;
        open();
    }

    function confirm() {
        if (!matches)
            return;
        answered = true;
        confirmed(connectionId, database, migrationName);
        close();
    }

    title: info.title ?? ""
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onAboutToShow: {
        answered = false;
        nameInput.text = "";
    }
    onOpened: cancelBtn.forceActiveFocus(Qt.TabFocusReason)
    onClosed: {
        if (!answered)
            declined(connectionId, database, migrationName);
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 2

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 20
            text: root.info.name ?? ""
            font.family: Theme.monoFontFamily
            font.pointSize: Theme.fontSize
            font.bold: true
            color: Theme.fgDefault
            wrapMode: Text.WrapAnywhere

            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Migration: %1").arg(text)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing * 2

            Text {
                text: qsTr("Connection: %1").arg(root.info.connectionName ?? "")
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
            }

            ProductionBadge {
                visible: root.production
            }
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Server: %1").arg(root.info.server ?? "")
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WrapAnywhere
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Database: %1").arg(root.info.database ?? "")
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WrapAnywhere
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Host: %1").arg(root.info.host ?? "")
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WrapAnywhere
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: Icons.triangleAlert
                color: Theme.danger
            }

            Text {
                Layout.fillWidth: true
                text: root.info.phrase ?? ""
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
                wrapMode: Text.WordWrap
            }
        }

        Text {
            Layout.fillWidth: true
            visible: (root.info.rollbackNote ?? "") !== ""
            text: root.info.rollbackNote ?? ""
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Command that will run")
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
        }

        QTextField {
            id: commandInput

            Layout.fillWidth: true
            readOnly: true
            text: root.info.command ?? ""
            font.family: Theme.monoFontFamily
            accessibleName: qsTr("Command that will run")
            Accessible.description: qsTr("Read only. You can select and copy it.")
        }

        Text {
            Layout.fillWidth: true
            visible: (root.info.workingDirectory ?? "") !== ""
            text: qsTr("In the folder: %1").arg(root.info.workingDirectory ?? "")
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WrapAnywhere
        }

        // Production: the name of the database, typed.
        Text {
            Layout.fillWidth: true
            visible: root.production
            text: qsTr("This is a production connection. Type the database name “%1” to confirm.").arg(root.info.typeToConfirm ?? "")
            font.pointSize: Theme.fontSize
            font.bold: true
            color: Theme.danger
            wrapMode: Text.WordWrap
        }

        QTextField {
            id: nameInput

            Layout.fillWidth: true
            visible: root.production
            accessibleName: qsTr("Type the database name “%1” to confirm").arg(root.info.typeToConfirm ?? "")
            placeholderText: root.info.typeToConfirm ?? ""
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
                id: confirmBtn

                text: root.info.confirmText ?? ""
                variant: "danger"
                enabled: root.matches
                Accessible.description: root.matches ? root.info.phrase ?? "" : qsTr("Type the database name first.")
                onClicked: root.confirm()
            }
        }
    }
}
