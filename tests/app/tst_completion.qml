import QtQuick
import QtTest
import Quarry.Controls

// Autocompletion in the SQL editor (H3b), driven the way a keyboard-only user would be: the popup
// opens by itself after "." and two characters, always with Mod+Space, takes Up/Down/PageUp/
// PageDown/Home/End, Enter and Tab accept, Esc closes; announcements; the setting; the palette's
// "@". Temporary connection list, the real SQLite plugin over the sample database (customers,
// invoices, items, products and the view totals), no keychain.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0
    property int originalMode: 0
    readonly property string slowSql: "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 100000000000) SELECT count(*) FROM c"
    readonly property int suggestKeyModifier: Qt.platform.os === "osx" ? Qt.MetaModifier : Qt.ControlModifier

    name: "Completion"
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

    // ---- helpers -----------------------------------------------------------------------------

    function tabs() {
        return shell.editorTabs;
    }

    function area() {
        return shell.editorArea;
    }

    function editor() {
        return shell.editorArea.currentEditor;
    }

    function model() {
        return shell.connections.model;
    }

    function completion() {
        return shell.completion;
    }

    function overlay() {
        return shell.overlayItem;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function newFile() {
        return testTempDir + "/completion-data-" + (++fileCounter) + ".db";
    }

    function typeText(text) {
        for (const ch of text) {
            if (ch === "\n")
                keyClick(Qt.Key_Return);
            else if (ch === " ")
                keyClick(Qt.Key_Space);
            else
                keyClick(ch);
        }
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

    // Mod+T, and waits for the tab and its editor (the shortcut and the views catch up a pass later).
    function newTab() {
        const n = tabs().count;
        // Another program may have taken the focus from the test window: keys need an active one.
        testCase.Window.window.requestActivate();
        shell.focusRegion(1); // a closed tab takes the focus with it, and keys need an item to reach
        keyClick(Qt.Key_T, Qt.ControlModifier);
        tryCompare(tabs(), "count", n + 1, 5000);
        waitForViews();
    }

    // A tab on the sample database, the editor focused. Returns the connection id.
    function openEditor(file) {
        const id = addSqlite(file ?? testSampleDb);
        shell.sidebar.selectId(id);
        tryVerify(() => shell.sidebar.currentId === id);
        newTab();
        tryVerify(() => editor().edit.activeFocus);
        return id;
    }

    function popup() {
        return editor().suggestionPopup;
    }

    function popupShown() {
        const p = editor() ? editor().suggestionPopup : null;
        return p !== null && p.visible;
    }

    function labels() {
        const m = completion().model;
        const out = [];
        for (let i = 0; i < m.count; ++i)
            out.push(m.labelAt(i));
        return out;
    }

    function closeOverlays() {
        for (const d of [shell.closeTabDialog, shell.connectionDialog, shell.deleteDialog, shell.paletteDialog, shell.settingsDialog]) {
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        }
        tryVerify(() => ![shell.closeTabDialog, shell.connectionDialog, shell.deleteDialog, shell.paletteDialog, shell.settingsDialog].some(d => d && d.visible), 5000);
    }

    function removeAll() {
        while (tabs().count > 0)
            tabs().closeTab(0);
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
        tryCompare(model(), "count", 0);
    }

    function createShell() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        const s = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/completion.db",
            providerDirectory: testPluginDir
        });
        verify(s !== null);
        return s;
    }

    // Puts everything back: popups, dialogs and tabs closed, language, theme, zoom and the setting.
    function reset() {
        if (shell === null)
            return;
        if (editor())
            editor().closeSuggestions();
        closeOverlays();
        Language.choice = Language.Choice.English;
        Theme.resetZoom();
        Theme.mode = originalMode;
        shell.completion.autoOpen = true;
        removeAll();
    }

    function initTestCase() {
        originalMode = Theme.mode;
        shell = createShell();
        tryVerify(() => !model().loading, 10000);
        tryVerify(() => tabs().loaded, 10000);
    }

    function init() {
        reset();
        shell.resetLayout();
        shell.focusRegion(1);
    }

    function cleanup() {
        reset();
    }

    function cleanupTestCase() {
        reset();
        shell.destroy();
        shell = null;
    }

    // ---- opening -----------------------------------------------------------------------------

    function test_opensByItselfAfterTwoCharacters() {
        openEditor();
        typeText("SELECT * FROM c");
        wait(400);
        verify(!popupShown(), "one character is not enough");
        typeText("u");
        // The tables are read on the first request: the popup opens when they arrive.
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.labelAt(0) === "customers");
        compare(completion().model.count, 1);
        compare(completion().model.kindTextAt(0), "table");
        verify(editor().edit.activeFocus, "the editor keeps the focus");
        compare(editor().edit.text, "SELECT * FROM cu");
    }

    function test_opensAfterADotWithTheColumns() {
        openEditor();
        typeText("SELECT customers.");
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.count === 2);
        compare(labels(), ["id", "name"]);
        compare(completion().model.kindTextAt(1), "column");
        verify(editor().edit.activeFocus);
        // Keeps filtering as the word grows.
        typeText("na");
        tryVerify(() => completion().model.count === 1);
        compare(labels(), ["name"]);
    }

    function test_modSpaceOpensEvenWithAutoOpenOff() {
        openEditor();
        completion().autoOpen = false;
        typeText("SELECT * FROM cu");
        wait(400);
        verify(!popupShown(), "the setting is off");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.labelAt(0) === "customers");
        compare(editor().edit.text, "SELECT * FROM cu", "Mod+Space inserts nothing");
        keyClick(Qt.Key_Escape);
        verify(!popupShown());
    }

    function test_neverOpensInsideStringsOrComments() {
        openEditor();
        typeText("SELECT 'cu");
        wait(500);
        verify(!popupShown());
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        wait(300);
        verify(!popupShown());
        compare(editor().lastAnnouncement, "No suggestions");
        keyClick(Qt.Key_A, Qt.ControlModifier);
        keyClick(Qt.Key_Delete);
        typeText("-- cu");
        wait(500);
        verify(!popupShown());
    }

    function test_noConnectionShowsTheNoticeAsText() {
        newTab();
        compare(tabs().connectionIdOf(0), "");
        typeText("SEL");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown());
        const notice = find(popup(), i => i.text === "Connect to suggest tables");
        verify(notice !== undefined, "the notice is shown as text");
        compare(notice.Accessible.role, Accessible.StaticText);
        compare(notice.Accessible.name, "Connect to suggest tables");
        tryVerify(() => editor().lastAnnouncement === "Connect to suggest tables");
        keyClick(Qt.Key_Escape);
        verify(!popupShown());
    }

    // ---- keys --------------------------------------------------------------------------------

    function test_enterAccepts() {
        openEditor();
        typeText("SELECT * FROM cu");
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.labelAt(0) === "customers");
        keyClick(Qt.Key_Return);
        compare(editor().edit.text, "SELECT * FROM customers");
        compare(editor().edit.cursorPosition, 23);
        verify(!popupShown());
        wait(400);
        verify(!popupShown(), "an accepted word does not open the popup again");
        compare(tabs().textOf(0), "SELECT * FROM customers");
        // One undo step brings the typed word back.
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(editor().edit.text, "SELECT * FROM cu");
    }

    function test_tabAcceptsOnlyWhileOpen() {
        openEditor();
        typeText("SELECT * FROM pr");
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.labelAt(0) === "products");
        keyClick(Qt.Key_Tab);
        compare(editor().edit.text, "SELECT * FROM products");
        verify(editor().edit.activeFocus, "Tab did not leave the editor");
        // Closed again: the old behaviour, an indent.
        keyClick(Qt.Key_Tab);
        compare(editor().edit.text, "SELECT * FROM products\t");
    }

    function test_arrowsPagesHomeEndMoveInTheList() {
        openEditor();
        typeText("SELECT * FROM ");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.count >= 6);
        const n = completion().model.count;
        const list = popup().listView;
        compare(list.currentIndex, 0);
        keyClick(Qt.Key_Down);
        compare(list.currentIndex, 1);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Up);
        compare(list.currentIndex, 1);
        keyClick(Qt.Key_End);
        compare(list.currentIndex, n - 1);
        keyClick(Qt.Key_Down);
        compare(list.currentIndex, n - 1, "it stops at the end");
        keyClick(Qt.Key_Home);
        compare(list.currentIndex, 0);
        keyClick(Qt.Key_PageDown);
        verify(list.currentIndex > 0);
        keyClick(Qt.Key_PageUp);
        compare(list.currentIndex, 0);
        compare(editor().edit.cursorPosition, 14, "the cursor never moved");
        // Accepting takes the current row.
        keyClick(Qt.Key_Down);
        const label = completion().model.labelAt(1);
        keyClick(Qt.Key_Return);
        verify(editor().edit.text.startsWith("SELECT * FROM " + label), editor().edit.text);
    }

    function test_escapeClosesOnlyThePopupNotARunningQuery() {
        openEditor();
        typeText(slowSql + ";");
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(shell.queries.current, "running", true, 10000);
        typeText("\nSELECT * FROM cu");
        tryVerify(() => popupShown(), 10000);
        keyClick(Qt.Key_Escape);
        verify(!popupShown());
        verify(shell.queries.current.running, "the first Esc only closed the popup");
        compare(editor().edit.text, slowSql + ";\nSELECT * FROM cu");
        keyClick(Qt.Key_Escape); // no popup: it cancels, as before
        tryCompare(shell.queries.current, "running", false, 10000);
    }

    function test_typingKeepsFilteringAndMovingAwayCloses() {
        openEditor();
        typeText("SELECT * FROM ");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.count >= 6);
        typeText("i");
        tryVerify(() => completion().model.count === 3);
        compare(labels(), ["invoices", "items", "main"]); // names that contain it come last
        typeText("nv");
        tryVerify(() => completion().model.count === 1);
        compare(labels(), ["invoices"]);
        keyClick(Qt.Key_Backspace);
        keyClick(Qt.Key_Backspace);
        tryVerify(() => completion().model.count === 3);
        verify(popupShown());
        // Out of the word: the cursor goes to the end of FROM.
        for (let i = 0; i < 3; ++i)
            keyClick(Qt.Key_Left);
        tryVerify(() => !popupShown());
    }

    function test_clickingARowAccepts() {
        openEditor();
        typeText("SELECT * FROM pr");
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.labelAt(0) === "products");
        tryVerify(() => popup().listView.itemAtIndex(0) !== null);
        mouseClick(popup().listView.itemAtIndex(0));
        compare(editor().edit.text, "SELECT * FROM products");
        verify(editor().edit.activeFocus);
    }

    function test_theClosingQuoteIsReplacedToo() {
        openEditor();
        typeText("SELECT * FROM \"cu\"");
        keyClick(Qt.Key_Left);
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.labelAt(0) === "customers");
        keyClick(Qt.Key_Return);
        compare(editor().edit.text, "SELECT * FROM customers");
    }

    function test_secondStatementOffsets() {
        openEditor();
        typeText("SELECT 1;\nSELECT * FROM in");
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.labelAt(0) === "invoices");
        keyClick(Qt.Key_Return);
        compare(editor().edit.text, "SELECT 1;\nSELECT * FROM invoices");
    }

    // ---- announcements -----------------------------------------------------------------------

    function test_announcesTheCountAndTheCurrentItem() {
        openEditor();
        typeText("SELECT * FROM ");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.count >= 6);
        const n = completion().model.count;
        const m = completion().model;
        tryVerify(() => editor().lastAnnouncement === n + " suggestions. " + m.labelAt(0) + ", " + m.kindTextAt(0), 5000, editor().lastAnnouncement);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        tryVerify(() => editor().lastAnnouncement === m.labelAt(2) + ", " + m.kindTextAt(2), 5000, editor().lastAnnouncement);
        typeText("cu");
        tryVerify(() => editor().lastAnnouncement === "1 suggestion. customers, table", 5000, editor().lastAnnouncement);
    }

    // ---- the popup as an accessible list ------------------------------------------------------

    function test_walkerAndRoles() {
        openEditor();
        typeText("SELECT * FROM ");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.count >= 6);
        compare(walker.problems(shell), []);
        const list = popup().listView;
        compare(list.Accessible.role, Accessible.List);
        compare(list.Accessible.name, "Suggestions");
        tryVerify(() => list.itemAtIndex(0) !== null);
        const row = list.itemAtIndex(0);
        compare(row.Accessible.role, Accessible.ListItem);
        verify(row.Accessible.selected);
        const m = completion().model;
        compare(row.Accessible.name, m.labelAt(0) + ", " + m.kindTextAt(0) + (row.detail !== "" ? ", " + row.detail : ""));
        // An icon and the kind as a word, never color only.
        verify(find(row, i => i.font !== undefined && i.font.family === Icons.fontFamily && i.text !== "") !== undefined, "no icon");
        verify(find(row, i => i.text === m.kindTextAt(0)) !== undefined, "no kind word");
        const other = list.itemAtIndex(1);
        verify(!other.Accessible.selected);
    }

    function test_atTwoHundredPercentAndFlippedAboveTheCaret() {
        openEditor();
        Theme.zoom = 2.0;
        // A long file with the cursor at the end: the caret is at the bottom of the view, so the
        // list goes above it.
        const lines = [];
        for (let i = 0; i < 80; ++i)
            lines.push("SELECT " + i + ";");
        editor().edit.text = lines.join("\n") + "\nSELECT * FROM ";
        editor().edit.cursorPosition = editor().edit.length;
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.count >= 6);
        const p = popup();
        tryVerify(() => p.flipped, 5000, "no room below the caret");
        tryVerify(() => p.y + p.height <= editor().caretRect.y + 1);
        verify(p.x >= 0 && p.x + p.width <= editor().width + 1, "within the editor, sideways");
        verify(p.y >= 0, "within the editor, above");
        tryVerify(() => p.listView.itemAtIndex(0) !== null);
        verify(p.listView.itemAtIndex(0).height >= Theme.controlHeight, "rows grow with the zoom");
        verify(find(p, i => i.text === completion().model.kindTextAt(0)) !== undefined);
        compare(walker.problems(shell), []);
        // The caret goes to the top: the list opens below it again.
        keyClick(Qt.Key_Escape);
        editor().edit.cursorPosition = 0;
        tryVerify(() => editor().caretRect.y < editor().height / 2);
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown() && !popup().flipped);
        tryVerify(() => popup().y >= editor().caretRect.y + editor().caretRect.height - 1);
    }

    function test_highContrastUsesTheTokens() {
        openEditor();
        Theme.mode = Theme.Mode.HighContrast;
        verify(Theme.isHighContrast);
        typeText("SELECT * FROM ");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.count >= 6);
        tryVerify(() => popup().listView.itemAtIndex(0) !== null);
        const p = popup();
        const frame = find(p, i => i.border !== undefined && i.color !== undefined && i.border.width === 1 && i.color.toString() === Theme.bgRaised.toString());
        verify(frame !== undefined, "background is bgRaised");
        compare(frame.border.color.toString(), Theme.borderStrong.toString());
        // The current row is marked by a bar in the accent color, not only by its fill.
        const row = p.listView.itemAtIndex(0);
        verify(find(row, i => i.color !== undefined && i.width === Theme.focusRingWidth && i.color.toString() === Theme.accent.toString()) !== undefined, "no marker on the current row");
        compare(walker.problems(shell), []);
    }

    // ---- commands, settings -------------------------------------------------------------------

    function test_commandsAreRegistered() {
        const suggest = CommandRegistry.find("editor.suggest");
        verify(suggest !== null);
        compare(suggest.title, "Show suggestions");
        compare(suggest.category, "Editor");
        compare(suggest.shortcutText, CommandRegistry.displayShortcut(Qt.platform.os === "osx" ? "Meta+Space" : "Ctrl+Space"));
        const refresh = CommandRegistry.find("connection.refreshMetadata");
        verify(refresh !== null);
        compare(refresh.title, "Refresh metadata");
        verify(!refresh.enabled, "no connection yet");
        // No two commands share a shortcut.
        const seen = {};
        for (const c of CommandRegistry.commands.slice()) {
            for (const s of c.sequences) {
                verify(seen[s] === undefined, s + " is used by " + seen[s] + " and " + c.commandId);
                seen[s] = c.commandId;
            }
        }
        // Where the key means something else (the grid selects a column, the tree has no editor)
        // the command is off, so its shortcut is not taken.
        verify(suggest.enabled);
        shell.focusRegion(0);
        tryVerify(() => !suggest.enabled);
        shell.focusRegion(2);
        tryVerify(() => !suggest.enabled);
        shell.focusRegion(1);
        tryVerify(() => suggest.enabled);
        // Without a tab the command says so.
        CommandRegistry.trigger("editor.suggest");
        compare(area().lastAnnouncement, "No query is open.");
    }

    function test_showSuggestionsFromThePalette() {
        openEditor();
        typeText("SELECT * FROM cu");
        completion().autoOpen = false;
        keyClick(Qt.Key_K, Qt.ControlModifier);
        tryVerify(() => shell.paletteDialog?.opened);
        typeText("show sugg");
        tryVerify(() => walker.findAll(shell.paletteDialog.contentItem, i => i.Accessible.role === Accessible.List)[0].count === 1);
        keyClick(Qt.Key_Return);
        tryVerify(() => !shell.paletteDialog.visible);
        tryVerify(() => popupShown(), 10000);
    }

    function test_refreshMetadataDropsTheCache() {
        const id = openEditor();
        typeText("SELECT * FROM ");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => completion().cacheState(id) === "ready", 10000);
        keyClick(Qt.Key_Escape);
        const refresh = CommandRegistry.find("connection.refreshMetadata");
        verify(refresh.enabled);
        CommandRegistry.trigger("connection.refreshMetadata");
        compare(completion().cacheState(id), "empty");
        compare(area().lastAnnouncement, "Metadata refreshed");
        // The next request reads it again.
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().cacheState(id) === "ready", 10000);
    }

    function test_aStatementThatChangesTheSchemaDropsTheCache() {
        const id = openEditor(newFile());
        typeText("SELECT * FROM ");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => completion().cacheState(id) === "ready", 10000);
        keyClick(Qt.Key_A, Qt.ControlModifier);
        keyClick(Qt.Key_Delete);
        typeText("SELECT 1");
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(shell.queries.current, "state", "done", 10000);
        compare(completion().cacheState(id), "ready", "a SELECT keeps the cache");
        keyClick(Qt.Key_A, Qt.ControlModifier);
        keyClick(Qt.Key_Delete);
        typeText("CREATE TABLE zz_new(id)");
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryVerify(() => completion().cacheState(id) === "empty", 10000);
        typeText(";\nSELECT * FROM zz");
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.labelAt(0) === "zz_new");
    }

    function test_settingsDialogHasTheSwitch() {
        keyClick(Qt.Key_Comma, Qt.ControlModifier);
        tryVerify(() => shell.settingsDialog?.opened);
        const box = find(shell.settingsDialog.contentItem, i => i.text === "Suggest while typing" && i.checked !== undefined);
        verify(box !== undefined);
        verify(box.checked, "on by default");
        compare(box.Accessible.role, Accessible.CheckBox);
        compare(box.Accessible.name, "Suggest while typing");
        compare(walker.problems(shell.settingsDialog.contentItem), []);
        box.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(!completion().autoOpen);
        keyClick(Qt.Key_Space);
        verify(completion().autoOpen);
    }

    // ---- the palette's "@" ---------------------------------------------------------------------

    function paletteField() {
        return walker.findAll(overlay(), i => i.visible && i.placeholderText !== undefined && i.accessibleName !== undefined)[0];
    }

    function objectList() {
        return walker.findAll(shell.paletteDialog.contentItem, i => i.Accessible.role === Accessible.List && i.model === completion().objects)[0];
    }

    function openPalette() {
        keyClick(Qt.Key_K, Qt.ControlModifier);
        tryVerify(() => shell.paletteDialog?.opened);
        tryVerify(() => paletteField().activeFocus);
    }

    function test_atSearchesTablesAndViews() {
        openEditor();
        openPalette();
        typeText("@");
        tryVerify(() => objectList().count === 5, 10000);
        tryVerify(() => objectList().itemAtIndex(0) !== null);
        compare(objectList().Accessible.role, Accessible.List);
        typeText("inv");
        tryVerify(() => objectList().count === 1);
        tryVerify(() => objectList().itemAtIndex(0) !== null);
        const row = objectList().itemAtIndex(0);
        compare(row.Accessible.role, Accessible.ListItem);
        compare(row.Accessible.name, "invoices, table, main");
        verify(find(row, i => i.text === "table") !== undefined, "the kind word");
        verify(find(row, i => i.text === "main") !== undefined, "the schema");
        compare(walker.problems(overlay()), []);
        for (let i = 0; i < 3; ++i)
            keyClick(Qt.Key_Backspace);
        typeText("tot");
        tryVerify(() => objectList().count === 1);
        tryVerify(() => objectList().itemAtIndex(0) !== null);
        compare(objectList().itemAtIndex(0).Accessible.name, "totals, view, main");
        // The commands are back without the "@".
        keyClick(Qt.Key_A, Qt.ControlModifier);
        keyClick(Qt.Key_Delete);
        typeText("go to");
        tryVerify(() => walker.findAll(shell.paletteDialog.contentItem, i => i.Accessible.role === Accessible.List)[0].count === 3);
        verify(!objectList().visible);
    }

    function test_enterOnAnObjectSelectsItInTheTree() {
        openEditor();
        openPalette();
        typeText("@prod");
        tryVerify(() => objectList().count === 1, 10000);
        keyClick(Qt.Key_Down); // stays on the only row
        keyClick(Qt.Key_Return);
        tryVerify(() => !shell.paletteDialog.visible);
        // The connection is expanded, the database, the Tables folder, and the table is current.
        tryVerify(() => shell.sidebar.currentObject.name === "products", 10000);
        compare(shell.sidebar.currentObject.openable, true);
        tryVerify(() => shell.sidebar.tree.activeFocus, 5000, "the tree takes the focus");
        verify(shell.sidebarVisible);
        tryVerify(() => shell.sidebar.lastAnnouncement === "products selected in the tree", 5000, shell.sidebar.lastAnnouncement);
        // And Enter in the tree opens its data, as for any table.
        keyClick(Qt.Key_Return);
        tryVerify(() => area().currentData !== null, 10000);
    }

    function test_atWithoutAConnectionOrWithNoMatchIsSaidInText() {
        newTab();
        openPalette();
        typeText("@x");
        const msg = () => find(shell.paletteDialog.contentItem, i => i.text === "Connect to search tables and views");
        tryVerify(() => msg() !== undefined);
        compare(msg().Accessible.role, Accessible.StaticText);
        compare(msg().Accessible.name, "Connect to search tables and views");
        keyClick(Qt.Key_Return); // nothing to choose: stays open
        verify(shell.paletteDialog.visible);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !shell.paletteDialog.visible);

        removeAll();
        openEditor();
        openPalette();
        typeText("@qqqq");
        tryVerify(() => find(shell.paletteDialog.contentItem, i => i.text === "No tables or views match 'qqqq'") !== undefined, 10000);
        compare(walker.problems(overlay()), []);
    }

    // ---- translation -------------------------------------------------------------------------

    function test_spanishStrings() {
        Language.choice = Language.Choice.Spanish;
        compare(CommandRegistry.find("editor.suggest").title, "Mostrar sugerencias");
        compare(CommandRegistry.find("connection.refreshMetadata").title, "Actualizar metadatos");
        openEditor();
        typeText("SELECT * FROM ");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 10000);
        tryVerify(() => completion().model.count >= 6);
        compare(popup().listView.Accessible.name, "Sugerencias");
        const m = completion().model;
        const first = m.labelAt(0) + ", " + m.kindTextAt(0);
        tryVerify(() => editor().lastAnnouncement === m.count + " sugerencias. " + first, 5000, editor().lastAnnouncement);
        verify(["tabla", "vista", "esquema"].includes(m.kindTextAt(0)), m.kindTextAt(0));
        typeText("cu");
        tryVerify(() => editor().lastAnnouncement === "1 sugerencia. customers, tabla", 5000, editor().lastAnnouncement);
        keyClick(Qt.Key_Escape);

        keyClick(Qt.Key_Comma, Qt.ControlModifier);
        tryVerify(() => shell.settingsDialog?.opened);
        verify(find(shell.settingsDialog.contentItem, i => i.text === "Sugerir al escribir") !== undefined);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !shell.settingsDialog.visible);

        openPalette();
        typeText("@tot");
        tryVerify(() => objectList().count === 1, 10000);
        tryVerify(() => objectList().itemAtIndex(0) !== null);
        compare(objectList().itemAtIndex(0).Accessible.name, "totals, vista, main");
        compare(objectList().Accessible.name, "Tablas y vistas");
        keyClick(Qt.Key_A, Qt.ControlModifier);
        keyClick(Qt.Key_Delete);
        typeText("@qqqq");
        tryVerify(() => find(shell.paletteDialog.contentItem, i => i.text === "Ninguna tabla ni vista coincide con 'qqqq'") !== undefined);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !shell.paletteDialog.visible);

        removeAll();
        newTab();
        typeText("SEL");
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown());
        verify(find(popup(), i => i.text === "Conecta para sugerir tablas") !== undefined, walker.findAll(popup(), i => i.text !== undefined && i.visible).map(i => i.text).join("|"));
    }

    // ---- typing ------------------------------------------------------------------------------

    function bigText(lines) {
        const out = [];
        for (let i = 1; i <= lines; ++i)
            out.push(i % 7 === 0 ? "-- note " + i : "SELECT id, name FROM t" + i + " WHERE id = " + i + ";");
        return out.join("\n");
    }

    function percentile(sorted, q) {
        return sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
    }

    // Types `keys` at the end of the text, and returns how long each key took (key press to the
    // text, the highlighting and the model updated). `pause` is the wait after each key: a
    // short one leaves the debounce timers asleep, a long one lets the popup ask, filter and
    // announce between two keys.
    function typeKeys(keys, pause) {
        const times = [];
        for (const key of keys) {
            const t0 = Date.now();
            keyClick(key);
            times.push(Date.now() - t0);
            wait(pause);
        }
        return times.sort((a, b) => a - b);
    }

    // The budget (PERFORMANCE.md): typing in a 10 000 line file, p95 <= 16 ms per key. With the
    // popup open the same keys must cost no more than without it (plus the same harness slack as
    // the editor test), and the popup's own work (the request after the debounce) must stay far
    // below a frame, so it can never block a key.
    function test_typingWithThePopupOpenStaysWithinBudget() {
        openEditor();
        const ed = editor();
        let idle = 0;
        for (let i = 0; i < 20; ++i) {
            const t0 = Date.now();
            wait(5);
            idle = Math.max(idle, Date.now() - t0 - 5);
        }
        console.log("IDLE max wait overshoot " + idle + " ms");
        const noisy = idle > testMaxKeystrokeMs; // the verdict is relative to the same keys without a popup

        ed.edit.text = bigText(10000) + "\nSELECT * FROM ";
        tryCompare(ed.edit, "lineCount", 10001);
        tryVerify(() => tabs().textOf(0).length === ed.edit.text.length);
        ed.edit.cursorPosition = ed.edit.length;
        wait(300);
        const word = ["c", "u", "s", "t", "o", "m", "e", "r", "s"];
        const back = word.map(() => Qt.Key_Backspace);
        const keys = word.concat(back).concat(word).concat(back);

        // Without a popup: the setting off, so nothing opens.
        completion().autoOpen = false;
        const base = typeKeys(keys, 2);

        // Popup open, keys typed fast: the requests wait for the pause.
        completion().autoOpen = true;
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 15000);
        tryVerify(() => completion().cacheState(tabs().connectionIdOf(0)) === "ready", 15000);
        const fast = typeKeys(keys, 2);

        // Popup open, a pause after each key: every key is followed by the request, the
        // filtering and the announcement.
        keyClick(Qt.Key_Space, testCase.suggestKeyModifier);
        tryVerify(() => popupShown(), 15000);
        const slow = typeKeys(keys.slice(0, 24), 200);

        // The request itself, as the debounce runs it, in the 10 000 line file.
        const spent = [];
        const text = tabs().textOf(0);
        for (let i = 0; i < 30; ++i) {
            tabs().setText(0, text + " ".repeat(i + 1)); // edited since the last split, as after a key
            const t0 = Date.now();
            completion().query(0, text.length);
            spent.push(Date.now() - t0);
        }
        spent.sort((a, b) => a - b);
        console.log("COMPLETION typing 10k lines, " + keys.length + " keys: without popup p50 " + percentile(base, 0.5) + " p95 " + percentile(base, 0.95) + " max " + base[base.length - 1] + " ms; popup open, fast p50 " + percentile(fast, 0.5) + " p95 " + percentile(fast, 0.95) + " max " + fast[fast.length - 1] + " ms; popup open, requests between keys (" + slow.length + " keys) p50 " + percentile(slow, 0.5) + " p95 " + percentile(slow, 0.95) + " max " + slow[slow.length - 1] + " ms; query() p50 " + percentile(spent, 0.5) + " p95 " + percentile(spent, 0.95) + " max " + spent[spent.length - 1] + " ms");
        // The numbers are logged above whatever the machine; only the verdict needs a quiet one.
        if (noisy)
            skip("idle overshoot " + idle + " ms: too noisy to judge a " + testMaxKeystrokeMs + " ms budget");
        verify(percentile(fast, 0.95) <= testMaxKeystrokeMs + percentile(base, 0.95), "p95 " + percentile(fast, 0.95) + " ms with the popup open, " + percentile(base, 0.95) + " ms without");
        verify(percentile(slow, 0.95) <= testMaxKeystrokeMs + percentile(base, 0.95), "p95 " + percentile(slow, 0.95) + " ms with requests between keys, " + percentile(base, 0.95) + " ms without");
        verify(percentile(spent, 0.95) <= testMaxKeystrokeMs, "a request takes " + percentile(spent, 0.95) + " ms (p95)");
    }
}
