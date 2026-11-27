pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
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
    // 0 = select the database engine (new connections only), 1 = the required settings,
    // 2 = the optional ones. The modal keeps one size across all of them.
    property int phase: 0
    // Which card was picked: MySQL and MariaDB are two cards for the same "mysql" provider.
    property string engineCard: ""
    property string engineFilter: ""

    readonly property var catalog: {
        const out = [];
        for (const e of connections.engines) {
            if (e.id === "mysql") {
                out.push({
                    "key": "mysql",
                    "id": "mysql",
                    "name": "MySQL",
                    "mark": "My"
                });
                out.push({
                    "key": "mariadb",
                    "id": "mysql",
                    "name": "MariaDB",
                    "mark": "Ma"
                });
            } else {
                out.push({
                    "key": e.id,
                    "id": e.id,
                    "name": e.name,
                    "mark": e.name.substring(0, 2)
                });
            }
        }
        return out;
    }
    readonly property var shownCatalog: catalog.filter(c => c.name.toLowerCase().includes(engineFilter.trim().toLowerCase()))
    readonly property var tabPhases: editing ? [1, 2] : providerId === "" ? [0] : [0, 1, 2]
    readonly property bool canNext: phase === 0 ? providerId !== "" : phase === 1 ? valid : false
    readonly property bool canFinish: phase > 0 && valid && !aiBlocked && !connections.saving
    readonly property string phaseTitle: phase === 0 ? qsTr("Select database") : phase === 1 ? engineName() : qsTr("Optional")
    readonly property alias searchField: searchInput
    readonly property alias backButton: backButton
    readonly property alias nextButton: nextButton
    readonly property alias cardGrid: cards

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
        // A new connection starts with no engine: the first phase asks for it.
        providerId = id === "" ? "" : (d.providerId ?? "");
        engineCard = providerId;
        engineFilter = "";
        phase = id === "" ? 0 : 1;
        flick.contentY = 0;
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
        for (const c of catalog)
            if (c.key === engineCard)
                return c.name;
        for (const e of connections.engines)
            if (e.id === providerId)
                return e.name;
        return providerId;
    }

    // A card of the first phase: picks the engine and moves on to its settings.
    function pickEngine(card) {
        engineCard = card.key;
        if (providerId !== card.id) {
            providerId = card.id;
            connections.selectEngine(providerId);
            password = "";
            passwordTouched = false;
            sshSecret = "";
            sshSecretTouched = false;
        }
        edited();
        goTo(1);
    }

    function goTo(p) {
        if (p < (editing ? 1 : 0) || p > 2 || p === phase)
            return;
        if (p > 0 && providerId === "")
            return;
        if (p === 2 && !valid) {
            showFieldErrors = true;
            announce(invalidReason, true);
            return;
        }
        phase = p;
        flick.contentY = 0;
        Qt.callLater(focusPhase);
        announce(phaseTitle, false);
    }

    function focusPhase() {
        if (phase === 0)
            searchInput.forceActiveFocus(Qt.TabFocusReason);
        else if (phase === 1)
            nameInput.forceActiveFocus(Qt.TabFocusReason);
        else
            colorPicker.focusCurrent();
    }

    // Enter in a text field: finish, or say what is missing.
    function submit() {
        if (phase > 0)
            trySave();
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
        required property string key
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

        // Host and port share a row; everything else takes the whole width.
        Layout.fillWidth: key !== "port"
        Layout.preferredWidth: key === "port" ? Theme.controlHeight * 6 : -1
        Layout.columnSpan: key === "host" || key === "port" ? 1 : 2
        Layout.alignment: Qt.AlignTop
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
                onAccepted: root.submit()

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
                onAccepted: root.submit()

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

    title: root.phaseTitle
    // One size for every phase and engine, so the modal never jumps around.
    width: Math.min(Theme.controlHeight * 26, parent.width - Theme.controlHeight)
    height: Math.min(Theme.controlHeight * 21, parent.height - Theme.controlHeight * 2)

    onOpened: focusPhase()
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

        QTabBar {
            id: tabBar

            Layout.fillWidth: true
            accessibleName: qsTr("Connection steps")
            currentIndex: root.tabPhases.indexOf(root.phase)
            onCurrentIndexChanged: {
                const p = root.tabPhases[currentIndex];
                if (p !== undefined && p !== root.phase)
                    root.goTo(p);
            }

            Repeater {
                model: root.tabPhases

                delegate: QTabButton {
                    required property int modelData

                    text: modelData === 0 ? qsTr("Select database") : modelData === 1 ? root.engineName() : qsTr("Optional")
                    enabled: modelData === 0 || (modelData === 1 ? root.providerId !== "" : root.valid)
                }
            }
        }

        // Phase 0: the engine cards.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.phase === 0
            spacing: Theme.spacing * 2

            Text {
                Layout.fillWidth: true
                text: qsTr("Choose the database engine you want to connect to.")
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }

            // Filters the cards below as you type.
            QTextField {
                id: searchInput

                Layout.fillWidth: true
                rightPadding: clearSearch.visible ? clearSearch.width + Theme.spacing * 2 : Theme.spacing * 2
                placeholderText: qsTr("Search databases…")
                accessibleName: qsTr("Search databases")
                text: root.engineFilter
                onTextEdited: root.engineFilter = text
                onAccepted: {
                    if (root.shownCatalog.length === 1)
                        root.pickEngine(root.shownCatalog[0]);
                }
                Keys.onDownPressed: cards.forceActiveFocus(Qt.TabFocusReason)

                IconButton {
                    id: clearSearch

                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.engineFilter !== ""
                    iconGlyph: Icons.x
                    accessibleName: qsTr("Clear search")
                    onClicked: {
                        root.engineFilter = "";
                        searchInput.forceActiveFocus(Qt.MouseFocusReason);
                    }
                }
            }

            GridView {
                id: cards

                readonly property int columns: 4

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                cellWidth: Math.floor(width / columns)
                cellHeight: Theme.controlHeight * 3
                model: root.shownCatalog
                currentIndex: 0
                keyNavigationEnabled: true
                activeFocusOnTab: true
                boundsBehavior: Flickable.StopAtBounds

                Accessible.role: Accessible.List
                Accessible.name: qsTr("Database engines")

                Keys.onReturnPressed: pickCurrent()
                Keys.onEnterPressed: pickCurrent()
                Keys.onSpacePressed: pickCurrent()

                function pickCurrent() {
                    if (currentIndex >= 0 && currentIndex < count)
                        root.pickEngine(root.shownCatalog[currentIndex]);
                }

                T.ScrollBar.vertical: QScrollBar {}

                delegate: Item {
                    id: card

                    required property var modelData
                    required property int index
                    readonly property bool current: GridView.isCurrentItem

                    width: cards.cellWidth
                    height: cards.cellHeight

                    Accessible.role: Accessible.Button
                    Accessible.name: card.modelData.name
                    Accessible.focusable: true
                    Accessible.onPressAction: root.pickEngine(card.modelData)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing
                        radius: Theme.radius
                        color: hover.hovered ? Theme.bgHover : Theme.bgSurface
                        border.width: card.modelData.key === root.engineCard ? 2 : 1
                        border.color: card.modelData.key === root.engineCard ? Theme.accent : Theme.borderDefault

                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: Theme.spacing

                            // shortcut: a monogram stands in for the official logo until the logo
                            // files (and their trademark terms) are added to the app resources.
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: Theme.controlHeight
                                Layout.preferredHeight: Theme.controlHeight
                                radius: width / 2
                                color: Theme.accent

                                Text {
                                    anchors.centerIn: parent
                                    text: card.modelData.mark
                                    font.pointSize: Theme.fontSize
                                    font.bold: true
                                    color: Theme.fgOnAccent
                                }
                            }

                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: card.modelData.name
                                font.pointSize: Theme.fontSize
                                color: Theme.fgDefault
                                elide: Text.ElideRight
                                Layout.maximumWidth: card.width - Theme.spacing * 4
                            }
                        }

                        HoverHandler {
                            id: hover
                        }

                        TapHandler {
                            onTapped: {
                                cards.currentIndex = card.index;
                                root.pickEngine(card.modelData);
                            }
                        }
                    }

                    FocusRing {
                        shown: card.current && cards.activeFocus
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.shownCatalog.length === 0
                text: qsTr("No database matches your search.")
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }
        }

        Flickable {
            id: flick

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.phase > 0
            contentWidth: width
            contentHeight: form.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: form

                width: flick.width
                spacing: Theme.spacing * 2

                // Phase 1: what the engine needs to connect.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.phase === 1
                    spacing: Theme.spacing * 2

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
                        onAccepted: root.submit()

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

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.spacing * 2
                    rowSpacing: Theme.spacing * 2

                    Repeater {
                        model: root.connections.form

                        delegate: FieldDelegate {}
                    }

                    QButton {
                        id: advancedToggle

                        Layout.columnSpan: 2
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
                        onAccepted: root.submit()
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

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing * 2

                            SshField {
                                id: sshHostInput

                                Layout.alignment: Qt.AlignTop
                                label: qsTr("SSH host")
                                value: sshSection.ssh.sshHost
                                error: sshSection.ssh.sshHostError
                                onValueEdited: text => {
                                    sshSection.ssh.sshHost = text;
                                    root.edited();
                                }
                            }

                            SshField {
                                Layout.alignment: Qt.AlignTop
                                Layout.fillWidth: false
                                Layout.preferredWidth: Theme.controlHeight * 6
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
                                onAccepted: root.submit()
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
                } // end of phase 1

                // Phase 2: everything optional.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.phase === 2
                    spacing: Theme.spacing * 2

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Heading {
                        Layout.fillWidth: true
                        level: 3
                        text: qsTr("Color")
                    }

                    ColorPicker {
                        id: colorPicker

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
                        onSubmitted: root.submit()
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

                } // end of phase 2
            }
        }

        // The result of Test connection (its button is in the footer), under the form so the
        // modal keeps its size.
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.phase === 1
            spacing: Theme.spacing

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

            QButton {
                id: testButton

                visible: root.phase === 1
                text: qsTr("Test connection")
                enabled: root.providerId !== "" && root.connections.testState !== "testing"
                onClicked: root.runTest()
            }

            Text {
                Layout.fillWidth: true
                visible: root.holdReason !== "" && root.phase > 0
                text: root.holdReason
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }

            Item {
                Layout.fillWidth: true
                visible: root.holdReason === "" || root.phase === 0
            }

            QButton {
                id: backButton

                text: qsTr("Previous")
                enabled: root.phase > (root.editing ? 1 : 0)
                onClicked: root.goTo(root.phase - 1)
            }

            QButton {
                id: nextButton

                text: qsTr("Next")
                enabled: root.canNext
                onClicked: root.goTo(root.phase + 1)
            }

            QButton {
                id: saveButton

                text: qsTr("Finish")
                variant: "primary"
                enabled: root.canFinish
                onClicked: root.trySave()

                Accessible.description: root.holdReason
            }

            QButton {
                id: cancelButton

                text: qsTr("Cancel")
                onClicked: root.close()
            }
        }
    }
}
