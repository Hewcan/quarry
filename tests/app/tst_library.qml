import QtQuick
import QtTest
import Quarry.Controls

// History and saved queries (H4): the side panel under the schema tree, driven the way a
// keyboard-only user would. Temporary databases and the real SQLite plugin; no keychain.
// Delegates are never read right after an event: tryVerify() with forceLayout() first.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0

    name: "Library"
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

    function editor() {
        return shell.editorArea.currentEditor;
    }

    function model() {
        return shell.connections.model;
    }

    function current() {
        return shell.queries.current;
    }

    function panel() {
        return shell.library;
    }

    function historyView() {
        return panel().historyView;
    }

    function savedView() {
        return panel().savedView;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function newFile() {
        return testTempDir + "/library-data-" + (++fileCounter) + ".db";
    }

    function typeText(text) {
        for (const ch of text)
            keyClick(ch === " " ? Qt.Key_Space : ch);
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
        return spy.signalArguments[0][0];
    }

    // A tab on the connection of the tree node, with `sql` as its text; the editor has the focus.
    function openQuery(sql) {
        shell.focusRegion(1);
        keyClick(Qt.Key_T, Qt.ControlModifier);
        waitForViews();
        tryVerify(() => editor().edit.activeFocus);
        editor().edit.text = sql;
        compare(tabs().textOf(tabs().currentIndex), sql);
    }

    // Runs the statement of the current tab and waits for it to end.
    function runCurrent(state) {
        shell.focusRegion(1);
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(current(), "state", state ?? "done", 20000);
    }

    function openPanel(tab) {
        CommandRegistry.trigger(tab === 1 ? "saved.show" : "history.show");
        tryVerify(() => shell.library !== null && shell.libraryVisible);
        tryVerify(() => panel().tabIndex === (tab ?? 0));
        // The focus arrives a moment later (Qt.callLater): keys sent before would go elsewhere.
        tryVerify(() => panel().defaultFocus.activeFocus);
    }

    // The ListView builds its delegates a pass after the model changes.
    function rowAt(list, index) {
        let item = null;
        tryVerify(() => {
            list.forceLayout();
            item = list.itemAtIndex(index);
            return item !== null && item !== undefined;
        });
        return item;
    }

    function closeOverlays() {
        const dialogs = [shell.confirmRunDialog, shell.closeTabDialog, shell.connectionDialog, shell.deleteDialog, shell.paletteDialog, shell.historyOptOutDialog];
        for (const d of dialogs) {
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        }
        if (shell.library)
            shell.library.closeDialogs();
        tryVerify(() => !dialogs.some(d => d && d.visible), 5000);
    }

    function removeAll() {
        while (tabs().count > 0)
            tabs().closeTab(0);
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
        tryCompare(model(), "count", 0);
    }

    // The stores are one file for the whole test case: empty them between tests.
    function emptyStores() {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.historyModel,
            signalName: "cleared"
        });
        shell.historyModel.connectionFilter = "";
        shell.historyModel.clear();
        tryCompare(spy, "count", 1);
        const saved = shell.savedModel;
        saved.start();
        tryVerify(() => !saved.loading);
        // Folders first: the queries of a collapsed folder are not rows, and deleting the folder
        // moves them to the top level, where the second pass finds them.
        const folders = [];
        for (let i = 0; i < saved.count; ++i)
            if (saved.get(i).kind === "folder")
                folders.push(saved.get(i).itemId);
        for (const id of folders)
            saved.removeFolder(id);
        tryCompare(saved, "folderCount", 0);
        const queries = [];
        for (let i = 0; i < saved.count; ++i)
            queries.push(saved.get(i).itemId);
        for (const id of queries)
            saved.removeQuery(id);
        tryCompare(saved, "count", 0);
    }

    function initTestCase() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/library.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        tryVerify(() => !model().loading, 10000);
        tryVerify(() => tabs().loaded, 10000);
        // Nothing of H4 exists before it is used: no panel, no stores, no data read.
        compare(shell.library, null);
        verify(!shell.libraryVisible);
        verify(!shell.historyModel.started);
        verify(!shell.savedModel.started);
    }

    function init() {
        closeOverlays();
        Language.choice = Language.Choice.English;
        Theme.resetZoom();
        shell.resetLayout();
        removeAll();
        shell.focusRegion(1);
    }

    function cleanup() {
        closeOverlays();
        Theme.mode = Theme.Mode.System;
        Theme.resetZoom();
        Language.choice = Language.Choice.English;
        if (shell.library) {
            if (shell.historyModel.onlyErrors)
                shell.historyModel.onlyErrors = false;
            shell.historyModel.text = "";
            shell.historyModel.connectionFilter = "";
        }
        shell.hideLibrary();
        while (tabs().count > 0)
            tabs().closeTab(0);
    }

    function cleanupTestCase() {
        Language.choice = Language.Choice.English;
    }

    // ---- commands ----------------------------------------------------------------------------

    function test_commandsHaveUniqueShortcuts() {
        const wanted = ["history.show", "saved.show", "saved.saveCurrent", "history.clear"];
        for (const id of wanted) {
            const c = CommandRegistry.find(id);
            verify(c !== null && c !== undefined, id + " is registered");
            verify(c.title !== "", id + " has a title");
            compare(c.category, "History");
        }
        compare(CommandRegistry.find("history.show").shortcut, "Ctrl+Shift+H");
        compare(CommandRegistry.find("saved.show").shortcut, "Ctrl+Shift+L");
        compare(CommandRegistry.find("saved.saveCurrent").shortcut, "Ctrl+Shift+S");
        const seen = {};
        for (const c of CommandRegistry.commands.slice()) {
            for (const s of [c.shortcut, c.alternateShortcut]) {
                if (s === "")
                    continue;
                verify(seen[s] === undefined, s + " is used by " + seen[s] + " and " + c.commandId);
                seen[s] = c.commandId;
            }
        }
        // Without an open query there is nothing to save.
        verify(!CommandRegistry.find("saved.saveCurrent").enabled);
    }

    // ---- the panel ---------------------------------------------------------------------------

    function test_panelOpensWithTheKeyboardAndCloses() {
        addSqlite(newFile());
        openQuery("SELECT 1");
        keyClick(Qt.Key_H, Qt.ControlModifier | Qt.ShiftModifier);
        tryVerify(() => shell.library !== null && shell.libraryVisible);
        tryVerify(() => historyView().searchField.activeFocus);
        compare(panel().tabBar.Accessible.role, Accessible.PageTabList);
        compare(historyView().entries.Accessible.role, Accessible.List);
        compare(savedView().tree.Accessible.role, Accessible.Tree);
        compare(walker.problems(panel()), []);
        // The data is read when the tab is shown (initTestCase checks nothing was read before).
        verify(shell.historyModel.started);
        // The second tab, with the keyboard: Left/Right on the tab bar.
        keyClick(Qt.Key_L, Qt.ControlModifier | Qt.ShiftModifier);
        tryVerify(() => panel().tabIndex === 1);
        verify(shell.savedModel.started);
        tryVerify(() => panel().defaultFocus.activeFocus);
        compare(walker.problems(panel()), []);
        // Esc closes the panel and gives the focus back to where it came from.
        keyClick(Qt.Key_Escape);
        tryVerify(() => !shell.libraryVisible);
        tryVerify(() => editor().edit.activeFocus);
    }

    function test_theCommandTogglesThePanel() {
        openPanel(0);
        tryVerify(() => historyView().searchField.activeFocus);
        CommandRegistry.trigger("history.show");
        tryVerify(() => !shell.libraryVisible);
    }

    function test_panelFollowsF6AsPartOfTheSidebarRegion() {
        openPanel(0);
        tryVerify(() => historyView().searchField.activeFocus);
        compare(shell.regionOf(testCase.Window.activeFocusItem), 0);
        compare(shell.regions.length, 4);
    }

    // ---- history -----------------------------------------------------------------------------

    function test_historyListsAndCopiesToTheEditor() {
        emptyStores();
        addSqlite(newFile());
        openQuery("SELECT 41 + 1 AS answer");
        runCurrent("done");
        openPanel(0);
        tryCompare(shell.historyModel, "count", 1);
        const list = historyView().entries;
        const row = rowAt(list, 0);
        compare(row.Accessible.role, Accessible.ListItem);
        compare(row.Accessible.name, "SELECT 41 + 1 AS answer");
        verify(row.Accessible.description.startsWith("Succeeded"), row.Accessible.description);

        // Down from the search field goes into the list; Enter copies, it never runs.
        keyClick(Qt.Key_Down);
        verify(list.activeFocus);
        keyClick(Qt.Key_Return);
        tryVerify(() => editor().edit.text.endsWith("SELECT 41 + 1 AS answer;"));
        tryVerify(() => editor().edit.activeFocus);
        compare(current().state, "done");
        compare(tabs().textOf(tabs().currentIndex), "SELECT 41 + 1 AS answer;\n\nSELECT 41 + 1 AS answer;");
        compare(shell.editorArea.lastAnnouncement, "Copied to Query 1");
        compare(shell.historyModel.count, 1); // copying is not running
    }

    function test_historyRunAddsTheStatementAndRunsIt() {
        emptyStores();
        addSqlite(newFile());
        openQuery("SELECT 1 AS first");
        runCurrent("done");
        openPanel(0);
        tryCompare(shell.historyModel, "count", 1);
        rowAt(historyView().entries, 0);
        const run = find(historyView(), i => i.text === "Run" && i.clicked !== undefined);
        verify(run !== undefined);
        tryVerify(() => run.enabled);
        mouseClick(run);
        // The statement ran again: a second entry, newest first.
        tryCompare(shell.historyModel, "count", 2, 20000);
        tryVerify(() => editor().edit.text.endsWith("SELECT 1 AS first;"));
    }

    function test_historyFailureShowsErrorAndFilter() {
        emptyStores();
        addSqlite(newFile());
        openQuery("SELECT 1 AS fine");
        runCurrent("done");
        editor().edit.text = "SELECT * FROM missing_table";
        runCurrent("failed");
        openPanel(0);
        tryCompare(shell.historyModel, "count", 2);
        const list = historyView().entries;
        const failed = rowAt(list, 0);
        verify(failed.Accessible.description.startsWith("Failed"), failed.Accessible.description);
        // "Only errors" keeps the failed one.
        mouseClick(historyView().onlyErrorsBox);
        tryCompare(shell.historyModel, "count", 1);
        tryVerify(() => rowAt(list, 0).Accessible.name === "SELECT * FROM missing_table");
        compare(historyView().filtered, true);
    }

    function test_historySearchIsDebouncedAndAnnounced() {
        emptyStores();
        addSqlite(newFile());
        openQuery("SELECT 1 AS alpha");
        runCurrent("done");
        editor().edit.text = "SELECT 2 AS bravo";
        runCurrent("done");
        openPanel(0);
        tryCompare(shell.historyModel, "count", 2);
        const search = historyView().searchField;
        tryVerify(() => search.activeFocus);
        typeText("bravo");
        // Not at every key: the model waits for the typing to stop.
        tryCompare(shell.historyModel, "count", 1);
        compare(shell.historyModel.get(0).sql, "SELECT 2 AS bravo");
        compare(historyView().lastAnnouncement, "1 history entry");
        search.text = "";
        search.textEdited();
        tryCompare(shell.historyModel, "count", 2);
        // Nothing matches: the empty state explains it.
        typeText("zzzz");
        tryCompare(shell.historyModel, "count", 0);
        tryVerify(() => find(historyView(), i => i.title === "No matching history") !== undefined);
        compare(historyView().lastAnnouncement, "No history entries");
    }

    function test_clearHistoryAsksAndNamesTheScope() {
        emptyStores();
        const id = addSqlite(newFile());
        openQuery("SELECT 1 AS alpha");
        runCurrent("done");
        openPanel(0);
        tryCompare(shell.historyModel, "count", 1);
        const view = historyView();
        mouseClick(view.clearButton);
        tryVerify(() => view.clearConfirm.opened);
        // The focus starts on the safe button and the destructive one is explicit.
        verify(view.clearConfirm.cancelButton.activeFocus);
        compare(view.clearConfirm.confirmButton.variant, "danger");
        compare(view.clearConfirm.confirmButton.text, "Clear all history");
        verify(view.clearConfirm.message.indexOf("all connections") >= 0, view.clearConfirm.message);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !view.clearConfirm.visible);
        compare(shell.historyModel.count, 1);

        // With the connection filter on, the scope is that connection.
        mouseClick(view.onlyConnectionBox);
        compare(shell.historyModel.connectionFilter, id);
        mouseClick(view.clearButton);
        tryVerify(() => view.clearConfirm.opened);
        verify(view.clearConfirm.confirmButton.text.startsWith("Clear history of"), view.clearConfirm.confirmButton.text);
        mouseClick(view.clearConfirm.confirmButton);
        tryCompare(shell.historyModel, "count", 0);
        // The focus came back to the control that opened the dialog.
        tryVerify(() => view.clearButton.activeFocus);
    }

    function test_deleteEntryFromTheKeyboard() {
        emptyStores();
        addSqlite(newFile());
        openQuery("SELECT 1 AS alpha");
        runCurrent("done");
        openPanel(0);
        tryCompare(shell.historyModel, "count", 1);
        rowAt(historyView().entries, 0);
        keyClick(Qt.Key_Down);
        verify(historyView().entries.activeFocus);
        keyClick(Qt.Key_Delete);
        tryCompare(shell.historyModel, "count", 0);
        compare(historyView().lastAnnouncement, "History entry deleted");
    }

    // ---- privacy -----------------------------------------------------------------------------

    function openEdit(id) {
        shell.openConnectionDialog(id, false);
        tryVerify(() => shell.connectionDialog?.opened);
        return shell.connectionDialog;
    }

    function turnHistoryOff(id) {
        const dialog = openEdit(id);
        const box = dialog.noHistoryBox;
        verify(!box.checked);
        compare(box.Accessible.role, Accessible.CheckBox);
        compare(box.text, "Don't save query history for this connection");
        box.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(box.checked);
        compare(dialog.draft().recordHistory, false);
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        dialog.trySave();
        tryCompare(spy, "count", 1);
    }

    // Turning it off asks about the history that exists; "Keep history" is the safe default.
    function test_turningHistoryOffAsksAndKeepsByDefault() {
        emptyStores();
        const id = addSqlite(newFile());
        openQuery("SELECT 1 AS secret");
        runCurrent("done");
        openPanel(0);
        tryCompare(shell.historyModel, "count", 1);
        shell.hideLibrary();

        turnHistoryOff(id);
        tryVerify(() => shell.historyOptOutDialog?.opened);
        const dialog = shell.historyOptOutDialog;
        compare(dialog.cancelButton.text, "Keep history");
        compare(dialog.confirmButton.text, "Delete history");
        compare(dialog.confirmButton.variant, "danger");
        verify(dialog.cancelButton.activeFocus, "the focus starts on the button that deletes nothing");
        verify(dialog.message.indexOf("Delete the history it already has too?") >= 0, dialog.message);
        keyClick(Qt.Key_Return); // the default button: keep
        tryVerify(() => !dialog.visible);
        // Nothing was deleted.
        shell.historyModel.refresh();
        tryCompare(shell.historyModel, "count", 1);
        // The tab's connection does not record: the status bar says so in words.
        tryVerify(() => shell.tabHistoryOff);
        verify(find(shell.statusItem, i => i.text === "History off") !== undefined);
        verify(shell.statusItem.Accessible.name.indexOf("History is off for this connection") >= 0);
        // New statements are not recorded (a failing one: the state changes, so the wait is real).
        editor().edit.text = "SELECT * FROM missing_table";
        runCurrent("failed");
        wait(300);
        shell.historyModel.refresh();
        wait(300);
        compare(shell.historyModel.count, 1);
    }

    function test_turningHistoryOffCanDeleteWhatExists() {
        emptyStores();
        const id = addSqlite(newFile());
        openQuery("SELECT 1 AS secret");
        runCurrent("done");
        openPanel(0);
        tryCompare(shell.historyModel, "count", 1);
        shell.hideLibrary();

        turnHistoryOff(id);
        tryVerify(() => shell.historyOptOutDialog?.opened);
        mouseClick(shell.historyOptOutDialog.confirmButton);
        shell.historyModel.refresh();
        tryCompare(shell.historyModel, "count", 0);
    }

    // A connection that never recorded has nothing to delete: no question.
    function test_noQuestionWhenNothingChangesForHistory() {
        const id = addSqlite(newFile());
        const dialog = openEdit(id);
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "historyDisabled"
        });
        dialog.trySave();
        wait(300);
        compare(spy.count, 0);
        verify(!shell.historyOptOutDialog?.visible);
    }

    // ---- saved queries -----------------------------------------------------------------------

    function test_saveCurrentQueryUsesTheStatementUnderTheCursor() {
        emptyStores();
        addSqlite(newFile());
        openQuery("SELECT 1 AS one;\nSELECT 2 AS two;");
        editor().edit.cursorPosition = editor().edit.length - 3;
        verify(CommandRegistry.find("saved.saveCurrent").enabled);
        keyClick(Qt.Key_S, Qt.ControlModifier | Qt.ShiftModifier);
        tryVerify(() => shell.library !== null && panel().tabIndex === 1);
        const dialog = savedView().nameDialog;
        tryVerify(() => dialog.opened);
        tryVerify(() => dialog.field.activeFocus);
        verify(dialog.detail.startsWith("SELECT 2 AS two"), dialog.detail);
        // An empty name is not accepted, and the reason is on screen.
        verify(!dialog.confirmButton.enabled);
        keyClick(Qt.Key_Return);
        verify(dialog.visible);
        typeText("Second");
        verify(dialog.confirmButton.enabled);
        keyClick(Qt.Key_Return);
        tryCompare(shell.savedModel, "queryCount", 1);
        tryVerify(() => !dialog.visible);
        const row = rowAt(savedView().tree, 0);
        compare(row.Accessible.role, Accessible.TreeItem);
        compare(row.Accessible.name, "Second");
        verify(row.Accessible.description.startsWith("Saved query"), row.Accessible.description);
        compare(shell.savedModel.get(0).sql.indexOf("SELECT 2 AS two"), 0);
    }

    function test_saveCurrentQueryUsesTheSelection() {
        emptyStores();
        addSqlite(newFile());
        openQuery("SELECT 1 AS one;\nSELECT 2 AS two;");
        editor().edit.select(0, 16);
        CommandRegistry.trigger("saved.saveCurrent");
        tryVerify(() => shell.library !== null && savedView().nameDialog.opened);
        compare(savedView().nameDialog.detail, "SELECT 1 AS one;");
    }

    function test_foldersAreATwoLevelAccessibleTree() {
        emptyStores();
        shell.savedModel.newFolder("Reports");
        tryCompare(shell.savedModel, "folderCount", 1);
        const folder = shell.savedModel.get(0).itemId;
        shell.savedModel.saveQuery("Monthly", "SELECT 1", "", folder);
        shell.savedModel.saveQuery("Daily", "SELECT 2", "", folder);
        shell.savedModel.saveQuery("Scratch", "SELECT 3", "", "");
        tryCompare(shell.savedModel, "queryCount", 3);
        openPanel(1);
        const tree = savedView().tree;
        tryVerify(() => shell.savedModel.count >= 3);
        tree.currentIndex = 0;
        tree.forceActiveFocus(Qt.TabFocusReason);

        let row = rowAt(tree, 0);
        compare(row.Accessible.role, Accessible.TreeItem);
        verify(row.Accessible.expandable);
        verify(row.Accessible.description.startsWith("Folder, 2 queries, level 1, 1 of 2"), row.Accessible.description);
        // Left on a collapsed or Right on an expanded folder: collapse and expand with arrows.
        const wasExpanded = row.expanded;
        keyClick(wasExpanded ? Qt.Key_Left : Qt.Key_Right);
        tryVerify(() => rowAt(tree, 0).expanded === !wasExpanded);
        if (wasExpanded)
            keyClick(Qt.Key_Right);
        tryVerify(() => rowAt(tree, 0).expanded);
        compare(shell.savedModel.count, 4);
        // Right again goes to the first child; Left goes back to the parent.
        keyClick(Qt.Key_Right);
        compare(tree.currentIndex, 1);
        const child = rowAt(tree, 1);
        verify(child.Accessible.description.indexOf("level 2") >= 0, child.Accessible.description);
        keyClick(Qt.Key_Left);
        compare(tree.currentIndex, 0);
        keyClick(Qt.Key_End);
        compare(tree.currentIndex, 3);
        compare(rowAt(tree, 3).Accessible.name, "Scratch");
    }

    function test_moveToFolderWithKeyboardOnly() {
        emptyStores();
        shell.savedModel.newFolder("Reports");
        tryCompare(shell.savedModel, "folderCount", 1);
        shell.savedModel.saveQuery("Scratch", "SELECT 3", "", "");
        tryCompare(shell.savedModel, "queryCount", 1);
        openPanel(1);
        const view = savedView();
        const tree = view.tree;
        tryVerify(() => shell.savedModel.count === 2);
        tree.currentIndex = 1; // the query (the folder is collapsed)
        tree.forceActiveFocus(Qt.TabFocusReason);
        rowAt(tree, 1);
        // Menu key: the actions of the query; "Move to…" is the third.
        keyClick(Qt.Key_F10, Qt.ShiftModifier);
        tryVerify(() => view.queryMenu.opened);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryVerify(() => view.moveDialog.opened);
        compare(view.moveDialog.targets.length, 2);
        compare(view.moveDialog.targetsList.Accessible.role, Accessible.List);
        verify(!view.moveDialog.moveButton.enabled, "it is already at the top level");
        keyClick(Qt.Key_Down);
        verify(view.moveDialog.moveButton.enabled);
        keyClick(Qt.Key_Return);
        tryVerify(() => !view.moveDialog.visible);
        tryVerify(() => shell.savedModel.get(1).depth === 1);
        compare(shell.savedModel.get(1).name, "Scratch");
        compare(view.lastAnnouncement, "“Scratch” moved to folder Reports");
    }

    function test_deleteFolderSaysWhatHappensToItsQueries() {
        emptyStores();
        shell.savedModel.newFolder("Reports");
        tryCompare(shell.savedModel, "folderCount", 1);
        shell.savedModel.saveQuery("Monthly", "SELECT 1", "", shell.savedModel.get(0).itemId);
        tryCompare(shell.savedModel, "queryCount", 1);
        openPanel(1);
        const view = savedView();
        const tree = view.tree;
        tryVerify(() => shell.savedModel.count >= 1);
        tree.currentIndex = 0;
        tree.forceActiveFocus(Qt.TabFocusReason);
        rowAt(tree, 0);
        keyClick(Qt.Key_Delete);
        tryVerify(() => view.confirmDialog.opened);
        verify(view.confirmDialog.cancelButton.activeFocus);
        verify(view.confirmDialog.message.indexOf("top level") >= 0, view.confirmDialog.message);
        compare(view.confirmDialog.confirmButton.variant, "danger");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !view.confirmDialog.visible);
        compare(shell.savedModel.folderCount, 1);

        keyClick(Qt.Key_Delete);
        tryVerify(() => view.confirmDialog.opened);
        mouseClick(view.confirmDialog.confirmButton);
        tryCompare(shell.savedModel, "folderCount", 0);
        compare(shell.savedModel.queryCount, 1); // never deleted with the folder
        compare(shell.savedModel.get(0).depth, 0);
    }

    function test_renameWithF2() {
        emptyStores();
        shell.savedModel.saveQuery("Old name", "SELECT 3", "", "");
        tryCompare(shell.savedModel, "queryCount", 1);
        openPanel(1);
        const view = savedView();
        tryVerify(() => shell.savedModel.count === 1);
        view.tree.currentIndex = 0;
        view.tree.forceActiveFocus(Qt.TabFocusReason);
        rowAt(view.tree, 0);
        keyClick(Qt.Key_F2);
        tryVerify(() => view.nameDialog.opened);
        compare(view.nameDialog.field.text, "Old name");
        view.nameDialog.field.text = "";
        typeText("New name");
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.savedModel.get(0).name === "New name");
    }

    // Opening a saved query copies it; a connection the tab lacks is offered, never applied.
    function test_openSavedQueryOffersItsConnection() {
        emptyStores();
        const id = addSqlite(newFile());
        const name = model().get(model().indexOf(id)).name;
        shell.savedModel.saveQuery("For db", "SELECT 7 AS seven", id, "");
        tryCompare(shell.savedModel, "queryCount", 1);
        openQuery("");
        tabs().setConnection(tabs().currentIndex, "");
        tryVerify(() => editor().connectionId === "");
        openPanel(1);
        const view = savedView();
        tryVerify(() => shell.savedModel.count === 1);
        view.tree.currentIndex = 0;
        view.tree.forceActiveFocus(Qt.TabFocusReason);
        rowAt(view.tree, 0);
        keyClick(Qt.Key_Return);
        tryVerify(() => editor().edit.text.indexOf("SELECT 7 AS seven;") >= 0);
        // The tab was not changed on its own...
        compare(tabs().connectionIdOf(tabs().currentIndex), "");
        // ...and the offer is there, with its button.
        tryVerify(() => view.offer !== null);
        compare(view.offer.name, name);
        tryVerify(() => view.offerButton.visible);
        compare(view.offerButton.text, "Use " + name);
        view.offerButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryVerify(() => tabs().connectionIdOf(tabs().currentIndex) === id);
        compare(view.offer, null);
    }

    // The sidebar keeps its tree: the panel is below it, in the same region.
    function test_treeStaysUsableWithThePanelOpen() {
        addSqlite(newFile());
        openPanel(0);
        const tree = shell.sidebar;
        verify(tree.height >= Theme.controlHeight * 5, "the tree keeps room: " + tree.height);
        verify(panel().height >= Theme.controlHeight * 8, "the panel has room: " + panel().height);
        // At 200 % zoom the panel is still inside the window and the controls are reachable.
        Theme.zoomIn();
        Theme.zoomIn();
        Theme.zoomIn();
        wait(50);
        verify(panel().height > 0);
        compare(walker.problems(panel()), []);
    }

    function test_panelPassesTheAccessibilityWalkerInHighContrast() {
        Theme.mode = Theme.Mode.HighContrast;
        openPanel(0);
        compare(walker.problems(panel()), []);
        openPanel(1);
        compare(walker.problems(panel()), []);
    }

    function test_spanishStrings() {
        Language.choice = Language.Choice.Spanish;
        compare(CommandRegistry.find("history.show").title, "Mostrar el historial");
        openPanel(0);
        compare(historyView().clearButton.text, "Borrar el historial…");
        compare(panel().tabBar.accessibleName, "Historial y consultas guardadas");
        openPanel(1);
        compare(savedView().saveButton.text, "Guardar la consulta actual…");
        compare(savedView().tree.Accessible.name, "Consultas guardadas");
    }
}
