pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The migration settings of one database of the tree (H12): a database of a server connection
// without a fixed database, or the connection's own one. They are stored per (connection, database)
// by MigrationController; `database` is that key ("" = the connection's own database).
QDialog {
    id: root

    required property var migrations // MigrationController
    required property var connections // ConnectionController

    property string connectionId: ""
    property string database: ""
    // The name shown: the database, or the connection when it is the connection's own one.
    property string base: ""
    // The last thing sent to the screen reader (tests read it; it is not shown).
    property string lastAnnouncement: ""
    readonly property alias form: form
    readonly property alias saveButton: saveBtn
    readonly property alias cancelButton: cancelBtn

    // `scope` is SchemaTreeModel.migrationScopeOf(): {valid, connectionId, database, base}.
    function openFor(scope, connectionName) {
        connectionId = scope.connectionId;
        database = scope.database;
        base = scope.base !== "" ? scope.base : connectionName;
        form.load(migrations.settingsOf(connectionId, database));
        open();
    }

    function trySave() {
        if (form.blocked) {
            lastAnnouncement = form.blockingMessage();
            saveBtn.Accessible.announce(lastAnnouncement, Accessible.Assertive);
            return;
        }
        form.save(connectionId, database);
        close();
    }

    title: qsTr("Migrations of %1").arg(base)
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onOpened: form.kindBox.forceActiveFocus(Qt.TabFocusReason)

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 2

        MigrationSettingsForm {
            id: form

            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 20
            migrations: root.migrations
            connections: root.connections
            connectionId: root.connectionId
            sshAvailable: root.migrations.connectionUsesSsh(root.connectionId)
            onSubmitted: root.trySave()
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

                text: qsTr("Save")
                variant: "primary"
                onClicked: root.trySave()
            }
        }
    }
}
