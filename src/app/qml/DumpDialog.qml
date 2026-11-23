pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// "Dump..." and "Restore..." of a connection (H14, UX.md "Volcado y restauracion"). One dialog, two
// modes: the form, then the same dialog shows the progress, the log and the result. A restore
// writes into the database, so its button stays disabled until the user types a fixed phrase and,
// for a production connection, the name of the database. Esc while a job runs asks whether to stop
// it (the dialog never closes under a running job). The work runs in the DumpRunner; nothing here
// waits for it. Announcements are only the start, the end, a cancellation and an error.
QDialog {
    id: root

    required property var controller // DumpController
    // "dump" | "restore"
    property string mode: "dump"
    property string connectionId: ""
    property string database: ""
    property var info: ({})
    property string startError: ""
    property string lastAnnouncement: ""
    // Rows (into controller.tables) the user ticked, as a map row -> true.
    property var selection: ({})
    readonly property bool isDump: root.mode === "dump"
    readonly property string stateName: root.controller.state
    readonly property bool formShown: root.stateName === "idle"
    readonly property bool busy: root.controller.busy
    readonly property bool isPostgres: root.info.isPostgres === true
    readonly property bool tablesReady: root.controller.tablesState === "ready"
    readonly property bool chooseTables: root.tablesReady && tablesBox.checked
    readonly property var selectedRows: Object.keys(root.selection).map(k => Number(k)).sort((a, b) => a - b)
    // What will be dumped: the ticked tables plus, when asked, what they reference.
    readonly property var effectiveRows: !root.chooseTables ? [] : refsBox.checked ? root.controller.closureOf(root.selectedRows) : root.selectedRows
    readonly property var visibleRows: root.filterRows(root.controller.tables, searchField.text)
    readonly property bool confirmOk: root.isDump || root.controller.restoreConfirmed(root.connectionId, root.database, phraseField.text, nameField.text)
    readonly property bool canSubmit: root.formShown && root.info.supported === true && pathField.text.trim() !== "" && (root.isDump ? (!root.chooseTables || root.selectedRows.length > 0) : root.confirmOk)
    readonly property alias contentCombo: contentCombo
    readonly property alias formatCombo: formatCombo
    readonly property alias schemasField: schemasField
    readonly property alias ownershipBox: ownershipBox
    readonly property alias tablesBox: tablesBox
    readonly property alias searchField: searchField
    readonly property alias tableList: tableList
    readonly property alias refsBox: refsBox
    readonly property alias selectAllButton: selectAllBtn
    readonly property alias clearButton: clearBtn
    readonly property alias pathField: pathField
    readonly property alias browseButton: browseBtn
    readonly property alias phraseField: phraseField
    readonly property alias nameField: nameField
    readonly property alias actionButton: actionBtn
    readonly property alias cancelButton: cancelBtn
    readonly property alias stopButton: stopBtn
    readonly property alias closeButton: closeBtn
    readonly property alias backButton: backBtn
    readonly property alias folderButton: folderBtn
    readonly property alias copyLogButton: copyLogBtn
    readonly property alias replaceDialog: replaceDialog
    readonly property alias stopDialog: stopDialog
    readonly property alias errorPanel: failure
    readonly property alias progressLabel: progressText
    readonly property alias progressBar: bar
    readonly property alias logList: logList
    readonly property alias flick: flick
    readonly property alias picker: pickerLoader

    function filterRows(tables, text) {
        const needle = text.trim().toLowerCase();
        const out = [];
        for (let i = 0; i < tables.length; ++i)
            if (needle === "" || tables[i].label.toLowerCase().includes(needle))
                out.push(i);
        return out;
    }

    function announce(text, assertive) {
        lastAnnouncement = text;
        body.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    // Opens the form for a connection (and database, "" = its own). `what`: "dump" or "restore".
    function openFor(what, id, db) {
        if (controller.busy) {
            // A job is running: show it, whatever was asked for.
            mode = controller.mode;
            open();
            return;
        }
        mode = what;
        connectionId = id;
        database = db ?? "";
        controller.reset();
        info = controller.infoFor(id, database);
        startError = "";
        selection = ({});
        searchField.text = "";
        tablesBox.checked = false;
        refsBox.checked = true;
        contentCombo.currentIndex = 0;
        formatCombo.currentIndex = 0;
        schemasField.text = "";
        ownershipBox.checked = false;
        phraseField.text = "";
        nameField.text = "";
        pathField.text = isDump ? controller.defaultPath(id, database, false) : "";
        if (isDump && info.supported === true)
            controller.loadTables(id, database);
        else
            controller.clearTables();
        open();
    }

    function toggleRow(row) {
        const next = Object.assign({}, selection);
        if (next[row])
            delete next[row];
        else
            next[row] = true;
        selection = next;
    }

    function setAll(on) {
        const next = {};
        if (on) {
            Object.assign(next, selection);
            for (const r of visibleRows)
                next[r] = true;
        } else {
            Object.assign(next, selection);
            for (const r of visibleRows)
                delete next[r];
        }
        selection = next;
    }

    function submit() {
        startError = "";
        if (!root.canSubmit)
            return;
        const path = pathField.text.trim();
        if (isDump && controller.fileExists(path)) {
            replaceDialog.message = qsTr("“%1” already exists. The old file is only replaced when the dump finishes without errors.").arg(path);
            replaceDialog.open();
        } else {
            begin();
        }
    }

    function begin() {
        let error = "";
        if (isDump) {
            error = controller.startDump({
                connectionId: connectionId,
                database: database,
                content: ["both", "schema", "data"][contentCombo.currentIndex] ?? "both",
                format: isPostgres && formatCombo.currentIndex === 1 ? "custom" : "plain",
                schemas: isPostgres ? schemasField.text.split(",").map(s => s.trim()).filter(s => s !== "") : [],
                tables: chooseTables ? selectedRows : [],
                includeReferenced: refsBox.checked,
                includeOwnership: ownershipBox.checked,
                filePath: pathField.text
            });
        } else {
            error = controller.startRestore({
                connectionId: connectionId,
                database: database,
                filePath: pathField.text,
                phrase: phraseField.text,
                typedName: nameField.text
            });
        }
        startError = error;
        if (error !== "") {
            announce(error, true);
            return;
        }
        Qt.callLater(root.focusProgress);
    }

    function focusProgress() {
        if (root.busy)
            stopBtn.forceActiveFocus(Qt.TabFocusReason);
    }

    // Esc or the close key while a job runs: asks, never closes by itself.
    function askStop() {
        if (root.busy && !root.controller.cancelling)
            stopDialog.open();
    }

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

    function chooseFile() {
        pickerLoader.active = true;
        const picker = pickerLoader.item;
        picker.restoring = !isDump;
        const custom = isDump && isPostgres && formatCombo.currentIndex === 1;
        picker.nameFilters = isDump ? [custom ? qsTr("PostgreSQL archives (*.dump)") : qsTr("SQL scripts (*.sql)"), qsTr("All files (*)")] : [qsTr("SQL scripts and archives (*.sql *.dump)"), qsTr("All files (*)")];
        picker.defaultSuffix = custom ? "dump" : "sql";
        picker.currentFolder = controller.folderUrl(pathField.text);
        if (isDump)
            picker.selectedFile = controller.fileUrl(pathField.text);
        picker.open();
    }

    title: isDump ? qsTr("Dump database") : qsTr("Restore database")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)
    // While it runs, Esc asks to stop it (see body) instead of closing.
    closePolicy: root.busy ? T.Popup.NoAutoClose : T.Popup.CloseOnEscape

    onOpened: {
        if (root.formShown)
            (root.info.supported === true ? pathField : cancelBtn).forceActiveFocus(Qt.TabFocusReason);
    }
    onClosed: {
        controller.clearTables();
        if (root.busy)
            controller.cancel();
    }

    Connections {
        target: root.controller

        function onAnnouncement(text, assertive) {
            root.announce(text, assertive);
        }

        function onStateChanged() {
            if (!root.visible)
                return;
            Qt.callLater(() => {
                if (root.stateName === "done")
                    closeBtn.forceActiveFocus(Qt.TabFocusReason);
                else if (root.stateName === "failed" || root.stateName === "cancelled")
                    backBtn.forceActiveFocus(Qt.TabFocusReason);
                else if (root.busy)
                    root.focusProgress();
            });
        }

        // The list may arrive after the user started choosing.
        function onTablesChanged() {
            if (root.controller.tablesState !== "ready")
                root.selection = ({});
        }
    }

    Connections {
        target: flick.Window.window

        function onActiveFocusItemChanged() {
            root.reveal(flick.Window.window.activeFocusItem);
        }
    }

    Loader {
        id: pickerLoader

        active: false
        source: "DumpFilePicker.qml"
    }

    Connections {
        target: pickerLoader.item

        function onAccepted() {
            // qmllint disable missing-property
            pathField.text = root.controller.toLocalPath(pickerLoader.item["selectedFile"]);
            // qmllint enable missing-property
            pathField.forceActiveFocus(Qt.TabFocusReason);
        }
    }

    ConfirmActionDialog {
        id: replaceDialog

        title: qsTr("Replace file?")
        confirmText: qsTr("Replace")
        onConfirmed: root.begin()
    }

    ConfirmActionDialog {
        id: stopDialog

        title: root.isDump ? qsTr("Stop the dump?") : qsTr("Stop the restore?")
        message: root.isDump ? qsTr("The dump is cancelled and no file is written.") : qsTr("The restore is cancelled and the database is rolled back to how it was, when the engine allows it.")
        confirmText: root.isDump ? qsTr("Cancel dump") : qsTr("Cancel restore")
        cancelText: qsTr("Keep running")
        onConfirmed: root.controller.cancel()
    }

    contentItem: ColumnLayout {
        id: body

        spacing: Theme.spacing * 2

        Keys.onEscapePressed: event => {
            if (root.busy) {
                root.askStop();
                event.accepted = true;
            } else {
                event.accepted = false;
            }
        }

        // ---- the form -----------------------------------------------------------------------

        Flickable {
            id: flick

            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 24
            Layout.preferredHeight: Math.min(form.implicitHeight, Math.max(Theme.controlHeight * 6, (root.parent ? root.parent.height : 600) - Theme.controlHeight * 8))
            visible: root.formShown
            contentWidth: width
            contentHeight: form.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: form

                width: flick.width
                spacing: Theme.spacing * 2

                Text {
                    id: intro

                    Layout.fillWidth: true
                    text: root.isDump ? qsTr("Dump “%1” (%2, connection %3) to a file.").arg(root.info.database ?? "").arg(root.info.engine ?? "").arg(root.info.name ?? "") : qsTr("Restore a file into “%1” (%2, connection %3).").arg(root.info.database ?? "").arg(root.info.engine ?? "").arg(root.info.name ?? "")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                // Not available for this connection: the reason, and nothing else to fill in.
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.info.supported !== true
                    spacing: Theme.spacing

                    Icon {
                        Layout.alignment: Qt.AlignTop
                        text: Icons.circleAlert
                        color: Theme.danger
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.info.reason ?? ""
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.danger
                        wrapMode: Text.WordWrap

                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }

                // ---- dump options ----

                GridLayout {
                    Layout.fillWidth: true
                    visible: root.isDump && root.info.supported === true
                    columns: 2
                    columnSpacing: Theme.spacing * 3
                    rowSpacing: Theme.spacing * 2

                    Text {
                        id: contentLabel

                        text: qsTr("Contents")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QComboBox {
                        id: contentCombo

                        Layout.fillWidth: true
                        accessibleName: contentLabel.text
                        model: [qsTr("Structure and data"), qsTr("Structure only"), qsTr("Data only")]
                    }

                    Text {
                        id: formatLabel

                        visible: root.isPostgres
                        text: qsTr("Format")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QComboBox {
                        id: formatCombo

                        Layout.fillWidth: true
                        visible: root.isPostgres
                        accessibleName: formatLabel.text
                        model: [qsTr("Plain SQL script (.sql)"), qsTr("Custom archive (.dump, restored with pg_restore)")]
                        onCurrentIndexChanged: {
                            if (pathField.text !== "")
                                pathField.text = root.controller.retarget(pathField.text, currentIndex === 1);
                        }
                    }

                    Text {
                        id: schemasLabel

                        visible: root.isPostgres
                        text: qsTr("Schemas")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QTextField {
                        id: schemasField

                        Layout.fillWidth: true
                        visible: root.isPostgres
                        accessibleName: schemasLabel.text
                        placeholderText: qsTr("All schemas (or names separated by commas)")
                    }

                    Text {
                        visible: root.isPostgres
                        text: qsTr("Owners and privileges")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QCheckBox {
                        id: ownershipBox

                        visible: root.isPostgres
                        text: qsTr("Include owners and privileges (GRANT, OWNER TO)")
                    }
                }

                // ---- the table selector ----

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.isDump && root.info.supported === true
                    spacing: Theme.spacing

                    QCheckBox {
                        id: tablesBox

                        enabled: root.tablesReady
                        text: qsTr("Choose the tables to dump")
                        Accessible.description: root.tablesReady ? "" : tablesNote.text
                    }

                    // Why the selector is off, or what it holds.
                    Text {
                        id: tablesNote

                        Layout.fillWidth: true
                        visible: text !== ""
                        text: {
                            const s = root.controller.tablesState;
                            if (s === "loading")
                                return qsTr("Loading the tables… The whole database is dumped unless you choose some.");
                            if (s === "failed")
                                return qsTr("The tables could not be listed: %1 The whole database is dumped.").arg(root.controller.tablesError);
                            if (s === "unavailable")
                                return root.info.canPickTables === true ? qsTr("Connect to the database first to choose tables. The whole database is dumped.") : qsTr("Choosing tables is not possible for this database. The whole database is dumped.");
                            if (s === "ready" && root.controller.tablesTruncated)
                                return qsTr("Showing the first %1 tables.").arg(root.controller.tables.length);
                            if (s === "ready" && root.controller.tables.length === 0)
                                return qsTr("This database has no tables.");
                            return "";
                        }
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap

                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.chooseTables
                        spacing: Theme.spacing

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing

                            QTextField {
                                id: searchField

                                Layout.fillWidth: true
                                accessibleName: qsTr("Search tables")
                                placeholderText: qsTr("Search tables")
                            }

                            QButton {
                                id: selectAllBtn

                                text: qsTr("Select shown")
                                onClicked: root.setAll(true)
                            }

                            QButton {
                                id: clearBtn

                                text: qsTr("Clear shown")
                                onClicked: root.setAll(false)
                            }
                        }

                        // One tab stop: Up/Down move, Space ticks. Tables that come along by a
                        // foreign key say so in words, not only in color.
                        ListView {
                            id: tableList

                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(contentHeight, Theme.controlHeight * 7)
                            clip: true
                            model: root.visibleRows
                            activeFocusOnTab: true
                            focusPolicy: Qt.StrongFocus
                            keyNavigationEnabled: true
                            boundsBehavior: Flickable.StopAtBounds
                            Keys.onSpacePressed: {
                                if (currentIndex >= 0)
                                    root.toggleRow(root.visibleRows[currentIndex]);
                            }

                            Accessible.role: Accessible.List
                            Accessible.name: qsTr("Tables")
                            Accessible.focusable: true

                            delegate: Rectangle {
                                id: tableRow

                                required property int modelData
                                required property int index
                                readonly property bool ticked: root.selection[modelData] === true
                                readonly property bool pulled: !ticked && root.effectiveRows.includes(modelData)

                                width: ListView.view.width
                                height: Theme.controlHeight
                                color: ListView.isCurrentItem && tableList.activeFocus ? Theme.selectionBg : Qt.alpha(Theme.bgRaised, 0)

                                Accessible.role: Accessible.CheckBox
                                Accessible.name: (root.controller.tables[modelData]?.label ?? "") + (pulled ? ", " + qsTr("included because a selected table references it") : "")
                                Accessible.checkable: true
                                Accessible.checked: ticked || pulled
                                Accessible.onToggleAction: root.toggleRow(modelData)

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spacing * 2
                                    anchors.rightMargin: Theme.spacing * 2
                                    spacing: Theme.spacing * 2

                                    Icon {
                                        text: tableRow.ticked || tableRow.pulled ? Icons.check : ""
                                        color: tableRow.ListView.isCurrentItem && tableList.activeFocus ? Theme.selectionFg : Theme.fgDefault
                                        opacity: tableRow.ticked ? 1 : 0.6
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: (root.controller.tables[tableRow.modelData]?.label ?? "")
                                        font.pointSize: Theme.fontSize
                                        font.bold: tableRow.ticked
                                        color: tableRow.ListView.isCurrentItem && tableList.activeFocus ? Theme.selectionFg : Theme.fgDefault
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        visible: tableRow.pulled
                                        text: qsTr("referenced")
                                        font.pointSize: Theme.fontSize
                                        color: tableRow.ListView.isCurrentItem && tableList.activeFocus ? Theme.selectionFg : Theme.fgMuted
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: {
                                        tableList.currentIndex = tableRow.index;
                                        tableList.forceActiveFocus(Qt.MouseFocusReason);
                                        root.toggleRow(tableRow.modelData);
                                    }
                                }
                            }

                            FocusRing {
                                shown: tableList.activeFocus
                                inset: true
                            }
                        }

                        QCheckBox {
                            id: refsBox

                            text: qsTr("Include tables they reference")
                            checked: true
                            Accessible.description: qsTr("Adds the tables that the chosen ones point at through foreign keys, so the dump can be restored on its own.")
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.selectedRows.length === 0 ? qsTr("No table chosen yet.") : root.effectiveRows.length > root.selectedRows.length ? qsTr("%1 chosen, %2 more by reference.").arg(root.selectedRows.length).arg(root.effectiveRows.length - root.selectedRows.length) : qsTr("%1 chosen.").arg(root.selectedRows.length)
                            font.pointSize: Theme.fontSize
                            color: Theme.fgMuted
                            wrapMode: Text.WordWrap

                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                    }
                }

                // ---- restore: what it does and the confirmation ----

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !root.isDump && root.info.supported === true
                    spacing: Theme.spacing * 2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing

                        Icon {
                            Layout.alignment: Qt.AlignTop
                            text: Icons.triangleAlert
                            color: Theme.danger
                        }

                        Text {
                            id: warning

                            Layout.fillWidth: true
                            text: qsTr("This writes into the database and cannot be undone, unless it fails: then everything it did is rolled back. Make a dump first if you need a way back.")
                            font.pointSize: Theme.fontSize
                            font.bold: true
                            color: Theme.fgDefault
                            wrapMode: Text.WordWrap

                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                    }

                    Text {
                        id: restoreFileHint

                        Layout.fillWidth: true
                        visible: root.isPostgres
                        text: qsTr("A plain .sql script or a custom archive (.dump) made by pg_dump.")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap
                    }
                }

                Text {
                    id: pathLabel

                    visible: root.info.supported === true
                    text: root.isDump ? qsTr("File to write") : qsTr("File to restore from")
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.info.supported === true
                    spacing: Theme.spacing

                    QTextField {
                        id: pathField

                        Layout.fillWidth: true
                        accessibleName: pathLabel.text
                        onAccepted: root.submit()
                    }

                    QButton {
                        id: browseBtn

                        text: qsTr("Browse…")
                        iconGlyph: Icons.folderOpen
                        onClicked: root.chooseFile()
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !root.isDump && root.info.supported === true
                    spacing: Theme.spacing

                    Text {
                        id: phraseLabel

                        Layout.fillWidth: true
                        text: qsTr("To confirm, type %1").arg(root.controller.restorePhrase())
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                    }

                    QTextField {
                        id: phraseField

                        Layout.fillWidth: true
                        accessibleName: phraseLabel.text
                        onAccepted: root.submit()
                    }

                    Text {
                        id: nameLabel

                        Layout.fillWidth: true
                        visible: root.info.production === true
                        text: qsTr("This is a production connection. Also type the name of the database: %1").arg(root.info.database ?? "")
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                    }

                    QTextField {
                        id: nameField

                        Layout.fillWidth: true
                        visible: root.info.production === true
                        accessibleName: qsTr("Database name")
                        onAccepted: root.submit()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.startError !== ""
                    spacing: Theme.spacing

                    Icon {
                        Layout.alignment: Qt.AlignTop
                        text: Icons.circleAlert
                        color: Theme.danger
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.startError
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.danger
                        wrapMode: Text.WordWrap

                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }
            }
        }

        // ---- progress, log and result ----------------------------------------------------------

        ColumnLayout {
            id: progress

            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 24
            visible: !root.formShown
            spacing: Theme.spacing * 2

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                visible: root.stateName !== "failed"

                Icon {
                    Layout.alignment: Qt.AlignTop
                    text: root.stateName === "done" ? Icons.circleCheck : root.stateName === "cancelled" ? Icons.info : Icons.loaderCircle
                    color: root.stateName === "done" ? Theme.success : Theme.fgMuted
                }

                // The name stays the same while the step changes, and the step is not an
                // announcement: a screen reader reads it when the user goes there.
                Text {
                    id: progressText

                    Layout.fillWidth: true
                    text: root.stateName === "preparing" ? qsTr("Preparing…") : root.stateName === "done" ? root.controller.resultText : root.stateName === "cancelled" ? (root.isDump ? qsTr("Dump cancelled. No file was written.") : qsTr("Restore cancelled. The database was rolled back where the engine allows it.")) : root.controller.cancelling ? qsTr("Cancelling…") : root.controller.step !== "" ? root.controller.step : (root.isDump ? qsTr("Dumping…") : qsTr("Restoring…"))
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                    wrapMode: Text.WrapAnywhere

                    Accessible.role: Accessible.StaticText
                    Accessible.name: root.isDump ? qsTr("Dump progress") : qsTr("Restore progress")
                    Accessible.description: text
                }
            }

            // A bar built from tokens: a determinate fill, or (total 0) a segment that moves.
            // The text above says the same in words.
            Rectangle {
                id: bar

                readonly property bool indeterminate: root.controller.progressTotal <= 0
                readonly property real fraction: indeterminate ? 0 : Math.min(1, root.controller.progressDone / root.controller.progressTotal)

                Layout.fillWidth: true
                Layout.preferredHeight: Theme.spacing * 2
                visible: root.busy
                radius: Theme.radius
                color: Theme.bgSurface
                border.width: 1
                border.color: Theme.borderDefault
                clip: true

                Accessible.role: Accessible.ProgressBar
                Accessible.name: root.isDump ? qsTr("Dump progress") : qsTr("Restore progress")
                Accessible.description: indeterminate ? qsTr("In progress") : qsTr("%1 of %2").arg(root.controller.progressDone).arg(root.controller.progressTotal)

                Rectangle {
                    id: fill

                    height: parent.height
                    radius: Theme.radius
                    color: Theme.accent
                    width: bar.indeterminate ? Math.round(parent.width / 4) : Math.round(parent.width * bar.fraction)

                    SequentialAnimation on x {
                        running: bar.indeterminate && bar.visible && Theme.animationDuration > 0
                        loops: Animation.Infinite

                        NumberAnimation {
                            from: 0
                            to: bar.width - fill.width
                            duration: Theme.animationDuration * 8
                        }

                        NumberAnimation {
                            from: bar.width - fill.width
                            to: 0
                            duration: Theme.animationDuration * 8
                        }
                    }
                }
            }

            Text {
                visible: root.busy && !bar.indeterminate
                text: qsTr("%1 of %2").arg(root.controller.progressDone).arg(root.controller.progressTotal)
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
            }

            ErrorPanel {
                id: failure

                Layout.fillWidth: true
                visible: root.stateName === "failed"
                title: root.isDump ? qsTr("The dump failed") : qsTr("The restore failed")
                message: root.controller.errorMessage
                hint: root.isDump ? qsTr("No file was written. An existing file with that name was left as it was.") : qsTr("The restore was rolled back where the engine allows it: check the log below.")
                detail: root.controller.errorDetail
            }

            Text {
                id: logLabel

                text: qsTr("Log")
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
            }

            // The tool's own output, newest line last; follows the end while it grows.
            ListView {
                id: logList

                Layout.fillWidth: true
                Layout.preferredHeight: Theme.controlHeight * 6
                clip: true
                model: root.controller.log
                activeFocusOnTab: true
                focusPolicy: Qt.StrongFocus
                keyNavigationEnabled: true
                boundsBehavior: Flickable.StopAtBounds
                onCountChanged: {
                    if (atYEnd || count <= 1)
                        Qt.callLater(() => logList.positionViewAtEnd());
                }

                Accessible.role: Accessible.List
                Accessible.name: logLabel.text
                Accessible.focusable: true

                delegate: Text {
                    required property string modelData

                    width: ListView.view.width
                    text: modelData
                    font.family: Theme.monoFontFamily
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WrapAnywhere

                    Accessible.role: Accessible.ListItem
                    Accessible.name: text
                }

                Rectangle {
                    anchors.fill: parent
                    z: -1
                    color: Theme.bgSurface
                    border.width: 1
                    border.color: Theme.borderDefault
                    radius: Theme.radius
                }

                FocusRing {
                    shown: logList.activeFocus
                    inset: true
                }
            }
        }

        // ---- buttons --------------------------------------------------------------------------

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: cancelBtn

                visible: root.formShown
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            QButton {
                id: actionBtn

                visible: root.formShown
                text: root.isDump ? qsTr("Dump") : qsTr("Restore")
                variant: root.isDump ? "primary" : "danger"
                enabled: root.canSubmit
                Accessible.description: enabled ? "" : (root.isDump ? qsTr("Choose a file and at least one table.") : qsTr("Choose a file and type the confirmation."))
                onClicked: root.submit()
            }

            QButton {
                id: copyLogBtn

                visible: !root.formShown && root.controller.log.length > 0
                text: qsTr("Copy log")
                iconGlyph: Icons.copy
                onClicked: {
                    root.controller.copyLog();
                    root.announce(qsTr("Log copied"), false);
                }
            }

            QButton {
                id: stopBtn

                visible: root.busy
                enabled: !root.controller.cancelling
                text: root.isDump ? qsTr("Cancel dump") : qsTr("Cancel restore")
                onClicked: root.controller.cancel()
            }

            QButton {
                id: folderBtn

                visible: root.stateName === "done" && root.isDump
                text: qsTr("Show in folder")
                iconGlyph: Icons.folderOpen
                onClicked: root.controller.showInFolder()
            }

            QButton {
                id: backBtn

                visible: root.stateName === "failed" || root.stateName === "cancelled"
                text: qsTr("Back")
                onClicked: {
                    root.controller.reset();
                    // A confirmation is for one restore.
                    phraseField.text = "";
                    nameField.text = "";
                    Qt.callLater(() => pathField.forceActiveFocus(Qt.TabFocusReason));
                }
            }

            QButton {
                id: closeBtn

                visible: !root.formShown && !root.busy
                text: qsTr("Close")
                variant: root.stateName === "done" ? "primary" : "default"
                onClicked: root.close()
            }
        }
    }
}
