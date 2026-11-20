import QtQuick
import QtTest
import Quarry.Controls

// Export (H5): the "Export..." dialog driven the way a keyboard-only user would, from the shortcut
// and from the palette, with the options of each format, the replace question, the progress and
// the end, Esc to cancel, the data tab's button, the accessibility walker in the normal and the
// high contrast theme, 200 % zoom and Spanish. Temporary databases and files and the real SQLite
// plugin; no keychain. Delegates are never read right after an event: tryVerify() first.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0

    name: "Export"
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

    function tabs() {
        return shell.editorTabs;
    }

    function model() {
        return shell.connections.model;
    }

    function current() {
        return shell.queries.current;
    }

    function ctl() {
        return shell.exporter;
    }

    function dlg() {
        return shell.exportDialog;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function newFile() {
        return testTempDir + "/export-data-" + (++fileCounter) + ".db";
    }

    function outPath(name) {
        return testTempDir + "/" + name;
    }

    function read(path) {
        return a11yProbe.readFile(path);
    }

    function waitForViews() {
        tryVerify(() => shell.sidebar.tree === null ? model().count === 0 : shell.sidebar.tree.rows >= model().count, 20000);
        if (tabs().count > 0)
            tryVerify(() => shell.editorArea.currentView !== null && shell.editorArea.currentView.visible);
    }

    function addSqlite(file) {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        shell.connections.openSqliteFile(Qt.url("file:///" + file));
        tryCompare(spy, "count", 1);
        waitForViews();
        return spy.signalArguments[0][0];
    }

    // A new tab on the connection, with `sql` as its text and the editor focused.
    function openQuery(sql) {
        shell.focusRegion(1);
        keyClick(Qt.Key_T, Qt.ControlModifier);
        waitForViews();
        const editor = shell.editorArea.currentEditor;
        tryVerify(() => editor.edit.activeFocus);
        editor.edit.text = sql;
        compare(tabs().textOf(tabs().currentIndex), sql);
    }

    // Runs all of the tab's statements and waits for the end.
    function runAll(state) {
        shell.focusRegion(1);
        keyClick(Qt.Key_Return, Qt.ControlModifier | Qt.ShiftModifier);
        tryCompare(current(), "state", state ?? "done", 60000);
    }

    // A connection with a table and a tab whose result is on the grid.
    function withResult(sql) {
        addSqlite(newFile());
        openQuery(sql ?? "CREATE TABLE t(a INTEGER, b TEXT); INSERT INTO t VALUES (1,'x'),(2,'y,z'),(3,NULL); SELECT a, b FROM t");
        runAll();
    }

    function openExport() {
        keyClick(Qt.Key_E, Qt.ControlModifier | Qt.ShiftModifier);
        tryVerify(() => dlg() !== null && dlg().opened);
        tryVerify(() => dlg().formatList.activeFocus);
    }

    function closeDialogs() {
        const list = [dlg(), dlg()?.replaceDialog, shell.paletteDialog, shell.confirmRunDialog, shell.closeTabDialog];
        // The replace question first: it sits on top of the export dialog.
        for (const d of list.reverse()) {
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        }
        tryVerify(() => !list.some(d => d && d.visible), 5000);
    }

    function pick(index) {
        const list = dlg().formatList;
        list.forceActiveFocus(Qt.TabFocusReason);
        for (let guard = 0; guard < 8 && list.currentIndex !== index; ++guard)
            keyClick(list.currentIndex < index ? Qt.Key_Down : Qt.Key_Up);
        compare(list.currentIndex, index);
    }

    // Tab through the dialog: the control that owns each focus stop, in order. A control may hand
    // the focus to a child, so a stop counts for the known control that contains it.
    function tabOrder(known) {
        const owner = item => known.find(k => walker.isInside(item, k)) ?? item;
        const seen = [];
        for (let guard = 0; guard < 20; ++guard) {
            const now = owner(testCase.Window.activeFocusItem);
            if (seen.includes(now))
                break;
            seen.push(now);
            keyClick(Qt.Key_Tab);
        }
        return seen;
    }

    function initTestCase() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/export.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        tryVerify(() => !model().loading, 10000);
        tryVerify(() => tabs().loaded, 10000);
        // Nothing of H5 exists before it is used: no dialog, no session, no export state.
        compare(shell.exportDialog, null);
        compare(ctl().state, "idle");
    }

    function init() {
        closeDialogs();
        LayoutStore.remove("exportFormat"); // the last format is remembered: start from CSV
        Language.choice = Language.Choice.English;
        Theme.resetZoom();
        shell.resetLayout();
        while (tabs().count > 0)
            tabs().closeTab(0);
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
        tryCompare(model(), "count", 0);
        ctl().reset();
        shell.focusRegion(1);
    }

    function cleanup() {
        if (ctl().busy)
            ctl().cancel();
        tryVerify(() => !ctl().busy, 20000);
        closeDialogs();
        Theme.mode = Theme.Mode.System;
        Theme.resetZoom();
        Language.choice = Language.Choice.English;
        while (tabs().count > 0)
            tabs().closeTab(0);
    }

    function cleanupTestCase() {
        Language.choice = Language.Choice.English;
    }

    // ---- the command and the button ----------------------------------------------------------

    function test_commandIsRegisteredWithAFreeShortcut() {
        const c = CommandRegistry.find("export.open");
        verify(c !== null && c !== undefined);
        compare(c.title, "Export…");
        compare(c.category, "Export");
        compare(c.shortcut, "Ctrl+Shift+E");
        const seen = {};
        for (const k of CommandRegistry.commands.slice()) {
            for (const s of [k.shortcut, k.alternateShortcut]) {
                if (s === "")
                    continue;
                verify(seen[s] === undefined, s + " is used by " + seen[s] + " and " + k.commandId);
                seen[s] = k.commandId;
            }
        }
    }

    function test_disabledWithoutAResultAndSaysWhy() {
        // No tab at all.
        verify(!CommandRegistry.find("export.open").enabled);
        compare(shell.resultsPanel.exportButton.enabled, false);
        verify(shell.resultsPanel.exportButton.Accessible.description !== "", "the reason is the description");
        addSqlite(newFile());
        openQuery("SELECT 1");
        // A tab that has not run yet: still nothing to export.
        verify(!CommandRegistry.find("export.open").enabled);
        verify(!shell.resultsPanel.exportButton.enabled);
        compare(shell.resultsPanel.exportButton.Accessible.description, ctl().info.reason);
        verify(ctl().info.reason.includes("result"));
        keyClick(Qt.Key_E, Qt.ControlModifier | Qt.ShiftModifier);
        wait(100);
        verify(!(dlg()?.visible ?? false), "the shortcut does nothing while it is disabled");
        runAll();
        tryVerify(() => CommandRegistry.find("export.open").enabled);
        verify(shell.resultsPanel.exportButton.enabled);
        compare(shell.resultsPanel.exportButton.accessibleName, "Export…");
        compare(shell.resultsPanel.exportButton.Accessible.role, Accessible.Button);
        verify(shell.resultsPanel.exportButton.height >= Theme.controlHeight);
        // The reason goes away with the result.
        compare(shell.resultsPanel.exportButton.Accessible.description, CommandRegistry.find("export.open").shortcutText);
    }

    function test_theButtonOpensTheDialogFromTheKeyboard() {
        withResult();
        const button = shell.resultsPanel.exportButton;
        tryVerify(() => button.enabled);
        button.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryVerify(() => dlg() !== null && dlg().opened);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dlg().visible);
        // The focus goes back to the button that opened it.
        tryVerify(() => button.activeFocus);
    }

    function test_paletteOpensTheDialog() {
        withResult();
        keyClick(Qt.Key_K, Qt.ControlModifier);
        tryVerify(() => shell.paletteDialog?.opened);
        const field = find(shell.overlayItem, i => i.accessibleName === "Search commands");
        tryVerify(() => field.activeFocus);
        for (const ch of "export")
            keyClick(ch);
        const list = walker.findAll(shell.paletteDialog.contentItem, i => i.Accessible.role === Accessible.List)[0];
        tryCompare(list, "count", 1);
        compare(list.currentItem.commandId, "export.open");
        keyClick(Qt.Key_Return);
        tryVerify(() => !shell.paletteDialog.visible);
        tryVerify(() => dlg() !== null && dlg().opened);
    }

    // ---- the dialog --------------------------------------------------------------------------

    function test_dialogIsAccessibleInEveryTheme() {
        withResult();
        openExport();
        compare(dlg().title, "Export");
        compare(dlg().formatList.Accessible.role, Accessible.List);
        compare(dlg().formatList.Accessible.name, "Format");
        compare(dlg().formatList.count, 6);
        compare(dlg().pathField.Accessible.name, "File");
        compare(dlg().scopeCombo.Accessible.name, "Rows to export");
        verify(dlg().pathField.text.endsWith(".csv"));
        compare(walker.problems(dlg().contentItem), []);
        compare(walker.problems(shell), []);
        Theme.mode = Theme.Mode.HighContrast;
        verify(Theme.isHighContrast);
        compare(walker.problems(dlg().contentItem), []);
        Theme.mode = Theme.Mode.Dark;
        compare(walker.problems(dlg().contentItem), []);
        Theme.mode = Theme.Mode.Light;
        // Every button has a text of at least the minimum target size.
        for (const b of walker.findAll(dlg().contentItem, i => i.Accessible.role === Accessible.Button && i.visible)) {
            verify(b.height >= Theme.controlHeight, b.text + " is big enough");
            verify(b.Accessible.name !== "");
        }
    }

    function test_optionsFollowTheFormat() {
        withResult();
        openExport();
        const d = dlg();
        // [format, delimiter, header, bom, table, rows per insert]
        const expected = [
            ["CSV", true, true, true, false, false],
            ["TSV", false, true, true, false, false],
            ["JSON", false, false, false, false, false],
            ["JSON Lines", false, false, false, false, false],
            ["SQL INSERT", false, false, false, true, true],
            ["Markdown", false, true, false, false, false]
        ];
        for (let i = 0; i < expected.length; ++i) {
            pick(i);
            const e = expected[i];
            compare(d.format, e[0]);
            compare(d.delimiterCombo.visible, e[1], e[0] + " separator");
            compare(d.headerBox.visible, e[2], e[0] + " header");
            compare(d.bomBox.visible, e[3], e[0] + " bom");
            compare(d.tableField.visible, e[4], e[0] + " table");
            compare(d.rowsField.visible, e[5], e[0] + " rows per insert");
        }
        // The extension follows the format while the file name is the suggested one.
        pick(0);
        verify(d.pathField.text.endsWith(".csv"));
        pick(3);
        verify(d.pathField.text.endsWith(".jsonl"));
        pick(4);
        verify(d.pathField.text.endsWith(".sql"));
        compare(d.tableField.text, "export_table");
        keyClick(Qt.Key_End);
        compare(d.format, "Markdown");
        verify(d.pathField.text.endsWith(".md"));
        keyClick(Qt.Key_Home);
        compare(d.format, "CSV");
        // A name the user typed with another extension is left alone.
        d.pathField.text = outPath("mine.txt");
        keyClick(Qt.Key_Down); // the focus is on the list
        compare(d.pathField.text, outPath("mine.txt"));
        // The rows per INSERT field takes only 1 to 1000.
        pick(4);
        d.rowsField.forceActiveFocus(Qt.TabFocusReason);
        d.rowsField.text = "";
        keyClick("5");
        keyClick("0");
        keyClick("0");
        keyClick("0");
        compare(d.rowsField.text, "500"); // 5000 is over the limit: the last digit is refused
        // Each option is a control that can be reached with the keyboard.
        for (const c of [d.delimiterCombo, d.headerBox, d.bomBox, d.tableField, d.rowsField, d.scopeCombo, d.pathField, d.browseButton]) {
            verify(c.activeFocusOnTab || c.focusPolicy !== Qt.NoFocus, c + " takes the focus");
        }
    }

    function test_tabOrderFollowsTheScreen() {
        withResult();
        openExport();
        const d = dlg();
        const known = [d.formatList, d.delimiterCombo, d.headerBox, d.bomBox, d.scopeCombo, d.pathField, d.browseButton, d.cancelButton, d.exportButton];
        const order = tabOrder(known);
        // Format, separator, header, bom, rows, file, browse, cancel, export.
        compare(order.length, known.length, order.map(i => i.toString()).join(", "));
        for (let i = 0; i < known.length; ++i)
            compare(order[i], known[i], "stop " + i);
        // Shift+Tab goes back.
        d.exportButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Tab, Qt.ShiftModifier);
        verify(d.cancelButton.activeFocus);
    }

    // ---- exporting ---------------------------------------------------------------------------

    function test_exportsWithTheKeyboardAndShowsTheResult() {
        withResult();
        openExport();
        const d = dlg();
        const file = outPath("keyboard.csv");
        d.pathField.text = file;
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return); // Enter in the file field starts it
        tryCompare(ctl(), "state", "done", 20000);
        tryVerify(() => d.closeButton.activeFocus, 5000, "the focus moves to Close");
        compare(read(file), "a,b\n1,x\n2,\"y,z\"\n3,\n");
        compare(d.progressLabel.text, ctl().resultText);
        verify(d.progressLabel.text.startsWith("Exported 3 rows ("));
        compare(d.progressLabel.Accessible.name, "Export progress");
        compare(d.progressLabel.Accessible.role, Accessible.StaticText);
        compare(d.lastAnnouncement, ctl().resultText);
        verify(d.folderButton.visible);
        verify(!d.formShown);
        compare(walker.problems(d.contentItem), []);
        // Esc closes it once it has ended, and the focus returns.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !d.visible);
        // Opening it again starts from the form, with the folder of the last export.
        openExport();
        verify(d.formShown);
        verify(d.pathField.text.startsWith(testTempDir));
    }

    function test_existingFileIsOnlyReplacedAfterAsking() {
        withResult();
        const file = outPath("exists.csv");
        a11yProbe.writeFile(file, "keep me");
        compare(a11yProbe.readFile(file), "keep me");
        tryVerify(() => ctl().fileExists(file));
        openExport();
        const d = dlg();
        d.pathField.text = file;
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryVerify(() => d.replaceDialog.opened);
        compare(d.replaceDialog.title, "Replace file?");
        verify(d.replaceDialog.cancelButton.activeFocus, "the focus starts on Cancel");
        compare(d.replaceDialog.confirmButton.text, "Replace");
        compare(walker.problems(d.replaceDialog.contentItem), []);
        // Esc: nothing started, the file is as it was.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !d.replaceDialog.visible);
        compare(ctl().state, "idle");
        compare(read(file), "keep me");
        verify(d.visible);
        // Again, and this time Replace, with Tab and Space.
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryVerify(() => d.replaceDialog.opened);
        keyClick(Qt.Key_Tab);
        verify(d.replaceDialog.confirmButton.activeFocus);
        keyClick(Qt.Key_Space);
        tryCompare(ctl(), "state", "done", 20000);
        compare(read(file), "a,b\n1,x\n2,\"y,z\"\n3,\n");
    }

    function test_errorsStayInTheFormWithAMessage() {
        withResult();
        openExport();
        const d = dlg();
        d.pathField.text = outPath("no/such/folder/x.csv");
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryVerify(() => d.startError !== "");
        verify(d.startError.includes("does not exist"));
        compare(d.lastAnnouncement, d.startError);
        verify(d.formShown);
        d.pathField.text = "";
        keyClick(Qt.Key_Return);
        tryCompare(d, "startError", "Choose a file to write.");
    }

    // A destination that cannot be written ends in the failure panel, with the message and the
    // details, and Back returns to the form.
    function test_failureShowsTheMessageAndDetailsAndGoesBack() {
        withResult("SELECT 1 AS a");
        const file = outPath("locked.csv");
        a11yProbe.writeFile(file, "old");
        compare(a11yProbe.readFile(file), "old");
        a11yProbe.setReadOnly(file, true);
        openExport();
        const d = dlg();
        d.pathField.text = file;
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryVerify(() => d.replaceDialog.opened);
        keyClick(Qt.Key_Tab);
        keyClick(Qt.Key_Space); // Replace
        tryCompare(ctl(), "state", "failed", 20000);
        a11yProbe.setReadOnly(file, false);
        tryVerify(() => d.errorPanel.visible);
        compare(d.errorPanel.title, "The export failed");
        compare(d.errorPanel.message, ctl().errorMessage);
        verify(ctl().errorMessage !== "");
        compare(d.lastAnnouncement, ctl().errorMessage);
        tryVerify(() => d.backButton.activeFocus, 5000);
        compare(read(file), "old"); // the old file was left as it was
        compare(walker.problems(d.contentItem), []);
        keyClick(Qt.Key_Return); // Back
        tryVerify(() => d.formShown);
        tryVerify(() => d.pathField.activeFocus);
        compare(d.pathField.text, file);
    }

    // The streaming scope: the grid has the first page only, the file has every row.
    function test_runAgainWritesEveryRow() {
        withResult("CREATE TABLE n AS WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 3000) SELECT x FROM c; SELECT x FROM n");
        openExport();
        const d = dlg();
        compare(d.scopeCombo.count, 2);
        verify(d.scopeCombo.enabled);
        compare(d.scopeCombo.displayText, "Current result (rows loaded: " + Number(shell.queries.current.results.rows).toLocaleString(Qt.locale(), "f", 0) + ")");
        verify(find(d.contentItem, i => i.text !== undefined && String(i.text).startsWith("More rows exist than the grid has loaded")) !== undefined, "it says the grid has not everything");
        d.scopeCombo.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        compare(d.scope, "query");
        compare(d.scopeCombo.displayText, "Run the query again (all rows)");
        const file = outPath("all.csv");
        d.pathField.text = file;
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryCompare(ctl(), "state", "done", 60000);
        compare(ctl().rows, 3000);
        const lines = read(file).trim().split("\n");
        compare(lines.length, 3001);
        compare(lines[0], "x");
        compare(lines[3000], "3000");
        verify(d.progressLabel.text.includes(Number(3000).toLocaleString(Qt.locale(), "f", 0)));
    }

    function test_aStatementThatReturnsNoRowsCanOnlyExportTheCurrentResult() {
        withResult("CREATE TABLE t(a); INSERT INTO t VALUES (1); PRAGMA table_info(t)");
        openExport();
        const d = dlg();
        compare(d.scopeCombo.count, 1);
        verify(!d.scopeCombo.enabled);
        const note = find(d.contentItem, i => i.text !== undefined && String(i.text).startsWith("Running again is only possible"));
        verify(note !== undefined, "it says why");
        compare(note.Accessible.role, Accessible.StaticText);
    }

    // Esc is the Cancel key while it runs; the file is not written.
    function test_escCancelsARunningExport() {
        withResult("CREATE TABLE n AS WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 2000000) SELECT x FROM c; SELECT x FROM n");
        openExport();
        const d = dlg();
        d.scopeCombo.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        compare(d.scope, "query");
        const file = outPath("cancel.csv");
        d.pathField.text = file;
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryCompare(ctl(), "state", "running", 20000);
        tryVerify(() => d.stopButton.activeFocus, 5000, "the focus is on Cancel export");
        compare(d.stopButton.text, "Cancel export");
        verify(d.progressLabel.text.includes("rows written") || d.progressLabel.text.includes("row written") || d.progressLabel.text === "Preparing…");
        compare(d.progressLabel.Accessible.name, "Export progress");
        keyClick(Qt.Key_Escape);
        verify(d.visible, "Esc cancels, it does not close the dialog");
        tryCompare(ctl(), "state", "cancelled", 30000);
        tryVerify(() => d.backButton.activeFocus, 5000);
        verify(d.progressLabel.text.startsWith("Export cancelled"));
        compare(d.lastAnnouncement, "Export cancelled");
        verify(!ctl().fileExists(file));
        compare(walker.problems(d.contentItem), []);
        // Esc once more closes it.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !d.visible);
    }

    // ---- data tabs ---------------------------------------------------------------------------

    function test_dataTabHasTheButtonAndTheTableName() {
        const id = addSqlite(newFile());
        openQuery("CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT); INSERT INTO people(name) VALUES ('Ana'),('Luis')");
        runAll();
        tabs().closeTab(tabs().currentIndex);
        const row = tabs().openData(id, ["people"], "table");
        verify(row >= 0);
        tryVerify(() => shell.editorArea.currentData !== null && shell.editorArea.currentData.visible);
        tryCompare(current(), "state", "done", 20000);
        const view = shell.editorArea.currentData;
        tryVerify(() => view.exportButton.enabled);
        compare(view.exportButton.accessibleName, "Export…"); // icon only: the name is its label
        compare(view.exportButton.Accessible.role, Accessible.Button);
        compare(view.exportButton.Accessible.description, CommandRegistry.find("export.open").shortcutText);
        // The button takes the focus with Tab like the others and opens the dialog with Enter.
        view.exportButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryVerify(() => dlg() !== null && dlg().opened);
        const d = dlg();
        compare(d.scopeCombo.count, 2);
        pick(4);
        compare(d.tableField.text, "people");
        verify(d.pathField.text.endsWith("people.sql"));
        d.pathField.text = outPath("people.sql");
        d.scopeCombo.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        compare(d.scope, "query");
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryCompare(ctl(), "state", "done", 20000);
        const sql = read(outPath("people.sql"));
        verify(sql.includes("INSERT INTO \"people\""), sql);
        verify(sql.includes("'Ana'") && sql.includes("'Luis'"));
        keyClick(Qt.Key_Escape);
        tryVerify(() => !d.visible);
        compare(walker.problems(view), []);
    }

    // ---- zoom and language -------------------------------------------------------------------

    function test_dialogFitsAt200PercentZoom() {
        withResult();
        Theme.zoom = 2.0;
        openExport();
        const d = dlg();
        pick(4); // the tallest form: the SQL options
        tryVerify(() => d.height <= shell.height, 5000, "the dialog is not taller than the window");
        tryVerify(() => d.width <= shell.width);
        verify(d.flick.height > 0);
        // The buttons are always visible, outside the scrolled part.
        const bottom = d.exportButton.mapToItem(shell, 0, d.exportButton.height).y;
        verify(bottom <= shell.height, "Export is inside the window");
        // A control that is out of the visible part scrolls into view when it takes the focus.
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        tryVerify(() => {
            const y = d.pathField.mapToItem(d.flick, 0, 0).y;
            return y >= 0 && y + d.pathField.height <= d.flick.height + 1;
        });
        compare(walker.problems(d.contentItem), []);
        Theme.resetZoom();
    }

    function test_spanishStrings() {
        withResult();
        Language.choice = Language.Choice.Spanish;
        compare(CommandRegistry.find("export.open").title, "Exportar…");
        compare(shell.resultsPanel.exportButton.accessibleName, "Exportar…");
        openExport();
        const d = dlg();
        compare(d.title, "Exportar");
        compare(d.formatList.Accessible.name, "Formato");
        compare(d.pathField.Accessible.name, "Archivo");
        compare(d.scopeCombo.Accessible.name, "Filas a exportar");
        compare(d.exportButton.text, "Exportar");
        compare(d.cancelButton.text, "Cancelar");
        compare(d.browseButton.text, "Examinar…");
        verify(d.scopeCombo.displayText.startsWith("Resultado actual (filas cargadas: "));
        compare(d.delimiterCombo.Accessible.name, "Separador");
        compare(d.headerBox.text, "La primera fila contiene los nombres de columna");
        pick(4);
        compare(d.tableField.Accessible.name, "Nombre de la tabla");
        compare(walker.problems(d.contentItem), []);
        const file = outPath("es.sql");
        d.pathField.text = file;
        d.pathField.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryCompare(ctl(), "state", "done", 20000);
        verify(d.progressLabel.text.startsWith("Exportadas 3 filas ("), d.progressLabel.text);
        compare(d.progressLabel.Accessible.name, "Progreso de la exportación");
        compare(d.closeButton.text, "Cerrar");
        compare(d.folderButton.text, "Mostrar en la carpeta");
        Language.choice = Language.Choice.English;
    }
}
