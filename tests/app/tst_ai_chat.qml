import QtQuick
import QtTest
import QtQuick.Templates as T
import Quarry.Controls

// The AI assistant of a query tab (H13 phase C): the panel to the right of the editor (closed at the
// start, opened by its button, its command and its shortcut), the reasons when it cannot work, a
// request with Ctrl+Enter, the states and the errors, and the review of the proposal over the editor
// with the keyboard. The `claude` that runs is the fake of tests/ai (FAKE_CLAUDE_MODE and
// FAKE_CLAUDE_RESULT pick what it does), never the real one. Temporary databases, the real SQLite
// plugin, no keychain. Nothing here runs SQL: the tests check that too.
TestCase {
    id: testCase

    property var shell: null
    readonly property string stdinFile: testTempDir + "/ai-chat-stdin.txt"
    readonly property string original: "SELECT * FROM customers;\nSELECT * FROM invoices;\nSELECT * FROM items;"
    // Three changes: the first select changes, a DELETE comes in (it changes data) and a select is added.
    readonly property string proposal: "Rewritten.\n<quarry-script>\nSELECT id FROM customers;\nSELECT * FROM invoices;\nDELETE FROM invoices WHERE id = 1;\nSELECT * FROM items;\nSELECT 1;\n</quarry-script>"
    // What Apply leaves when change 1 is accepted, change 2 rejected and change 3 left pending.
    readonly property string partial: "SELECT id FROM customers;\nSELECT * FROM invoices;\nSELECT * FROM items;"

    name: "AiChat"
    when: windowShown
    width: 1100
    height: 760
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
        return area().currentEditor;
    }

    function model() {
        return shell.connections.model;
    }

    function panel() {
        return area().chatPanel;
    }

    function chat() {
        return area().currentChat;
    }

    function review() {
        return area().review;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function visibleTexts(root, text) {
        return walker.findAll(root, i => i.visible && i.text === text && i.contentWidth !== undefined);
    }

    function setFake(mode, result) {
        a11yProbe.setEnvironment("FAKE_CLAUDE_MODE", mode);
        a11yProbe.setEnvironment("FAKE_CLAUDE_RESULT", result ?? "");
        a11yProbe.setEnvironment("FAKE_CLAUDE_STDIN", stdinFile);
    }

    function waitForViews() {
        tryVerify(() => shell.sidebar.tree === null ? model().count === 0 : shell.sidebar.tree.rows >= model().count, 20000);
        if (tabs().count > 0)
            tryVerify(() => editor() !== null && editor().visible);
    }

    // The sample database as a connection, with the AI turned on for it (or not).
    function addSqlite(enabled) {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        shell.connections.openSqliteFile(Qt.url("file:///" + testSampleDb));
        tryCompare(spy, "count", 1);
        waitForViews();
        const id = spy.signalArguments[0][0];
        if (enabled) {
            verify(shell.ai.saveSettings(id, {
                "enabled": true
            }));
            tryVerify(() => shell.ai.settingsReady && shell.ai.isEnabled(id), 10000);
        }
        return id;
    }

    // A query tab with `sql`, the editor focused.
    function openQuery(sql) {
        shell.focusRegion(1);
        keyClick(Qt.Key_T, Qt.ControlModifier);
        waitForViews();
        tryVerify(() => editor().edit.activeFocus);
        editor().edit.text = sql;
        compare(tabs().textOf(tabs().currentIndex), sql);
    }

    function openPanel() {
        area().aiButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryVerify(() => area().aiPanelVisible);
        tryVerify(() => panel() !== null && panel().visible && panel().width > 0);
        tryVerify(() => panel().input.activeFocus || !panel().input.enabled);
    }

    function ask(text) {
        panel().input.text = text;
        panel().input.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return, Qt.ControlModifier);
    }

    function removeAll() {
        while (tabs().count > 0)
            tabs().closeTab(0);
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
        tryCompare(model(), "count", 0);
    }

    function closeOverlays() {
        const dialogs = [shell.connectionDialog, shell.settingsDialog, shell.closeTabDialog, shell.deleteDialog, shell.paletteDialog];
        for (const d of dialogs) {
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        }
        tryVerify(() => !dialogs.some(d => d && d.visible), 5000);
    }

    function initTestCase() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/aichat.db",
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
        shell.resetLayout();
        removeAll();
        setFake("ok", "");
        shell.ai.cliPath = testFakeClaude;
        shell.focusRegion(1);
    }

    function cleanupTestCase() {
        Language.choice = Language.Choice.English;
        shell.ai.cliPath = "";
        setFake("", "");
        a11yProbe.setEnvironment("FAKE_CLAUDE_STDIN", "");
    }

    // ---- the panel and its button --------------------------------------------------------------

    function test_00_thePanelStartsClosedAndNothingExistsUntilItOpens() {
        addSqlite(true);
        openQueryWithoutPanel();
        compare(area().aiPanelVisible, false);
        compare(area().chatPanelLoader.active, false); // not even the QML of the panel
        compare(shell.ai.chatCount, 0);
        verify(!shell.ai.hasRunner());
        verify(!shell.ai.hasAdapter());
        compare(area().chatPanel, null);

        // The button of the query bar: a name, the shortcut in the tooltip, and a toggle state.
        const button = area().aiButton;
        verify(button.visible);
        compare(button.accessibleName, "Toggle AI assistant");
        compare(button.shortcutText, CommandRegistry.displayShortcut("Ctrl+Shift+I"));
        compare(button.Accessible.role, Accessible.Button);
        compare(button.Accessible.checkable, true);
        compare(button.Accessible.checked, false);
        compare(walker.problems(shell), []);

        // Opening it makes the chat of that tab, and nothing starts yet.
        openPanel();
        compare(button.Accessible.checked, true);
        compare(shell.ai.chatCount, 1);
        verify(!shell.ai.hasRunner());
        compare(panel().Accessible.name, "AI assistant");
        verify(panel().noticeText.visible); // the first time ever, the notice is open
        compare(panel().input.Accessible.name, "Ask Claude");
        compare(panel().input.Accessible.role, Accessible.EditableText);
        compare(walker.problems(shell), []);

        // The command (menu, palette, shortcut) closes it; the chat of the tab is kept.
        CommandRegistry.trigger("ai.toggleChat");
        compare(area().aiPanelVisible, false);
        compare(button.Accessible.checked, false);
        compare(shell.ai.chatCount, 1);
        tryVerify(() => editor().edit.activeFocus); // the focus goes back to the editor
        keyClick(Qt.Key_I, Qt.ControlModifier | Qt.ShiftModifier);
        tryVerify(() => area().aiPanelVisible);
        tryVerify(() => panel().input.activeFocus);
        // The close button of the panel does the same.
        compare(panel().closeButton.accessibleName, "Close AI assistant");
        mouseClick(panel().closeButton);
        compare(area().aiPanelVisible, false);
        tryVerify(() => editor().edit.activeFocus);
    }

    function openQueryWithoutPanel() {
        openQuery(original);
    }

    function test_theCommandIsInTheEditorMenuAndTheBarOnlyForQueryTabs() {
        const command = CommandRegistry.commands.find(c => c.commandId === "ai.toggleChat");
        verify(command !== undefined);
        compare(command.title, "Toggle AI assistant");
        compare(command.shortcutText, CommandRegistry.displayShortcut("Ctrl+Shift+I"));
        // No query open: it cannot act, and the button is not there.
        compare(command.enabled, false);
        verify(!area().aiButton.visible);
        addSqlite(true);
        openQuery("SELECT 1;");
        compare(command.enabled, true);
        verify(area().aiButton.visible);
        // It lives in the Editor menu, in the menu bar only.
        const group = shell.menuBar.groups.find(g => g.key === "editor");
        verify(group !== undefined);
        verify(group.sections.some(section => section.some(c => c.commandId === "ai.toggleChat")));
        verify(shell.menuBar.groups.every(g => g.key === "editor" || !g.sections.some(section => section.some(c => c.commandId === "ai.toggleChat"))));
    }

    function test_theWidthOfThePanelIsRemembered() {
        addSqlite(true);
        openQuery("SELECT 1;");
        openPanel();
        const loader = area().chatPanelLoader;
        verify(loader.width >= Theme.controlHeight * 8);
        loader.T.SplitView.preferredWidth = Theme.controlHeight * 14;
        area().chatSplit.resized(); // what a finished drag of the handle emits
        tryVerify(() => Math.abs(Number(LayoutStore.value("aiChatWidth", -1)) - loader.width) <= 1, 5000);
        verify(Number(LayoutStore.value("aiChatWidth", -1)) > 0);
        // Reset layout forgets it and closes the panels.
        shell.resetLayout();
        compare(LayoutStore.value("aiChatWidth", -1), -1);
        compare(area().aiPanelVisible, false);
    }

    function test_theNoticeAndTheMemoryNoteAreVisible() {
        addSqlite(true);
        openQuery("SELECT 1;");
        openPanel();
        const p = panel();
        p.noticeOpen = true;
        verify(p.noticeText.visible);
        compare(p.noticeText.text, "Claude Code runs on your own installation: your request and the structure you allow are sent to Anthropic through it, and use your Claude subscription or API key. Quarry sends nothing by itself.");
        tryVerify(() => visibleTexts(p, "This conversation is kept in memory only: it is lost when you close this query.").length === 1);
        // Foldable.
        compare(p.noticeButton.Accessible.expandable, true);
        compare(p.noticeButton.Accessible.expanded, true);
        p.noticeButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(!p.noticeText.visible);
        compare(p.noticeButton.Accessible.expanded, false);
        keyClick(Qt.Key_Space);
        verify(p.noticeText.visible);
    }

    // ---- when it cannot work -------------------------------------------------------------------

    function test_withTheAiOffTheFieldIsDisabledWithTheReasonAndTheButton() {
        const id = addSqlite(false);
        openQuery("SELECT 1;");
        openPanel();
        const p = panel();
        tryVerify(() => p.unavailableBox.visible);
        verify(p.unavailableText.text.indexOf("The AI assistant is off for this connection") === 0, p.unavailableText.text);
        verify(!p.input.enabled);
        verify(!p.sendButton.enabled);
        compare(chat().state, "off");
        compare(p.unavailableSettingsButton.text, "Open connection settings");
        compare(walker.problems(shell), []);
        // Esc in the panel gives the focus back to the editor.
        p.sendButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Escape);
        tryVerify(() => editor().edit.activeFocus);
        // The button leads to the options of the connection.
        mouseClick(p.unavailableSettingsButton);
        tryVerify(() => shell.connectionDialog?.opened);
        closeOverlays();

        // Turning it on there frees the panel.
        verify(shell.ai.saveSettings(id, {
            "enabled": true
        }));
        tryVerify(() => p.input.enabled, 10000);
        verify(!p.unavailableBox.visible);
    }

    function test_withoutClaudeCodeTheFieldIsDisabledAndTheButtonOpensTheSettings() {
        addSqlite(true);
        openQuery("SELECT 1;");
        shell.ai.cliPath = testTempDir + "/no-such-claude";
        openPanel();
        const p = panel();
        tryVerify(() => p.unavailableBox.visible);
        verify(p.unavailableText.text.indexOf("Claude Code was not found") === 0, p.unavailableText.text);
        verify(!p.input.enabled);
        compare(p.unavailableSettingsButton.text, "Open settings");
        compare(walker.problems(shell), []);
        mouseClick(p.unavailableSettingsButton);
        tryVerify(() => shell.settingsDialog?.opened);
        closeOverlays();
        shell.ai.cliPath = testFakeClaude;
        tryVerify(() => p.input.enabled);
    }

    function test_aTabWithoutAConnectionSaysSo() {
        addSqlite(true);
        keyClick(Qt.Key_T, Qt.ControlModifier);
        waitForViews();
        tabs().setConnection(tabs().currentIndex, "");
        openPanel();
        tryVerify(() => panel().unavailableBox.visible);
        compare(chat().unavailableKind, "noConnection");
        verify(!panel().input.enabled);
        verify(!panel().unavailableSettingsButton.visible);
        compare(walker.problems(shell), []);
    }

    // ---- a request -----------------------------------------------------------------------------

    function test_ctrlEnterSendsAndNeverRunsTheScript() {
        setFake("ok", "Use a join on customer_id.");
        addSqlite(true);
        openQuery("SELECT * FROM customers;");
        openPanel();
        const p = panel();
        compare(p.sendButton.Accessible.role, Accessible.Button);
        verify(!p.sendButton.enabled); // nothing typed
        // Typed with the keyboard: Enter adds a line, it does not send.
        p.input.forceActiveFocus(Qt.TabFocusReason);
        keyClick("h");
        keyClick("i");
        keyClick(Qt.Key_Return);
        keyClick("x");
        compare(p.input.text, "hi\nx");
        compare(chat().messages.count, 0);
        verify(p.sendButton.enabled);
        compare(p.sendButton.Accessible.description, CommandRegistry.displayShortcut("Ctrl+Enter"));

        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(chat(), "state", "done", 20000);
        // The window's own Ctrl+Enter ("run") did not fire: the editor ran nothing.
        compare(shell.queries.current.state, "idle");
        compare(shell.queries.sessionCount, 0);
        compare(p.input.text, "");
        compare(chat().draft, "");
        compare(chat().messages.count, 2);
        compare(p.messageList.count, 2);
        compare(p.statusText.text, "Done.");
        verify(p.statusCard.visible);
        verify(p.lastAnnouncement.indexOf("Claude answered: Use a join on customer_id.") === 0, p.lastAnnouncement);
        compare(editor().edit.text, "SELECT * FROM customers;");
        // The authors are words, and the items of the list have names.
        const items = walker.findAll(p.messageList.contentItem, i => i.Accessible.role === Accessible.ListItem);
        compare(items.length, 2);
        compare(items[0].Accessible.name, "You: hi\nx");
        compare(items[1].Accessible.name, "Claude: Use a join on customer_id.");
        compare(p.messageList.Accessible.role, Accessible.List);
        compare(walker.problems(shell), []);
        compare(a11yProbe.readFile(stdinFile).indexOf("hi\nx") >= 0, true);
    }

    function test_thinkingShowsCancelAndEscStopsItWithoutAnError() {
        setFake("slow", "");
        addSqlite(true);
        openQuery("SELECT 1;");
        openPanel();
        const p = panel();
        ask("take your time");
        tryCompare(chat(), "state", "thinking", 20000);
        compare(p.statusText.text, "Claude is thinking…");
        verify(p.cancelButton.visible);
        verify(!p.sendButton.visible);
        compare(p.cancelButton.Accessible.role, Accessible.Button);
        verify(p.lastAnnouncement !== "");
        compare(walker.problems(shell), []);
        // Esc with the focus in the panel cancels.
        p.input.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Escape);
        compare(chat().state, "cancelled");
        compare(p.statusText.text, "Cancelled.");
        compare(chat().errorKind, "");
        verify(!p.detailsButton.visible);
        verify(p.sendButton.visible);
        compare(shell.queries.current.state, "idle");
        compare(walker.problems(shell), []);
        // The button does the same.
        ask("again");
        tryCompare(chat(), "state", "thinking", 20000);
        mouseClick(p.cancelButton);
        compare(chat().state, "cancelled");
    }

    function test_anErrorShowsItsMessageTheLoginCommandAndDetails() {
        setFake("notloggedin", "");
        addSqlite(true);
        openQuery("SELECT 1;");
        openPanel();
        const p = panel();
        ask("hello");
        tryCompare(chat(), "state", "error", 20000);
        verify(p.statusCard.visible);
        compare(p.statusText.text, "Claude Code is not signed in. Sign in from a terminal, then ask again.");
        verify(p.loginText.visible);
        compare(p.loginText.text, shell.ai.loginHint);
        compare(p.loginText.Accessible.name, "Sign-in command");
        verify(p.lastAnnouncement.indexOf("not signed in") > 0);
        compare(walker.problems(shell), []);

        // "Details": collapsed, then selectable text with a copy button.
        tryVerify(() => p.detailsButton.visible);
        compare(p.detailsButton.Accessible.expandable, true);
        compare(p.detailsButton.Accessible.expanded, false);
        verify(!p.detailsText.visible);
        p.detailsButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(p.detailsText.visible);
        compare(p.detailsButton.Accessible.expanded, true);
        compare(p.detailsText.Accessible.name, "Details");
        compare(p.detailsText.Accessible.role, Accessible.EditableText);
        verify(p.detailsText.text !== "");
        compare(walker.problems(shell), []);

        // The error of the CLI that is missing or old leads to the settings.
        setFake("old", "");
        ask("again");
        tryCompare(chat(), "state", "error", 20000);
        verify(p.settingsButton.visible);
        verify(!p.loginText.visible);
        compare(p.statusText.text, "This version of Claude Code is not supported. Update Claude Code and try again.");
        mouseClick(p.settingsButton);
        tryVerify(() => shell.settingsDialog?.opened);
        closeOverlays();
        // The usage limit says whose quota it is.
        setFake("usagelimit", "");
        ask("once more");
        tryCompare(chat(), "state", "error", 20000);
        verify(p.statusText.text.indexOf("usage limit") >= 0);
        verify(p.statusText.text.indexOf("subscription or API key") >= 0);
        compare(walker.problems(shell), []);
    }

    function test_contextSentShowsWhatWasSentAndNoRows() {
        setFake("ok", "fine");
        addSqlite(true);
        openQuery("SELECT * FROM invoices WHERE total > 10;");
        openPanel();
        const p = panel();
        verify(!p.contextBody.visible); // folded
        compare(p.contextButton.Accessible.expandable, true);
        compare(p.contextButton.Accessible.expanded, false);
        ask("explain the foreign key");
        tryCompare(chat(), "state", "done", 20000);
        p.contextButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(p.contextBody.visible);
        compare(p.contextButton.Accessible.expanded, true);
        compare(p.notSentText.text, "Not sent: rows, results or connection details.");
        verify(p.notSentText.visible);
        tryVerify(() => visibleTexts(p.contextBody.contentItem, "Request 1").length === 1);
        const copy = find(p.contextBody.contentItem, i => i.text === "Copy what was sent" && i.clicked !== undefined);
        verify(copy !== undefined);
        compare(copy.Accessible.name, "Copy what request 1 sent");
        const blocks = walker.findAll(p.contextBody.contentItem, i => i.visible && i.Accessible.role === Accessible.EditableText && i.readOnly === true);
        verify(blocks.length >= 2);
        const all = blocks.map(b => b.text).join("\n");
        verify(all.indexOf("CREATE") >= 0 || all.indexOf("invoices") >= 0, all);
        verify(all.indexOf("SELECT * FROM invoices WHERE total > 10;") >= 0);
        // The structure, never a row of the sample database.
        verify(all.indexOf("Ana") < 0);
        compare(walker.problems(shell), []);
    }

    // ---- the review of a proposal --------------------------------------------------------------

    function proposeAndWait() {
        setFake("ok", proposal);
        ask("rewrite it");
        tryCompare(chat(), "state", "done", 20000);
        tryVerify(() => review() !== null && review().visible && review().width > 0);
    }

    function decisionTexts() {
        return {
            "generated": visibleTexts(review(), "Generated by AI").length,
            "accepted": visibleTexts(review(), "Accepted").length,
            "rejected": visibleTexts(review(), "Rejected").length
        };
    }

    // Presses a button by keyboard once it is laid out: the Flow of the bar settles over a frame.
    function press(button) {
        tryVerify(() => button.visible && button.enabled && button.width > 0);
        button.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
    }

    // The list makes its blocks over a few frames: the tests wait for them.
    function expectCount(key, n) {
        tryVerify(() => decisionTexts()[key] === n, 10000, key + " should be " + n + " but is " + decisionTexts()[key]);
    }

    function reviewItems() {
        return walker.findAll(review().list.contentItem, i => i.Accessible.role === Accessible.ListItem && i.Accessible.name.indexOf("Change ") === 0);
    }

    function test_theProposalIsReviewedWithTheKeyboardAndAppliedAsOneEdit() {
        addSqlite(true);
        openQuery(original);
        openPanel();
        proposeAndWait();
        const r = review();
        const diff = chat().diff;

        // Announced, and nothing changed in the editor yet.
        compare(diff.changeCount, 3);
        tryVerify(() => area().lastAnnouncement.indexOf("Claude proposed 3 changes") === 0 || r.lastAnnouncement === "Claude proposed 3 changes");
        compare(r.lastAnnouncement, "Claude proposed 3 changes");
        compare(editor().edit.text, original);
        compare(tabs().textOf(tabs().currentIndex), original);
        verify(!editor().edit.activeFocus); // the editor under the review takes no keys
        compare(shell.queries.current.state, "idle");
        compare(r.Accessible.name, "Review of Claude's proposal");

        // Every change says where it comes from, in words (and an icon), and the + / - lines.
        expectCount("generated", 3);
        verify(visibleTexts(r, "+ SELECT id FROM customers;").length === 1);
        verify(visibleTexts(r, "- SELECT * FROM customers;").length === 1);
        verify(visibleTexts(r, "+ DELETE FROM invoices WHERE id = 1;").length === 1);
        // The block that changes data says so, and so does the header.
        tryVerify(() => visibleTexts(r, "This block changes data or the schema: review it before you run the script").length === 1);
        tryVerify(() => visibleTexts(r, "Some changes add statements that change data or the schema. Review them before you run the script.").length === 1);
        verify(diff.modifiesData);
        // The list and its blocks are named for the screen reader.
        compare(r.list.Accessible.role, Accessible.List);
        compare(r.list.Accessible.name, "Proposed changes");
        tryVerify(() => reviewItems().length === 3);
        const items = reviewItems();
        compare(items[0].Accessible.name, "Change 1 of 3, replaces 1 line with 1 line, pending");
        compare(items[1].Accessible.name, "Change 2 of 3, adds 1 line, pending, changes data");
        compare(items[2].Accessible.name, "Change 3 of 3, adds 1 line, pending");
        compare(walker.problems(shell), []);

        // "Review changes" in the panel leads to the list; F6 / Mod+2 land there too.
        mouseClick(panel().reviewButton);
        tryVerify(() => r.list.activeFocus);
        // The keys: Enter accepts (and moves on), Delete rejects, Ctrl+Z undoes.
        compare(r.list.currentIndex, diff.firstChange());
        keyClick(Qt.Key_Return);
        expectCount("accepted", 1);
        expectCount("generated", 2);
        verify(r.lastAnnouncement.indexOf("Change 1 of 3, replaces 1 line with 1 line, accepted") === 0, r.lastAnnouncement);
        verify(r.lastAnnouncement.indexOf("2 pending") > 0);
        keyClick(Qt.Key_Delete);
        expectCount("rejected", 1);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        expectCount("rejected", 0); // undone: back to pending
        expectCount("generated", 2);
        keyClick(Qt.Key_Backspace); // Backspace rejects as well
        expectCount("rejected", 1);
        // Up and Down walk the changes only.
        keyClick(Qt.Key_Up);
        const first = r.list.currentIndex;
        keyClick(Qt.Key_Down);
        verify(r.list.currentIndex > first);
        keyClick(Qt.Key_Down);
        const last = r.list.currentIndex;
        keyClick(Qt.Key_Down); // past the last change: stays
        compare(r.list.currentIndex, last);
        compare(editor().edit.text, original); // still nothing in the editor

        // The buttons of the bar: Undo is there while there is something to undo.
        verify(r.undoButton.enabled);
        compare(r.applyButton.Accessible.role, Accessible.Button);
        verify(r.applyButton.enabled);
        compare(walker.problems(shell), []);

        // Apply: ONE edit of the editor with only what was accepted; nothing runs.
        r.applyButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryVerify(() => area().review === null);
        compare(editor().edit.text, partial);
        compare(tabs().textOf(tabs().currentIndex), partial);
        verify(chat().diff === null);
        compare(shell.queries.current.state, "idle");
        compare(shell.queries.sessionCount, 0);
        verify(area().lastAnnouncement.indexOf("Nothing was run") > 0, area().lastAnnouncement);
        tryVerify(() => editor().edit.activeFocus);
        // One Undo of the editor brings the whole script back.
        editor().edit.undo();
        compare(editor().edit.text, original);
        compare(tabs().textOf(tabs().currentIndex), original);
        compare(walker.problems(shell), []);
    }

    function test_acceptAllRejectAllAndDiscardWithEsc() {
        addSqlite(true);
        openQuery(original);
        openPanel();
        proposeAndWait();
        const r = review();
        press(r.acceptAllButton);
        expectCount("accepted", 3);
        compare(r.diff.pendingCount, 0);
        press(r.rejectAllButton);
        expectCount("rejected", 3);
        press(r.undoButton);
        expectCount("accepted", 3);
        press(r.acceptAllButton);
        // Discard (Esc): nothing is touched.
        r.list.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Escape);
        tryVerify(() => area().review === null);
        compare(editor().edit.text, original);
        verify(chat().diff === null);
        verify(area().lastAnnouncement.indexOf("The script was not changed") > 0, area().lastAnnouncement);
        // The panel still has the conversation.
        compare(chat().messages.count, 2);

        // The same by the Discard button, and Apply with everything accepted gives the proposal.
        proposeAndWait();
        press(review().discardButton);
        tryVerify(() => area().review === null);
        compare(editor().edit.text, original);
        proposeAndWait();
        press(review().acceptAllButton);
        press(review().applyButton);
        tryVerify(() => area().review === null);
        compare(editor().edit.text, "SELECT id FROM customers;\nSELECT * FROM invoices;\nDELETE FROM invoices WHERE id = 1;\nSELECT * FROM items;\nSELECT 1;");
        compare(shell.queries.current.state, "idle");
    }

    function test_aScriptEditedDuringTheReviewIsNotOverwritten() {
        addSqlite(true);
        openQuery(original);
        openPanel();
        proposeAndWait();
        const r = review();
        // The text of the tab changes by another way while the proposal waits.
        const edited = original + "\n-- my note";
        editor().edit.text = edited;
        tryVerify(() => r.stale);
        tryVerify(() => visibleTexts(r, "The script changed while Claude was working. Nothing was applied: make the proposal again for the current script, or discard it.").length === 1);
        verify(!r.applyButton.enabled);
        verify(!r.acceptAllButton.enabled);
        verify(r.redoButton.visible);
        compare(editor().edit.text, edited);
        compare(walker.problems(shell), []);
        // Redo it for what is there now.
        press(r.redoButton);
        tryVerify(() => !r.stale);
        verify(r.applyButton.enabled);
        compare(r.diff.original, edited);
        press(r.discardButton);
        tryVerify(() => area().review === null);
        compare(editor().edit.text, edited);
    }

    function test_theReviewBelongsToItsTab() {
        addSqlite(true);
        openQuery(original);
        openPanel();
        proposeAndWait();
        const firstTab = tabs().currentIndex;
        // Another tab has no review and no panel; coming back shows both again.
        keyClick(Qt.Key_T, Qt.ControlModifier);
        waitForViews();
        verify(tabs().currentIndex !== firstTab);
        tryVerify(() => area().review === null);
        compare(area().aiPanelVisible, false);
        compare(editor().edit.text, "");
        tabs().currentIndex = firstTab;
        tryVerify(() => area().review !== null);
        tryVerify(() => area().aiPanelVisible);
        // Closing the tab takes its chat with it.
        const id = tabs().idOf(firstTab);
        verify(shell.ai.existingChat(id) !== null);
        tabs().closeTab(firstTab);
        verify(shell.ai.existingChat(id) === null);
    }

    // ---- the local log switch of the settings ---------------------------------------------------

    function test_theSettingsHaveTheLocalLogSwitchOffByDefault() {
        shell.ai.keepLog = false;
        CommandRegistry.trigger("settings.open");
        tryVerify(() => shell.settingsDialog?.opened);
        const dlg = shell.settingsDialog;
        const box = dlg.keepLogBox;
        compare(box.text, "Keep a local log of what is sent");
        compare(box.Accessible.role, Accessible.CheckBox);
        compare(box.checked, false);
        verify(!dlg.logPathText.visible);
        verify(!dlg.openLogFolderButton.visible);
        compare(walker.problems(dlg.contentItem), []);
        box.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        compare(shell.ai.keepLog, true);
        tryVerify(() => dlg.logPathText.visible);
        verify(dlg.logPathText.text.indexOf("ai-requests.log") > 0, dlg.logPathText.text);
        verify(dlg.openLogFolderButton.visible);
        compare(walker.problems(dlg.contentItem), []);
        Language.choice = Language.Choice.Spanish;
        compare(box.text, "Guardar un registro local de lo que se envía");
        Language.choice = Language.Choice.English;
        keyClick(Qt.Key_Space);
        compare(shell.ai.keepLog, false);
        closeOverlays();
    }

    // ---- language and size ---------------------------------------------------------------------

    function test_theSpanishUi() {
        addSqlite(true);
        openQuery(original);
        openPanel();
        Language.choice = Language.Choice.Spanish;
        const p = panel();
        compare(p.input.Accessible.name, "Pregunta a Claude");
        compare(p.closeButton.accessibleName, "Cerrar el asistente de IA");
        compare(p.sendButton.text, "Enviar");
        compare(p.contextButton.Accessible.name, "Contexto enviado");
        compare(area().aiButton.accessibleName, "Mostrar u ocultar el asistente de IA");
        compare(walker.problems(shell), []);
        proposeSpanish();
        const r = review();
        compare(r.Accessible.name, "Revisión de la propuesta de Claude");
        compare(r.applyButton.text, "Aplicar");
        compare(r.discardButton.text, "Descartar");
        tryVerify(() => visibleTexts(r, "Generado por IA").length === 3);
        tryVerify(() => visibleTexts(r, "Este bloque cambia datos o el esquema: revísalo antes de ejecutar el script").length === 1);
        compare(walker.problems(shell), []);
        Language.choice = Language.Choice.English;
        compare(r.applyButton.text, "Apply");
    }

    function proposeSpanish() {
        setFake("ok", proposal);
        ask("reescríbelo");
        tryCompare(chat(), "state", "done", 20000);
        tryVerify(() => review() !== null && review().visible && review().width > 0);
    }

    function test_at200PercentZoomEverythingStillWorks() {
        addSqlite(true);
        openQuery(original);
        openPanel();
        Theme.zoom = 2.0;
        // Only that the layout settles and the controls stay reachable and named: how a narrow row of
        // text wraps is the font's business, and differs between machines.
        tryVerify(() => panel().visible && panel().width > 0);
        compare(walker.problems(shell), []);
        proposeAndWait();
        tryVerify(() => review().list.height > 0);
        compare(walker.problems(shell), []);
        verify(review().applyButton.visible);
        press(review().discardButton);
        tryVerify(() => area().review === null);
        Theme.resetZoom();
    }
}
