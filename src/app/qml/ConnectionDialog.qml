pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Create or edit a saved connection. The fields come from the engine (ConnectionController.form),
// so the dialog does not know any engine. Save stays disabled, with the reason written next to
// it, until the name and the required fields are filled in.
QDialog {
    id: root

    required property var connections // ConnectionController

    property string editingId: ""
    property string providerId: ""
    property string profileName: ""
    property int colorIndex: -1
    property bool production: false
    // false = "Don't save query history for this connection" (the privacy opt-out).
    property bool recordHistory: true
    property int passwordMode: 1 // 1 = keychain, 2 = ask every time
    property string password: ""
    property bool passwordTouched: false
    // The SSH password or key passphrase, typed here; whether it is kept follows
    // connections.form.sshSecretMode.
    property string sshSecret: ""
    property bool sshSecretTouched: false
    property bool advancedOpen: false
    property bool passwordNotCopied: false
    property bool nameTouched: false
    property bool showFieldErrors: false
    property string saveError: ""
    property string saveDetail: ""
    property bool keychainError: false
    // The last thing sent to the screen reader (tests read it; it is not shown).
    property string lastAnnouncement: ""

    readonly property bool editing: editingId !== ""
    readonly property bool nameValid: profileName.trim() !== ""
    readonly property bool valid: nameValid && providerId !== "" && connections.form.valid
    readonly property bool sshInvalid: connections.form.sshEnabled && (connections.form.sshHostError !== "" || connections.form.sshPortError !== "" || connections.form.sshUserError !== "" || connections.form.sshKeyPathError !== "")
    readonly property string invalidReason: providerId === "" ? qsTr("No database engine is available.") : !nameValid ? qsTr("Enter a name to save.") : sshInvalid ? qsTr("Fill in the SSH tunnel settings to save.") : qsTr("Fill in the required fields to save.")
    readonly property alias flick: flick
    readonly property alias nameField: nameInput
    readonly property alias noHistoryBox: noHistoryBox
    readonly property alias testButton: testButton
    readonly property alias saveButton: saveButton
    readonly property alias testError: testError
    readonly property alias sshCheckBox: sshBox
    readonly property alias sshHostField: sshHostInput.input
    readonly property alias sshSecretField: sshSecretInput

    property int pickRow: -1

    // H12: the project whose migrations this connection's own database holds. Stored locally by
    // MigrationController (never inside the connection itself), with database "". Optional for
    // tests. Hidden when the engine's database field is blank: each database of the server has its
    // own settings then, set from the tree.
    property var migrations: null
    readonly property bool migrationsShown: migrations !== null && !connections.form.databaseBlank
    readonly property alias migrationForm: migrationForm

    // H13: the AI assistant of this connection (off by default). Stored locally by AiController
    // (quarry.db, never inside the connection itself). Optional for tests; hidden without it.
    property var ai: null
    readonly property bool aiShown: ai !== null
    readonly property bool aiBlocked: aiShown && aiForm.blocked
    // Why Save waits: the missing fields, or what the AI section asks for first.
    readonly property string holdReason: !valid ? invalidReason : aiBlocked ? aiForm.blockingReason : ""
    readonly property alias aiForm: aiForm

    function openFor(id, copied) {
        const d = connections.prepare(id);
        editingId = id;
        providerId = d.providerId ?? "";
        profileName = d.name ?? "";
        colorIndex = d.colorIndex ?? -1;
        production = d.production ?? false;
        recordHistory = d.recordHistory ?? true;
        passwordMode = d.passwordMode === 2 ? 2 : 1;
        password = "";
        passwordTouched = false;
        sshSecret = "";
        sshSecretTouched = false;
        passwordNotCopied = copied === true;
        advancedOpen = false;
        nameTouched = false;
        showFieldErrors = false;
        saveError = "";
        saveDetail = "";
        keychainError = false;
        testError.expanded = false;
        migrationForm.load(migrations !== null && id !== "" && copied !== true ? migrations.settingsOf(id, "") : ({}));
        // Asked for only when an existing connection is edited: a new one has nothing stored yet.
        aiForm.reloadId = ai !== null && id !== "" && copied !== true ? id : "";
        aiForm.load(aiForm.reloadId !== "" ? ai.settingsOf(id) : ({}));
        open();
    }

    function draft() {
        return {
            "id": editingId,
            "providerId": providerId,
            "name": profileName,
            "colorIndex": colorIndex,
            "production": production,
            "recordHistory": recordHistory,
            "passwordMode": passwordMode,
            "password": password,
            "passwordTouched": passwordTouched,
            "sshSecret": sshSecret,
            "sshSecretTouched": sshSecretTouched
        };
    }

    function announce(text, assertive) {
        lastAnnouncement = text;
        saveButton.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    function pickFile(row) {
        pickRow = row;
        pickerLoader.active = true;
        // qmllint disable missing-property
        pickerLoader.item["purpose"] = "database";
        // qmllint enable missing-property
        pickerLoader.item.open();
    }

    // The SSH private key: only its path is kept, the file is never read here.
    function pickKey() {
        pickRow = -1;
        pickerLoader.active = true;
        // qmllint disable missing-property
        pickerLoader.item["purpose"] = "key";
        // qmllint enable missing-property
        pickerLoader.item.open();
    }

    // Any change makes an earlier test result or save error stale.
    function edited() {
        connections.resetTest();
        saveError = "";
    }

    function engineName() {
        for (const e of connections.engines)
            if (e.id === providerId)
                return e.name;
        return providerId;
    }

    function engineRow() {
        for (let i = 0; i < connections.engines.length; ++i)
            if (connections.engines[i].id === providerId)
                return i;
        return -1;
    }

    function trySave() {
        if (!valid) {
            nameTouched = true;
            showFieldErrors = true;
            announce(invalidReason, true);
            return;
        }
        if (migrationsShown && migrationForm.blocked) {
            announce(migrationForm.blockingMessage(), true);
            return;
        }
        if (aiBlocked) {
            announce(aiForm.blockingReason, true);
            return;
        }
        connections.save(draft());
    }

    function runTest() {
        showFieldErrors = true;
        if (!connections.testConnection(draft()))
            announce(qsTr("Fix the fields marked as required before testing."), true);
    }

    // Keeps the focused control inside the scrolled area.
    function reveal(item) {
        if (!item)
            return;
        let inside = false;
        for (let p = item; p; p = p.parent)
            if (p === flick.contentItem) {
                inside = true;
                break;
            }
        if (!inside)
            return;
        const top = item.mapToItem(flick.contentItem, 0, 0).y;
        const bottom = top + item.height;
        if (top < flick.contentY)
            flick.contentY = Math.max(0, top - Theme.spacing);
        else if (bottom > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.contentHeight - flick.height, bottom - flick.height + Theme.spacing);
    }

    // One generated field of the engine.
    component FieldDelegate: ColumnLayout {
        id: field

        required property int index
        required property string label
        required property string kind
        required property bool isRequired
        required property bool isAdvanced
        required property var enumValues
        required property var value
        required property string error
        property bool advancedSection: false
        property bool touched: false
        readonly property string shownError: touched || root.showFieldErrors ? error : ""

        function setValue(v) {
            root.connections.form.setValue(index, v);
            root.edited();
        }

        Layout.fillWidth: true
        visible: isAdvanced === advancedSection && (!advancedSection || root.advancedOpen)
        spacing: Theme.spacing

        onShownErrorChanged: {
            if (shownError !== "")
                root.announce(label + ": " + shownError, false);
        }

        Text {
            Layout.fillWidth: true
            visible: field.kind !== "bool"
            text: field.isRequired ? qsTr("%1 *").arg(field.label) : field.label
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: field.kind === "text" || field.kind === "number" || field.kind === "path"
            spacing: Theme.spacing

            QTextField {
                id: edit

                Layout.fillWidth: true
                text: String(field.value)
                accessibleName: field.label
                inputMethodHints: field.kind === "number" ? Qt.ImhDigitsOnly : Qt.ImhNone
                onTextEdited: field.setValue(text)
                onEditingFinished: field.touched = true
                onAccepted: root.trySave()

                Accessible.description: [field.isRequired ? qsTr("Required") : "", field.shownError].filter(s => s !== "").join(". ")
            }

            QButton {
                visible: field.kind === "path"
                text: qsTr("Browse…")
                onClicked: root.pickFile(field.index)

                Accessible.name: qsTr("Browse… (%1)").arg(field.label)
            }
        }

        QComboBox {
            Layout.fillWidth: true
            visible: field.kind === "enum"
            accessibleName: field.label
            model: field.enumValues
            currentIndex: field.enumValues.indexOf(field.value)
            onActivated: i => field.setValue(field.enumValues[i])
        }

        QCheckBox {
            Layout.fillWidth: true
            visible: field.kind === "bool"
            text: field.label
            checked: field.value === true
            onToggled: field.setValue(checked)
        }

        RowLayout {
            Layout.fillWidth: true
            visible: field.shownError !== ""
            spacing: Theme.spacing

            Icon {
                text: Icons.circleAlert
                color: Theme.danger
            }

            Text {
                Layout.fillWidth: true
                text: field.shownError
                font.pointSize: Theme.fontSize
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
        }
    }

    // One text field of the SSH tunnel section: label, edit (optionally with Browse…) and its error.
    component SshField: ColumnLayout {
        id: sf

        property string label: ""
        property string value: ""
        property string error: ""
        property bool isRequired: true
        property bool browse: false
        property int inputHints: Qt.ImhNone
        property bool touched: false
        readonly property string shownError: touched || root.showFieldErrors ? error : ""
        readonly property alias input: edit

        signal valueEdited(string text)
        signal browseRequested

        Layout.fillWidth: true
        spacing: Theme.spacing

        onShownErrorChanged: {
            if (shownError !== "")
                root.announce(label + ": " + shownError, false);
        }

        Text {
            Layout.fillWidth: true
            text: sf.isRequired ? qsTr("%1 *").arg(sf.label) : sf.label
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            QTextField {
                id: edit

                Layout.fillWidth: true
                text: sf.value
                accessibleName: sf.label
                inputMethodHints: sf.inputHints
                onTextEdited: sf.valueEdited(text)
                onEditingFinished: sf.touched = true
                onAccepted: root.trySave()

                Accessible.description: [sf.isRequired ? qsTr("Required") : "", sf.shownError].filter(t => t !== "").join(". ")
            }

            QButton {
                visible: sf.browse
                text: qsTr("Browse…")
                onClicked: sf.browseRequested()

                Accessible.name: qsTr("Browse… (%1)").arg(sf.label)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: sf.shownError !== ""
            spacing: Theme.spacing

            Icon {
                text: Icons.circleAlert
                color: Theme.danger
            }

            Text {
                Layout.fillWidth: true
                text: sf.shownError
                font.pointSize: Theme.fontSize
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
        }
    }

    title: root.editing ? qsTr("Edit connection") : qsTr("New connection")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onOpened: nameInput.forceActiveFocus(Qt.TabFocusReason)
    onClosed: {
        connections.resetTest();
        password = ""; // do not keep the typed passwords around
        sshSecret = "";
    }

    Connections {
        target: root.connections
        enabled: root.visible

        function onConnectionSaved(id) {
            if (root.migrationsShown && root.opened)
                migrationForm.save(id, "");
            // After the connection exists (a new one has its id only now); its production flag is
            // already in the list, which is what AiController checks.
            if (root.aiShown && root.opened)
                aiForm.save(id);
            root.close();
        }

        function onSaveFailed(message, detail, keychain) {
            root.saveError = message;
            root.saveDetail = detail;
            root.keychainError = keychain;
            root.announce(message, true);
        }

        function onTestChanged() {
            const c = root.connections;
            if (c.testState === "testing")
                root.announce(qsTr("Testing…"), false);
            else if (c.testState === "success")
                root.announce(c.testMessage, false);
            else if (c.testState === "failed")
                root.announce(qsTr("Connection failed. %1 %2").arg(c.testMessage).arg(c.testHint), true);
        }
    }

    // Created on first use: the platform file dialog is not needed to show the form.
    Loader {
        id: pickerLoader

        // By URL, not as a type: naming FilePicker would load QtQuick.Dialogs with the dialog.
        active: false
        source: "FilePicker.qml"
    }

    Connections {
        target: pickerLoader.item

        function onAccepted() {
            // qmllint disable missing-property
            const path = root.connections.toLocalPath(pickerLoader.item["selectedFile"]);
            // qmllint enable missing-property
            if (root.pickRow < 0)
                root.connections.form.sshKeyPath = path;
            else
                root.connections.form.setValue(root.pickRow, path);
            root.edited();
        }
    }

    Connections {
        target: flick.Window.window

        function onActiveFocusItemChanged() {
            root.reveal(flick.Window.window.activeFocusItem);
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 2

        Flickable {
            id: flick

            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 22
            Layout.preferredHeight: Math.min(form.implicitHeight, Math.max(Theme.controlHeight * 6, (root.parent ? root.parent.height : 600) - Theme.controlHeight * 8))
            contentWidth: width
            contentHeight: form.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: form

                width: flick.width
                spacing: Theme.spacing * 2

                // Engine: chosen when creating, fixed afterwards.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Text {
                        Layout.fillWidth: true
                        visible: !root.editing
                        text: qsTr("Engine")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QComboBox {
                        Layout.fillWidth: true
                        visible: !root.editing
                        accessibleName: qsTr("Engine")
                        model: root.connections.engines
                        textRole: "name"
                        currentIndex: root.engineRow()
                        onActivated: i => {
                            root.providerId = root.connections.engines[i].id;
                            root.connections.selectEngine(root.providerId);
                            root.password = "";
                            root.passwordTouched = false;
                            root.sshSecret = "";
                            root.sshSecretTouched = false;
                            root.edited();
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root.editing
                        text: qsTr("Engine: %1").arg(root.engineName())
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Name *")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QTextField {
                        id: nameInput

                        Layout.fillWidth: true
                        text: root.profileName
                        accessibleName: qsTr("Name")
                        onTextEdited: {
                            root.profileName = text;
                            root.edited();
                        }
                        onEditingFinished: root.nameTouched = true
                        onAccepted: root.trySave()

                        Accessible.description: [qsTr("Required"), nameError.visible ? nameError.text : ""].filter(s => s !== "").join(". ")
                    }

                    RowLayout {
                        id: nameError

                        readonly property string text: qsTr("Enter a name for the connection.")

                        Layout.fillWidth: true
                        visible: !root.nameValid && (root.nameTouched || root.showFieldErrors)
                        spacing: Theme.spacing

                        Icon {
                            text: Icons.circleAlert
                            color: Theme.danger
                        }

                        Text {
                            Layout.fillWidth: true
                            text: nameError.text
                            font.pointSize: Theme.fontSize
                            color: Theme.danger
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Repeater {
                    model: root.connections.form

                    delegate: FieldDelegate {}
                }

                QButton {
                    id: advancedToggle

                    visible: root.connections.form.hasAdvanced
                    variant: "flat"
                    text: qsTr("Advanced options")
                    iconGlyph: root.advancedOpen ? Icons.chevronDown : Icons.chevronRight
                    onClicked: root.advancedOpen = !root.advancedOpen

                    Accessible.expandable: true
                    Accessible.expanded: root.advancedOpen
                }

                Repeater {
                    model: root.connections.form

                    delegate: FieldDelegate {
                        advancedSection: true
                    }
                }

                // Password: only for engines that have a secret field.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.connections.form.hasSecret
                    spacing: Theme.spacing

                    Heading {
                        Layout.fillWidth: true
                        level: 3
                        text: qsTr("Password")
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root.passwordNotCopied
                        text: qsTr("The password was not copied from the original connection. Enter it again.")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                    }

                    QComboBox {
                        Layout.fillWidth: true
                        accessibleName: qsTr("Password storage")
                        model: [qsTr("Save password in the system keychain"), qsTr("Ask every time")]
                        currentIndex: root.passwordMode === 2 ? 1 : 0
                        onModelChanged: currentIndex = Qt.binding(() => root.passwordMode === 2 ? 1 : 0)
                        onActivated: i => {
                            root.passwordMode = i === 1 ? 2 : 1;
                            root.edited();
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Password")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QTextField {
                        Layout.fillWidth: true
                        text: root.password
                        echoMode: TextInput.Password
                        accessibleName: qsTr("Password")
                        placeholderText: root.passwordMode === 2 ? qsTr("Used only to test; never saved") : root.editing && !root.passwordNotCopied ? qsTr("Leave empty to keep the saved password") : ""
                        onTextEdited: {
                            root.password = text;
                            root.passwordTouched = true;
                            root.edited();
                        }
                        onAccepted: root.trySave()
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root.passwordMode === 2
                        text: qsTr("The password is asked for every time you connect. It is never saved.")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap
                    }
                }

                // SSH tunnel: only for engines that can go through one.
                ColumnLayout {
                    id: sshSection

                    readonly property var ssh: root.connections.form

                    Layout.fillWidth: true
                    visible: ssh.sshAvailable
                    spacing: Theme.spacing

                    Heading {
                        Layout.fillWidth: true
                        level: 3
                        text: qsTr("SSH tunnel")
                    }

                    QCheckBox {
                        id: sshBox

                        Layout.fillWidth: true
                        text: qsTr("Connect through an SSH tunnel")
                        checked: sshSection.ssh.sshEnabled
                        onToggled: {
                            sshSection.ssh.sshEnabled = checked;
                            root.edited();
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: sshSection.ssh.sshEnabled
                        spacing: Theme.spacing

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("The database host and port below are reached from the SSH server. The first time you connect, Quarry asks you to confirm the server's fingerprint.")
                            font.pointSize: Theme.fontSize
                            color: Theme.fgMuted
                            wrapMode: Text.WordWrap
                        }

                        SshField {
                            id: sshHostInput

                            label: qsTr("SSH host")
                            value: sshSection.ssh.sshHost
                            error: sshSection.ssh.sshHostError
                            onValueEdited: text => {
                                sshSection.ssh.sshHost = text;
                                root.edited();
                            }
                        }

                        SshField {
                            label: qsTr("SSH port")
                            isRequired: false
                            value: sshSection.ssh.sshPort
                            error: sshSection.ssh.sshPortError
                            inputHints: Qt.ImhDigitsOnly
                            onValueEdited: text => {
                                sshSection.ssh.sshPort = text;
                                root.edited();
                            }
                        }

                        SshField {
                            label: qsTr("SSH user")
                            value: sshSection.ssh.sshUser
                            error: sshSection.ssh.sshUserError
                            onValueEdited: text => {
                                sshSection.ssh.sshUser = text;
                                root.edited();
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Authentication")
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                        }

                        QComboBox {
                            id: sshAuthBox

                            readonly property var methods: ["password", "key", "agent"]

                            Layout.fillWidth: true
                            accessibleName: qsTr("SSH authentication")
                            model: [qsTr("Password"), qsTr("Private key"), qsTr("SSH agent")]
                            currentIndex: Math.max(0, methods.indexOf(sshSection.ssh.sshAuth))
                            onActivated: i => {
                                sshSection.ssh.sshAuth = methods[i];
                                root.sshSecret = "";
                                root.sshSecretTouched = false;
                                root.edited();
                            }
                        }

                        SshField {
                            visible: sshSection.ssh.sshAuth === "key"
                            label: qsTr("Private key file")
                            value: sshSection.ssh.sshKeyPath
                            error: sshSection.ssh.sshKeyPathError
                            browse: true
                            onValueEdited: text => {
                                sshSection.ssh.sshKeyPath = text;
                                root.edited();
                            }
                            onBrowseRequested: root.pickKey()
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: sshSection.ssh.sshAuth === "key"
                            text: qsTr("Quarry only remembers where the key file is. It never reads or copies the key.")
                            font.pointSize: Theme.fontSize
                            color: Theme.fgMuted
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: sshSection.ssh.sshAuth === "agent"
                            text: qsTr("Quarry asks your SSH agent to sign in (Pageant or the OpenSSH agent on Windows, SSH_AUTH_SOCK on Linux and macOS). Nothing is stored.")
                            font.pointSize: Theme.fontSize
                            color: Theme.fgMuted
                            wrapMode: Text.WordWrap
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: sshSection.ssh.sshNeedsSecret
                            spacing: Theme.spacing

                            Text {
                                Layout.fillWidth: true
                                visible: root.passwordNotCopied && sshSection.ssh.sshSecretMode === 1
                                text: qsTr("The SSH secret was not copied from the original connection. Enter it again.")
                                font.pointSize: Theme.fontSize
                                color: Theme.fgDefault
                                wrapMode: Text.WordWrap
                            }

                            QComboBox {
                                Layout.fillWidth: true
                                accessibleName: qsTr("SSH secret storage")
                                model: [qsTr("Save in the system keychain"), qsTr("Ask every time")]
                                currentIndex: sshSection.ssh.sshSecretMode === 2 ? 1 : 0
                                onModelChanged: currentIndex = Qt.binding(() => sshSection.ssh.sshSecretMode === 2 ? 1 : 0)
                                onActivated: i => {
                                    sshSection.ssh.sshSecretMode = i === 1 ? 2 : 1;
                                    root.edited();
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: sshSection.ssh.sshAuth === "key" ? qsTr("Key passphrase (optional)") : qsTr("SSH password")
                                font.pointSize: Theme.fontSize
                                color: Theme.fgDefault
                            }

                            QTextField {
                                id: sshSecretInput

                                Layout.fillWidth: true
                                text: root.sshSecret
                                echoMode: TextInput.Password
                                accessibleName: sshSection.ssh.sshAuth === "key" ? qsTr("Key passphrase") : qsTr("SSH password")
                                placeholderText: sshSection.ssh.sshSecretMode === 2 ? qsTr("Used only to test; never saved") : root.editing && !root.passwordNotCopied ? qsTr("Leave empty to keep the saved secret") : ""
                                onTextEdited: {
                                    root.sshSecret = text;
                                    root.sshSecretTouched = true;
                                    root.edited();
                                }
                                onAccepted: root.trySave()
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: sshSection.ssh.sshSecretMode === 2
                                text: qsTr("It is asked for every time you connect. It is never saved.")
                                font.pointSize: Theme.fontSize
                                color: Theme.fgMuted
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Heading {
                        Layout.fillWidth: true
                        level: 3
                        text: qsTr("Color")
                    }

                    ColorPicker {
                        Layout.fillWidth: true
                        colorIndex: root.colorIndex
                        onColorPicked: i => {
                            root.colorIndex = i;
                            root.edited();
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    QCheckBox {
                        id: productionBox

                        Layout.fillWidth: true
                        text: qsTr("Production connection")
                        checked: root.production
                        onToggled: root.production = checked
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Shows a PRODUCTION label next to this connection and asks you to type its name before deleting it.")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    QCheckBox {
                        id: noHistoryBox

                        Layout.fillWidth: true
                        text: qsTr("Don't save query history for this connection")
                        checked: !root.recordHistory
                        onToggled: {
                            root.recordHistory = !checked;
                            root.edited();
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("The statements you run on this connection are not added to the history. Use it when queries may contain sensitive data.")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap
                    }
                }

                // H12: project migrations (compared read-only; run only from a migration tab, on
                // request). Only for the connection's own database (SQLite, or a fixed one): a
                // server connection without a database sets them per database, from the tree.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.migrationsShown
                    spacing: Theme.spacing

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Project migrations")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    MigrationSettingsForm {
                        id: migrationForm

                        Layout.fillWidth: true
                        migrations: root.migrations
                        connections: root.connections
                        onSubmitted: root.trySave()
                    }
                }

                // H13: the AI assistant (off by default).
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.aiShown
                    spacing: Theme.spacing

                    Heading {
                        Layout.fillWidth: true
                        level: 3
                        text: qsTr("AI assistant")
                    }

                    AiConnectionForm {
                        id: aiForm

                        Layout.fillWidth: true
                        ai: root.ai
                        connectionName: root.profileName
                        production: root.production
                    }
                }

                // Test connection and its result.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    QButton {
                        id: testButton

                        text: qsTr("Test connection")
                        enabled: root.providerId !== "" && root.connections.testState !== "testing"
                        onClicked: root.runTest()
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.connections.testState === "testing"
                        spacing: Theme.spacing

                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Testing…")

                        Icon {
                            id: spinner

                            text: Icons.loaderCircle
                            color: Theme.fgMuted

                            RotationAnimator on rotation {
                                running: spinner.visible && Theme.animationDuration > 0
                                from: 0
                                to: 360
                                duration: 1000
                                loops: Animation.Infinite
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Testing…")
                            font.pointSize: Theme.fontSize
                            color: Theme.fgMuted
                        }
                    }

                    RowLayout {
                        id: testOk

                        Layout.fillWidth: true
                        visible: root.connections.testState === "success"
                        spacing: Theme.spacing

                        Accessible.role: Accessible.StaticText
                        Accessible.name: root.connections.testMessage

                        Icon {
                            text: Icons.circleCheck
                            color: Theme.success
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.connections.testMessage
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            wrapMode: Text.WordWrap
                        }
                    }

                    ErrorPanel {
                        id: testError

                        Layout.fillWidth: true
                        visible: root.connections.testState === "failed"
                        title: qsTr("Could not connect")
                        message: root.connections.testMessage
                        hint: root.connections.testHint
                        detail: root.connections.testDetail
                    }
                }
            }
        }

        // Save failed: stays visible above the buttons, with the way out for keychain problems.
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.saveError !== ""
            spacing: Theme.spacing

            ErrorPanel {
                Layout.fillWidth: true
                title: qsTr("The connection could not be saved")
                message: root.saveError
                detail: root.saveDetail
            }

            QButton {
                visible: root.keychainError
                text: qsTr("Ask every time instead")
                onClicked: {
                    root.passwordMode = 2;
                    root.connections.form.sshSecretMode = 2;
                    root.saveError = "";
                    root.trySave();
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing * 2

            Text {
                Layout.fillWidth: true
                visible: root.holdReason !== ""
                text: root.holdReason
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }

            Item {
                Layout.fillWidth: true
                visible: root.holdReason === ""
            }

            QButton {
                id: cancelButton

                text: qsTr("Cancel")
                onClicked: root.close()
            }

            QButton {
                id: saveButton

                text: qsTr("Save")
                variant: "primary"
                enabled: root.valid && !root.aiBlocked && !root.connections.saving
                onClicked: root.trySave()

                Accessible.description: root.holdReason
            }
        }
    }
}
