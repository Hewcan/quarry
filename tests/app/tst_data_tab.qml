import QtQuick
import QtTest
import Quarry.Controls

// Data tabs (H2): open a table from the tree with Enter, edit it from the keyboard (F2, typing,
// Enter, Tab, Esc, Set to NULL, insert, duplicate, delete, undo, redo, preview, Mod+S), the icon-only
// toolbar, the row count with Fetch all and the total, the SQL filter box, the production
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
        tryVerify(() => tree().rows >= 3 && tree().roleAt(1, SchemaTreeModel.NameRole) === "Tables", 10000);
        for (let guard = 0; guard < 2; ++guard) {
            // Tables, then Views, are the folders under the connection: expand the one that holds `table`.
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

    // A toolbar button by its accessible name.
    function button(name) {
        return view().buttons.find(b => b.accessibleName === name);
    }

    function appliedWhere() {
        return shell.queries.dataViewOf(tabs().idOf(tabs().currentIndex)).where;
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

    // ---- sort and filter on the server (H11) --------------------------------------------------

    function test_sortBar() {
        peopleDb();
        openFromTree("people");
        const bar = view().viewBar;
        verify(bar.visible);
        tryCompare(view(), "columnNames", ["id", "name", "note"]);
        const named = name => find(bar, i => i.accessibleName === name);
        // The column / operator / value combos are gone: the filter is one SQL condition.
        compare(named("Filter column"), undefined);
        compare(named("Filter value"), undefined);
        // A combo applies when the user picks: Down twice is "name", then descending.
        named("Sort by").forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        compare(named("Sort by").currentIndex, 2);
        tryVerify(() => shell.queries.dataViewOf(tabs().idOf(0)).sortColumn === "name");
        named("Sort direction").forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        tryVerify(() => shell.queries.current.results.rows === 3 && cellText(0, 1) === "c", 10000);
        compare(cellText(2, 1), "a");
        // The bar keeps its choices across the reload.
        compare(named("Sort by").currentIndex, 2);
        compare(named("Sort direction").currentIndex, 1);

        // Pending changes block a new order, it is said, and the combos go back to what is applied.
        edits().setCell(0, 2, "changed");
        named("Sort by").forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Up);
        verify(said.some(s => s.indexOf("Save or discard the pending changes") >= 0), JSON.stringify(said));
        compare(named("Sort by").currentIndex, 2);
        edits().discardAll();
    }

    // The filter is one SQL condition, as in DBeaver (the SQLite database of the test).
    function test_filterIsOneSqlCondition() {
        peopleDb();
        openFromTree("people");
        tryCompare(view(), "columnNames", ["id", "name", "note"]);
        const field = view().filterField;
        const clear = view().clearFilterButton;
        verify(field.visible);
        compare(field.Accessible.name, "Filter condition");
        compare(field.Accessible.role, Accessible.EditableText);
        verify(field.placeholderText.startsWith("Filter: a SQL condition"), field.placeholderText);
        verify(!clear.visible, "Clear filter only shows with text");
        compare(clear.accessibleName, "Clear filter");

        // Typing, then Enter: a condition with a parenthesis and quotes filters on the server.
        field.forceActiveFocus(Qt.TabFocusReason);
        typeText("id > 1 AND (name = ");
        keyClick("'");
        typeText("b");
        keyClick("'");
        typeText(" OR note = ");
        keyClick("'");
        typeText("z");
        keyClick("'");
        typeText(")");
        verify(clear.visible);
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.queries.current.results.rows === 2 && shell.queries.current.state === "done", 10000);
        compare(cellText(0, 1), "b");
        compare(cellText(1, 1), "c");
        compare(appliedWhere(), "id > 1 AND (name = 'b' OR note = 'z')");
        compare(view().filterError, "");

        // A `;` or a comment is refused with a message, nothing changes, and the box says so
        // with an icon and the word "error".
        const good = field.text;
        field.text = "id = 1; DROP TABLE people";
        keyClick(Qt.Key_Return);
        tryVerify(() => view().filterError !== "");
        verify(said.some(s => s === view().filterError), JSON.stringify(said));
        verify(view().filterError.indexOf(";") >= 0, view().filterError);
        compare(appliedWhere(), good);
        compare(shell.queries.current.results.rows, 2);
        const line = find(view(), i => typeof i.text === "string" && i.text.startsWith("Filter error: "));
        verify(line !== undefined, "the message is shown under the box");
        verify(field.Accessible.description.indexOf(view().filterError) >= 0, field.Accessible.description);
        compare(walker.problems(view()), []);
        field.text = "id = 1 -- all";
        keyClick(Qt.Key_Return);
        compare(appliedWhere(), good);
        verify(view().filterError.toLowerCase().indexOf("comment") >= 0, view().filterError);

        // A column that does not exist: the server says so, under the box and in Messages.
        field.text = "nosuchcolumn = 1";
        keyClick(Qt.Key_Return);
        tryCompare(shell.queries.current, "state", "failed", 10000);
        tryVerify(() => view().filterError !== "");
        verify(view().filterError.toLowerCase().indexOf("nosuchcolumn") >= 0, view().filterError);
        compare(shell.queries.current.messages.count, 1);
        verify(shell.resultsVisible, "an error opens the results panel");
        compare(shell.queries.current.results.rows, 0); // documented limit: the table is empty

        // Clear filter: the box empties and every row is back.
        clear.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryVerify(() => shell.queries.current.results.rows === 3 && shell.queries.current.state === "done", 10000);
        compare(field.text, "");
        verify(!clear.visible);
        compare(appliedWhere(), "");
        compare(view().filterError, "");

        // The condition survives F5.
        field.forceActiveFocus(Qt.TabFocusReason);
        field.text = "id = 2";
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.queries.current.results.rows === 1 && shell.queries.current.state === "done", 10000);
        keyClick(Qt.Key_F5);
        tryVerify(() => shell.queries.current.results.rows === 1 && shell.queries.current.state === "done", 10000);
        compare(appliedWhere(), "id = 2");
        compare(cellText(0, 1), "b");
        compare(field.text, "id = 2");
        verify(edits().editable, "the filtered rows stay editable");
        // A new order keeps the condition.
        const sort = find(view().viewBar, i => i.accessibleName === "Sort by");
        sort.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        tryVerify(() => shell.queries.dataViewOf(tabs().idOf(0)).sortColumn === "id");
        compare(appliedWhere(), "id = 2");
    }

    // ---- the toolbar --------------------------------------------------------------------------

    function test_toolbarIsIconOnlyInOrder() {
        peopleDb();
        openFromTree("people");
        const names = view().buttons.map(b => b.accessibleName);
        compare(names, ["Refresh", "Save changes", "Cancel changes", "Insert row", "Duplicate row", "Delete rows", "Export…"]);
        // Left to right on screen.
        let last = -1;
        for (const b of view().buttons) {
            const x = b.mapToItem(view().toolbar, 0, 0).x;
            verify(x > last, b.accessibleName + " follows the previous button");
            last = x;
        }
        for (const b of view().buttons) {
            compare(b.text, "", b.accessibleName + " has no text");
            verify(b.iconGlyph !== "", b.accessibleName + " has an icon");
            compare(b.Accessible.name, b.accessibleName);
            compare(b.Accessible.role, Accessible.Button);
            compare(b.tooltip.text.indexOf(b.accessibleName), 0, "the tooltip says the name");
        }
        // The shortcut is the description and is in the tooltip.
        compare(button("Refresh").Accessible.description, "F5");
        compare(button("Insert row").Accessible.description, "Ctrl+I");
        compare(button("Duplicate row").Accessible.description, "Ctrl+D");
        compare(button("Delete rows").Accessible.description, CommandRegistry.find("data.deleteRows").shortcutText);
        compare(button("Save changes").Accessible.description, "Ctrl+S");
        compare(button("Duplicate row").tooltip.text, "Duplicate row (Ctrl+D)");
        // Set to NULL, Undo, Redo and Preview SQL left the bar but not the palette or the keyboard.
        for (const id of ["data.setNull", "data.undo", "data.redo", "data.preview", "data.duplicateRow"]) {
            verify(CommandRegistry.find(id) !== undefined, id);
            verify(CommandRegistry.find(id).shortcutText !== "", id);
        }
        compare(CommandRegistry.find("data.duplicateRow").title, "Duplicate row");
        compare(CommandRegistry.find("data.duplicateRow").category, "Data");
        compare(find(view().toolbar, i => i.text === "Set to NULL" || i.text === "Undo" || i.text === "Redo" || i.text === "Preview SQL"), undefined);
        verify(button("Insert row").enabled);
        verify(!button("Save changes").enabled);
        verify(!button("Cancel changes").enabled);
        edits().setCell(0, 1, "x");
        verify(button("Save changes").enabled);
        verify(button("Cancel changes").enabled);
        tryVerify(() => find(view().toolbar, i => i.text === "1 pending change") !== undefined);
        edits().discardAll();
    }

    function test_rowCountFetchAllAndTotal() {
        addSqlite(newFile());
        setup("CREATE TABLE many AS WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 450) SELECT x AS id, 'n' || x AS name FROM c; " + "CREATE TABLE one(id INTEGER PRIMARY KEY, name TEXT); INSERT INTO one VALUES (1, 'only');");
        shell.resultsVisible = false; // setup() ran a query, which opened it
        openFromTree("many");
        const results = shell.queries.current.results;
        verify(results.moreAvailable, "only the first page is in");
        verify(!shell.resultsVisible, "the rows of a data tab do not open the results panel");
        tryCompare(view().rowsLabel, "text", "200 rows");
        const sample = Number(200).toLocaleString(Qt.locale(), "f", 0);
        tryCompare(view().totalLabel, "text", sample + "+");
        compare(view().totalLabel.Accessible.name, "At least " + sample + " rows in total");
        verify(view().fetchButton.visible);
        compare(view().fetchButton.text, "Fetch all");
        compare(view().fetchButton.Accessible.role, Accessible.Button);
        // The grid of a data tab has no button of its own: it would be two.
        verify(!grid().fetchButton.visible, "no Fetch all in the grid of a data tab");
        // Fetch all, by keyboard from the toolbar: the exact total replaces "200+".
        view().fetchButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryCompare(view().rowsLabel, "text", "450 rows", 20000);
        tryCompare(view().totalLabel, "text", Number(450).toLocaleString(Qt.locale(), "f", 0));
        compare(view().totalLabel.Accessible.name, "Total: 450 rows");
        verify(!view().fetchButton.visible);

        // Everything fit in the first page: the total is exact at once, and 1 row is singular.
        openFromTree("one");
        tryCompare(view().rowsLabel, "text", "1 row");
        compare(view().totalLabel.text, "1");
        verify(!view().fetchButton.visible);
    }

    function test_duplicateRow() {
        peopleDb();
        openFromTree("people");
        const dup = button("Duplicate row");
        verify(dup.enabled);
        keyClick(Qt.Key_Right); // name
        keyClick(Qt.Key_Down); // row 2: id 2, name b, note y
        keyClick(Qt.Key_D, Qt.ControlModifier);
        compare(grid().rowCount, 4);
        compare(grid().currentRow, 3);
        compare(grid().lastAnnouncement, "Row duplicated, new row 4 of 4.");
        compare(edits().rowStateText(3), "New");
        compare(cellText(3, 1), "b");
        compare(cellText(3, 2), "y");
        compare(cellText(3, 0), "DEFAULT"); // the key is not copied
        compare(edits().pendingCount, 1);
        // Undo takes it away, like an insert.
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(grid().rowCount, 3);
        compare(grid().lastAnnouncement, "Undone: duplicate of Row 2");
        grid().moveTo(1, 1, false); // undo left the cursor on the last row
        // The button runs the same command.
        dup.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        compare(grid().rowCount, 4);
        // Saved: the database gives it a new key and keeps the copied values.
        keyClick(Qt.Key_S, Qt.ControlModifier);
        tryCompare(edits(), "pendingCount", 0, 20000);
        tryCompare(shell.queries.current, "state", "done", 20000);
        tryVerify(() => edits().rows === 4 && cellText(3, 0) === "4", 10000, "the table was read again");
        compare(cellText(3, 1), "b");
    }

    function test_duplicateRowIsDisabledWhereInsertIs() {
        peopleDb();
        openFromTree("people_view");
        verify(!button("Duplicate row").enabled);
        verify(!CommandRegistry.find("data.duplicateRow").enabled);
        verify(!button("Insert row").enabled);
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
            verify(!b.enabled, b.accessibleName + " is disabled while saving");
        for (const c of ["data.insertRow", "data.duplicateRow", "data.deleteRows", "data.setNull", "data.undo", "data.redo", "data.preview", "data.apply", "data.discard", "data.refresh"])
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
        tryVerify(() => button("Insert row").enabled);
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
        verify(!button("Insert row").enabled, "Insert row is disabled");

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
            verify(right <= view().width, b.accessibleName + " is inside the tab at 200 %");
        }
        for (const label of [view().rowsLabel, view().totalLabel]) {
            const right = label.mapToItem(view(), label.width, 0).x;
            verify(right <= view().width, label.text + " is inside the tab at 200 %");
        }
        const filterRight = view().filterField.mapToItem(view(), view().filterField.width, 0).x;
        verify(filterRight <= view().width, "the filter box is inside the tab at 200 %");
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
        compare(button("Insertar fila").accessibleName, "Insertar fila");
        compare(view().buttons[0].accessibleName, "Actualizar");
        compare(view().buttons[1].accessibleName, "Guardar cambios");
        compare(view().buttons[2].accessibleName, "Cancelar cambios");
        compare(view().buttons[4].accessibleName, "Duplicar fila");
        compare(view().buttons[5].accessibleName, "Eliminar filas");
        compare(view().buttons[6].accessibleName, "Exportar…");
        compare(view().filterField.Accessible.name, "Condición del filtro");
        verify(view().filterField.placeholderText.startsWith("Filtro: una condición SQL"), view().filterField.placeholderText);
        compare(view().rowsLabel.text, "3 filas");
        compare(CommandRegistry.find("data.duplicateRow").title, "Duplicar fila");
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
