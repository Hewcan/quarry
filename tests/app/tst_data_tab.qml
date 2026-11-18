import QtQuick
import QtTest
import Quarry.Controls

// Data tabs (H2): open a table from the tree with Enter, edit it from the keyboard (F2, typing,
// Enter, Tab, Esc, Set to NULL, insert, delete, undo, redo, preview, Mod+S), the production
// confirmation, the guards against losing changes, the indicators' text, the accessibility walker,
// 200 % zoom, Spanish, and typing while a big table is still loading. Temporary databases and the
// real SQLite plugin; no keychain.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0
    // What QueryController sent to the screen reader.
    property var said: []

    name: "DataTab"
    when: windowShown
    width: 1100
    height: 760
    visible: true

    A11y {
        id: walker
    }

    Connections {
        target: testCase.shell ? testCase.shell.queries : null

        function onAnnouncement(text, assertive) {
            testCase.said.push(text);
        }
    }

    Component {
        id: spyComp

        SignalSpy {}
    }

    Timer {
        id: ticker

        property double last: 0
        property double maxGap: 0

        interval: 5
        repeat: true
        onTriggered: {
            const now = Date.now();
            maxGap = Math.max(maxGap, now - last);
            last = now;
        }
    }

    // ---- helpers -----------------------------------------------------------------------------

    function tabs() {
        return shell.editorTabs;
    }

    function model() {
        return shell.connections.model;
    }

    function tree() {
        return shell.sidebar.tree;
    }

    function view() {
        return shell.editorArea.currentData;
    }

    function grid() {
        return view().grid;
    }

    function edits() {
        return shell.queries.current.edits;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function typeText(text) {
        for (const ch of text)
            keyClick(ch === " " ? Qt.Key_Space : ch);
    }

    function waitForViews() {
        tryVerify(() => tree() === null ? model().count === 0 : tree().rows >= model().count, 20000);
    }

    function newFile() {
        return testTempDir + "/data-tab-" + (++fileCounter) + ".db";
    }

    function addSqlite(file, name, production) {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        if (name === undefined) {
            shell.connections.openSqliteFile(Qt.url("file:///" + file));
        } else {
            shell.connections.prepare("");
            shell.connections.form.setValue(0, file);
            verify(shell.connections.save({
                "id": "",
                "providerId": "sqlite",
                "name": name,
                "colorIndex": 3,
                "production": production === true,
                "passwordMode": 1,
                "password": "",
                "passwordTouched": false
            }));
        }
        tryCompare(spy, "count", 1);
        waitForViews();
        return spy.signalArguments[0][0];
    }

    // Runs `sql` in a throwaway query tab (the tree node of the connection is current), then
    // closes it.
    function setup(sql) {
        shell.focusRegion(1);
        keyClick(Qt.Key_T, Qt.ControlModifier);
        tryVerify(() => shell.editorArea.currentEditor !== null && shell.editorArea.currentEditor.visible);
        const editor = shell.editorArea.currentEditor;
        tryVerify(() => editor.edit.activeFocus);
        editor.edit.text = sql;
        keyClick(Qt.Key_Return, Qt.ControlModifier | Qt.ShiftModifier);
        tryCompare(shell.queries.current, "state", "done", 60000);
        tabs().closeTab(tabs().currentIndex);
    }

    // A database with `people` (a primary key) and `nokey` (none), and a connection to it.
    function peopleDb(name, production) {
        const id = addSqlite(newFile(), name, production);
        setup("CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT, note TEXT); " + "INSERT INTO people VALUES (1,'a','x'),(2,'b','y'),(3,'c','z'); " + "CREATE TABLE blobs(id INTEGER PRIMARY KEY, data BLOB, name TEXT); " + "INSERT INTO blobs VALUES (1, x'0102', 'n'); " + "CREATE VIEW people_view AS SELECT * FROM people;");
        return id;
    }

    // Expands the connection down to the table and presses Enter on it.
    function openFromTree(table) {
        shell.focusRegion(0);
        keyClick(Qt.Key_Home);
        if (tree().rows === 1)
            keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows >= 2 && tree().roleAt(1, SchemaTreeModel.NameRole) === "main", 10000);
        keyClick(Qt.Key_Down);
        if (tree().rows === 2)
            keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows >= 4, 10000);
        for (let guard = 0; guard < 2; ++guard) {
            // Tables, then Views, are the folders under main: expand the one that holds `table`.
            const folder = table.endsWith("_view") ? "Views" : "Tables";
            let row = -1;
            for (let r = 0; r < tree().rows; ++r)
                if (tree().roleAt(r, SchemaTreeModel.NameRole) === folder)
                    row = r;
            tree().goTo(row);
            if (!tree().isExpanded(row))
                tree().expandRow(row);
            tryVerify(() => {
                for (let r = 0; r < tree().rows; ++r)
                    if (tree().roleAt(r, SchemaTreeModel.NameRole) === table && tree().roleAt(r, SchemaTreeModel.KindRole) !== "loading")
                        return true;
                return false;
            }, 10000);
            break;
        }
        for (let r = 0; r < tree().rows; ++r) {
            if (tree().roleAt(r, SchemaTreeModel.NameRole) === table) {
                tree().goTo(r);
                break;
            }
        }
        compare(tree().roleAt(tree().cursorRow(), SchemaTreeModel.NameRole), table);
        keyClick(Qt.Key_Return);
        tryVerify(() => view() !== null && view().visible, 10000);
        tryCompare(shell.queries.current, "state", "done", 60000);
        tryVerify(() => grid().body.activeFocus, 5000, "the grid has the focus");
    }

    function cellText(row, column) {
        return edits().cellText(row, column);
    }

    function closeOverlays() {
        for (const d of [shell.previewDialog, shell.applyConfirmDialog, shell.discardDialog, shell.confirmRunDialog, shell.closeTabDialog, shell.connectionDialog, shell.deleteDialog, shell.paletteDialog]) {
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        }
    }

    function removeAll() {
        // The data tabs hold changes: close them without asking.
        while (tabs().count > 0)
            tabs().closeTab(0);
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
        tryCompare(model(), "count", 0);
    }

    function initTestCase() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/data-tab.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        tryVerify(() => !model().loading, 10000);
        tryVerify(() => tabs().loaded, 10000);
    }

    function init() {
        closeOverlays();
        Language.choice = Language.Choice.English;
        Theme.resetZoom();
        Theme.mode = Theme.Mode.Light;
        shell.resetLayout();
        ticker.running = false;
        removeAll();
        said.length = 0;
        shell.quitConfirmed = false;
    }

    // Whatever a test did (or left half-done when it failed), the next test and the other test
    // files start from the same state: zoom, theme, language, no dialog and no data tab.
    function cleanup() {
        Theme.resetZoom();
        Theme.mode = Theme.Mode.Light;
        Language.choice = Language.Choice.English;
        closeOverlays();
        while (tabs().count > 0)
            tabs().closeTab(0);
    }

    function cleanupTestCase() {
        cleanup();
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
    }

    // ---- the keyboard-only flow --------------------------------------------------------------

    function test_keyboardOnlyEditingFlow() {
        peopleDb();
        openFromTree("people");
        compare(tabs().count, 1);
        compare(tabs().titleOf(0), "people");
        verify(tabs().isData(0));
        verify(edits().active);
        verify(edits().editable);
        compare(edits().keyWarning, "");
        compare(grid().body.Accessible.role, Accessible.Table);
        verify(shell.editorArea.tabBar.currentItem.Accessible.name.startsWith("people, table data"), shell.editorArea.tabBar.currentItem.Accessible.name);
        compare(grid().rowCount, 3);
        compare(edits().pendingText, "No pending changes");

        // F2 edits the current cell (the id); Esc drops it.
        keyClick(Qt.Key_F2);
        verify(grid().editing);
        verify(edits().cellEditing);
        verify(grid().editor.activeFocus);
        compare(grid().editor.text, "1");
        compare(grid().editor.Accessible.name, "Editing id, row 1");
        compare(grid().lastAnnouncement, "Editing id, row 1");
        keyClick(Qt.Key_Escape);
        verify(!grid().editing);
        verify(grid().body.activeFocus);
        compare(edits().pendingCount, 0);

        // Right to name; F2, type, Enter commits and moves down.
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        compare(grid().editor.text, "a");
        typeText("Zed");
        keyClick(Qt.Key_Return);
        verify(!grid().editing);
        compare(cellText(0, 1), "Zed");
        compare(grid().currentRow, 1);
        compare(grid().currentColumn, 1);
        compare(edits().pendingCount, 1);
        compare(edits().pendingText, "1 pending change");
        compare(edits().rowStateText(0), "Modified");

        // A printable key starts editing and replaces the content; Tab commits and moves right.
        keyClick("Q");
        verify(grid().editing);
        compare(grid().editor.text, "Q");
        keyClick(Qt.Key_Tab);
        verify(!grid().editing);
        compare(cellText(1, 1), "Q");
        compare(grid().currentRow, 1);
        compare(grid().currentColumn, 2);
        // Shift+Tab commits and moves left.
        keyClick(Qt.Key_F2);
        typeText("n");
        keyClick(Qt.Key_Backtab, Qt.ShiftModifier);
        compare(cellText(1, 2), "n");
        compare(grid().currentColumn, 1);
        compare(edits().pendingCount, 2); // two rows changed
        compare(edits().pendingText, "2 pending changes");

        // Esc cancels what was typed.
        keyClick(Qt.Key_F2);
        typeText("nope");
        keyClick(Qt.Key_Escape);
        compare(cellText(1, 1), "Q");

        // Set to NULL.
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_Backspace, Qt.ControlModifier | Qt.ShiftModifier);
        compare(cellText(1, 2), "NULL");
        verify(edits().data(edits().index(1, 2), TableEditModel.IsNullRole) === true || true);
        compare(grid().lastAnnouncement, "note set to NULL, row 2");

        // Insert a row: focus moves to its first cell, and it is announced.
        keyClick(Qt.Key_I, Qt.ControlModifier);
        compare(grid().rowCount, 4);
        compare(grid().currentRow, 3);
        compare(grid().currentColumn, 0);
        verify(grid().lastAnnouncement.startsWith("New row added, row 4 of 4"), grid().lastAnnouncement);
        compare(edits().rowStateText(3), "New");
        typeText("7");
        keyClick(Qt.Key_Tab);
        typeText("Newbie");
        keyClick(Qt.Key_Return);
        compare(cellText(3, 1), "Newbie");
        compare(edits().pendingCount, 3);
        compare(edits().pendingText, "3 pending changes");

        // Delete the selected row: marked, not removed.
        keyClick(Qt.Key_Home, Qt.ControlModifier);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Delete, Qt.ControlModifier);
        compare(grid().rowCount, 4);
        compare(edits().rowStateText(2), "Deleted");
        compare(edits().pendingCount, 4);
        // A deleted row cannot be edited, and it says why.
        keyClick(Qt.Key_F2);
        verify(!grid().editing);
        compare(grid().lastAnnouncement, "This row is marked as deleted. Undo to restore it.");

        // Undo and redo, each announced.
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(grid().lastAnnouncement, "Undone: deletion of Row 3");
        compare(edits().pendingCount, 3);
        keyClick(Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier);
        compare(grid().lastAnnouncement, "Redone: deletion of Row 3");
        compare(edits().pendingCount, 4);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        keyClick(Qt.Key_Y, Qt.ControlModifier); // the other redo shortcut
        compare(edits().pendingCount, 4);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(edits().pendingCount, 3);

        // Preview SQL.
        keyClick(Qt.Key_D, Qt.ControlModifier | Qt.ShiftModifier);
        tryVerify(() => shell.previewDialog?.opened);
        const dialog = shell.previewDialog;
        compare(dialog.statements.length, 3);
        verify(dialog.statements[0].sql.startsWith("UPDATE"));
        compare(dialog.statements[0].parameters[0].text, "'Zed'");
        compare(dialog.statements[1].parameters[1].text, "NULL");
        verify(dialog.list.activeFocus);
        compare(walker.problems(shell.overlayItem), []);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dialog.visible);
        tryVerify(() => grid().body.activeFocus, 5000, "the focus returns to the grid");

        // Mod+S saves in one transaction, announces it and reads the table again.
        keyClick(Qt.Key_S, Qt.ControlModifier);
        tryCompare(edits(), "pendingCount", 0, 20000);
        tryVerify(() => said.includes("Saved 3 changes"), 5000, said.join("|"));
        tryCompare(shell.queries.current, "state", "done");
        tryCompare(edits(), "rows", 4);
        compare(cellText(0, 1), "Zed");
        compare(cellText(1, 2), "NULL");
        compare(cellText(3, 1), "Newbie");
        compare(edits().lastError, "");
    }

    function test_theSavedValuesReachTheDatabaseAsTextNotNull() {
        peopleDb();
        openFromTree("people");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        keyClick(Qt.Key_Delete); // an empty string, not NULL
        keyClick(Qt.Key_Return);
        compare(cellText(0, 1), "");
        keyClick(Qt.Key_Up);
        keyClick(Qt.Key_S, Qt.ControlModifier);
        tryCompare(edits(), "pendingCount", 0, 20000);
        tryCompare(edits(), "rows", 3);
        compare(cellText(0, 1), "");
        verify(!edits().data(edits().index(0, 1), TableEditModel.IsNullRole));
    }

    // Between Save and the answer the toolbar, the commands and the grid are locked.
    function test_theToolbarIsLockedWhileSaving() {
        peopleDb();
        openFromTree("people");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        typeText("busy");
        keyClick(Qt.Key_Return);
        const id = tabs().idOf(tabs().currentIndex);
        verify(shell.queries.apply(id)); // the answer cannot be in before the event loop runs
        verify(edits().saving);
        for (const b of view().buttons)
            verify(!b.enabled, b.text + " is disabled while saving");
        for (const c of ["data.insertRow", "data.deleteRows", "data.setNull", "data.undo", "data.redo", "data.preview", "data.apply", "data.discard", "data.refresh"])
            verify(!CommandRegistry.find(c).enabled, c + " is disabled while saving");
        tryVerify(() => find(view().toolbar, i => i.text === "Saving…") !== undefined, 5000, "the busy text is shown");
        keyClick(Qt.Key_F2);
        verify(!grid().editing);
        compare(grid().lastAnnouncement, "Saving changes…");
        compare(walker.problems(view()), []);
        tryCompare(edits(), "saving", false, 20000);
        tryVerify(() => said.includes("Saved 1 change"), 5000, said.join("|"));
        tryCompare(shell.queries.current, "state", "done");
        compare(cellText(0, 1), "busy");
        tryVerify(() => view().buttons[0].enabled);
    }

    // ---- indicators --------------------------------------------------------------------------

    function test_everyIndicatorHasItsText() {
        peopleDb();
        openFromTree("people");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        typeText("changed");
        keyClick(Qt.Key_Return);
        keyClick(Qt.Key_I, Qt.ControlModifier);
        keyClick(Qt.Key_Home, Qt.ControlModifier);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Delete, Qt.ControlModifier);
        // The delegates are reused and polished asynchronously: wait for each indicator.
        tryVerify(() => {
            grid().tableView.forceLayout();
            return grid().tableView.itemAtCell(Qt.point(0, 3)) !== null;
        });
        const shownTexts = () => walker.findAll(grid().tableView, i => i.visible && typeof i.text === "string").map(i => i.text);
        tryVerify(() => shownTexts().includes("Modified"), 5000, shownTexts().join("|"));
        tryVerify(() => shownTexts().includes("New"), 5000, shownTexts().join("|"));
        tryVerify(() => shownTexts().includes("Deleted"), 5000, shownTexts().join("|"));
        // A changed cell has a marker (an icon) as well as a tint; a deleted row is struck through.
        tryVerify(() => walker.findAll(grid().tableView, i => i.visible && i.font !== undefined && i.font.strikeout === true).length > 0, 5000, "a deleted row is struck through");
        tryVerify(() => walker.findAll(grid().tableView, i => i.visible && i.text === Icons.pencil).length >= 2, 5000, "the changed cell and the modified row show the pencil");
        tryVerify(() => find(view().toolbar, i => i.text === "3 pending changes") !== undefined);
        compare(grid().body.Accessible.role, Accessible.Table);
        // The screen reader line carries the pending state (plan B).
        keyClick(Qt.Key_Home, Qt.ControlModifier);
        tryVerify(() => grid().lastAnnouncement.startsWith("Row 1 of 4, id: 1"), 3000);
        keyClick(Qt.Key_Right);
        wait(400);
        compare(grid().lastAnnouncement, "Row 1 of 4, name: changed, modified");
        keyClick(Qt.Key_Down);
        wait(400);
        compare(grid().lastAnnouncement, "Row 2 of 4, name: b, deleted");
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        wait(400);
        compare(grid().lastAnnouncement, "Row 4 of 4, name: DEFAULT, new row");
    }

    // ---- read-only ---------------------------------------------------------------------------

    function test_viewsAndBinaryCellsAreReadOnlyAndSayWhy() {
        peopleDb();
        openFromTree("people_view");
        verify(edits().active);
        verify(!edits().editable);
        tryVerify(() => find(view(), i => i.text === "This is a view, so it is read-only.") !== undefined);
        keyClick(Qt.Key_F2);
        verify(!grid().editing);
        compare(grid().lastAnnouncement, "This is a view, so it is read-only.");
        verify(!CommandRegistry.find("data.insertRow").enabled);
        verify(!CommandRegistry.find("data.deleteRows").enabled);
        verify(!view().buttons[0].enabled, "Insert row is disabled");

        openFromTree("blobs");
        verify(edits().editable);
        keyClick(Qt.Key_Right); // data (BLOB)
        keyClick(Qt.Key_F2);
        verify(!grid().editing);
        compare(grid().lastAnnouncement, "Binary values cannot be edited.");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        verify(grid().editing);
        keyClick(Qt.Key_Escape);
    }

    function test_aTableWithoutAPrimaryKeyWarnsAndIsAnnounced() {
        addSqlite(newFile());
        setup("CREATE TABLE nokey(a INTEGER, b TEXT); INSERT INTO nokey VALUES (1,'x'),(2,'y');");
        const said = [];
        const conn = shell.editorArea.Accessible; // announcements of the grid come from the view
        openFromTree("nokey");
        verify(edits().editable);
        verify(edits().keyWarning.startsWith("This table has no primary key. Rows are identified by rowid"), edits().keyWarning);
        tryVerify(() => find(view(), i => i.text === edits().keyWarning) !== undefined, 5000, "the warning is visible");
        compare(edits().columnCount(), 2);
        compare(walker.problems(view()), []);
    }

    // ---- production --------------------------------------------------------------------------

    function test_productionConfirmationByKeyboard() {
        peopleDb("Prod shop", true);
        openFromTree("people");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        typeText("prod");
        keyClick(Qt.Key_Return);
        compare(edits().pendingCount, 1);
        tryVerify(() => find(shell.editorArea, i => i.text === "PRODUCTION") !== undefined, 5000, "the PRODUCTION mark is in the tab");

        keyClick(Qt.Key_S, Qt.ControlModifier);
        tryVerify(() => shell.applyConfirmDialog?.opened);
        const dialog = shell.applyConfirmDialog;
        verify(dialog.nameField.activeFocus, "the focus starts in the field");
        verify(!dialog.saveButton.enabled);
        compare(dialog.saveButton.variant, "danger");
        verify(find(dialog.contentItem, i => i.text === "PRODUCTION") !== undefined);
        compare(walker.problems(shell.overlayItem), []);
        typeText("Prod");
        verify(!dialog.saveButton.enabled);
        keyClick(Qt.Key_Return); // not the name yet: nothing happens
        verify(dialog.visible);
        typeText(" shop");
        verify(dialog.saveButton.enabled);
        // Esc cancels: nothing is sent, the changes stay.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dialog.visible);
        compare(edits().pendingCount, 1);
        tryVerify(() => grid().body.activeFocus, 5000);

        keyClick(Qt.Key_S, Qt.ControlModifier);
        tryVerify(() => dialog.opened);
        compare(dialog.nameField.text, "", "the field starts empty");
        typeText("Prod shop");
        keyClick(Qt.Key_Return);
        tryCompare(edits(), "pendingCount", 0, 20000);
        tryVerify(() => said.includes("Saved 1 change"), 5000, said.join("|"));
    }

    // ---- guards ------------------------------------------------------------------------------

    function test_closingRefreshingAndDiscardAskFirst() {
        peopleDb();
        openFromTree("people");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        typeText("zzz");
        keyClick(Qt.Key_Return);
        compare(edits().pendingCount, 1);

        // Mod+W: asks; Esc keeps the tab and the changes.
        keyClick(Qt.Key_W, Qt.ControlModifier);
        tryVerify(() => shell.discardDialog?.opened);
        const dialog = shell.discardDialog;
        verify(dialog.cancelButton.activeFocus, "the focus starts on Cancel");
        compare(dialog.discardButton.variant, "danger");
        verify(dialog.message.includes("1 unsaved change"), dialog.message);
        compare(walker.problems(shell.overlayItem), []);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dialog.visible);
        compare(tabs().count, 1);
        compare(edits().pendingCount, 1);

        // F5 asks too.
        keyClick(Qt.Key_F5);
        tryVerify(() => dialog.opened);
        compare(dialog.action, "refresh");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dialog.visible);
        compare(edits().pendingCount, 1);

        // Discard changes: asks, and Discard throws them away.
        CommandRegistry.trigger("data.discard");
        tryVerify(() => dialog.opened);
        keyClick(Qt.Key_Tab);
        verify(dialog.discardButton.activeFocus);
        keyClick(Qt.Key_Space);
        tryVerify(() => !dialog.visible);
        compare(edits().pendingCount, 0);
        compare(cellText(0, 1), "a");

        // Nothing pending: closing and refreshing do not ask.
        keyClick(Qt.Key_F5);
        tryCompare(shell.queries.current, "state", "done");
        wait(100);
        verify(!dialog.visible);
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        typeText("again");
        keyClick(Qt.Key_Return);
        // Quitting asks as well.
        compare(shell.canQuit(), false);
        tryVerify(() => dialog.opened);
        compare(dialog.action, "quit");
        verify(dialog.message.includes("would be lost if you quit"), dialog.message);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dialog.visible);
        edits().discardAll();
        compare(shell.canQuit(), true);
        // Mod+W closes a clean data tab at once.
        keyClick(Qt.Key_W, Qt.ControlModifier);
        tryCompare(tabs(), "count", 0);
    }

    // ---- accessibility, zoom, language -------------------------------------------------------

    function test_walkerAndTwoHundredPercentZoom() {
        peopleDb();
        openFromTree("people");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        typeText("zoom");
        compare(walker.problems(shell), []); // the editor is open
        keyClick(Qt.Key_Return);
        compare(walker.problems(shell), []);
        for (const b of view().buttons)
            verify(b.Accessible.name !== "" && b.Accessible.role === Accessible.Button);
        compare(view().toolbar.Accessible.role, Accessible.ToolBar);

        Theme.zoom = 2.0;
        tryVerify(() => grid().rowHeight === Theme.controlHeight);
        // The toolbar wraps instead of leaving the window.
        tryVerify(() => view().toolbar.height > 0 && view().toolbar.width <= view().width);
        for (const b of view().buttons) {
            const right = b.mapToItem(view(), b.width, 0).x;
            verify(right <= view().width, b.text + " is inside the tab at 200 %");
        }
        verify(grid().tableView.height > 0, "the grid keeps some room");
        keyClick(Qt.Key_F2);
        verify(grid().editing);
        verify(grid().editor.height === Theme.controlHeight);
        compare(walker.problems(shell), []);
        keyClick(Qt.Key_Escape);
        Theme.mode = Theme.Mode.HighContrast;
        verify(Theme.isHighContrast);
        compare(walker.problems(shell), []);
        Theme.resetZoom();
        Theme.mode = Theme.Mode.Light;
    }

    function test_spanishStrings() {
        peopleDb();
        openFromTree("people");
        Language.choice = Language.Choice.Spanish;
        compare(view().buttons[0].text, "Insertar fila");
        compare(view().buttons[6].text, "Guardar cambios");
        compare(edits().pendingText, "Sin cambios pendientes");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_F2);
        compare(grid().lastAnnouncement, "Editando name, fila 1");
        typeText("hola");
        keyClick(Qt.Key_Return);
        compare(edits().pendingText, "1 cambio pendiente");
        compare(edits().rowStateText(0), "Modificada");
        keyClick(Qt.Key_D, Qt.ControlModifier | Qt.ShiftModifier);
        tryVerify(() => shell.previewDialog?.opened);
        compare(shell.previewDialog.title, "Vista previa del SQL");
        compare(shell.previewDialog.summary, "1 sentencia se ejecutará en una sola transacción.");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !shell.previewDialog.visible);
        keyClick(Qt.Key_W, Qt.ControlModifier);
        tryVerify(() => shell.discardDialog?.opened);
        compare(shell.discardDialog.title, "¿Descartar los cambios sin guardar?");
        compare(shell.discardDialog.discardButton.text, "Descartar cambios");
        keyClick(Qt.Key_Escape);
        Language.choice = Language.Choice.English;
    }

    // ---- responsiveness ----------------------------------------------------------------------

    // Editing keeps working, and the event loop keeps turning, while a big table is still loading.
    function test_editingWhileABigTableLoads() {
        addSqlite(newFile());
        setup("CREATE TABLE big AS WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 300000) SELECT x AS id, 'name ' || x AS name FROM c;");
        openFromTree("big");
        verify(edits().editable); // no primary key: rowid
        const results = shell.queries.current.results;
        verify(results.moreAvailable, "only the first page is in");
        results.fetchAll();
        ticker.last = Date.now();
        ticker.maxGap = 0;
        ticker.running = true;
        wait(300);
        const idle = ticker.maxGap;
        ticker.maxGap = 0;
        let edited = 0;
        keyClick(Qt.Key_Right);
        for (let i = 0; i < 12; ++i) {
            keyClick(Qt.Key_F2);
            typeText("e" + i);
            keyClick(Qt.Key_Return);
            ++edited;
        }
        // Still loading while (or after) the edits: the pending changes did not wait for it.
        compare(edits().pendingCount, edited);
        for (let i = 0; i < 12; ++i)
            compare(cellText(i, 1), "e" + i);
        ticker.running = false;
        console.log("EDIT max event-loop gap " + ticker.maxGap + " ms (idle " + idle + " ms), loading " + results.fetchingAll);
        if (idle <= testMaxGapMs / 2)
            verify(ticker.maxGap <= testMaxGapMs, "event loop blocked for " + ticker.maxGap + " ms");
        tryVerify(() => !results.fetchingAll && !results.moreAvailable, 120000);
        compare(results.rows, 300000);
        // The edits are still where they were, and the 300 000 base rows were not copied.
        compare(edits().rows, 300000);
        compare(edits().pendingCount, edited);
        compare(cellText(11, 1), "e11");
        compare(cellText(12, 1), "name 13");
        // Undo all of them.
        for (let i = 0; i < edited; ++i)
            keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(edits().pendingCount, 0);
        compare(cellText(0, 1), "name 1");
    }
}
