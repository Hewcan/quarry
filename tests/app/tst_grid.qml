import QtQuick
import QtTest
import Quarry.Controls

// The results grid (H1.7) inside the real main window: keyboard navigation, selection, copy,
// Fetch all, NULL in italics, the accessibility walker, 200 % zoom, high contrast, Spanish texts and
// the event loop while 100 000 rows stream in. Temporary databases and the real SQLite plugin.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0
    property string savedClipboard: ""
    // What the grid sent to the screen reader, in order.
    property var heard: []

    name: "Grid"
    when: windowShown
    width: 1000
    height: 700
    visible: true

    A11y {
        id: walker
    }

    Component {
        id: spyComp

        SignalSpy {}
    }

    Connections {
        target: testCase.shell ? testCase.shell.resultsPanel.grid : null

        function onAnnounced(text, assertive) {
            testCase.heard.push(text);
        }
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

    function editor() {
        return shell.editorArea.currentEditor;
    }

    function model() {
        return shell.connections.model;
    }

    function current() {
        return shell.queries.current;
    }

    function grid() {
        return shell.resultsPanel.grid;
    }

    function results() {
        return current().results;
    }

    function localized(n) {
        return n.toLocaleString(Qt.locale(), "f", 0);
    }

    // `rows` rows of: id, name, score (NULL on every 5th row).
    function sqlRows(rows) {
        return "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < " + rows + ") SELECT x AS id, 'name ' || x AS name, CASE WHEN x % 5 = 0 THEN NULL ELSE x * 1.5 END AS score FROM c";
    }

    function newFile() {
        return testTempDir + "/grid-data-" + (++fileCounter) + ".db";
    }

    function waitForViews() {
        tryVerify(() => shell.sidebar.tree === null ? model().count === 0 : shell.sidebar.tree.rows >= model().count);
        if (tabs().count > 0)
            tryVerify(() => editor() !== null && editor().visible);
    }

    function addSqlite(file) {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        shell.connections.openSqliteFile(Qt.url("file:///" + file));
        tryCompare(spy, "count", 1);
        waitForViews();
    }

    function removeAll() {
        while (tabs().count > 0)
            tabs().closeTab(0);
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
        tryCompare(model(), "count", 0);
    }

    // Runs `sql` in a new query tab (the statement under the cursor) and waits for it.
    function runSql(sql) {
        shell.focusRegion(1);
        keyClick(Qt.Key_T, Qt.ControlModifier);
        waitForViews();
        tryVerify(() => editor().edit.activeFocus);
        editor().edit.text = sql;
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(current(), "state", "done", 20000);
    }

    // Mod+3, the way a user gets to the grid.
    function focusGrid() {
        keyClick(Qt.Key_3, Qt.ControlModifier);
        tryVerify(() => grid().body.activeFocus);
    }

    function cellPoint(row, column) {
        const item = grid().tableView.itemAtCell(Qt.point(column, row));
        verify(item !== null, "cell " + row + "," + column + " is loaded");
        return item.mapToItem(grid().tableView, item.width / 2, item.height / 2);
    }

    function clickCell(row, column, modifiers) {
        const p = cellPoint(row, column);
        mouseClick(grid().tableView, p.x, p.y, Qt.LeftButton, modifiers === undefined ? Qt.NoModifier : modifiers);
    }

    function expectSaid(text) {
        tryVerify(() => heard.indexOf(text) >= 0, 5000, "not announced: " + text + " (heard: " + heard.slice(-6).join(" | ") + ")");
    }

    // The header cells (the Row also holds the Repeater).
    function headerCell(i) {
        return grid().header.children.filter(c => c.columnTitle !== undefined)[i];
    }

    function selectionIs(top, left, bottom, right) {
        const s = grid().selection;
        return s.top === top && s.left === left && s.bottom === bottom && s.right === right;
    }

    function textItems(text) {
        return walker.findAll(grid().tableView, i => i.visible && i.text === text);
    }

    function initTestCase() {
        savedClipboard = a11yProbe.clipboardText();
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/grid.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        tryVerify(() => !model().loading, 10000);
        tryVerify(() => tabs().loaded, 10000);
    }

    function init() {
        ticker.running = false;
        Language.choice = Language.Choice.English;
        Theme.mode = Theme.Mode.Light;
        Theme.resetZoom();
        shell.resetLayout();
        removeAll();
        shell.resultsPanel.tabBar.currentIndex = 0;
        shell.focusRegion(1);
        heard.length = 0;
        addSqlite(newFile());
    }

    function cleanupTestCase() {
        Language.choice = Language.Choice.English;
        Theme.mode = Theme.Mode.System;
        Theme.resetZoom();
        a11yProbe.setClipboardText(savedClipboard);
    }

    // ---- states --------------------------------------------------------------------------------

    function test_theGridReplacesThePlaceholder() {
        const panel = shell.resultsPanel;
        verify(grid() === null || !grid().visible, "no grid before the first result");
        verify(panel.resultsEmpty.visible);
        compare(panel.resultsEmpty.message, "Run a query to see results here.");
        runSql(sqlRows(1200));
        verify(grid().visible);
        verify(!panel.resultsEmpty.visible);
        tryCompare(results(), "rows", 500);
        compare(grid().counterText, "500 rows, more available");
        // Each header is the column name.
        compare(grid().header.children.filter(c => c.columnTitle !== undefined).map(c => c.columnTitle), ["id", "name", "score"]);
        // The status bar still has the count and the time.
        verify(current().statusText.startsWith("500+ rows · "), current().statusText);

        // A statement without rows: the placeholder explains, no grid.
        editor().edit.text = "CREATE TABLE t(a)";
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(current(), "state", "done");
        verify(!grid().visible);
        verify(panel.resultsEmpty.visible);
        compare(panel.resultsEmpty.message, "This statement returns no rows.");

        // A query with columns and no rows: the header and a message.
        editor().edit.text = "SELECT a FROM t";
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryVerify(() => grid().visible && current().state === "done");
        compare(grid().counterText, "0 rows");
        verify(walker.findAll(grid().body, i => i.visible && i.text === "This query returned no rows.").length === 1);
    }

    // ---- keyboard ------------------------------------------------------------------------------

    function test_keyboardNavigation() {
        runSql(sqlRows(100));
        tryCompare(results(), "rows", 100);
        focusGrid();
        const g = grid();
        compare(g.currentRow, 0);
        compare(g.currentColumn, 0);
        expectSaid("Row 1 of 100, id: 1");
        compare(g.body.Accessible.role, Accessible.Table);

        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        compare(g.currentRow, 3);
        keyClick(Qt.Key_Right);
        compare(g.currentColumn, 1);
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_Right); // the edge: stays
        compare(g.currentColumn, 2);
        keyClick(Qt.Key_Home);
        compare(g.currentColumn, 0);
        compare(g.currentRow, 3);
        keyClick(Qt.Key_End);
        compare(g.currentColumn, 2);
        keyClick(Qt.Key_Left);
        compare(g.currentColumn, 1);
        keyClick(Qt.Key_Up);
        keyClick(Qt.Key_Up);
        keyClick(Qt.Key_Up);
        keyClick(Qt.Key_Up); // the top: stays
        compare(g.currentRow, 0);
        expectSaid("Row 1 of 100, name: name 1");

        keyClick(Qt.Key_End, Qt.ControlModifier);
        compare(g.currentRow, 99);
        compare(g.currentColumn, 2);
        tryVerify(() => g.tableView.bottomRow >= 99, 5000, "the view follows the current cell");
        expectSaid("Row 100 of 100, score: " + results().cellText(99, 2));
        keyClick(Qt.Key_Home, Qt.ControlModifier);
        compare(g.currentRow, 0);
        compare(g.currentColumn, 0);
        tryVerify(() => g.tableView.topRow === 0);

        const page = g.pageRows();
        verify(page >= 1);
        keyClick(Qt.Key_PageDown);
        compare(g.currentRow, page);
        keyClick(Qt.Key_PageDown);
        compare(g.currentRow, page * 2);
        keyClick(Qt.Key_PageUp);
        compare(g.currentRow, page);
        tryVerify(() => g.tableView.topRow <= page && g.tableView.bottomRow >= page);

        // A NULL is said as such.
        keyClick(Qt.Key_Home, Qt.ControlModifier);
        for (let i = 0; i < 4; ++i)
            keyClick(Qt.Key_Down);
        keyClick(Qt.Key_End);
        expectSaid("Row 5 of 100, score: NULL");
    }

    // A held key says only where it ended.
    function test_theAnnouncementIsDebounced() {
        runSql(sqlRows(100));
        tryCompare(results(), "rows", 100);
        focusGrid();
        expectSaid("Row 1 of 100, id: 1");
        const count = heard.length;
        for (let i = 0; i < 6; ++i)
            keyClick(Qt.Key_Down);
        compare(heard.length, count, "nothing is said while the key repeats");
        expectSaid("Row 7 of 100, id: 7");
        compare(heard.length, count + 1, "only where it ended");
    }

    function test_selectionByKeyboard() {
        runSql(sqlRows(100));
        tryCompare(results(), "rows", 100);
        focusGrid();
        const g = grid();
        verify(selectionIs(0, 0, 0, 0));
        keyClick(Qt.Key_Down, Qt.ShiftModifier);
        keyClick(Qt.Key_Down, Qt.ShiftModifier);
        keyClick(Qt.Key_Right, Qt.ShiftModifier);
        verify(selectionIs(0, 0, 2, 1), JSON.stringify(g.selection));
        compare(g.selectedCells, 6);
        expectSaid("Row 3 of 100, name: name 3, 6 cells selected");
        // Shrinking goes back towards the anchor.
        keyClick(Qt.Key_Up, Qt.ShiftModifier);
        verify(selectionIs(0, 0, 1, 1));
        // Without Shift the selection collapses to the current cell.
        keyClick(Qt.Key_Down);
        verify(selectionIs(2, 1, 2, 1), JSON.stringify(g.selection));
        // The row, the column, everything.
        keyClick(Qt.Key_Space, Qt.ShiftModifier);
        verify(selectionIs(2, 0, 2, 2));
        keyClick(Qt.Key_Space, Qt.ControlModifier);
        verify(selectionIs(0, 1, 99, 1));
        keyClick(Qt.Key_A, Qt.ControlModifier);
        verify(selectionIs(0, 0, 99, 2));
        compare(g.selectedCells, 300);
        // Home / End and Page keys extend too.
        keyClick(Qt.Key_Home, Qt.ControlModifier);
        keyClick(Qt.Key_End, Qt.ControlModifier | Qt.ShiftModifier);
        verify(selectionIs(0, 0, 99, 2));
    }

    function test_selectionByMouse() {
        runSql(sqlRows(100));
        tryCompare(results(), "rows", 100);
        const g = grid();
        tryVerify(() => g.tableView.itemAtCell(Qt.point(2, 3)) !== null); // the region shows about four rows
        clickCell(2, 1);
        verify(g.body.activeFocus, "a click focuses the grid");
        compare(g.currentRow, 2);
        compare(g.currentColumn, 1);
        verify(selectionIs(2, 1, 2, 1));
        clickCell(3, 2, Qt.ShiftModifier);
        verify(selectionIs(2, 1, 3, 2), JSON.stringify(g.selection));
        // A drag.
        const a = cellPoint(0, 0);
        const b = cellPoint(2, 1);
        mousePress(g.tableView, a.x, a.y);
        mouseMove(g.tableView, (a.x + b.x) / 2, (a.y + b.y) / 2);
        mouseMove(g.tableView, b.x, b.y);
        wait(20);
        mouseMove(g.tableView, b.x, b.y);
        mouseRelease(g.tableView, b.x, b.y);
        verify(selectionIs(0, 0, 2, 1), JSON.stringify(g.selection));
        // The header selects the column.
        const head = headerCell(1);
        const hp = head.mapToItem(g, head.width / 2, head.height / 2);
        mouseClick(g, hp.x, hp.y);
        verify(selectionIs(0, 1, 99, 1), JSON.stringify(g.selection));
    }

    // ---- copy ----------------------------------------------------------------------------------

    function test_copyAsTsv() {
        runSql(sqlRows(100));
        tryCompare(results(), "rows", 100);
        focusGrid();
        a11yProbe.setClipboardText("before");
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down, Qt.ShiftModifier);
        keyClick(Qt.Key_Right, Qt.ShiftModifier); // rows 2..3, columns id..name
        keyClick(Qt.Key_C, Qt.ControlModifier);
        compare(a11yProbe.clipboardText(), "2\tname 2\n3\tname 3");
        expectSaid("Copied 4 cells");
        keyClick(Qt.Key_C, Qt.ControlModifier | Qt.ShiftModifier);
        compare(a11yProbe.clipboardText(), "id\tname\n2\tname 2\n3\tname 3");

        // NULL copies as nothing: row 5 has a NULL score.
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_End);
        keyClick(Qt.Key_Left, Qt.ShiftModifier);
        compare(grid().selection.bottom, 4);
        keyClick(Qt.Key_C, Qt.ControlModifier);
        compare(a11yProbe.clipboardText(), "name 5\t");
        keyClick(Qt.Key_C, Qt.ControlModifier | Qt.ShiftModifier);
        compare(a11yProbe.clipboardText(), "name\tscore\nname 5\t");
        // The whole column from the keyboard.
        keyClick(Qt.Key_Space, Qt.ControlModifier);
        keyClick(Qt.Key_C, Qt.ControlModifier);
        compare(a11yProbe.clipboardText().split("\n").length, 100);
    }

    // 33 400 rows x 3 columns is over the 100 000 cell cap: nothing is copied, and it is said.
    function test_aSelectionOverTheCapIsNotCopied() {
        runSql(sqlRows(33400));
        tryCompare(results(), "rows", 500);
        focusGrid();
        results().fetchAll();
        tryCompare(results(), "rows", 33400, 30000);
        tryVerify(() => !results().fetchingAll);
        a11yProbe.setClipboardText("untouched");
        heard.length = 0;
        keyClick(Qt.Key_A, Qt.ControlModifier);
        compare(grid().selectedCells, 100200);
        const text = "Too many cells to copy (" + localized(100200) + "). Select fewer than " + localized(100000) + " or use export.";
        keyClick(Qt.Key_C, Qt.ControlModifier);
        expectSaid(text);
        compare(a11yProbe.clipboardText(), "untouched");
        compare(grid().notice, text);
        keyClick(Qt.Key_C, Qt.ControlModifier | Qt.ShiftModifier);
        compare(a11yProbe.clipboardText(), "untouched");
        // The notice is visible in the counter row, and goes with the next move.
        tryVerify(() => walker.findAll(grid(), i => i.visible && i.text === text).length === 1);
        keyClick(Qt.Key_Down);
        compare(grid().notice, "");
        // A smaller selection copies again.
        keyClick(Qt.Key_Home, Qt.ControlModifier);
        keyClick(Qt.Key_C, Qt.ControlModifier);
        compare(a11yProbe.clipboardText(), "1");
    }

    function test_tabsAndNewlinesInAValueBecomeSpaces() {
        runSql("SELECT 'a' || char(9) || 'b' || char(10) || 'c' AS v, NULL AS n, x'00ff' AS b");
        tryCompare(results(), "rows", 1);
        focusGrid();
        keyClick(Qt.Key_A, Qt.ControlModifier);
        keyClick(Qt.Key_C, Qt.ControlModifier);
        compare(a11yProbe.clipboardText(), "a b c\t\t0x00ff");
        // The grid paints the first line only, and the blob as a size.
        compare(results().cellText(0, 0), "a\tb…");
        compare(results().cellText(0, 2), "<binary 2 B>");
    }

    // ---- NULL ------------------------------------------------------------------------------------

    function test_nullIsTheWordInItalicsAndMuted() {
        runSql("SELECT 'first' AS f, NULL AS n, 'plain' AS s");
        tryCompare(results(), "rows", 1);
        tryVerify(() => textItems("NULL").length === 1);
        const nulls = textItems("NULL")[0];
        verify(nulls.font.italic, "NULL is italic, not only a different colour");
        compare(nulls.color, Theme.gridNull);
        const plain = textItems("plain")[0];
        verify(!plain.font.italic);
        compare(plain.color, Theme.fgDefault);
        verify(Qt.colorEqual(nulls.color, plain.color) === false);
        // Selected, the text takes the selection colours and stays italic.
        clickCell(0, 1);
        compare(textItems("NULL")[0].color, Theme.selectionFg);
        verify(textItems("NULL")[0].font.italic);
        // High contrast too.
        Theme.mode = Theme.Mode.HighContrast;
        compare(textItems("NULL")[0].font.italic, true);
    }

    // ---- Fetch all -----------------------------------------------------------------------------

    function test_fetchAllFromTheKeyboard() {
        runSql(sqlRows(3000));
        tryCompare(results(), "rows", 500);
        const g = grid();
        verify(g.fetchButton.visible);
        compare(g.fetchButton.Accessible.name, "Fetch all");
        compare(g.fetchButton.Accessible.role, Accessible.Button);
        focusGrid();
        keyClick(Qt.Key_Tab);
        verify(g.fetchButton.activeFocus, "Fetch all is the next tab stop");
        verify(g.fetchButton.visualFocus);
        keyClick(Qt.Key_Space);
        tryCompare(results(), "rows", 3000, 20000);
        tryVerify(() => !results().fetchingAll);
        compare(g.counterText, localized(3000) + " rows");
        verify(!g.fetchButton.visible);
        verify(g.body.activeFocus, "the focus stays in the grid when the button goes away");
        expectSaid("All " + localized(3000) + " rows loaded");
        verify(current().atEnd);
        verify(current().statusText.startsWith("3000 rows · "), current().statusText);
        // The last row is there.
        keyClick(Qt.Key_End, Qt.ControlModifier);
        expectSaid("Row 3000 of 3000, score: " + results().cellText(2999, 2));
        compare(results().cellText(2999, 0), "3000");
    }

    function test_escapeStopsFetchAllAndOtherwiseCancels() {
        runSql(sqlRows(3000));
        tryCompare(results(), "rows", 500);
        focusGrid();
        results().fetchAll();
        verify(results().fetchingAll);
        compare(grid().fetchButton.text, "Stop fetching");
        keyClick(Qt.Key_Escape);
        verify(!results().fetchingAll, "Esc stops Fetch all");
        compare(grid().fetchButton.text, "Fetch all");
        // The page in flight lands; the rest can still be asked for.
        tryVerify(() => results().rows >= 1000 || !results().fetching);
        verify(results().moreAvailable || results().rows === 3000);
        // With nothing to stop, Esc still reaches the region and cancels a running query.
        keyClick(Qt.Key_Escape);
        compare(current().state, "done");
    }

    // Scrolling to the end of the loaded rows asks for the next page by itself.
    function test_theViewLoadsTheNextPageNearTheEnd() {
        runSql(sqlRows(3000));
        tryCompare(results(), "rows", 500);
        focusGrid();
        keyClick(Qt.Key_End, Qt.ControlModifier);
        tryCompare(results(), "rows", 1000, 10000);
        verify(results().moreAvailable);
        compare(grid().counterText, localized(1000) + " rows, more available");
    }

    // ---- columns -------------------------------------------------------------------------------

    function test_columnWidthByKeyboardAndMouse() {
        runSql(sqlRows(20));
        tryCompare(results(), "rows", 20);
        focusGrid();
        const g = grid();
        tryVerify(() => g.tableView.itemAtCell(Qt.point(0, 0)) !== null);
        const w0 = g.widthOf(0);
        const head = headerCell(0);
        compare(head.width, w0);
        compare(g.tableView.itemAtCell(Qt.point(0, 0)).width, w0);
        keyClick(Qt.Key_Right, Qt.AltModifier | Qt.ShiftModifier);
        verify(g.widthOf(0) > w0);
        compare(g.currentColumn, 0, "the cell did not move");
        tryCompare(head, "width", g.widthOf(0));
        tryCompare(g.tableView.itemAtCell(Qt.point(0, 0)), "width", g.widthOf(0));
        tryVerify(() => heard.some(h => h.startsWith("Column id, width ")));
        const wider = g.widthOf(0);
        keyClick(Qt.Key_Left, Qt.AltModifier | Qt.ShiftModifier);
        verify(g.widthOf(0) < wider);
        for (let i = 0; i < 30; ++i)
            keyClick(Qt.Key_Left, Qt.AltModifier | Qt.ShiftModifier);
        compare(g.widthOf(0), g.minColumnWidth, "never below the minimum");
        // The second column did not change, and moved with the first.
        tryCompare(headerCell(1), "x", g.widthOf(0) + 1);

        // The edge of the header, with the mouse.
        const edge = walker.findAll(g.header, i => i.cursorShape === Qt.SplitHCursor)[1];
        verify(edge !== undefined);
        const before = g.widthOf(1);
        const p = edge.mapToItem(g, edge.width / 2, edge.height / 2);
        mousePress(g, p.x, p.y);
        mouseMove(g, p.x + 40, p.y);
        mouseMove(g, p.x + 60, p.y);
        mouseRelease(g, p.x + 60, p.y);
        compare(g.widthOf(1), before + 60);
        tryCompare(headerCell(2), "x", g.widthOf(0) + g.widthOf(1) + 2);
        // A new result starts again from the sample.
        shell.focusRegion(1);
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(current(), "state", "done");
        tryVerify(() => g.widthOf(1) !== before + 60);
    }

    // ---- accessibility -------------------------------------------------------------------------

    function test_walkerAndRoles() {
        runSql(sqlRows(1200));
        tryCompare(results(), "rows", 500);
        compare(walker.problems(shell.resultsPanel), []);
        compare(walker.problems(shell), []);
        const g = grid();
        compare(g.body.Accessible.role, Accessible.Table);
        compare(g.body.Accessible.name, "Results grid");
        verify(g.body.Accessible.description.includes("500 rows, 3 columns"), g.body.Accessible.description);
        verify(g.body.Accessible.description.includes("Alt+Shift+Left or Right resizes the column"));
        verify(g.body.activeFocusOnTab);
        verify(g.fetchButton.activeFocusOnTab);
        // The cells and headers are not in the accessibility tree: the grid speaks for them.
        const texts = [...walker.findAll(g.tableView, i => i.text !== undefined && i.Accessible.ignored !== true), ...walker.findAll(g.header, i => i.text !== undefined && i.Accessible.ignored !== true)];
        compare(texts.length, 0, "cell and header texts are ignored by assistive technology");
        const info = a11yProbe.inspect(g.body);
        compare(info.name, "Results grid");
        verify(info.focusable);
        // Qt Quick still lists the delegates (unnamed panes); none of them carries a name or a text.
        verify(info.children.every(c => c.split(":")[1] === ""), "unnamed children only: " + info.children.join(", "));
        // Tab order: tabs, grid, Fetch all.
        shell.focusRegion(2);
        verify(g.body.activeFocus);
        keyClick(Qt.Key_Tab);
        verify(g.fetchButton.activeFocus);
        keyClick(Qt.Key_Tab, Qt.ShiftModifier);
        verify(g.body.activeFocus);
    }

    // ---- zoom and contrast ---------------------------------------------------------------------

    function test_200PercentZoomAndHighContrast() {
        runSql(sqlRows(1200));
        tryCompare(results(), "rows", 500);
        const g = grid();
        Theme.zoom = 2.0;
        tryVerify(() => g.rowHeight === Theme.controlHeight && g.tableView.itemAtCell(Qt.point(0, 0)) !== null);
        tryCompare(g.tableView.itemAtCell(Qt.point(0, 0)), "height", Theme.controlHeight);
        tryVerify(() => headerCell(0).height === Theme.controlHeight);
        // The footer fits: counter and Fetch all are inside the grid, and the grid inside the region.
        const right = g.fetchButton.mapToItem(g, g.fetchButton.width, 0).x;
        verify(right <= g.width, "the Fetch all button is inside the grid at 200 %");
        verify(g.width <= shell.resultsPanel.width);
        verify(g.tableView.height > 0, "the table keeps some room");
        focusGrid();
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Right);
        compare(g.currentRow, 1);
        tryVerify(() => g.tableView.itemAtCell(Qt.point(1, 1)) !== null);
        compare(walker.problems(shell.resultsPanel), []);
        Theme.mode = Theme.Mode.HighContrast;
        verify(Theme.isHighContrast);
        const cell = g.tableView.itemAtCell(Qt.point(1, 1));
        compare(cell.border.width, Theme.focusRingWidth, "the current cell has an outline, not only a colour");
        compare(cell.border.color, Theme.focusRing);
        compare(walker.problems(shell.resultsPanel), []);
        Theme.resetZoom();
        Theme.mode = Theme.Mode.Light;
    }

    // ---- translation ---------------------------------------------------------------------------

    function test_spanishStrings() {
        runSql(sqlRows(1200));
        tryCompare(results(), "rows", 500);
        Language.choice = Language.Choice.Spanish;
        const g = grid();
        compare(g.counterText, "500 filas, hay más disponibles");
        compare(g.fetchButton.text, "Traer todo");
        compare(g.body.Accessible.name, "Cuadrícula de resultados");
        focusGrid();
        expectSaid("Fila 1 de 500, id: 1");
        keyClick(Qt.Key_Down, Qt.ShiftModifier);
        expectSaid("Fila 2 de 500, id: 2, 2 celdas seleccionadas");
        a11yProbe.setClipboardText("");
        keyClick(Qt.Key_C, Qt.ControlModifier);
        expectSaid("Se copiaron 2 celdas");
        results().fetchAll();
        compare(g.fetchButton.text, "Detener la carga");
        keyClick(Qt.Key_Escape);
        editor().edit.text = "SELECT 1 AS one";
        shell.focusRegion(1);
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(current(), "state", "done");
        compare(g.counterText, "1 fila");
        editor().edit.text = "SELECT 1 AS a WHERE 0";
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryVerify(() => walker.findAll(g.body, i => i.visible && i.text === "Esta consulta no devolvió filas.").length === 1);
        Language.choice = Language.Choice.English;
    }

    // ---- the event loop while rows stream in -----------------------------------------------------

    // 100 000 rows through Fetch all: the loop keeps turning (the gap budget of the other tests; a
    // debug build only has to stay responsive).
    function test_theUiRespondsWhileFetchAllStreams() {
        tryVerify(() => shell.paletteDialog && shell.settingsDialog, 10000);
        wait(200);
        runSql(sqlRows(100000));
        tryCompare(results(), "rows", 500);
        focusGrid();
        ticker.last = Date.now();
        ticker.maxGap = 0;
        ticker.running = true;
        wait(700);
        console.log("IDLE max event-loop gap " + ticker.maxGap + " ms");
        if (ticker.maxGap > testMaxGapMs / 2) {
            ticker.running = false;
            skip("idle event-loop gap " + ticker.maxGap + " ms: too noisy to measure a " + testMaxGapMs + " ms budget");
        }
        ticker.maxGap = 0;
        ticker.last = Date.now();
        const started = Date.now();
        results().fetchAll();
        // Navigate while it streams: the keys are served.
        for (let i = 0; i < 5; ++i) {
            keyClick(Qt.Key_Down);
            wait(20);
        }
        tryCompare(results(), "rows", 100000, 60000);
        ticker.running = false;
        console.log("FETCH ALL 100000 rows in " + (Date.now() - started) + " ms, max event-loop gap " + ticker.maxGap + " ms");
        verify(grid().currentRow >= 5, "the keys were served while streaming");
        verify(ticker.maxGap <= testMaxGapMs, "event loop blocked for " + ticker.maxGap + " ms (limit " + testMaxGapMs + ")");
        compare(results().cellText(99999, 0), "100000");
    }
}
