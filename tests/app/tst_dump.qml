import QtQuick
import QtTest
import Quarry.Controls

// Dump and Restore (H14 phase B): the entries of the connection menu, the dialog driven the way a
// keyboard-only user would, the confirmation of a restore (the phrase and, for production, the name
// of the database), the table selector with its search and the "tables they reference" box, the
// accessible names and the Database tools section of the settings. Temporary databases and files
// and the real SQLite plugin; the keychain is never touched. A restore is never run here (the
// sample database is shared): its job is covered by tst_dump_controller.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0

    name: "Dump"
    when: windowShown
    width: 1100
    height: 800
    visible: true

    A11y {
        id: walker
    }

    Component {
        id: spyComp

        SignalSpy {}
    }

    // ---- helpers -----------------------------------------------------------------------------

    function model() {
        return shell.connections.model;
    }

    function ctl() {
        return shell.dumper;
    }

    function dlg() {
        return shell.dumpDialog;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function outPath(name) {
        return testTempDir + "/" + (++fileCounter) + "-" + name;
    }

    function waitForTreeRoots() {
        tryVerify(() => shell.sidebar.tree !== null && shell.sidebar.tree.rows >= model().count, 20000);
    }

    function typeText(text) {
        for (const ch of text)
            keyClick(ch === " " ? Qt.Key_Space : ch);
    }

    function addSqlite(file) {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        shell.connections.openSqliteFile(Qt.url("file:///" + file));
        tryCompare(spy, "count", 1);
        waitForTreeRoots();
        return spy.signalArguments[0][0];
    }

    // A connection saved the way the dialog does it; `production` and the engine id are the point.
    function addConnection(name, file, production, providerId) {
        shell.connections.prepare("");
        shell.connections.form.setValue(0, file);
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        verify(shell.connections.save({
            "id": "",
            "providerId": providerId ?? "sqlite",
            "name": name,
            "colorIndex": 3,
            "production": production,
            "passwordMode": 1,
            "password": "",
            "passwordTouched": false
        }));
        tryCompare(spy, "count", 1);
        waitForTreeRoots();
        return spy.signalArguments[0][0];
    }

    function openDialog(mode, id) {
        shell.openDump(mode, id, "");
        tryVerify(() => dlg() !== null && dlg().opened);
        return dlg();
    }

    function closeDialogs() {
        const list = [dlg()?.replaceDialog, dlg()?.stopDialog, dlg(), shell.settingsDialog, shell.paletteDialog];
        for (const d of list) {
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        }
        tryVerify(() => !list.some(d => d && d.visible), 5000);
    }

    function removeAll() {
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
        tryCompare(model(), "count", 0);
    }

    // Tab through the dialog: the control that owns each focus stop, in order.
    function tabOrder(known) {
        const owner = item => known.find(k => walker.isInside(item, k)) ?? item;
        const seen = [];
        for (let guard = 0; guard < 30; ++guard) {
            const now = owner(testCase.Window.activeFocusItem);
            if (seen.includes(now))
                break;
            seen.push(now);
            keyClick(Qt.Key_Tab);
        }
        return seen;
    }

    function menuTexts() {
        const m = shell.sidebar.menu;
        const out = [];
        for (let i = 0; i < m.count; ++i) {
            const item = m.itemAt(i);
            if (item.visible && item.text)
                out.push(item.text);
        }
        return out;
    }

    function initTestCase() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/dump.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        tryVerify(() => !model().loading, 10000);
        // Nothing of H14 exists before it is used: no dialog, nothing running.
        compare(shell.dumpDialog, null);
        compare(ctl().state, "idle");
    }

    function init() {
        closeDialogs();
        Language.choice = Language.Choice.English;
        Theme.resetZoom();
        removeAll();
        ctl().reset();
        ctl().pgDumpPath = "";
        shell.focusRegion(0);
    }

    function cleanup() {
        a11yProbe.setEnvironment("FAKE_PG_MODE", "");
        if (ctl().busy)
            ctl().cancel();
        tryVerify(() => !ctl().busy, 20000);
        closeDialogs();
        Theme.mode = Theme.Mode.System;
        Theme.resetZoom();
        Language.choice = Language.Choice.English;
    }

    function cleanupTestCase() {
        Language.choice = Language.Choice.English;
    }

    // ---- the menu ----------------------------------------------------------------------------

    function test_menuOffersDumpAndRestoreForSqlite() {
        const id = addSqlite(testSampleDb);
        const idx = shell.connections.tree.indexOfConnection(id);
        shell.sidebar.popupMenu(shell.sidebar.tree, idx);
        tryVerify(() => shell.sidebar.menu.opened);
        verify(shell.sidebar.menu.dumpAvailable);
        const texts = menuTexts();
        verify(texts.includes("Dump…"), texts.join("|"));
        verify(texts.includes("Restore…"), texts.join("|"));
        // The entries sit before Delete, which stays last.
        compare(texts[texts.length - 1], "Delete");
        // Choosing one opens the dialog once the menu has closed.
        for (let i = 0; i < shell.sidebar.menu.count; ++i) {
            const item = shell.sidebar.menu.itemAt(i);
            if (item.text === "Dump…")
                item.triggered();
        }
        shell.sidebar.menu.close();
        tryVerify(() => dlg() !== null && dlg().opened);
        compare(dlg().mode, "dump");
        compare(dlg().connectionId, id);
        compare(dlg().title, "Dump database");
    }

    function test_menuHidesDumpForEnginesWithoutIt() {
        const id = addConnection("Elsewhere", testTempDir + "/none.db", false, "mysql");
        const idx = shell.connections.tree.indexOfConnection(id);
        shell.sidebar.popupMenu(shell.sidebar.tree, idx);
        tryVerify(() => shell.sidebar.menu.opened);
        verify(!shell.sidebar.menu.dumpAvailable);
        const texts = menuTexts();
        verify(!texts.includes("Dump…"), texts.join("|"));
        verify(!texts.includes("Restore…"), texts.join("|"));
        shell.sidebar.menu.close();
        tryVerify(() => !shell.sidebar.menu.opened);
    }

    // ---- the form ----------------------------------------------------------------------------

    function test_dumpDialogIsLabelledAndKeyboardOnly() {
        const id = addSqlite(testSampleDb);
        const d = openDialog("dump", id);
        compare(d.title, "Dump database");
        tryVerify(() => d.pathField.activeFocus); // the file is where to start
        verify(d.pathField.text.endsWith(".sql"));
        compare(d.pathField.Accessible.name, "File to write");
        compare(d.contentCombo.Accessible.name, "Contents");
        compare(d.tablesBox.Accessible.role, Accessible.CheckBox);
        compare(d.actionButton.text, "Dump");
        verify(d.actionButton.enabled);
        // SQLite has no format, schemas or owners: those controls are not offered.
        verify(!d.formatCombo.visible);
        verify(!d.schemasField.visible);
        verify(!d.ownershipBox.visible);
        compare(walker.problems(d.contentItem), []);
        compare(walker.problems(shell), []);
        Theme.mode = Theme.Mode.HighContrast;
        compare(walker.problems(d.contentItem), []);
        Theme.mode = Theme.Mode.Dark;
        compare(walker.problems(d.contentItem), []);
        Theme.mode = Theme.Mode.Light;
        for (const b of walker.findAll(d.contentItem, i => i.Accessible.role === Accessible.Button && i.visible)) {
            verify(b.height >= Theme.controlHeight, b.text + " is big enough");
            verify(b.Accessible.name !== "");
        }
        // Tab order: the file, Browse, then the buttons; every stop reachable.
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        const order = tabOrder([d.contentCombo, d.tablesBox, d.pathField, d.browseButton, d.cancelButton, d.actionButton]);
        verify(order.indexOf(d.pathField) < order.indexOf(d.browseButton));
        verify(order.indexOf(d.browseButton) < order.indexOf(d.cancelButton));
        verify(order.indexOf(d.cancelButton) < order.indexOf(d.actionButton));
        // Esc closes the form.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !d.visible);
    }

    function test_unsupportedConnectionSaysWhy() {
        const id = addConnection("Elsewhere", testTempDir + "/none.db", false, "mysql");
        const d = openDialog("dump", id);
        verify(!d.actionButton.enabled);
        verify(!d.pathField.visible);
        compare(d.info.supported, false);
        verify(d.info.reason.length > 0);
        const text = find(d.contentItem, i => i.text !== undefined && i.text === d.info.reason);
        verify(text !== undefined, "the reason is on screen");
        compare(walker.problems(d.contentItem), []);
        // Cancel has the focus: there is nothing to fill in.
        tryVerify(() => d.cancelButton.activeFocus);
    }

    // ---- the table selector --------------------------------------------------------------------

    function test_tableSelectorSearchesAndClosesOverForeignKeys() {
        const id = addSqlite(testSampleDb);
        const d = openDialog("dump", id);
        tryCompare(ctl(), "tablesState", "ready", 20000);
        verify(d.tablesBox.enabled);
        verify(!d.tableList.visible); // whole database until the box is ticked
        d.tablesBox.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(d.tablesBox.checked);
        tryVerify(() => d.tableList.visible);
        // Tables only: the sample's view is not listed.
        compare(d.tableList.count, 4);
        compare(d.searchField.Accessible.name, "Search tables");
        compare(d.tableList.Accessible.name, "Tables");
        verify(!d.actionButton.enabled, "a selector with nothing chosen cannot dump");

        // The search narrows the list.
        d.searchField.forceActiveFocus(Qt.TabFocusReason);
        typeText("inv");
        tryCompare(d.tableList, "count", 1);
        compare(ctl().tables[d.visibleRows[0]].name, "invoices");

        // Space ticks the current row; invoices points at customers, which comes along.
        d.tableList.forceActiveFocus(Qt.TabFocusReason);
        d.tableList.currentIndex = 0;
        keyClick(Qt.Key_Space);
        compare(d.selectedRows.length, 1);
        compare(d.effectiveRows.length, 2);
        verify(d.actionButton.enabled);
        // Clearing the search shows all of them: the pulled-in one says so in words.
        d.searchField.text = "";
        tryCompare(d.tableList, "count", 4);
        const pulled = find(d.tableList, i => i.Accessible && i.Accessible.name === "customers, included because a selected table references it");
        verify(pulled !== undefined, "the referenced table is named as such");
        // Without the box only the chosen table is dumped.
        d.refsBox.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(!d.refsBox.checked);
        compare(d.effectiveRows.length, 1);
        keyClick(Qt.Key_Space);
        verify(d.refsBox.checked);
        compare(d.effectiveRows.length, 2);
        // Select shown / Clear shown.
        d.selectAllButton.clicked();
        compare(d.selectedRows.length, 4);
        d.clearButton.clicked();
        compare(d.selectedRows.length, 0);
        verify(!d.actionButton.enabled);
        compare(walker.problems(d.contentItem), []);
    }

    function test_dumpWritesOnlyTheChosenTablesAndWhatTheyReference() {
        const id = addSqlite(testSampleDb);
        const d = openDialog("dump", id);
        tryCompare(ctl(), "tablesState", "ready", 20000);
        d.tablesBox.checked = true;
        tryVerify(() => d.tableList.visible);
        const invoices = ctl().tables.findIndex(t => t.name === "invoices");
        d.toggleRow(invoices);
        const out = outPath("chosen.sql");
        d.pathField.text = out;
        const spy = createTemporaryObject(spyComp, testCase, {
            target: ctl(),
            signalName: "announcement"
        });
        d.actionButton.clicked();
        tryCompare(ctl(), "state", "done", 60000);
        const sql = a11yProbe.readFile(out);
        verify(sql.includes("invoices"));
        verify(sql.includes("customers"), "the referenced table came along");
        verify(!sql.includes("products"));
        verify(spy.count >= 2); // started and finished
        // The end: the result and the way on.
        tryVerify(() => d.closeButton.activeFocus);
        verify(d.progressLabel.text.includes("chosen.sql"));
        verify(d.folderButton.visible);
        compare(walker.problems(d.contentItem), []);
        d.closeButton.clicked();
        tryVerify(() => !d.visible);
    }

    function test_anExistingFileAsksBeforeItIsReplaced() {
        const id = addSqlite(testSampleDb);
        const d = openDialog("dump", id);
        const out = outPath("keep.sql");
        a11yProbe.writeFile(out, "keep me");
        d.pathField.text = out;
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryVerify(() => d.replaceDialog.opened);
        compare(a11yProbe.readFile(out), "keep me");
        compare(ctl().state, "idle");
        compare(walker.problems(d.replaceDialog.contentItem), []);
        // The safe button has the focus; Escape leaves the file alone.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !d.replaceDialog.visible);
        compare(a11yProbe.readFile(out), "keep me");
        compare(ctl().state, "idle");
    }

    // ---- restore -------------------------------------------------------------------------------

    function test_restoreNeedsThePhraseBeforeItCanRun() {
        const id = addSqlite(testSampleDb);
        const d = openDialog("restore", id);
        compare(d.title, "Restore database");
        compare(d.actionButton.text, "Restore");
        compare(d.actionButton.variant, "danger");
        verify(!d.actionButton.enabled);
        verify(!d.nameField.visible, "not a production connection: no database name");
        verify(!d.tablesBox.visible);
        compare(d.pathField.Accessible.name, "File to restore from");
        compare(d.phraseField.Accessible.name, "To confirm, type RESTORE");
        verify(find(d.contentItem, i => i.text !== undefined && String(i.text).includes("cannot be undone")) !== undefined);
        // A file that exists, but no phrase yet.
        d.pathField.text = testSampleDb;
        verify(!d.actionButton.enabled);
        d.phraseField.forceActiveFocus(Qt.TabFocusReason);
        typeText("restore"); // the case matters
        verify(!d.actionButton.enabled);
        d.phraseField.text = "";
        typeText("RESTOR");
        verify(!d.actionButton.enabled);
        typeText("E");
        verify(d.actionButton.enabled);
        compare(d.actionButton.Accessible.description, "");
        // No file, no restore.
        d.pathField.text = "";
        verify(!d.actionButton.enabled);
        compare(d.actionButton.Accessible.description, "Choose a file and type the confirmation.");
        compare(walker.problems(d.contentItem), []);
        // Enter in the phrase field does nothing while the button is off.
        d.phraseField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        compare(ctl().state, "idle");
    }

    function test_restoreOfAProductionConnectionAlsoNeedsTheDatabaseName() {
        const id = addConnection("Live", testSampleDb, true);
        const d = openDialog("restore", id);
        verify(d.nameField.visible);
        compare(d.nameField.Accessible.name, "Database name");
        d.pathField.text = testSampleDb;
        d.phraseField.text = "RESTORE";
        verify(!d.actionButton.enabled, "the phrase alone is not enough");
        d.nameField.forceActiveFocus(Qt.TabFocusReason);
        typeText("wrong.db");
        verify(!d.actionButton.enabled);
        d.nameField.text = "";
        typeText(d.info.database.slice(0, -1));
        verify(!d.actionButton.enabled);
        typeText(d.info.database.slice(-1));
        verify(d.actionButton.enabled);
        // Changing the phrase takes it back.
        d.phraseField.text = "RESTORE ";
        verify(d.actionButton.enabled); // surrounding blanks are ignored
        d.phraseField.text = "restore";
        verify(!d.actionButton.enabled);
        // The button never starts without both: Enter in the name field is ignored too.
        d.nameField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        compare(ctl().state, "idle");
        // The tab order follows the form: file, phrase, name, then the buttons.
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        const order = tabOrder([d.pathField, d.browseButton, d.phraseField, d.nameField, d.cancelButton, d.actionButton]);
        verify(order.indexOf(d.pathField) < order.indexOf(d.phraseField));
        verify(order.indexOf(d.phraseField) < order.indexOf(d.nameField));
        verify(order.indexOf(d.nameField) < order.indexOf(d.cancelButton));
        compare(walker.problems(d.contentItem), []);
    }

    function test_restoreOfAMissingFileFailsBeforeAnythingRuns() {
        const id = addSqlite(testSampleDb);
        const d = openDialog("restore", id);
        d.pathField.text = outPath("nothing.sql");
        d.phraseField.text = "RESTORE";
        verify(d.actionButton.enabled);
        d.actionButton.clicked();
        compare(ctl().state, "idle");
        verify(d.startError !== "");
        verify(find(d.contentItem, i => i.text !== undefined && i.text === d.startError) !== undefined);
        verify(!d.stopButton.visible);
    }

    // ---- a running job (PostgreSQL through the fake tool of tests/core) --------------------------

    function startFakePgDump(mode) {
        a11yProbe.setEnvironment("FAKE_PG_MODE", mode);
        ctl().pgDumpPath = testFakePgTool;
        // No Secret field in this form: the connection has no password and the keychain stays out.
        const id = addConnection("Pg", testTempDir + "/pg.db", false, "postgres");
        const d = openDialog("dump", id);
        verify(d.isPostgres);
        verify(d.formatCombo.visible);
        verify(d.schemasField.visible);
        verify(d.ownershipBox.visible);
        verify(!d.ownershipBox.checked, "owners and privileges are off by default");
        d.pathField.text = outPath("pg.sql");
        d.actionButton.clicked();
        return d;
    }

    function test_aRunningDumpShowsTheLogAndAsksBeforeStopping() {
        const d = startFakePgDump("slow");
        tryCompare(ctl(), "state", "running", 20000);
        tryVerify(() => d.stopButton.visible && d.stopButton.activeFocus);
        tryVerify(() => d.logList.count > 0, 20000);
        verify(d.progressBar.visible);
        compare(d.progressBar.Accessible.role, Accessible.ProgressBar);
        verify(d.copyLogButton.visible);
        verify(!d.actionButton.visible);
        compare(walker.problems(d.contentItem), []);
        // Copy log puts the tool's output on the clipboard.
        d.copyLogButton.clicked();
        verify(a11yProbe.clipboardText().includes("dumping contents"));
        // Esc asks; "Keep running" has the focus and leaves everything as it was.
        keyClick(Qt.Key_Escape);
        tryVerify(() => d.stopDialog.opened);
        verify(d.stopDialog.cancelButton.activeFocus);
        compare(d.stopDialog.cancelButton.text, "Keep running");
        compare(walker.problems(d.stopDialog.contentItem), []);
        keyClick(Qt.Key_Space);
        tryVerify(() => !d.stopDialog.visible);
        compare(ctl().state, "running");
        verify(d.visible);
        // Asking again and confirming cancels it, writes no file and puts the focus on Back.
        keyClick(Qt.Key_Escape);
        tryVerify(() => d.stopDialog.opened);
        d.stopDialog.confirmButton.clicked();
        tryCompare(ctl(), "state", "cancelled", 20000);
        tryVerify(() => d.backButton.activeFocus);
        verify(d.progressLabel.text.includes("cancelled"));
        verify(d.visible);
        compare(a11yProbe.readFile(ctl().filePath), "");
        d.backButton.clicked();
        tryCompare(ctl(), "state", "idle");
        tryVerify(() => d.pathField.activeFocus);
    }

    function test_aFailedToolShowsTheErrorWithCopyableDetails() {
        const d = startFakePgDump("authfail");
        tryCompare(ctl(), "state", "failed", 20000);
        tryVerify(() => d.errorPanel.visible);
        tryVerify(() => d.backButton.activeFocus);
        verify(ctl().errorMessage.length > 0);
        verify(ctl().errorDetail.length > 0);
        compare(d.errorPanel.title, "The dump failed");
        verify(d.errorPanel.detailsButton.visible);
        verify(d.errorPanel.hint.includes("No file was written"));
        verify(d.copyLogButton.visible);
        compare(walker.problems(d.contentItem), []);
    }

    function test_aFinishedDumpShowsTheResultAndTheFolderButton() {
        const d = startFakePgDump("ok");
        tryCompare(ctl(), "state", "done", 20000);
        tryVerify(() => d.closeButton.activeFocus);
        verify(d.progressLabel.text.includes("pg.sql"));
        verify(d.folderButton.visible);
        verify(!d.stopButton.visible);
        verify(!d.progressBar.visible);
        compare(a11yProbe.readFile(ctl().filePath), "-- fake dump
");
        compare(walker.problems(d.contentItem), []);
        d.closeButton.clicked();
        tryVerify(() => !d.visible);
    }

    // ---- settings ------------------------------------------------------------------------------

    function test_settingsHaveTheDatabaseToolsSection() {
        CommandRegistry.trigger("settings.open");
        tryVerify(() => shell.settingsDialog?.opened);
        const s = shell.settingsDialog;
        verify(s.toolsSection.visible);
        compare(s.pgDumpRow.pathField.Accessible.name, "pg_dump");
        compare(s.pgRestoreRow.pathField.Accessible.name, "pg_restore");
        compare(s.psqlRow.pathField.Accessible.name, "psql");
        compare(s.pgDumpRow.browseButton.Accessible.name, "Browse… (pg_dump)");
        compare(s.pgDumpRow.testButton.Accessible.name, "Test (pg_dump)");
        compare(walker.problems(s.contentItem), []);
        // Test shows an error, translated, for a path that is not a program.
        const missing = outPath("no-such-pg_dump");
        s.pgDumpRow.pathField.text = missing;
        s.pgDumpRow.testButton.clicked();
        compare(ctl().pgDumpPath, missing);
        tryVerify(() => s.pgDumpRow.statusText.visible);
        verify(s.pgDumpRow.statusText.text.length > 0);
        verify(s.lastAnnouncement.length > 0);
        // An empty path means "look for it": the test result is either found or an error, never silent.
        s.pgDumpRow.pathField.text = "";
        s.pgDumpRow.testButton.clicked();
        compare(ctl().pgDumpPath, "");
        verify(s.pgDumpRow.statusText.text.length > 0);
        compare(walker.problems(s.contentItem), []);
    }

    // ---- language and zoom -----------------------------------------------------------------------

    function test_spanishStrings() {
        const id = addSqlite(testSampleDb);
        Language.choice = Language.Choice.Spanish;
        const d = openDialog("dump", id);
        compare(d.title, "Volcar base de datos");
        compare(d.pathField.Accessible.name, "Archivo de destino");
        compare(d.contentCombo.Accessible.name, "Contenido");
        compare(d.actionButton.text, "Volcar");
        compare(d.cancelButton.text, "Cancelar");
        compare(d.browseButton.text, "Examinar…");
        compare(walker.problems(d.contentItem), []);
        d.close();
        tryVerify(() => !d.visible);
        const r = openDialog("restore", id);
        compare(r.title, "Restaurar base de datos");
        compare(r.actionButton.text, "Restaurar");
        compare(r.phraseField.Accessible.name, "Para confirmar, escribe RESTORE");
        compare(r.pathField.Accessible.name, "Archivo desde el que restaurar");
    }

    function test_zoomedDialogKeepsTheFileFieldInView() {
        const id = addSqlite(testSampleDb);
        Theme.zoom = 2.0;
        const d = openDialog("dump", id);
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        tryVerify(() => {
            const y = d.pathField.mapToItem(d.flick.contentItem, 0, 0).y - d.flick.contentY;
            return y >= 0 && y + d.pathField.height <= d.flick.height + 1;
        });
        compare(walker.problems(d.contentItem), []);
        Theme.resetZoom();
    }
}
