import QtQuick
import QtTest
import Quarry.Controls

// The query plan (H9): PlanView on synthetic plans (the tree, its keyboard, the details, the text
// view, truncated and failed plans) and the whole path through the main window with the real
// SQLite plugin (Mod+E, the Plan tab, the focus, a normal run, the refusal of Explain analyze).
// No clicks at fixed coordinates: focus and keys only.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0
    property var said: []
    property string savedClipboard: ""

    name: "Plan"
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
        target: testCase.shell ? testCase.shell.queries : null

        function onAnnouncement(text, assertive) {
            testCase.said.push(text);
        }
    }

    // ---- helpers -----------------------------------------------------------------------------

    function tabs() {
        return shell.editorTabs;
    }

    function editor() {
        return shell.editorArea.currentEditor;
    }

    function current() {
        return shell.queries.current;
    }

    function panel() {
        return shell.resultsPanel;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function newFile() {
        return testTempDir + "/plan-data-" + (++fileCounter) + ".db";
    }

    function addSqlite(file) {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        shell.connections.openSqliteFile(Qt.url("file:///" + file));
        tryCompare(spy, "count", 1);
        tryVerify(() => shell.sidebar.tree === null ? shell.connections.model.count === 0 : shell.sidebar.tree.rows >= shell.connections.model.count, 20000);
        return spy.signalArguments[0][0];
    }

    function openQuery(sql) {
        shell.focusRegion(1);
        keyClick(Qt.Key_T, Qt.ControlModifier);
        tryVerify(() => editor() !== null && editor().visible && editor().edit.activeFocus);
        editor().edit.text = sql;
        compare(tabs().textOf(tabs().currentIndex), sql);
    }

    function removeAll() {
        while (tabs().count > 0)
            tabs().closeTab(0);
        for (let i = shell.connections.model.count - 1; i >= 0; --i)
            shell.connections.remove(shell.connections.model.get(i).connectionId);
        tryCompare(shell.connections.model, "count", 0);
    }

    function closeOverlays() {
        for (const d of [shell.confirmAnalyzeDialog, shell.confirmRunDialog, shell.closeTabDialog]) {
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        }
        tryVerify(() => ![shell.confirmAnalyzeDialog, shell.confirmRunDialog, shell.closeTabDialog].some(d => d && d.visible), 5000);
    }

    // A PlanView over a synthetic plan (see AccessibilityProbe::makePlan).
    function makeView(kind) {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/PlanView.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        const plan = a11yProbe.makePlan(kind);
        const view = createTemporaryObject(comp, testCase, {
            "plan": plan,
            "width": 700,
            "height": 420
        });
        verify(view !== null);
        return view;
    }

    function row(view, n) {
        return view.tree.itemAtIndex(view.tree.index(n, 0));
    }

    function initTestCase() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/plan.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        tryVerify(() => !shell.connections.model.loading, 10000);
        tryVerify(() => tabs().loaded, 10000);
    }

    function init() {
        closeOverlays();
        Language.choice = Language.Choice.English;
        Theme.resetZoom();
        shell.resetLayout();
        removeAll();
        said.length = 0;
        panel().tabBar.currentIndex = 0;
        shell.focusRegion(1);
    }

    function cleanupTestCase() {
        Language.choice = Language.Choice.English;
    }

    // ---- PlanView on synthetic plans ---------------------------------------------------------

    function test_theTreeShowsEveryStepWithItsShareInWords() {
        const view = makeView("costs");
        tryVerify(() => view.tree !== null && view.tree.rows === 4, 5000, "every step is expanded");
        compare(view.page, 0);
        verify(view.headerText.includes("PostgreSQL"), view.headerText);
        verify(view.headerText.includes("analyzed"), view.headerText);
        verify(view.headerText.includes("4 steps"), view.headerText);
        verify(view.headerText.includes("planning 0" + Qt.locale().decimalPoint + "3 ms"), view.headerText);
        verify(view.headerText.includes("execution 50 ms"), view.headerText);
        verify(view.basisText.includes("execution time"), view.basisText);

        const top = row(view, 0);
        verify(top !== null && top !== undefined);
        compare(top.Accessible.name, "Hash Join");
        compare(top.Accessible.role, Accessible.TreeItem);
        compare(top.Accessible.expandable, true);
        compare(top.Accessible.expanded, true);
        // The bar is the nodes own time: 50 ms minus 30 and 10 = 10 ms = 20 %.
        compare(top.hasBar, true);
        compare(top.costPercentText, "20%");
        fuzzyCompare(top.costFraction, 0.2, 0.001);
        verify(top.Accessible.description.includes("20% of the total execution time"), top.Accessible.description);
        verify(top.Accessible.description.includes("cost 100"), top.Accessible.description);
        verify(top.Accessible.description.includes("level 1"), top.Accessible.description);
        const scan = row(view, 1);
        compare(scan.Accessible.name, "Seq Scan on orders");
        compare(scan.costPercentText, "60%");
        verify(scan.Accessible.description.includes("warning: Rows estimate off by 10x"), scan.Accessible.description);
        verify(scan.Accessible.description.includes(Qt.locale().toString(1200) + " rows"), scan.Accessible.description);
        verify(scan.Accessible.description.includes("30 ms"), scan.Accessible.description);
        // The share and the warning are said in words and icons, never only by a color.
        const percent = find(scan, i => i.text === "60%");
        verify(percent !== undefined, "the percentage is text");
        const warning = find(scan, i => i.text === "1 warning");
        verify(warning !== undefined, "the warning is text");

        // What the platform accessibility layer gets.
        const info = a11yProbe.inspect(top);
        verify(info.expandable);
        verify(info.expanded);
        compare(info.name, "Hash Join");
        verify(info.description.includes("20%"), info.description);
        compare(walker.problems(view), []);
    }

    function test_theKeyboardWalksAndFoldsTheTree() {
        const view = makeView("costs");
        tryVerify(() => view.tree !== null && view.tree.rows === 4, 5000);
        view.takeFocus();
        tryVerify(() => view.tree.activeFocus);
        compare(view.tree.cursorRow(), 0);
        keyClick(Qt.Key_Down);
        compare(view.tree.cursorRow(), 1);
        keyClick(Qt.Key_End);
        compare(view.tree.cursorRow(), 3);
        keyClick(Qt.Key_Home);
        compare(view.tree.cursorRow(), 0);
        keyClick(Qt.Key_PageDown);
        compare(view.tree.cursorRow(), 3);
        keyClick(Qt.Key_PageUp);
        compare(view.tree.cursorRow(), 0);
        // Left on an open step folds it; Right opens it; Right again enters the first child.
        keyClick(Qt.Key_Left);
        tryCompare(view.tree, "rows", 1);
        compare(row(view, 0).Accessible.expanded, false);
        keyClick(Qt.Key_Right);
        tryVerify(() => view.tree.rows > 1);
        keyClick(Qt.Key_Right);
        compare(view.tree.cursorRow(), 1);
        // Left on a leaf goes to its parent.
        keyClick(Qt.Key_Left);
        compare(view.tree.cursorRow(), 0);
        // Enter folds and unfolds as well.
        keyClick(Qt.Key_Return);
        tryCompare(view.tree, "rows", 1);
        keyClick(Qt.Key_Return);
        tryVerify(() => view.tree.rows > 1);
        // Space too; the other keys are left alone.
        keyClick(Qt.Key_Space);
        tryCompare(view.tree, "rows", 1);
        keyClick(Qt.Key_Space);
        tryVerify(() => view.tree.rows > 1);
        compare(view.tree.activeFocus, true);
    }

    function test_theSelectedStepHasItsDetailsAndWarnings() {
        const view = makeView("costs");
        tryVerify(() => view.tree !== null && view.tree.rows === 4, 5000);
        view.takeFocus();
        tryVerify(() => view.tree.activeFocus);
        keyClick(Qt.Key_Down); // Seq Scan on orders
        const entries = view.detailList.model;
        verify(entries.some(e => e.kind === "metric" && e.name === "Share" && e.value === "60% of the total execution time"), JSON.stringify(entries));
        verify(entries.some(e => e.kind === "warning" && e.value === "Rows estimate off by 10x"), JSON.stringify(entries));
        verify(entries.some(e => e.kind === "detail" && e.name === "Filter" && e.value === "(total > 100)"), JSON.stringify(entries));
        compare(view.detailList.Accessible.name, "Details of Seq Scan on orders");
        // The warning is an icon and words in the panel too.
        tryVerify(() => find(view.detailList, i => i.text === "Warning: Rows estimate off by 10x") !== undefined);
        const filter = find(view.detailList, i => i.text === "Filter");
        verify(filter !== undefined);
        // The list is reachable from the keyboard: the tree, then the list.
        keyClick(Qt.Key_Tab);
        compare(view.detailList.activeFocus, true);
    }

    function test_withoutCostOrTimeThereAreNoBars() {
        const view = makeView("sqlite");
        tryVerify(() => view.tree !== null && view.tree.rows === 3, 5000);
        verify(view.headerText.includes("SQLite"), view.headerText);
        verify(view.headerText.includes("estimated"), view.headerText);
        verify(view.basisText.includes("SQLite gives no cost or time"), view.basisText);
        const top = row(view, 0);
        compare(top.hasBar, false);
        compare(top.costPercentText, "");
        verify(!top.Accessible.description.includes("%"), top.Accessible.description);
        compare(find(top, i => i.text === "%"), undefined);
        // The labels are the engine's own.
        compare(top.Accessible.name, "SEARCH orders USING INDEX orders_customer (customer_id=?)");
    }

    function test_anEstimatedPlanSaysSo() {
        const view = makeView("estimated");
        tryVerify(() => view.tree !== null && view.tree.rows === 4, 5000);
        verify(view.headerText.includes("estimated: the query was not run"), view.headerText);
        verify(view.basisText.includes("estimated cost"), view.basisText);
        compare(row(view, 0).hasBar, true);
        compare(row(view, 0).Accessible.description.includes("ms"), false);
    }

    function test_theTextViewIsTheEnginesOutputAndCanBeCopied() {
        const view = makeView("costs");
        tryVerify(() => view.tree !== null && view.tree.rows === 4, 5000);
        const raw = view.plan.rawText;
        verify(raw.length > 0);
        compare(view.textEdit, null); // not laid out until it is shown
        view.viewTabs.currentIndex = 1;
        tryVerify(() => view.textEdit !== null);
        compare(view.page, 1);
        compare(view.textEdit.text, raw);
        compare(view.textEdit.readOnly, true);
        compare(view.textEdit.Accessible.name, "Plan text");
        compare(view.focusTarget, view.textEdit);
        view.takeFocus();
        tryVerify(() => view.textEdit.activeFocus);
        keyClick(Qt.Key_A, Qt.ControlModifier);
        compare(view.textEdit.selectedText.split(String.fromCharCode(0x2029)).join(String.fromCharCode(10)), raw);
        // The Copy button puts all of it on the clipboard (for the keyboard and the screen reader).
        savedClipboard = a11yProbe.clipboardText();
        a11yProbe.setClipboardText("");
        view.copyButton.clicked();
        compare(a11yProbe.clipboardText().replace(/\r\n/g, "\n"), raw);
        a11yProbe.setClipboardText(savedClipboard);
        // Back to the tree.
        view.viewTabs.currentIndex = 0;
        compare(view.page, 0);
        compare(walker.problems(view), []);
    }

    // The most steps the parsers give: the tree keeps up with the keyboard.
    function test_fiveThousandStepsStayUsable() {
        const view = makeView("many");
        compare(view.plan.nodeCount, 5000);
        tryVerify(() => view.tree !== null && view.tree.rows === 5000, 60000, "every step is a row");
        view.takeFocus();
        tryVerify(() => view.tree.activeFocus);
        const started = Date.now();
        keyClick(Qt.Key_End);
        compare(view.tree.cursorRow(), 4999);
        keyClick(Qt.Key_Home);
        compare(view.tree.cursorRow(), 0);
        for (let i = 0; i < 20; ++i)
            keyClick(Qt.Key_Down);
        compare(view.tree.cursorRow(), 20);
        console.log("5000 steps: End, Home and 20 steps down took " + (Date.now() - started) + " ms");
        verify(Date.now() - started < testMaxGapMs * 4, "the keys are answered");
        // Folding the first step hides its 99 children at once.
        keyClick(Qt.Key_Home);
        keyClick(Qt.Key_Left);
        tryCompare(view.tree, "rows", 4901);
    }

    function test_aTruncatedPlanSaysSoVisibly() {
        const view = makeView("truncated");
        tryVerify(() => view.tree !== null && view.tree.rows === 4, 5000);
        const warning = find(view, i => i.text !== undefined && String(i.text).startsWith("Showing the first"));
        verify(warning !== undefined, "the notice of a cut plan is visible");
        compare(warning.text, "Showing the first 4 steps");
        compare(warning.Accessible.role, Accessible.StaticText);
        const complete = makeView("costs");
        tryVerify(() => complete.tree !== null);
        compare(find(complete, i => i.text !== undefined && String(i.text).startsWith("Showing the first")), undefined);
    }

    function test_aPlanThatCannotBeReadShowsTheErrorAndTheText() {
        const view = makeView("failed");
        compare(view.state, "failed");
        compare(view.page, 1); // never an empty tab
        verify(view.errorPanel.visible);
        compare(view.errorPanel.message, "The plan could not be read.");
        compare(view.errorPanel.hint, "The engine's own output is shown in the Text view.");
        compare(view.errorPanel.detail, "unexpected token");
        tryVerify(() => view.textEdit !== null);
        verify(view.textEdit.text.startsWith("QUERY PLAN"));
        compare(view.tree, null);
        compare(view.viewTabs.visible, false);
        compare(view.focusTarget, view.textEdit);
        compare(walker.problems(view), []);
    }

    function test_aPlanBeingMadeShowsAStatusAndTheTreeTakesTheFocusWhenItArrives() {
        const view = makeView("running");
        compare(view.page, 2);
        verify(view.statusPage.title.startsWith("Getting the query plan"), view.statusPage.title);
        compare(view.focusTarget, view.statusPage);
        view.takeFocus();
        tryVerify(() => view.statusPage.activeFocus);
        a11yProbe.refill(view.plan, "costs");
        tryVerify(() => view.tree !== null && view.tree.rows === 4, 5000);
        tryVerify(() => view.tree.activeFocus, 5000, "the focus follows the plan");
        compare(view.page, 0);
    }

    // ---- the whole path ----------------------------------------------------------------------

    function test_explainFromTheEditorOpensThePlanTabAndFocusesTheTree() {
        addSqlite(newFile());
        openQuery("SELECT * FROM sqlite_master WHERE name = 'x'");
        const cmd = CommandRegistry.find("editor.explain");
        verify(cmd !== null);
        compare(cmd.title, "Explain current statement");
        compare(cmd.shortcutText, CommandRegistry.displayShortcut("Ctrl+E"));
        verify(cmd.enabled);
        compare(panel().tabBar.count, 2); // no plan asked for yet

        keyClick(Qt.Key_E, Qt.ControlModifier);
        tryCompare(current().plan, "state", "ready", 10000);
        compare(panel().tabBar.count, 3);
        compare(panel().tabBar.currentIndex, 2);
        tryVerify(() => panel().planView !== null && panel().planView.tree !== null && panel().planView.tree.activeFocus, 5000, "the focus is in the tree");
        verify(said.some(t => t === "Getting the query plan"), said.join("|"));
        verify(said.some(t => t.startsWith("Query plan ready, ")), said.join("|"));
        compare(panel().planView.plan.engine, "SQLite");
        compare(panel().planView.plan.analyzed, false);
        compare(panel().planView.plan.hasBars, false);
        compare(walker.problems(panel()), []);
        // The status bar shows that it ended.
        tryCompare(current(), "state", "done");
    }

    function test_aNormalRunBringsResultsBackAndLeavesThePlan() {
        addSqlite(newFile());
        openQuery("SELECT 1");
        keyClick(Qt.Key_E, Qt.ControlModifier);
        tryCompare(current().plan, "state", "ready", 10000);
        compare(panel().tabBar.currentIndex, 2);
        shell.focusRegion(1);
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        tryCompare(current(), "state", "done");
        compare(panel().tabBar.currentIndex, 0);
        compare(panel().tabBar.count, 3); // the Plan tab stays
        compare(current().plan.state, "ready");
        compare(current().results.rows, 1);
        // Explain again: the Plan tab comes to the front again.
        shell.focusRegion(1);
        keyClick(Qt.Key_E, Qt.ControlModifier);
        tryCompare(panel().tabBar, "currentIndex", 2);
        tryCompare(current(), "state", "done", 10000);
    }

    function test_theToolbarButtonsDoTheSame() {
        addSqlite(newFile());
        openQuery("SELECT 1");
        const area = shell.editorArea;
        compare(area.explainButton.accessibleName, "Explain current statement");
        compare(area.explainAnalyzeButton.accessibleName, "Explain analyze current statement");
        verify(area.explainButton.enabled);
        area.explainButton.clicked();
        tryCompare(current().plan, "state", "ready", 10000);
        compare(panel().tabBar.currentIndex, 2);
    }

    function test_explainAnalyzeIsRefusedByAnEngineThatCannotAnalyzeAndSaysWhy() {
        addSqlite(newFile());
        openQuery("SELECT 1");
        const cmd = CommandRegistry.find("editor.explainAnalyze");
        verify(cmd !== null);
        compare(cmd.shortcutText, CommandRegistry.displayShortcut("Ctrl+Shift+A"));
        keyClick(Qt.Key_A, Qt.ControlModifier | Qt.ShiftModifier);
        tryCompare(current().plan, "requested", true);
        verify(current().plan.notice.includes("SQLite"), current().plan.notice);
        verify(current().plan.notice.includes("Explain analyze"), current().plan.notice);
        compare(shell.editorArea.lastAnnouncement, current().plan.notice);
        compare(current().state, "idle"); // nothing ran
        compare(shell.confirmAnalyzeDialog === null || !shell.confirmAnalyzeDialog.visible, true);
        compare(panel().tabBar.currentIndex, 2);
        tryVerify(() => find(panel().planView, i => i.text === current().plan.notice) !== undefined, 5000, "the reason is on screen");
        compare(panel().planView.state, "idle");
    }

    function test_theCommandsAreRegisteredAndOnlyOfferedWhereTheyApply() {
        const expected = {
            "editor.explain": "Ctrl+E",
            "editor.explainAnalyze": "Ctrl+Shift+A"
        };
        for (const id in expected) {
            const c = CommandRegistry.find(id);
            verify(c !== null, id);
            compare(c.shortcutText, CommandRegistry.displayShortcut(expected[id]), id);
            compare(c.category, "Editor");
            verify(!c.enabled, id + " needs an editor with a connection");
        }
        // No clash with the shortcuts that were already there (Export is Mod+Shift+E).
        const mine = [].concat(CommandRegistry.find("editor.explain").sequences, CommandRegistry.find("editor.explainAnalyze").sequences);
        compare(mine.length, 2);
        for (const c of CommandRegistry.commands) {
            if (c.commandId === "editor.explain" || c.commandId === "editor.explainAnalyze")
                continue;
            for (const s of c.sequences)
                verify(!mine.includes(s), s + " is also used by " + c.commandId);
        }
        addSqlite(newFile());
        openQuery("SELECT 1");
        for (const id in expected)
            verify(CommandRegistry.find(id).enabled, id);
    }

    function test_theAnalyzeConfirmationIsAKeyboardFlowThatStartsFromCancel() {
        shell.askAnalyze({
            "tabId": "tab-x",
            "connectionName": "Shop",
            "production": true,
            "sql": "SELECT * FROM orders"
        });
        tryVerify(() => shell.confirmAnalyzeDialog?.opened);
        const dialog = shell.confirmAnalyzeDialog;
        verify(dialog.cancelButton.activeFocus, "the focus starts on Cancel");
        verify(dialog.summary.includes("Shop"), dialog.summary);
        verify(dialog.summary.includes("production"), dialog.summary);
        verify(find(dialog.contentItem, i => i.text === "PRODUCTION") !== undefined, "the PRODUCTION mark is in the dialog");
        verify(find(dialog.contentItem, i => i.text === "SELECT * FROM orders") !== undefined);
        compare(dialog.runButton.text, "Run and analyze");
        compare(walker.problems(shell.overlayItem), []);
        const declined = createTemporaryObject(spyComp, testCase, {
            target: dialog,
            signalName: "declined"
        });
        const confirmed = createTemporaryObject(spyComp, testCase, {
            target: dialog,
            signalName: "confirmed"
        });
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dialog.visible);
        compare(declined.count, 1);
        compare(declined.signalArguments[0][0], "tab-x");
        compare(confirmed.count, 0);

        // Again, and now the explicit button.
        shell.askAnalyze({
            "tabId": "tab-x",
            "connectionName": "Shop",
            "production": false,
            "sql": "SELECT 1"
        });
        tryVerify(() => dialog.opened);
        keyClick(Qt.Key_Tab);
        verify(dialog.runButton.activeFocus);
        keyClick(Qt.Key_Space);
        tryVerify(() => !dialog.visible);
        compare(confirmed.count, 1);
        compare(confirmed.signalArguments[0][0], "tab-x");
        compare(declined.count, 1, "answering is not declining");
    }

    function test_thePlanTabGoesWithItsEditorTab() {
        addSqlite(newFile());
        openQuery("SELECT 1");
        keyClick(Qt.Key_E, Qt.ControlModifier);
        tryCompare(current().plan, "state", "ready", 10000);
        compare(panel().tabBar.count, 3);
        compare(panel().tabBar.currentIndex, 2);
        // Another editor tab has no plan: no Plan tab, and the panel is not left on a tab that is gone.
        openQuery("SELECT 2");
        compare(panel().tabBar.count, 2);
        compare(panel().tabBar.currentIndex, 0);
        tabs().currentIndex = 0;
        tryCompare(panel().tabBar, "count", 3);
        // Closing the editor tab takes its plan with it.
        shell.closeQueryNow(tabs().idOf(0));
        tryCompare(panel().tabBar, "count", 2);
    }

    function test_spanishStrings() {
        Language.choice = Language.Choice.Spanish;
        compare(CommandRegistry.find("editor.explain").title, "Explicar la sentencia actual");
        compare(CommandRegistry.find("editor.explainAnalyze").title, "Explicar y analizar la sentencia actual");
        addSqlite(newFile());
        openQuery("SELECT 1");
        keyClick(Qt.Key_E, Qt.ControlModifier);
        tryCompare(current().plan, "state", "ready", 10000);
        compare(panel().tabBar.itemAt(2).text, "Plan");
        const view = panel().planView;
        verify(view.headerText.includes("estimado: no se ejecutó la consulta"), view.headerText);
        verify(view.headerText.includes("paso"), view.headerText);
        verify(view.basisText.includes("no da coste ni tiempo"), view.basisText);
        compare(view.tree.Accessible.name, "Plan de consulta");
        verify(said.some(t => t.startsWith("Plan de consulta listo, ")), said.join("|"));
        compare(view.viewTabs.itemAt(1).text, "Texto");
        Language.choice = Language.Choice.English;
        compare(CommandRegistry.find("editor.explain").title, "Explain current statement");
    }
}
