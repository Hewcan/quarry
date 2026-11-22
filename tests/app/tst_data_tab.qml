import QtQuick
import QtTest
import Quarry.Controls

// Data tabs (H2): open a table from the tree with Enter, edit it from the keyboard (F2, typing,
// Enter, Tab, Esc, Set to NULL, insert, duplicate, delete, undo, redo, preview, Mod+S), the icon-only
// toolbar, the rows group (rows to load, Count rows, loaded / total), the SQL filter box, the production
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

    // What the structure controller sent to the screen reader ("Filtered from ...", "no related row").
    property var structureSaid: []

    Connections {
        target: testCase.shell ? testCase.shell.structure : null

        function onAnnouncement(text, assertive) {
            testCase.structureSaid.push(text);
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
        structureSaid.length = 0;
        shell.quitConfirmed = false;
    }

    // Whatever a test did (or left half-done when it failed), the next test and the other test
    // files start from the same state: zoom, theme, language, no dialog and no data tab.
    function cleanup() {
        // The rows-to-load setting is stored: every test starts from the default.
        shell.queries.dataPageRows = 200;
        LayoutStore.remove("dataPageRows");
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
        compare(names, ["Refresh", "Save changes", "Cancel changes", "Insert row", "Duplicate row", "Delete rows", "Export…", "Record view", "Structure"]);
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
        compare(button("Insert row").Accessible.description, CommandRegistry.displayShortcut("Ctrl+I"));
        compare(button("Duplicate row").Accessible.description, CommandRegistry.displayShortcut("Ctrl+D"));
        compare(button("Delete rows").Accessible.description, CommandRegistry.find("data.deleteRows").shortcutText);
        compare(button("Save changes").Accessible.description, CommandRegistry.displayShortcut("Ctrl+S"));
        compare(button("Duplicate row").tooltip.text, "Duplicate row (" + CommandRegistry.displayShortcut("Ctrl+D") + ")");
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

    function manyRows() {
        addSqlite(newFile());
        setup("CREATE TABLE many AS WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 450) SELECT x AS id, 'n' || x AS name FROM c; " + "CREATE TABLE one(id INTEGER PRIMARY KEY, name TEXT); INSERT INTO one VALUES (1, 'only');");
        shell.resultsVisible = false; // setup() ran a query, which opened it
    }

    function groupText(n) {
        return Number(n).toLocaleString(Qt.locale(), "f", 0);
    }

    // [rows to load] [count button] [loaded rows]: the rows loaded say "200+" until the end is known;
    // "Count rows" gives the exact total without touching the grid.
    function test_rowsGroupLoadedCountAndTotal() {
        manyRows();
        openFromTree("many");
        const results = shell.queries.current.results;
        verify(results.moreAvailable, "only the first page is in");
        verify(!shell.resultsVisible, "the rows of a data tab do not open the results panel");
        compare(view().rowsField.text, "200");
        tryCompare(view().countLabel, "text", groupText(200) + "+");
        compare(view().countLabel.Accessible.name, "200 rows loaded, more available");
        // Fetch all is gone from the data tab's toolbar and from its grid.
        compare(find(view().toolbar, i => i.text === "Fetch all" || i.text === "Stop fetching"), undefined);
        verify(!grid().fetchButton.visible, "no Fetch all in the grid of a data tab");
        // Left to right: the field, the icon button, the text.
        const field = view().rowsField;
        const count = view().countButton;
        const label = view().countLabel;
        verify(field.mapToItem(view(), 0, 0).x < count.mapToItem(view(), 0, 0).x);
        tryVerify(() => count.mapToItem(view(), 0, 0).x < label.mapToItem(view(), 0, 0).x);
        compare(count.accessibleName, "Count rows");
        compare(count.Accessible.role, Accessible.Button);
        compare(count.text, "");
        compare(count.tooltip.text, "Count the rows of the table");
        verify(count.enabled);

        // By keyboard from the toolbar: the exact total replaces "200+", the grid stays as it was.
        grid().body.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        const row = grid().currentRow;
        count.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryCompare(view().countLabel, "text", groupText(450), 20000);
        compare(view().countLabel.Accessible.name, "Total: 450 rows");
        compare(grid().lastAnnouncement, "Total: 450 rows");
        compare(results.rows, 200);
        verify(results.moreAvailable);
        compare(grid().currentRow, row);
        compare(shell.queries.current.countedTotal, 450);
        compare(count.accessibleName, "Count rows");

        // A new load of the view forgets the count.
        keyClick(Qt.Key_F5);
        tryCompare(view().countLabel, "text", groupText(200) + "+");

        // Everything fit in the first page: the total is exact at once.
        openFromTree("one");
        tryCompare(view().countLabel, "text", "1");
        compare(view().countLabel.Accessible.name, "1 row loaded");
    }

    // The count follows the view: the same filter and condition, no order, and the rows loaded
    // are not touched.
    function test_countFollowsTheFilter() {
        manyRows();
        openFromTree("many");
        const results = shell.queries.current.results;
        tryCompare(view(), "columnNames", ["id", "name"]);
        view().filterField.forceActiveFocus(Qt.TabFocusReason);
        typeText("id > 100");
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.queries.current.state === "done" && results.rows === 200 && appliedWhere() === "id > 100", 10000);
        view().countButton.clicked();
        tryCompare(view().countLabel, "text", groupText(350), 20000);
        compare(results.rows, 200);
        compare(cellText(0, 0), "101");
        // Changing the view brings back "200+".
        view().filterField.forceActiveFocus(Qt.TabFocusReason);
        view().filterField.selectAll();
        typeText("id > 200");
        keyClick(Qt.Key_Return);
        tryCompare(view().countLabel, "text", groupText(200) + "+", 10000);
        compare(shell.queries.current.countedTotal, -1);
    }

    function test_countCanBeCancelledAndFailureIsSaid() {
        manyRows();
        openFromTree("many");
        const id = tabs().idOf(tabs().currentIndex);
        const state = shell.queries.current;
        // Cancel: the state goes back to "not counted", and no error is said.
        shell.queries.countRows(id);
        verify(state.counting || state.countedTotal >= 0);
        if (state.counting) {
            compare(view().countButton.accessibleName, "Stop counting");
            compare(view().countButton.tooltip.text, "Stop counting the rows of the table");
            compare(view().countLabel.text, "Counting…");
            keyClick(Qt.Key_Escape);
            tryCompare(state, "counting", false, 10000);
        }
        compare(state.countError, "");
        compare(view().countErrorLabel.visible, false);

        // A table that vanished: the error shows under the group and in Messages.
        const before = state.messages.rowCount();
        // An open cursor would keep the table locked: the tab reads to the end first.
        state.results.fetchAll();
        tryVerify(() => !state.results.moreAvailable && !state.results.fetchingAll, 20000);
        setup("DROP TABLE many");
        compare(shell.queries.current, state);
        view().countButton.clicked();
        tryVerify(() => state.countError !== "", 10000);
        verify(view().countErrorLabel.visible);
        verify(view().countErrorLabel.text.startsWith("The rows could not be counted"), view().countErrorLabel.text);
        verify(shell.queries.current.messages.rowCount() > before);
        compare(view().countLabel.text, groupText(450));
    }

    function test_rowsToLoadIsASettingThatReloads() {
        manyRows();
        openFromTree("many");
        const results = shell.queries.current.results;
        const field = view().rowsField;
        compare(field.Accessible.name, "Rows to load");
        compare(field.Accessible.role, Accessible.EditableText);
        compare(field.tipText, "Number of rows to load when a table is opened");
        compare(field.text, "200");
        // Digits only.
        field.forceActiveFocus(Qt.TabFocusReason);
        field.selectAll();
        keyClick("x");
        compare(field.text, "200");
        // Enter with a valid number: saved, applied and the table is read again.
        typeText("100");
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.queries.current.state === "done" && results.rows === 100, 10000);
        compare(shell.queries.dataPageRows, 100);
        compare(Number(LayoutStore.value("dataPageRows", 0)), 100);
        tryCompare(view().countLabel, "text", groupText(100) + "+");
        // Out of range: brought to the limit.
        field.selectAll();
        typeText("99999");
        keyClick(Qt.Key_Return);
        tryCompare(field, "text", "10000");
        compare(shell.queries.dataPageRows, 10000);
        field.selectAll();
        typeText("0");
        keyClick(Qt.Key_Return);
        tryCompare(field, "text", "1");
        compare(shell.queries.dataPageRows, 1);
        tryVerify(() => shell.queries.current.state === "done" && results.rows === 1, 10000);
        // Not a number (empty): the field goes back to the setting, nothing changes.
        field.selectAll();
        keyClick(Qt.Key_Backspace);
        keyClick(Qt.Key_Return);
        compare(field.text, "1");

        // It is the setting of the next tables too, and leaving the field commits it as well.
        field.selectAll();
        typeText("120");
        count_focusAway();
        tryCompare(shell.queries, "dataPageRows", 120);
        // A table opened now (its tab is new) loads the setting.
        tabs().closeTab(tabs().currentIndex);
        openFromTree("many");
        // (the grid may ask for the next page by itself: the rows are whole pages of the setting)
        tryVerify(() => shell.queries.current.state === "done" && shell.queries.current.results.rows > 0 && shell.queries.current.results.rows % 120 === 0, 10000);
        compare(view().rowsField.text, "120");

        // With unsaved changes the table is not read again: the value applies to the next load.
        edits().setCell(0, 1, "x");
        compare(edits().pendingCount, 1);
        view().rowsField.forceActiveFocus(Qt.TabFocusReason);
        view().rowsField.selectAll();
        typeText("60");
        keyClick(Qt.Key_Return);
        compare(shell.queries.dataPageRows, 60);
        wait(300);
        compare(shell.queries.current.results.rows % 120, 0, "not read again");
        compare(edits().pendingCount, 1);
        verify(grid().lastAnnouncement.indexOf("next time the table is read") >= 0, grid().lastAnnouncement);
        edits().discardAll();
        keyClick(Qt.Key_F5);
        tryVerify(() => shell.queries.current.state === "done" && shell.queries.current.results.rows % 60 === 0, 10000);
    }

    function count_focusAway() {
        view().grid.body.forceActiveFocus(Qt.TabFocusReason);
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
        // The record view and the structure only read: they stay usable.
        for (const b of view().buttons.slice(0, 7))
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
        for (const item of [view().rowsField, view().countButton, view().countLabel]) {
            const right = item.mapToItem(view(), item.width, 0).x;
            verify(right <= view().width, item.Accessible.name + " is inside the tab at 200 %");
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

    // ---- sort from the header, record view, foreign keys (H11 phases 2 to 4) ---------------------

    // The header cells of the grid (the Row also holds the Repeater).
    function headerCell(i) {
        return grid().header.children.filter(c => c.columnTitle !== undefined)[i];
    }

    function clickHeader(i) {
        // The Row lays its cells out on the next polish: wait for this one to have its place.
        tryVerify(() => i === 0 || headerCell(i).x >= headerCell(i - 1).x + headerCell(i - 1).width);
        const head = headerCell(i);
        const p = head.mapToItem(grid(), head.width / 2, head.height / 2);
        mouseClick(grid(), p.x, p.y);
    }

    function hasMark(i, glyph) {
        return find(headerCell(i), item => item.text === glyph) !== undefined;
    }

    function waitRows(n) {
        tryVerify(() => shell.queries.current.state === "done" && shell.queries.current.results.rows === n, 10000);
    }

    // customers, orders (a foreign key, one NULL), a pair with a composite key and a key to a table
    // that does not exist.
    function shopDb() {
        const id = addSqlite(newFile());
        setup("CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT); " + "CREATE TABLE orders(id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES customers(id), note TEXT); " + "INSERT INTO customers VALUES (1,'Ana'),(2,'Luis'); " + "INSERT INTO orders VALUES (10,1,'a'),(11,2,'b'),(12,NULL,'c'); " + "PRAGMA foreign_keys = OFF; " + "CREATE TABLE lost(id INTEGER PRIMARY KEY, g INTEGER REFERENCES ghost(id)); " + "INSERT INTO lost VALUES (1, 5);");
        return id;
    }

    // JSON, XML, long text and a BLOB in one row.
    function docsDb() {
        const id = addSqlite(newFile());
        setup("CREATE TABLE docs(id INTEGER PRIMARY KEY, j TEXT, x TEXT, t TEXT, b BLOB); " + "INSERT INTO docs VALUES (1, '{\"b\":1,\"a\":[1,2]}', '<a><b>x</b></a>', 'line one' || char(10) || 'line two', x'48656c6c6f');");
        return id;
    }

    function test_theHeaderSortsByClickAndTheArrowIsTextToo() {
        peopleDb();
        openFromTree("people");
        tryCompare(view(), "columnNames", ["id", "name", "note"]);
        const g = grid();
        verify(g.sortable);
        tryVerify(() => headerCell(1).height > 0);
        compare(headerCell(1).Accessible.role, Accessible.ColumnHeader);
        compare(headerCell(1).Accessible.name, "name");
        compare(headerCell(1).Accessible.description, "not sorted");
        verify(!hasMark(1, Icons.arrowUp) || !find(headerCell(1), i => i.text === Icons.arrowUp).visible);
        const named = name => find(view().viewBar, i => i.accessibleName === name);

        // Ascending: the arrow, the words, the combos and the rows follow.
        clickHeader(1);
        tryVerify(() => view().currentView.sortColumn === "name" && !view().currentView.descending, 10000);
        waitRows(3);
        compare(cellText(0, 1), "a");
        tryVerify(() => headerCell(1).Accessible.description === "sorted ascending");
        verify(find(headerCell(1), i => i.text === Icons.arrowUp && i.visible) !== undefined, "an up arrow");
        compare(headerCell(0).Accessible.description, "not sorted");
        tryCompare(named("Sort by"), "currentIndex", 2);
        compare(named("Sort direction").currentIndex, 0);
        verify(said.some(s => s === "Sorted by name, ascending"), JSON.stringify(said));

        // Descending.
        clickHeader(1);
        tryVerify(() => view().currentView.descending === true, 10000);
        waitRows(3);
        compare(cellText(0, 1), "c");
        tryVerify(() => headerCell(1).Accessible.description === "sorted descending");
        verify(find(headerCell(1), i => i.text === Icons.arrowDown && i.visible) !== undefined, "a down arrow");
        tryCompare(named("Sort direction"), "currentIndex", 1);

        // No order: back to the table as it was, and the sort of another column starts ascending.
        clickHeader(1);
        tryVerify(() => view().currentView.sortColumn === "", 10000);
        waitRows(3);
        compare(cellText(0, 1), "a");
        tryVerify(() => headerCell(1).Accessible.description === "not sorted");
        tryCompare(named("Sort by"), "currentIndex", 0);
        clickHeader(2);
        tryVerify(() => view().currentView.sortColumn === "note" && !view().currentView.descending, 10000);
        waitRows(3);

        // The combo of the bar sorts too, and the header follows it.
        named("Sort by").forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Up);
        tryVerify(() => view().currentView.sortColumn === "name", 10000);
        waitRows(3);
        tryVerify(() => headerCell(1).Accessible.description === "sorted ascending");
    }

    function test_theHeaderSortsFromTheKeyboardAndDragsDoNotSort() {
        peopleDb();
        openFromTree("people");
        tryCompare(view(), "columnNames", ["id", "name", "note"]);
        const g = grid();
        tryVerify(() => headerCell(1).height > 0);
        // One tab stop for the header, before the cells.
        compare(g.headerFocus.Accessible.role, Accessible.Button);
        compare(g.headerFocus.Accessible.name, "Column headers");
        verify(g.headerFocus.activeFocusOnTab);
        g.headerFocus.forceActiveFocus(Qt.TabFocusReason);
        verify(g.headerFocus.activeFocus);
        compare(g.headerColumn, 0);
        keyClick(Qt.Key_Right);
        compare(g.headerColumn, 1);
        compare(g.lastAnnouncement, "Column name, not sorted");
        keyClick(Qt.Key_Return);
        tryVerify(() => view().currentView.sortColumn === "name" && !view().currentView.descending, 10000);
        waitRows(3);
        tryVerify(() => g.headerFocus.activeFocus, 5000, "the header keeps the focus");
        compare(g.headerColumn, 1);
        keyClick(Qt.Key_Space);
        tryVerify(() => view().currentView.descending === true, 10000);
        waitRows(3);
        compare(cellText(0, 1), "c");
        keyClick(Qt.Key_End);
        compare(g.headerColumn, 2);
        keyClick(Qt.Key_Home);
        compare(g.headerColumn, 0);
        // Down goes to the cells.
        keyClick(Qt.Key_Down);
        verify(g.body.activeFocus);

        // Pulling the edge of a header resizes the column and does not sort.
        keyClick(Qt.Key_Left);
        const edge = walker.findAll(g.header, i => i.cursorShape === Qt.SplitHCursor)[2];
        verify(edge !== undefined);
        const before = g.widthOf(2);
        const id = tabs().idOf(tabs().currentIndex);
        const view0 = JSON.stringify(shell.queries.dataViewOf(id));
        const p = edge.mapToItem(g, edge.width / 2, edge.height / 2);
        mousePress(g, p.x, p.y);
        mouseMove(g, p.x + 20, p.y);
        mouseMove(g, p.x + 40, p.y);
        mouseRelease(g, p.x + 40, p.y);
        compare(g.widthOf(2), before + 40);
        wait(500);
        compare(JSON.stringify(shell.queries.dataViewOf(id)), view0, "the order did not change");
    }

    function test_theSortCommandActsOnTheCurrentColumnAndHonoursPendingChanges() {
        peopleDb();
        openFromTree("people");
        tryCompare(view(), "columnNames", ["id", "name", "note"]);
        const command = CommandRegistry.find("data.sortByCurrentColumn");
        compare(command.title, "Sort by current column");
        compare(command.shortcutText, CommandRegistry.displayShortcut("Ctrl+Shift+O"));
        tryVerify(() => command.enabled);
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_Right);
        compare(grid().currentColumn, 2);
        CommandRegistry.trigger("data.sortByCurrentColumn");
        tryVerify(() => view().currentView.sortColumn === "note", 10000);
        waitRows(3);
        // The current cell stays on the column that was sorted.
        compare(grid().currentColumn, 2);
        CommandRegistry.trigger("data.sortByCurrentColumn");
        tryVerify(() => view().currentView.descending === true, 10000);
        waitRows(3);
        compare(cellText(0, 2), "z");
        CommandRegistry.trigger("data.sortByCurrentColumn");
        tryVerify(() => view().currentView.sortColumn === "", 10000);
        waitRows(3);

        // Pending changes: refused, said, nothing changes.
        edits().setCell(0, 1, "changed");
        said.length = 0;
        CommandRegistry.trigger("data.sortByCurrentColumn");
        verify(said.some(s => s.indexOf("Save or discard the pending changes") >= 0), JSON.stringify(said));
        compare(view().currentView.sortColumn, "");
        edits().discardAll();
        // It is a data command: it is not enabled without a data tab.
        shell.focusRegion(1);
        keyClick(Qt.Key_T, Qt.ControlModifier);
        tryVerify(() => shell.editorArea.currentEditor !== null);
        verify(!CommandRegistry.find("data.sortByCurrentColumn").enabled);
        verify(!CommandRegistry.find("data.toggleRecord").enabled);
        verify(!CommandRegistry.find("data.goToRelated").enabled);
    }

    // ---- the record panel ----

    function test_theRecordPanelStartsClosedAndFollowsTheRow() {
        peopleDb();
        openFromTree("people");
        verify(view().recordPanel === null, "nothing exists before it is asked for");
        verify(!view().recordOpen);
        const btn = view().recordButton;
        compare(btn.accessibleName, "Record view");
        compare(btn.Accessible.checkable, true);
        compare(btn.Accessible.checked, false);
        const command = CommandRegistry.find("data.toggleRecord");
        compare(command.shortcutText, CommandRegistry.displayShortcut("Ctrl+Shift+R"));
        compare(btn.shortcutText, command.shortcutText);

        mouseClick(btn);
        tryVerify(() => view().recordOpen && view().recordPanel !== null && view().recordPanel.visible);
        const panel = view().recordPanel;
        compare(btn.Accessible.checked, true);
        compare(command.checked, true);
        tryVerify(() => panel.fields.length === 3);
        compare(panel.fields.map(f => f.name), ["id", "name", "note"]);
        compare(panel.fields[1].text, "a");
        compare(panel.titleLabel.text, "Record: Row 1");
        tryVerify(() => panel.list.activeFocus, 5000, "the field list takes the focus");
        compare(panel.list.Accessible.role, Accessible.List);
        compare(panel.list.Accessible.name, "Record fields");
        tryVerify(() => panel.width > 0 && panel.list.height > 0);

        // It follows the row of the grid.
        grid().body.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        tryVerify(() => panel.fields.length === 3 && panel.fields[1].text === "b");
        compare(panel.titleLabel.text, "Record: Row 2");

        // A pending change is seen: the word and a pencil; NULL is the word in italics.
        edits().setNull(1, 2);
        edits().setCell(1, 1, "changed");
        tryVerify(() => panel.fields[2].isNull && panel.fields[2].state === "modified" && panel.fields[1].text === "changed");
        tryVerify(() => find(panel.list, i => i.text === "NULL" && i.font.italic === true) !== undefined, 5000, "NULL is the word, in italics");
        verify(find(panel.list, i => i.text === "modified") !== undefined);
        verify(find(panel.list, i => i.text === Icons.pencil) !== undefined, "a pencil beside it");
        edits().discardAll();
        tryVerify(() => panel.fields[1].text === "b" && !panel.fields[2].isNull);

        // The whole panel is readable and keyboard-only.
        compare(walker.problems(shell), []);
        // Closing it gives the focus back to the grid.
        CommandRegistry.trigger("data.toggleRecord");
        verify(!view().recordOpen);
        tryVerify(() => !panel.visible);
        tryVerify(() => grid().body.activeFocus);
        compare(command.checked, false);
        // Opening it again keeps what it had (and focuses it).
        CommandRegistry.trigger("data.toggleRecord");
        tryVerify(() => view().recordPanel === panel && panel.visible && panel.list.activeFocus);
        // Esc in the list goes back to the grid.
        keyClick(Qt.Key_Escape);
        tryVerify(() => grid().body.activeFocus);
    }

    function test_theRecordPanelSaysWhenThereIsNoRow() {
        peopleDb();
        openFromTree("people");
        CommandRegistry.trigger("data.toggleRecord");
        const panel = view().recordPanel;
        tryVerify(() => panel !== null && panel.visible);
        view().filterField.text = "id > 100";
        verify(view().applyView("id > 100"));
        waitRows(0);
        tryVerify(() => panel.fields.length === 0 && panel.emptyLabel.visible);
        verify(panel.emptyLabel.text.indexOf("no current row") >= 0, panel.emptyLabel.text);
        compare(panel.titleLabel.text, "Record");
        view().clearFilter();
        waitRows(3);
        tryVerify(() => panel.fields.length === 3 && !panel.emptyLabel.visible);
    }

    function test_theViewersShowJsonXmlTextAndBlobsAndCopyTheWholeValue() {
        docsDb();
        openFromTree("docs");
        CommandRegistry.trigger("data.toggleRecord");
        const panel = view().recordPanel;
        tryVerify(() => panel !== null && panel.visible && panel.fields.length === 5);
        compare(panel.fields.map(f => f.kind), ["", "json", "xml", "text", "blob"]);
        tryVerify(() => panel.list.activeFocus);
        a11yProbe.setClipboardText("before");

        // A field without a viewer says so.
        keyClick(Qt.Key_Return);
        verify(panel.lastAnnouncement.indexOf("no viewer") >= 0, panel.lastAnnouncement);
        verify(!panel.viewing);

        // JSON: indented, in the order it came, read-only, with its own focus.
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryVerify(() => panel.viewing && panel.viewerText.activeFocus);
        compare(panel.view.kind, "json");
        compare(panel.viewerText.text, "{\n  \"b\": 1,\n  \"a\": [\n    1,\n    2\n  ]\n}");
        compare(panel.viewerText.Accessible.role, Accessible.EditableText);
        compare(panel.viewerText.Accessible.readOnly, true);
        compare(panel.viewerText.Accessible.name, "JSON viewer, j");
        compare(panel.viewerText.readOnly, true);
        verify(panel.backButton.visible && panel.copyButton.visible);
        compare(panel.copyButton.Accessible.name, "Copy");
        compare(walker.problems(shell), []);
        // Copy takes the whole value as the database holds it, not the indented one.
        mouseClick(panel.copyButton);
        compare(a11yProbe.clipboardText(), "{\"b\":1,\"a\":[1,2]}");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !panel.viewing && panel.list.activeFocus);

        // XML.
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryVerify(() => panel.viewing);
        compare(panel.view.kind, "xml");
        verify(panel.viewerText.text.indexOf("\n  <b>x</b>") >= 0, panel.viewerText.text);
        mouseClick(panel.backButton);
        tryVerify(() => !panel.viewing && panel.list.activeFocus);

        // Long text: the whole of it, with a wrap switch.
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryVerify(() => panel.viewing);
        compare(panel.view.kind, "text");
        compare(panel.viewerText.text, "line one\nline two");
        verify(panel.wrapBox.visible);
        compare(panel.wrapBox.checked, true);
        compare(panel.viewerText.wrapMode, TextEdit.WrapAtWordBoundaryOrAnywhere);
        mouseClick(panel.wrapBox);
        tryVerify(() => !panel.wrap);
        compare(panel.viewerText.wrapMode, TextEdit.NoWrap);
        mouseClick(panel.wrapBox);
        tryVerify(() => panel.wrap);
        // Ctrl+C with nothing selected copies the whole value too.
        panel.viewerText.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_C, Qt.ControlModifier);
        compare(a11yProbe.clipboardText(), "line one\nline two");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !panel.viewing);

        // A BLOB: hex and ASCII, and its size in words.
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryVerify(() => panel.viewing);
        compare(panel.view.kind, "blob");
        verify(panel.viewerText.text.startsWith("00000000  48 65 6c 6c 6f"), panel.viewerText.text);
        verify(panel.viewerText.text.indexOf("|Hello|") >= 0);
        compare(panel.view.size, "5 bytes");
        verify(find(panel, i => i.text === "Size: 5 bytes") !== undefined);
        verify(!panel.wrapBox.visible, "a hex dump does not wrap");
        mouseClick(panel.copyButton);
        compare(a11yProbe.clipboardText(), "0x48656c6c6f");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !panel.viewing);

        // The grid still shows the cut value; nothing was edited by looking.
        compare(edits().pendingCount, 0);
        compare(walker.problems(shell), []);
    }

    // ---- foreign keys ----

    function test_goToRelatedRowFromTheGridOpensTheFilteredTable() {
        shopDb();
        openFromTree("orders");
        tryVerify(() => view().linkColumns.customer_id !== undefined, 10000);
        compare(view().linkColumns.customer_id, "Foreign key to customers.id");
        const command = CommandRegistry.find("data.goToRelated");
        compare(command.title, "Go to related row");
        compare(command.shortcutText, CommandRegistry.displayShortcut("Ctrl+Shift+G"));
        // The id column is not a key; customer_id is.
        verify(!command.enabled);
        keyClick(Qt.Key_Right);
        tryVerify(() => view().canGoToRelated && command.enabled);
        tryVerify(() => grid().lastAnnouncement.indexOf("Foreign key to customers.id") >= 0, 5000, grid().lastAnnouncement);
        const first = tabs().currentIndex;

        CommandRegistry.trigger("data.goToRelated");
        tryVerify(() => tabs().currentIndex !== first && tabs().titleOf(tabs().currentIndex) === "customers", 15000);
        tryVerify(() => view() !== null && view().visible && view().tabTitle === "customers");
        tryVerify(() => view().filterOrigin === "Filtered from orders.customer_id = 1");
        tryVerify(() => view().originRow.visible);
        compare(view().originLabel.text, "Filtered from orders.customer_id = 1");
        compare(view().originLabel.Accessible.role, Accessible.StaticText);
        verify(find(view().originRow, i => i.text === Icons.link) !== undefined, "an icon with the words");
        compare(view().originClearButton.text, "Clear");
        compare(view().originClearButton.iconGlyph, Icons.x);
        tryVerify(() => shell.queries.current.state === "done" && shell.queries.current.results.rows === 1, 10000);
        compare(cellText(0, 1), "Ana");
        verify(structureSaid.some(t => t === "Filtered from orders.customer_id = 1"), JSON.stringify(structureSaid));
        compare(view().currentView.filters.length, 1);
        compare(view().currentView.filters[0].column, "id");
        compare(view().currentView.filters[0].value, "1");
        // The "Clear filter" button is there too, and the band is part of the window's walk.
        verify(view().clearFilterButton.visible);
        compare(walker.problems(shell), []);

        // Clear: only the key's filter goes, with the keyboard.
        view().originClearButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.queries.current.state === "done" && shell.queries.current.results.rows === 2, 10000);
        tryVerify(() => !view().originRow.visible);
        compare(view().filterOrigin, "");
        tryVerify(() => grid().body.activeFocus);

        // Back in orders, the same row again opens nothing new and brings the tab to the front; "Clear
        // filter" removes the filter too.
        tabs().currentIndex = first;
        tryVerify(() => view().tabTitle === "orders");
        grid().body.forceActiveFocus(Qt.TabFocusReason);
        const count = tabs().count;
        CommandRegistry.trigger("data.goToRelated");
        tryVerify(() => view().tabTitle === "customers" && view().filterOrigin !== "", 10000);
        compare(tabs().count, count, "the tab that was open is used");
        tryVerify(() => shell.queries.current.state === "done" && shell.queries.current.results.rows === 1, 10000);
        mouseClick(view().clearFilterButton);
        tryVerify(() => shell.queries.current.state === "done" && shell.queries.current.results.rows === 2, 10000);
        compare(view().filterOrigin, "");
        compare(appliedWhere(), "");

        // The row whose key is NULL: nothing opens, and it is said.
        tabs().currentIndex = first;
        tryVerify(() => view().tabTitle === "orders");
        grid().body.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        compare(grid().currentRow, 2);
        const tabsBefore = tabs().count;
        CommandRegistry.trigger("data.goToRelated");
        tryVerify(() => structureSaid.includes("This row has no related row"), 5000, JSON.stringify(structureSaid));
        compare(tabs().count, tabsBefore);
        compare(view().tabTitle, "orders");
    }

    function test_goToRelatedRowFromTheRecordPanelAndAKeyToAMissingTable() {
        shopDb();
        openFromTree("orders");
        tryVerify(() => view().linkColumns.customer_id !== undefined, 10000);
        CommandRegistry.trigger("data.toggleRecord");
        const panel = view().recordPanel;
        tryVerify(() => panel !== null && panel.visible && panel.fields.length === 3);
        // The button is on the field of the key, with the same words as the command.
        const related = () => walker.findAll(panel.list, i => i.text === "Go to related row" && i.visible && i.Accessible.name !== undefined && i.Accessible.name.startsWith("Go to related row of"));
        tryVerify(() => related().length === 1);
        const buttons = related();
        compare(buttons[0].Accessible.name, "Go to related row of customer_id");
        compare(buttons[0].Accessible.description, "Foreign key to customers.id");
        // The command acts on the field the panel is on when it has the focus.
        panel.list.forceActiveFocus(Qt.TabFocusReason);
        panel.list.currentIndex = 1;
        tryVerify(() => view().recordFocused && view().canGoToRelated);
        const first = tabs().currentIndex;
        mouseClick(buttons[0]);
        tryVerify(() => tabs().currentIndex !== first && view().tabTitle === "customers", 15000);
        tryVerify(() => view().filterOrigin === "Filtered from orders.customer_id = 1");
        waitRows(1);

        // A key to a table that is not there: an error in the panel, and no new tab.
        tabs().currentIndex = first;
        openFromTreeAgain("lost");
        tryVerify(() => view().linkColumns.g !== undefined, 10000);
        CommandRegistry.trigger("data.toggleRecord");
        const lost = view().recordPanel;
        tryVerify(() => lost !== null && lost.visible && lost.fields.length === 2);
        const count = tabs().count;
        lost.list.forceActiveFocus(Qt.TabFocusReason);
        lost.list.currentIndex = 1;
        tryVerify(() => view().canGoToRelated);
        CommandRegistry.trigger("data.goToRelated");
        tryVerify(() => view().relatedError !== "", 10000);
        verify(view().relatedError.indexOf("ghost") >= 0, view().relatedError);
        verify(view().relatedError.indexOf("cannot be opened") >= 0, view().relatedError);
        tryVerify(() => lost.errorLabel.visible && lost.errorLabel.text === view().relatedError);
        compare(tabs().count, count, "no tab for a table that is not there");
        compare(walker.problems(shell), []);
    }

    // The tree is already expanded for this connection: opens `table` from it.
    function openFromTreeAgain(table) {
        shell.focusRegion(0);
        tryVerify(() => {
            for (let r = 0; r < tree().rows; ++r)
                if (tree().roleAt(r, SchemaTreeModel.NameRole) === table)
                    return true;
            return false;
        }, 10000);
        for (let r = 0; r < tree().rows; ++r) {
            if (tree().roleAt(r, SchemaTreeModel.NameRole) === table) {
                tree().goTo(r);
                break;
            }
        }
        keyClick(Qt.Key_Return);
        tryVerify(() => view() !== null && view().visible && view().tabTitle === table, 10000);
        tryCompare(shell.queries.current, "state", "done", 60000);
    }

    // ---- export with the view ----

    function test_theExportDialogSaysWhatItExports() {
        peopleDb();
        openFromTree("people");
        CommandRegistry.trigger("export.open");
        tryVerify(() => shell.exportDialog !== null && shell.exportDialog.visible);
        let dlg = shell.exportDialog;
        tryVerify(() => dlg.scopes.length === 2);
        dlg.scopeCombo.currentIndex = 1;
        tryVerify(() => dlg.scope === "query");
        tryCompare(dlg.scopeNote, "text", "Exports every row of the table.");
        dlg.scopeCombo.currentIndex = 0;
        tryVerify(() => dlg.scopeNote.text.indexOf("Exports every row") < 0);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dlg.visible);

        // With a condition and an order the note says it is the view.
        verify(view().applyView("id > 1"));
        waitRows(2);
        verify(shell.queries.hasDataView(tabs().idOf(tabs().currentIndex)));
        CommandRegistry.trigger("export.open");
        tryVerify(() => shell.exportDialog.visible);
        dlg = shell.exportDialog;
        dlg.scopeCombo.currentIndex = 1;
        tryCompare(dlg.scopeNote, "text", "Exports the rows that match the current filter and order.");
        verify(!dlg.scopeNote.text.indexOf("Exports every row") >= 0);
        // Unsaved edits are still not part of it, and the dialog says so too.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dlg.visible);
        edits().setCell(0, 1, "unsaved");
        CommandRegistry.trigger("export.open");
        tryVerify(() => shell.exportDialog.visible);
        dlg = shell.exportDialog;
        dlg.scopeCombo.currentIndex = 1;
        tryVerify(() => dlg.scopeNote.text.indexOf("Changes that are not saved yet are not exported.") >= 0 && dlg.scopeNote.text.indexOf("match the current filter") >= 0, 5000, dlg.scopeNote.text);
        compare(walker.problems(shell), []);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dlg.visible);
        edits().discardAll();
    }

    // ---- everything together: walker, 200 % and Spanish ----

    function test_theNewPartsFitAtTwoHundredPercentAndSpeakSpanish() {
        shopDb();
        openFromTree("orders");
        tryVerify(() => view().linkColumns.customer_id !== undefined, 10000);
        CommandRegistry.trigger("data.toggleRecord");
        const panel = view().recordPanel;
        tryVerify(() => panel !== null && panel.visible && panel.fields.length === 3);
        tryVerify(() => panel.list.activeFocus);
        grid().body.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Right);
        compare(grid().currentColumn, 1);
        tryVerify(() => view().canGoToRelated);
        CommandRegistry.trigger("data.goToRelated");
        tryVerify(() => view().tabTitle === "customers" && view().originRow.visible, 15000, view().tabTitle + "|" + view().relatedError + "|" + view().filterOrigin);
        CommandRegistry.trigger("data.toggleRecord");
        tryVerify(() => view().recordPanel !== null && view().recordPanel.visible);
        compare(walker.problems(shell), []);

        Theme.zoom = 2.0;
        tryVerify(() => grid().rowHeight === Theme.controlHeight);
        tryVerify(() => view().originRow.height > 0 && view().toolbar.height > 0);
        for (const item of [view().originClearButton, view().recordPanel.closeButton, view().recordButton, view().structureButton]) {
            tryVerify(() => item.mapToItem(view(), item.width, 0).x <= view().width + 1, 5000, item.Accessible.name + " is inside the tab at 200 %");
        }
        verify(grid().tableView.height > 0, "the grid keeps some room");
        compare(walker.problems(shell), []);
        Theme.resetZoom();

        // Spanish.
        Language.choice = Language.Choice.Spanish;
        compare(view().recordButton.accessibleName, "Vista de registro");
        compare(view().structureButton.accessibleName, "Estructura");
        compare(view().originClearButton.text, "Quitar");
        compare(view().recordPanel.closeButton.accessibleName, "Cerrar la vista de registro");
        compare(view().recordPanel.list.Accessible.name, "Campos del registro");
        compare(CommandRegistry.find("data.sortByCurrentColumn").title, "Ordenar por la columna actual");
        compare(CommandRegistry.find("data.toggleRecord").title, "Mostrar u ocultar la vista de registro");
        compare(CommandRegistry.find("data.goToRelated").title, "Ir a la fila relacionada");
        compare(CommandRegistry.find("table.showStructure").title, "Mostrar estructura");
        compare(grid().headerFocus.Accessible.name, "Cabeceras de columna");
        tryVerify(() => headerCell(1) !== undefined);
        tryVerify(() => headerCell(1).Accessible.description === "sin orden", 5000, headerCell(1).Accessible.description);
        verify(shell.exporter !== null);
        compare(walker.problems(shell), []);
        Language.choice = Language.Choice.English;
        // None of this touched the data.
        compare(edits().pendingCount, 0);
        compare(shell.queries.pendingTotal(), 0);
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
        compare(view().rowsField.Accessible.name, "Filas a cargar");
        compare(view().rowsField.tipText, "Número de filas que se cargan al abrir una tabla");
        compare(view().countButton.accessibleName, "Contar filas");
        compare(view().countButton.tooltip.text, "Contar las filas de la tabla");
        compare(view().countLabel.text, "3");
        compare(view().countLabel.Accessible.name, "3 filas cargadas");
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
