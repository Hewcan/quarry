import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The migration settings of one database (H12): type of project, folder, table, php and project
// root. Shared by the connection dialog (a connection's own database: SQLite, or a fixed one) and
// by MigrationSettingsDialog (a database of the tree). The values are kept here until save() hands
// them to MigrationController; the folder is checked on the worker thread while typing.
ColumnLayout {
    id: form

    property var migrations: null // MigrationController
    property var connections: null // ConnectionController (toLocalPath for the pickers)

    property string kind: ""
    property string folder: ""
    property string table: ""
    // Phase C: the php executable (empty = the one in the PATH) and the project root (empty = two
    // levels above the migrations folder).
    property string phpPath: ""
    property string projectRoot: ""
    // What those two come to, and what is missing: {phpText, phpError, rootText, rootError}. Only
    // information: it never stops a save (the buttons of a migration say it when it matters).
    readonly property var env: migrations === null || kind === "" ? ({
            "phpText": "",
            "phpError": "",
            "rootText": "",
            "rootError": ""
        }) : migrations.checkEnvironment(kind, folder, phpPath, projectRoot)
    readonly property var kindInfo: {
        if (migrations === null)
            return null;
        for (const k of migrations.kinds)
            if (k.id === kind)
                return k;
        return null;
    }
    readonly property string problem: migrations === null ? "" : migrations.validate(kind, folder, table)
    readonly property bool blocked: kind !== "" && (problem !== "" || migrations.folderState === "failed" || migrations.folderState === "checking")
    readonly property alias kindBox: migrationKindBox
    readonly property alias folderField: migrationFolderInput
    readonly property alias phpField: migrationPhpInput
    readonly property alias rootField: migrationRootInput
    readonly property alias statusLabel: migrationStatus

    // Enter in one of the fields.
    signal submitted

    // `settings` is MigrationController.settingsOf(): {kind, folder, table, phpPath, projectRoot}.
    function load(settings) {
        kind = settings.kind ?? "";
        folder = settings.folder ?? "";
        table = settings.table ?? "";
        phpPath = settings.phpPath ?? "";
        projectRoot = settings.projectRoot ?? "";
        if (migrations !== null) {
            migrations.resetFolderCheck();
            if (kind !== "")
                migrations.checkFolder(kind, folder, table);
        }
    }

    function checkFolder() {
        if (migrations !== null && kind !== "" && problem === "")
            migrations.checkFolder(kind, folder, table);
        else if (migrations !== null)
            migrations.resetFolderCheck();
    }

    // The message that stops a save, or "".
    function blockingMessage() {
        if (!blocked)
            return "";
        return problem !== "" ? problem : migrations.folderMessage;
    }

    function save(connectionId, database) {
        migrations.saveSettings(connectionId, database, kind, folder, table, phpPath, projectRoot);
    }

    function pickPhp() {
        pickerLoader.active = true;
        // qmllint disable missing-property
        pickerLoader.item["purpose"] = "php";
        // qmllint enable missing-property
        pickerLoader.item.open();
    }

    function pickRoot() {
        folderLoader.active = true;
        folderLoader.item.open();
    }

    spacing: Theme.spacing

    // Created on first use: the platform file dialog is not needed to show the form. By URL, not as
    // a type: naming FilePicker would load QtQuick.Dialogs with the form.
    Loader {
        id: pickerLoader

        active: false
        source: "FilePicker.qml"
    }

    Loader {
        id: folderLoader

        active: false
        source: "FolderPicker.qml"
    }

    Connections {
        target: folderLoader.item

        function onAccepted() {
            // qmllint disable missing-property
            form.projectRoot = form.connections.toLocalPath(folderLoader.item["selectedFolder"]);
            // qmllint enable missing-property
        }
    }

    Connections {
        target: pickerLoader.item

        function onAccepted() {
            // qmllint disable missing-property
            form.phpPath = form.connections.toLocalPath(pickerLoader.item["selectedFile"]);
            // qmllint enable missing-property
        }
    }

    QComboBox {
        id: migrationKindBox

        Layout.fillWidth: true
        accessibleName: qsTr("Type of project")
        model: form.migrations === null ? [] : form.migrations.kinds.map(k => k.name)
        currentIndex: {
            if (form.migrations === null)
                return 0;
            const i = form.migrations.kinds.findIndex(k => k.id === form.kind);
            return i < 0 ? 0 : i;
        }
        onActivated: i => {
            form.kind = form.migrations.kinds[i].id;
            form.checkFolder();
        }
    }

    Text {
        Layout.fillWidth: true
        visible: form.kind !== ""
        text: qsTr("Migrations folder")
        font.pointSize: Theme.fontSize
        color: Theme.fgDefault
    }

    QTextField {
        id: migrationFolderInput

        Layout.fillWidth: true
        visible: form.kind !== ""
        text: form.folder
        accessibleName: qsTr("Migrations folder")
        placeholderText: qsTr("Full path, for example …/database/migrations")
        onTextEdited: {
            form.folder = text;
            migrationCheckTimer.restart();
        }
        onEditingFinished: form.checkFolder()
        onAccepted: form.submitted()
    }

    Text {
        Layout.fillWidth: true
        visible: form.kind !== "" && form.kindInfo !== null && form.kindInfo.usesTable
        text: qsTr("Migrations table")
        font.pointSize: Theme.fontSize
        color: Theme.fgDefault
    }

    QTextField {
        Layout.fillWidth: true
        visible: form.kind !== "" && form.kindInfo !== null && form.kindInfo.usesTable
        text: form.table
        accessibleName: qsTr("Migrations table")
        placeholderText: form.kindInfo !== null ? form.kindInfo.defaultTable : ""
        onTextEdited: form.table = text
        onAccepted: form.submitted()
    }

    // Phase C: php and the project, for Run, Rollback and Dry run. Both optional.
    Text {
        Layout.fillWidth: true
        visible: form.kind !== ""
        text: qsTr("PHP executable (optional)")
        font.pointSize: Theme.fontSize
        color: Theme.fgDefault
    }

    RowLayout {
        Layout.fillWidth: true
        visible: form.kind !== ""
        spacing: Theme.spacing

        QTextField {
            id: migrationPhpInput

            Layout.fillWidth: true
            text: form.phpPath
            accessibleName: qsTr("PHP executable")
            placeholderText: qsTr("Empty: php from the PATH")
            onTextEdited: form.phpPath = text
            onAccepted: form.submitted()
            Accessible.description: form.env.phpError !== "" ? form.env.phpError : form.env.phpText
        }

        QButton {
            id: migrationPhpBrowse

            text: qsTr("Browse…")
            onClicked: form.pickPhp()
            Accessible.name: qsTr("Browse… (%1)").arg(qsTr("PHP executable"))
        }
    }

    Text {
        id: migrationPhpHelp

        Layout.fillWidth: true
        visible: form.kind !== ""
        text: form.env.phpText
        font.pointSize: Theme.fontSize
        color: Theme.fgMuted
        wrapMode: Text.WrapAnywhere
    }

    RowLayout {
        id: migrationPhpProblem

        Layout.fillWidth: true
        visible: form.kind !== "" && form.env.phpError !== ""
        spacing: Theme.spacing

        Icon {
            Layout.alignment: Qt.AlignTop
            text: Icons.triangleAlert
            color: Theme.warning
        }

        Text {
            id: migrationPhpProblemText

            Layout.fillWidth: true
            text: form.env.phpError
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Warning: %1").arg(text)
        }
    }

    Text {
        Layout.fillWidth: true
        visible: form.kind !== ""
        text: qsTr("Project root (optional)")
        font.pointSize: Theme.fontSize
        color: Theme.fgDefault
    }

    RowLayout {
        Layout.fillWidth: true
        visible: form.kind !== ""
        spacing: Theme.spacing

        QTextField {
            id: migrationRootInput

            Layout.fillWidth: true
            text: form.projectRoot
            accessibleName: qsTr("Project root")
            placeholderText: qsTr("Empty: two levels above the migrations folder")
            onTextEdited: form.projectRoot = text
            onAccepted: form.submitted()
            Accessible.description: form.env.rootError !== "" ? form.env.rootError : form.env.rootText
        }

        QButton {
            id: migrationRootBrowse

            text: qsTr("Browse…")
            onClicked: form.pickRoot()
            Accessible.name: qsTr("Browse… (%1)").arg(qsTr("Project root"))
        }
    }

    Text {
        id: migrationRootHelp

        Layout.fillWidth: true
        visible: form.kind !== ""
        text: form.env.rootText
        font.pointSize: Theme.fontSize
        color: Theme.fgMuted
        wrapMode: Text.WrapAnywhere
    }

    RowLayout {
        id: migrationRootProblem

        Layout.fillWidth: true
        visible: form.kind !== "" && form.env.rootError !== ""
        spacing: Theme.spacing

        Icon {
            Layout.alignment: Qt.AlignTop
            text: Icons.triangleAlert
            color: Theme.warning
        }

        Text {
            id: migrationRootProblemText

            Layout.fillWidth: true
            text: form.env.rootError
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Warning: %1").arg(text)
        }
    }

    // The folder check; an error is written out, not only colored.
    Text {
        id: migrationStatus

        Layout.fillWidth: true
        visible: form.kind !== "" && text !== ""
        text: form.problem !== "" ? form.problem : form.migrations === null ? "" : form.migrations.folderMessage
        font.pointSize: Theme.fontSize
        color: form.problem !== "" || (form.migrations !== null && form.migrations.folderState === "failed") ? Theme.danger : Theme.fgMuted
        wrapMode: Text.WordWrap
        Accessible.role: Accessible.StaticText
        Accessible.name: color === Theme.danger ? qsTr("Error: %1").arg(text) : text
    }

    Text {
        Layout.fillWidth: true
        visible: form.kind !== ""
        text: qsTr("Quarry reads the file names and the migrations table to list pending migrations. It runs a migration only when you press Run, Rollback or Dry run on its tab: that starts php artisan in the project, which executes the project's PHP code. These settings stay on this computer.")
        font.pointSize: Theme.fontSize
        color: Theme.fgMuted
        wrapMode: Text.WordWrap
    }

    Timer {
        id: migrationCheckTimer

        interval: 400
        onTriggered: form.checkFolder()
    }
}
