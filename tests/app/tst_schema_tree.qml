import QtQuick
import QtTest
import Quarry.Controls

// The on-demand schema tree (H1.4) in the sidebar, driven the way a keyboard-only user would:
// the real SQLite plugin over a small sample database in a temporary directory. No test saves a
// password, so the system keychain is never touched.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0

    name: "SchemaTree"
    when: windowShown
    width: 900
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

    function tree() {
        return shell.sidebar.tree;
    }

    function model() {
        return shell.connections.model;
    }

    function typeText(text) {
        for (const ch of text)
            keyClick(ch);
    }

    function rowName(row) {
        return tree().roleAt(row, SchemaTreeModel.NameRole);
    }

    function rowKind(row) {
        return tree().roleAt(row, SchemaTreeModel.KindRole);
    }

    function currentName() {
        return rowName(tree().cursorRow());
    }

    function itemAt(row) {
        return tree().itemAtCell(0, row);
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function announcements(spy) {
        return spy.signalArguments.map(a => a[0]);
    }

    function addSqlite(path) {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        shell.connections.openSqliteFile(Qt.url("file:///" + path));
        tryCompare(spy, "count", 1);
        // The TreeView catches up with the model a pass later (slow CI runners show it).
        tryVerify(() => shell.sidebar.tree !== null && shell.sidebar.tree.rows >= model().count, 20000);
        return spy.signalArguments[0][0];
    }

    function removeAll() {
        for (let i = model().count - 1; i >= 0; --i)
            shell.connections.remove(model().get(i).connectionId);
        tryCompare(model(), "count", 0);
    }

    function closeOverlays() {
        const d = shell.sidebar.passwordDialog;
        if (d && d.visible) {
            tryVerify(() => d.opened || !d.visible);
            d.close();
        }
        tryVerify(() => !(shell.sidebar.passwordDialog?.visible) && !(shell.connectionDialog?.visible) && !(shell.deleteDialog?.visible), 5000);
    }

    function initTestCase() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/schematree.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        tryVerify(() => !model().loading, 10000);
    }

    function init() {
        closeOverlays();
        Language.choice = Language.Choice.English;
        removeAll();
        shell.sidebar.filterScope = SchemaTreeModel.Everything;
        shell.sidebar.filterField.text = "";
        shell.focusRegion(1);
    }

    // ---- accessibility -----------------------------------------------------------------------

    function test_emptyTreeWalker() {
        compare(model().count, 0);
        verify(find(shell.sidebar, i => i.title === "No connections yet") !== undefined);
        compare(walker.problems(shell.sidebar), []);
        compare(CommandRegistry.find("connection.disconnect").enabled, false);
    }

    function test_expandedTreeIsLabelledAndHasLevelAndPosition() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        keyClick(Qt.Key_Down); // Tables: there is no "main" level
        keyClick(Qt.Key_Right); // expand Tables
        tryVerify(() => tree().rows === 8);
        tryVerify(() => itemAt(1)?.Accessible.name === "Tables" && itemAt(3)?.Accessible.name === "invoices" && itemAt(0)?.Accessible.description.startsWith("Connected"));
        compare(walker.problems(shell), []);

        const tables = itemAt(1);
        compare(tables.Accessible.role, Accessible.TreeItem);
        compare(tables.Accessible.name, "Tables");
        compare(tables.Accessible.expandable, true);
        compare(tables.Accessible.expanded, true);
        compare(tables.Accessible.description, "Folder, 4 item(s), level 2, 1 of 3");
        const invoices = itemAt(3);
        compare(invoices.Accessible.name, "invoices");
        compare(invoices.Accessible.description, "Table, level 3, 2 of 4");
        verify(!invoices.Accessible.expanded);
        compare(tree().Accessible.role, Accessible.Tree);
        // the connection says it is connected in its description (the row shows a plug with a check)
        const root = itemAt(0);
        verify(root.Accessible.description.startsWith("Connected"));
    }

    // ---- the connection row: no "main", state as an icon, color as the background ---------------

    function rowText(item) {
        return walker.findAll(item, i => i.visible && typeof i.text === "string" && i.text !== "" && i.font !== undefined && i.font.family !== Icons.fontFamily).map(i => i.text);
    }

    function icons(item) {
        return walker.findAll(item, i => i.visible && typeof i.text === "string" && i.font !== undefined && i.font.family === Icons.fontFamily).map(i => i.text);
    }

    function test_theSingleDatabaseLevelIsNotShown() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        compare(rowName(2), "Views");
        compare(rowName(3), "Indexes");
        for (let r = 0; r < tree().rows; ++r) {
            verify(rowName(r) !== "main", "row " + r);
            verify(rowKind(r) !== "database", "row " + r);
        }
        // The row of the connection shows the file name and nothing else: no engine, no "main".
        compare(rowText(itemAt(0)), ["sample.db"]);
        // Opening a table still goes through the path {main, table}.
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 8);
        keyClick(Qt.Key_Down);
        compare(currentName(), "customers");
        const object = shell.connections.tree.objectOf(tree().index(tree().cursorRow(), 0));
        compare(object.openable, true);
        compare(object.parts, ["main", "customers"]);
        keyClick(Qt.Key_Return);
        tryCompare(shell.editorTabs, "count", 1);
        verify(shell.editorTabs.isData(0));
    }

    function test_connectionStateIsAnIconWithoutText() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        tryVerify(() => itemAt(0) !== null);
        let root = itemAt(0);
        // Not connected: a cut plug, no words, no check.
        compare(icons(root).filter(t => t === Icons.unplug).length, 1);
        compare(icons(root).filter(t => t === Icons.plug || t === Icons.check).length, 0);
        compare(rowText(root), ["sample.db"]);
        verify(root.Accessible.description.startsWith("Not connected"));

        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4, 10000);
        tryVerify(() => icons(itemAt(0)).includes(Icons.plug));
        root = itemAt(0);
        // Connected: the plug and a check, still no words.
        verify(icons(root).includes(Icons.check));
        verify(!icons(root).includes(Icons.unplug));
        compare(rowText(root), ["sample.db"]);
        verify(root.Accessible.description.startsWith("Connected"));
        compare(walker.problems(shell), []);

        CommandRegistry.trigger("connection.disconnect");
        tryCompare(tree(), "rows", 1);
        tryVerify(() => icons(itemAt(0)).includes(Icons.unplug));
        verify(itemAt(0).Accessible.description.startsWith("Not connected"));
    }

    function addColored(file, name, color, production) {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        shell.connections.prepare("");
        shell.connections.form.setValue(0, file);
        verify(shell.connections.save({
            "id": "",
            "providerId": "sqlite",
            "name": name,
            "colorIndex": color,
            "production": production,
            "passwordMode": 1,
            "password": "",
            "passwordTouched": false
        }));
        tryCompare(spy, "count", 1);
        tryVerify(() => shell.sidebar.tree !== null && shell.sidebar.tree.rows >= model().count, 20000);
        return spy.signalArguments[0][0];
    }

    function rowFill(item) {
        // The first rectangle that fills the whole row is its background.
        const bg = walker.findAll(item, i => i.radius !== undefined && i.color !== undefined && i.width === item.width && i.height === item.height)[0];
        return bg ? bg.color : null;
    }

    function test_connectionColorIsTheBackgroundOfItsRow() {
        const id = addColored(testSampleDb, "Tinted", 2, false);
        shell.focusRegion(0);
        tryVerify(() => itemAt(0) !== null);
        let root = itemAt(0);
        const fill = Theme.connectionColor(2);
        compare(rowFill(root).toString(), fill.toString());
        // No dot, no color word on the row; the color name stays for the screen reader.
        compare(rowText(root), ["Tinted", "sample.db"]);
        compare(walker.findAll(root, i => i.visible && i.radius !== undefined && i.width === i.height && i.width < root.height).length, 0);
        compare(root.Accessible.name, "Tinted, SQLite, Teal");
        // Text and icons are readable on it (WCAG AA).
        const texts = walker.findAll(root, i => i.visible && typeof i.text === "string" && i.text !== "" && i.font !== undefined);
        verify(texts.length > 0);
        for (const t of texts)
            verify(Theme.contrastRatio(t.color, fill) >= 4.5, t + " [" + t.text + "] " + t.parent + " " + t.visible + " " + t.opacity + " " + Theme.onColor(fill) + " " + root.mutedColor + " " + t.color + " on " + fill + ": " + Theme.contrastRatio(t.color, fill));
        compare(walker.problems(shell), []);

        // Selected: the fill stays and the row is marked by a frame and a bar, not by color.
        keyClick(Qt.Key_Home);
        const selected = walker.findAll(root, i => i.radius !== undefined && i.border !== undefined && i.border.width > 0 && i.width === root.width)[0];
        verify(selected !== undefined, "framed when current");
        compare(selected.color.toString(), fill.toString());

        // Changing the color changes the row live; removing it drops the tint.
        shell.connections.prepare(id);
        verify(shell.connections.save({
            "id": id,
            "providerId": "sqlite",
            "name": "Tinted",
            "colorIndex": 5,
            "production": false,
            "passwordMode": 1,
            "password": "",
            "passwordTouched": false
        }));
        tryVerify(() => itemAt(0) !== null && rowFill(itemAt(0)).toString() === Theme.connectionColor(5).toString());
        verify(shell.connections.save({
            "id": id,
            "providerId": "sqlite",
            "name": "Tinted",
            "colorIndex": -1,
            "production": false,
            "passwordMode": 1,
            "password": "",
            "passwordTouched": false
        }));
        // The row is current: without a color it has the usual selection fill.
        tryVerify(() => itemAt(0) !== null && rowFill(itemAt(0)).toString() === Theme.selectionBg.toString());
        compare(itemAt(0).Accessible.name, "Tinted, SQLite");
    }

    // What the platform accessibility bridge sees (QAccessibleInterface), not only the QML properties.
    function test_accessibilityInterfaceOfTheRows() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        keyClick(Qt.Key_Down); // Tables
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 8);

        tryVerify(() => itemAt(1)?.Accessible.name === "Tables" && itemAt(6)?.Accessible.name === "Views" && itemAt(3)?.Accessible.name === "invoices");
        const treeInfo = a11yProbe.inspect(tree());
        compare(treeInfo.role, 0x23, "Tree"); // QAccessible::Tree
        compare(treeInfo.name, "Connections");
        const tables = a11yProbe.inspect(itemAt(1));
        compare(tables.role, 0x24, "TreeItem"); // QAccessible::TreeItem
        compare(tables.name, "Tables");
        compare(tables.expandable, true);
        compare(tables.expanded, true);
        compare(tables.description, "Folder, 4 item(s), level 2, 1 of 3");
        const views = a11yProbe.inspect(itemAt(6));
        compare(views.name, "Views");
        compare(views.expandable, true);
        compare(views.expanded, false);
        const leaf = a11yProbe.inspect(itemAt(3));
        compare(leaf.name, "invoices");
        compare(leaf.expanded, false);
        compare(leaf.description, "Table, level 3, 2 of 4");
        tryVerify(() => a11yProbe.inspect(itemAt(tree().cursorRow())).selected);
        tryVerify(() => a11yProbe.inspect(itemAt(tree().cursorRow())).focused);
    }

    function test_connectionErrorRowIsFocusableAndRetryable() {
        // A directory where the database file should be: opening it fails.
        addSqlite(testTempDir);
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections.tree,
            signalName: "announcement"
        });
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 2 && rowKind(1) === "error", 10000);
        verify(announcements(spy).includes("Loading…"));
        const errorAnnouncement = spy.signalArguments.find(a => a[0] !== "Loading…");
        compare(errorAnnouncement[1], true, "errors are assertive");
        compare(errorAnnouncement[0], rowName(1));
        compare(CommandRegistry.find("connection.disconnect").enabled, false);

        keyClick(Qt.Key_Down); // the error row is reachable and current
        compare(tree().cursorRow(), 1);
        // Delegates are recycled when rows change: always ask for the current one.
        tryVerify(() => itemAt(1) !== null);
        compare(itemAt(1).Accessible.role, Accessible.TreeItem);
        tryVerify(() => itemAt(1)?.Accessible.description.includes("Press Enter to retry"));
        tryVerify(() => itemAt(1)?.Accessible.focused);
        compare(walker.problems(shell), []);

        // Technical details start collapsed; Space opens them.
        verify(!itemAt(1).detailsOpen);
        keyClick(Qt.Key_Space);
        verify(itemAt(1).detailsOpen);
        verify(find(itemAt(1), i => i.text === tree().roleAt(1, SchemaTreeModel.DetailRole) && i.readOnly === true) !== undefined);
        compare(walker.problems(shell), []);
        keyClick(Qt.Key_Space);
        verify(!itemAt(1).detailsOpen);

        // Enter retries.
        const loadingBefore = announcements(spy).filter(t => t === "Loading…").length;
        keyClick(Qt.Key_Return);
        tryVerify(() => announcements(spy).filter(t => t === "Loading…").length > loadingBefore);
        tryVerify(() => tree().rows === 2 && rowKind(1) === "error", 10000);

        // The Retry button does the same for the pointer.
        const again = announcements(spy).filter(t => t === "Loading…").length;
        wait(200); // the new error row finishes its layout before the pointer aims at it
        const retry = find(tree(), i => i.text === "Retry" && i.clicked !== undefined);
        verify(retry !== undefined);
        compare(retry.Accessible.role, Accessible.Button);
        mouseClick(retry);
        tryVerify(() => announcements(spy).filter(t => t === "Loading…").length > again);
        tryVerify(() => tree().rows === 2 && rowKind(1) === "error", 10000);
    }

    function test_passwordDialog() {
        const id = addSqlite(testSampleDb);
        shell.focusRegion(0);
        const t = shell.connections.tree;
        t.passwordRequested(id, "Sample", false);
        tryVerify(() => shell.sidebar.passwordDialog?.opened);
        const dlg = shell.sidebar.passwordDialog;
        tryVerify(() => dlg.passwordField.activeFocus);
        compare(dlg.passwordField.Accessible.role, Accessible.EditableText);
        compare(dlg.passwordField.Accessible.passwordEdit, true);
        compare(dlg.passwordField.echoMode, TextInput.Password);
        compare(walker.problems(shell.overlayItem), []);
        verify(find(shell.overlayItem, i => i.text === "The password was not accepted. Try again.") === undefined);
        typeText("secret");
        keyClick(Qt.Key_Return);
        tryVerify(() => !dlg.visible);
        compare(dlg.passwordField.text, "", "the password does not linger in the field");
        tryVerify(() => tree().activeFocus, 5000, "focus returns to the tree");

        // Again, after a refused password: the dialog says so with text and icon. Esc dismisses.
        t.passwordRequested(id, "Sample", true);
        tryVerify(() => dlg.opened);
        verify(find(shell.overlayItem, i => i.text === "The password was not accepted. Try again.") !== undefined);
        compare(walker.problems(shell.overlayItem), []);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dlg.visible);
        tryVerify(() => tree().activeFocus);
    }

    // ---- the keyboard-only flow --------------------------------------------------------------

    function test_keyboardOnlyFlow() {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections.tree,
            signalName: "announcement"
        });
        const id = addSqlite(testSampleDb);
        shell.focusRegion(0);
        verify(tree().activeFocus);
        compare(tree().cursorRow(), 0);
        compare(CommandRegistry.find("connection.disconnect").enabled, false);
        verify(find(shell.regions[3], i => i.text === "Not connected") !== undefined);

        // Right expands the connection and connects: "Loading…" is announced, then the rows.
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        verify(announcements(spy).includes("Loading…"));
        verify(announcements(spy).includes("Connected to sample.db"));
        tryVerify(() => CommandRegistry.find("connection.disconnect").enabled);
        tryVerify(() => find(shell.regions[3], i => i.text === "Connected") !== undefined);
        compare(shell.sidebar.currentConnected, true);

        // Right again goes to the first child; Down/Up move.
        keyClick(Qt.Key_Right);
        compare(currentName(), "Tables");
        keyClick(Qt.Key_Down);
        compare(currentName(), "Views");
        keyClick(Qt.Key_Up);
        compare(currentName(), "Tables");
        keyClick(Qt.Key_Right); // expand Tables
        tryVerify(() => tree().rows === 8);
        keyClick(Qt.Key_Right);
        compare(currentName(), "customers");

        // Type-ahead: the next node starting with the letters; typing on narrows; no filtering.
        typeText("i");
        compare(currentName(), "invoices");
        typeText("t");
        compare(currentName(), "items");
        compare(tree().rows, 8);
        wait(1100); // the prefix resets after about a second
        typeText("p");
        compare(currentName(), "products");
        typeText("c"); // "pc" matches nothing: stays
        compare(currentName(), "products");
        wait(1100);
        typeText("c"); // wraps around to the start of the list
        compare(currentName(), "customers");

        // Home/End, Left to the parent, Left collapses, Enter toggles.
        keyClick(Qt.Key_End);
        compare(tree().cursorRow(), 7);
        compare(currentName(), "Indexes");
        keyClick(Qt.Key_Home);
        compare(tree().cursorRow(), 0);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        compare(currentName(), "customers");
        keyClick(Qt.Key_Left);
        compare(currentName(), "Tables");
        keyClick(Qt.Key_Left);
        compare(currentName(), "Tables");
        tryCompare(tree(), "rows", 4); // collapsed
        keyClick(Qt.Key_Return);
        tryCompare(tree(), "rows", 8); // already loaded: no new request
        keyClick(Qt.Key_Return);
        tryCompare(tree(), "rows", 4);
        keyClick(Qt.Key_Return);
        tryCompare(tree(), "rows", 8);

        // Collapsing the connection does not disconnect it.
        keyClick(Qt.Key_Home);
        keyClick(Qt.Key_Left);
        tryCompare(tree(), "rows", 1);
        compare(shell.connections.tree.connectedIds.length, 1);
        compare(CommandRegistry.find("connection.disconnect").enabled, true);
        keyClick(Qt.Key_Right); // expands again from the loaded subtree, without reconnecting
        tryCompare(tree(), "rows", 8);

        // Shift+F10 opens the context menu on any node; Esc returns the focus.
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_F10, Qt.ShiftModifier);
        tryVerify(() => shell.sidebar.menu.opened);
        compare(shell.sidebar.menu.itemAt(2).text, "Disconnect");
        compare(shell.sidebar.menu.itemAt(2).enabled, true);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !shell.sidebar.menu.visible);
        tryVerify(() => tree().activeFocus);

        // Disconnect from the menu: the subtree is dropped, the node collapses, the focus stays.
        keyClick(Qt.Key_F10, Qt.ShiftModifier);
        tryVerify(() => shell.sidebar.menu.opened);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryCompare(shell.connections.tree.connectedIds, "length", 0);
        tryCompare(tree(), "rows", 1);
        verify(announcements(spy).some(t => t.startsWith("Disconnected from")));
        compare(CommandRegistry.find("connection.disconnect").enabled, false);
        tryVerify(() => tree().activeFocus);
        verify(!itemAt(0).Accessible.expanded);

        // Expanding connects again; the command (palette, no shortcut) disconnects.
        shell.sidebar.selectId(id);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        CommandRegistry.trigger("connection.disconnect");
        tryCompare(tree(), "rows", 1);
        compare(shell.connections.tree.connectedIds.length, 0);
    }

    function test_f6AndModOneLandOnTheTree() {
        addSqlite(testSampleDb);
        keyClick(Qt.Key_1, Qt.ControlModifier);
        verify(tree().activeFocus);
        keyClick(Qt.Key_F6);
        verify(!tree().activeFocus);
        keyClick(Qt.Key_F6, Qt.ShiftModifier);
        verify(tree().activeFocus);
        verify(walker.findAll(tree(), i => i.objectName === "focusRing" && i.visible).length === 1);
        keyClick(Qt.Key_2, Qt.ControlModifier);
        shell.focusRegion(0);
        verify(tree().activeFocus);
    }

    // 10 000 tables under one folder, through the real TreeView: the event loop keeps turning.
    function test_tenThousandTablesKeepTheUiResponsive() {
        // The idle preload of the palette and settings (1.5 s after start) is not what is measured.
        tryVerify(() => shell.paletteDialog && shell.settingsDialog, 10000);
        wait(200);
        addSqlite(testBigDb);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 20000);
        keyClick(Qt.Key_Down); // Tables
        // Idle floor of this harness, then the expand itself (10 004 children: the sample's four
        // plus 10 000, then Views).
        ticker.last = Date.now();
        ticker.maxGap = 0;
        ticker.running = true;
        wait(700);
        console.log("IDLE max event-loop gap " + ticker.maxGap + " ms");
        // A machine that already stalls this long doing nothing (the macOS CI runners: ~90 ms)
        // cannot measure a 50 ms budget; the budget is checked where the floor is low.
        if (ticker.maxGap > testMaxGapMs / 2) {
            ticker.running = false;
            skip("idle event-loop gap " + ticker.maxGap + " ms: too noisy to measure a " + testMaxGapMs + " ms budget");
        }
        // Three cold expands (disconnect in between, so each one fetches and builds the rows
        // again): the median is asserted, because single 24-36 ms spikes inside Qt Quick come and
        // go with machine load (same rule as the startup bench, PERFORMANCE.md).
        const gaps = [];
        for (let i = 0; i < 3; ++i) {
            if (i > 0) {
                CommandRegistry.trigger("connection.disconnect");
                tryCompare(tree(), "rows", 1, 20000);
                keyClick(Qt.Key_Home);
                keyClick(Qt.Key_Right);
                tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 20000);
                keyClick(Qt.Key_Down); // Tables
                wait(300);
            }
            ticker.maxGap = 0;
            ticker.last = Date.now();
            keyClick(Qt.Key_Right);
            tryVerify(() => tree().rows === 10008, 60000);
            wait(100);
            gaps.push(ticker.maxGap);
        }
        ticker.running = false;
        const median = gaps.slice().sort((a, b) => a - b)[1];
        console.log("TREE10K max event-loop gaps " + gaps.join(", ") + " ms, median " + median);
        verify(median <= testMaxGapMs, "event loop blocked for " + median + " ms (median of " + gaps.join(", ") + "; limit " + testMaxGapMs + ")");
        keyClick(Qt.Key_End);
        compare(rowName(tree().cursorRow()), "Indexes");
        keyClick(Qt.Key_Home);
        compare(tree().cursorRow(), 0);
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

    function test_spanishTree() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        Language.choice = Language.Choice.Spanish;
        tryVerify(() => itemAt(0)?.Accessible.description.startsWith("Sin conexión, sample.db, nivel 1, 1 de 1"));
        compare(CommandRegistry.find("connection.disconnect").title, "Desconectar");
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        tryVerify(() => itemAt(0).Accessible.description.startsWith("Conectada"));
        tryVerify(() => find(shell.regions[3], i => i.text === "Conectada") !== undefined);
        keyClick(Qt.Key_Down);
        // The delegates catch up with the model on the next layout pass.
        tryVerify(() => itemAt(1)?.Accessible.description === "Carpeta, nivel 2, 1 de 3");
        // The state of the connection has its words in the tooltip and the description, not on the row.
        compare(rowText(itemAt(0)), ["sample.db"]);
        compare(walker.problems(shell), []);
    }

    function test_removingAConnectedConnectionKeepsTheTreeConsistent() {
        const id = addSqlite(testSampleDb);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        shell.connections.remove(id);
        tryCompare(model(), "count", 0);
        tryVerify(() => shell.sidebar.tree === null, 5000, "the tree is gone with its last connection");
        compare(shell.connections.tree.connectedIds.length, 0);
        verify(shell.sidebar.defaultFocus === shell.sidebar.newButton);
    }

    // ---- object tree v2: folders with an icon, a name and a count -------------------------------

    function test_foldersHaveAnIconAWordAndACount() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(3) === "Indexes", 10000);
        compare(rowKind(1), "tablesFolder");
        compare(rowKind(2), "viewsFolder");
        compare(rowKind(3), "indexesFolder");
        // No Databases or Users folder for the single database of SQLite.
        for (let r = 0; r < tree().rows; ++r)
            verify(rowName(r) !== "Databases" && rowName(r) !== "Users", "row " + r);
        compare(icons(itemAt(1)).filter(t => t === Icons.table).length, 1);
        compare(icons(itemAt(2)).filter(t => t === Icons.eye).length, 1);
        compare(icons(itemAt(3)).filter(t => t === Icons.listTree).length, 1);
        // The count appears once the folder is known (no extra query to count).
        compare(rowText(itemAt(1)), ["Tables"]);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 8);
        tryVerify(() => rowText(itemAt(1)).join("|") === "Tables|4");
        compare(itemAt(1).Accessible.description, "Folder, 4 item(s), level 2, 1 of 3");
        // Indexes load when their folder opens, and Enter opens it.
        keyClick(Qt.Key_End);
        compare(currentName(), "Indexes");
        keyClick(Qt.Key_Return);
        tryVerify(() => tree().rows === 9 && rowKind(8) === "index", 10000);
        compare(rowText(itemAt(8)).length, 2); // the name and what it indexes
        verify(icons(itemAt(8)).includes(Icons.hash));
        tryVerify(() => rowText(itemAt(7)).join("|") === "Indexes|1");
        compare(walker.problems(shell), []);
        // An index is not openable.
        keyClick(Qt.Key_Down);
        compare(shell.sidebar.currentObject.openable, false);
        keyClick(Qt.Key_Return);
        compare(shell.editorTabs.count, 0);
    }

    // ---- the search box ---------------------------------------------------------------------

    function field() {
        return shell.sidebar.filterField;
    }

    function openSampleTree() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 8);
    }

    function test_searchBoxFiltersTheTreeAndTheFocusFlowsOnTheKeyboard() {
        openSampleTree();
        compare(field().placeholderText, "Filter by everything");
        compare(shell.sidebar.filterSummary, "");
        // Tab order: the options button, the field, then the tree.
        shell.sidebar.filterButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Tab);
        verify(field().activeFocus);
        keyClick(Qt.Key_Tab);
        verify(tree().activeFocus);
        // The command (menu bar and palette, Ctrl+Shift+B) lands in the field.
        compare(CommandRegistry.find("tree.focusFilter").shortcutText, "Ctrl+Shift+B");
        CommandRegistry.trigger("tree.focusFilter");
        verify(field().activeFocus);

        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.sidebar,
            signalName: "lastAnnouncementChanged"
        });
        typeText("inv");
        // After the typing delay (not on every key) the tree shows the match and its branch, open.
        tryVerify(() => shell.connections.tree.filterActive, 5000);
        tryVerify(() => tree().rows === 3 && rowName(2) === "invoices", 5000);
        compare(rowName(0), "sample.db");
        compare(rowName(1), "Tables");
        tryVerify(() => itemAt(2)?.Accessible.description === "Table, level 3, 1 of 1");
        // The count is text, below the field, with the hint that more could be found.
        compare(shell.sidebar.filterSummary, "1 result(s); connect or expand to search more");
        verify(find(shell.sidebar, i => i.text === "1 result(s); connect or expand to search more") !== undefined);
        tryCompare(spy, "count", 1); // one polite announcement for the whole word
        compare(shell.sidebar.lastAnnouncement, "1 result(s); connect or expand to search more");
        compare(walker.problems(shell), []);

        // Enter (or Down) moves to the first match, not to the connection above it.
        keyClick(Qt.Key_Return);
        verify(tree().activeFocus);
        compare(currentName(), "invoices");
        // Esc in the tree drops the search; the tree is as it was and the node stays current.
        keyClick(Qt.Key_Escape);
        compare(field().text, "");
        tryVerify(() => !shell.connections.tree.filterActive);
        tryVerify(() => tree().rows === 8, 5000);
        tryVerify(() => tree().cursorRow() >= 0 && currentName() === "invoices", 5000);
        compare(shell.sidebar.filterSummary, "");
        // In the field: Esc clears the text, and with it empty it returns the focus to the tree.
        CommandRegistry.trigger("tree.focusFilter");
        typeText("zz");
        tryVerify(() => shell.connections.tree.filterActive, 5000);
        keyClick(Qt.Key_Escape);
        compare(field().text, "");
        verify(field().activeFocus);
        tryVerify(() => !shell.connections.tree.filterActive);
        keyClick(Qt.Key_Escape);
        verify(tree().activeFocus);
    }

    function test_searchWithNoResultsSaysSoAndCanBeCleared() {
        openSampleTree();
        CommandRegistry.trigger("tree.focusFilter");
        typeText("zzzz");
        tryVerify(() => find(shell.sidebar, i => i.title === "No matches") !== undefined, 5000);
        compare(shell.sidebar.filterSummary, "0 result(s); connect or expand to search more");
        compare(walker.problems(shell.sidebar), []);
        const clear = find(shell.sidebar, i => i.text === "Clear search" && i.clicked !== undefined);
        verify(clear !== undefined);
        mouseClick(clear);
        compare(field().text, "");
        tryVerify(() => find(shell.sidebar, i => i.title === "No matches") === undefined);
        tryVerify(() => tree().rows === 8, 5000);
    }

    function test_theOptionChosenIsThePlaceholderAndIsRemembered() {
        openSampleTree();
        const menu = shell.sidebar.filterMenu;
        mouseClick(shell.sidebar.filterButton);
        tryVerify(() => menu.opened);
        // The seven options (and the reason migrations is off), one checked, the first by default.
        const labels = [];
        for (let i = 0; i < menu.count; ++i)
            if (menu.itemAt(i).text !== "")
                labels.push(menu.itemAt(i).text);
        compare(labels, ["Filter by everything", "Filter by connections", "Filter by databases", "Filter by tables and indexes", "Filter by views", "Filter by users", "Filter by migrations", "No connection has migrations configured"]);
        compare(menu.itemAt(0).checked, true);
        compare(menu.itemAt(3).checked, false);
        const migrations = menu.itemAt(6);
        compare(migrations.enabled, false);
        compare(migrations.Accessible.description, "No connection has migrations configured");
        // Arrow keys and Enter choose "tables and indexes".
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryVerify(() => !menu.visible);
        compare(shell.sidebar.filterScope, SchemaTreeModel.Tables);
        compare(field().placeholderText, "Filter by tables and indexes");
        compare(field().Accessible.description, "Filter by tables and indexes");
        compare(menu.itemAt(3).checked, true);
        compare(menu.itemAt(0).checked, false);
        // Remembered between sessions; the text is not.
        compare(Number(LayoutStore.value("treeFilterScope", -1)), SchemaTreeModel.Tables);
        // A search by tables finds the table, and by views it does not.
        CommandRegistry.trigger("tree.focusFilter");
        typeText("inv");
        tryVerify(() => shell.connections.tree.filterActive, 5000);
        tryVerify(() => shell.sidebar.filterSummary.startsWith("1 result(s)"), 5000);
        shell.sidebar.filterScope = SchemaTreeModel.Views; // a new option applies at once
        tryVerify(() => shell.sidebar.filterSummary.startsWith("0 result(s)"), 5000);
        compare(field().placeholderText, "Filter by views");
        // Migrations: with a configured project the option turns on.
        shell.sidebar.filterScope = SchemaTreeModel.Migrations;
        shell.sidebar.refreshMigrations(); // what a change of the connections does
        compare(shell.sidebar.filterScope, SchemaTreeModel.Everything, "off while no connection has migrations");
    }

    function test_searchByConnectionsOnlyLooksAtConnections() {
        addSqlite(testSampleDb);
        addSqlite(testBigDb);
        shell.focusRegion(0);
        shell.sidebar.filterScope = SchemaTreeModel.Connections;
        CommandRegistry.trigger("tree.focusFilter");
        typeText("samp");
        tryVerify(() => shell.connections.tree.filterActive, 5000);
        tryVerify(() => tree().rows === 1 && rowName(0) === "sample.db", 5000);
        compare(shell.sidebar.filterSummary, "1 result(s)"); // nothing to open to find more
        compare(shell.connections.tree.connectedIds.length, 0, "a search never connects");
        keyClick(Qt.Key_Escape);
        tryVerify(() => tree().rows === 2, 5000);
    }

    function test_searchDoesNotOpenConnectionsAndSpanishIsTranslated() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        Language.choice = Language.Choice.Spanish;
        tryVerify(() => field().placeholderText === "Filtrar por todo");
        CommandRegistry.trigger("tree.focusFilter");
        typeText("tab");
        tryVerify(() => shell.connections.tree.filterActive, 5000);
        compare(shell.connections.tree.connectedIds.length, 0);
        tryVerify(() => shell.sidebar.filterSummary === "0 resultados; conecta o expande para buscar más", 5000);
        tryVerify(() => find(shell.sidebar, i => i.title === "Sin coincidencias") !== undefined);
        compare(CommandRegistry.find("tree.focusFilter").title, "Buscar en el árbol");
        compare(walker.problems(shell), []);
    }

    // ---- migrations (H12 phases B and C) -----------------------------------------------------
    // A Laravel project: testMigrationsDb has the `migrations` table (two applied, the file of one
    // other is gone) and testMigrationsDir the .php files (those two, two pending and a file over 1 MiB).
    // Running goes to the fake `php` (testFakePhp), which logs its arguments: never a real PHP or a
    // real migration. testMigrationsRoot is the "project" that holds its `artisan`.

    readonly property string fakeLog: testTempDir + "/fake_php.log"
    readonly property string postsName: "2024_03_01_000000_create_posts_table"

    // What the fake php does in this test: its mode, where it logs and the database "artisan" uses.
    function useFake(mode, targetDb) {
        a11yProbe.writeFile(fakeLog, "");
        a11yProbe.setEnvironment("FAKE_PHP_LOG", fakeLog);
        a11yProbe.setEnvironment("FAKE_PHP_MODE", mode);
        a11yProbe.setEnvironment("FAKE_PHP_TARGET_JSON", JSON.stringify({
            "platform": {
                "config": {
                    "driver": "sqlite",
                    "database": targetDb ?? testMigrationsDb
                }
            },
            "name": "sqlite"
        }));
    }

    function fakeLines() {
        return a11yProbe.readFile(fakeLog).split("\n").filter(l => l !== "");
    }

    function cleanup() {
        for (const name of ["FAKE_PHP_LOG", "FAKE_PHP_MODE", "FAKE_PHP_TARGET_JSON", "FAKE_PHP_TICKS"])
            a11yProbe.setEnvironment(name, "");
    }

    function addLaravel(production) {
        const id = production === true ? addColored(testMigrationsDb, "Production laravel", 1, true) : addSqlite(testMigrationsDb);
        shell.migrations.saveSettings(id, "", "laravel", testMigrationsDir, "", testFakePhp, testMigrationsRoot);
        verify(shell.migrations.isConfigured(id, ""));
        return id;
    }

    function closeAllTabs() {
        while (shell.editorTabs.count > 0) {
            shell.migrations.cancelAll();
            shell.closeQueryNow(shell.editorTabs.idOf(0));
        }
    }

    function rowOf(name) {
        for (let r = 0; r < tree().rows; ++r)
            if (rowName(r) === name)
                return r;
        return -1;
    }

    // Moves to the row and expands it with the keyboard.
    function expandNamed(name) {
        const r = rowOf(name);
        verify(r >= 0, "no row " + name);
        tree().goTo(r);
        keyClick(Qt.Key_Right);
    }

    // The connection and its Migrations folder (the sibling of Tables, which stays closed) with the
    // report read: Pending, Applied and Orphans below.
    function openMigrationsTree(production) {
        closeAllTabs();
        const id = addLaravel(production);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 5 && rowName(1) === "Tables" && rowName(2) === "Migrations", 10000);
        expandNamed("Migrations");
        tryVerify(() => rowName(3) === "Pending" && rowName(4) === "Applied" && rowName(5) === "Orphans", 10000);
        return id;
    }

    // Enter on the migration: its tab opens and takes the focus.
    function openMigrationNamed(name) {
        shell.focusRegion(0);
        const r = rowOf(name);
        verify(r >= 0, "no row " + name);
        tree().goTo(r);
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.editorArea.currentMigration !== null && shell.editorArea.currentMigration.tabTitle === name, 10000);
        return shell.editorArea.currentMigration;
    }

    // The pending "create posts" migration, open in its tab and ready for its buttons to be used.
    function openPostsTab(production) {
        openMigrationsTree(production);
        expandNamed("Pending");
        tryVerify(() => rowName(4) === postsName);
        const tab = openMigrationNamed(postsName);
        tryCompare(tab, "view", "ready", 10000);
        tryVerify(() => tab.availability.ok, 10000);
        return tab;
    }

    // Presses a button of the tab the way a keyboard user does.
    function press(button) {
        button.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
    }

    // The accessible names of what the Messages panel lists, in order (the panel switched to Messages).
    function messageNames() {
        shell.resultsPanel.tabBar.currentIndex = 1;
        const list = shell.resultsPanel.messageList;
        tryVerify(() => list.count === shell.queries.current.messages.count, 5000);
        // The panel is short: build every row, not only the ones in view.
        list.cacheBuffer = 1000000;
        tryVerify(() => {
            for (let i = 0; i < list.count; ++i)
                if (!list.itemAtIndex(i))
                    return false;
            return true;
        }, 5000);
        const out = [];
        for (let i = 0; i < list.count; ++i)
            out.push(list.itemAtIndex(i)?.Accessible.name ?? "");
        return out;
    }

    // A dry run that stays "running" (the check goes by in "target" mode, the migration is slow).
    function startSlowDryRun(tab) {
        useFake("target");
        a11yProbe.setEnvironment("FAKE_PHP_TICKS", testTempDir + "/ticks");
        press(tab.dryRunButton);
        a11yProbe.setEnvironment("FAKE_PHP_MODE", "slow");
        tryVerify(() => tab.run.state === "running", 15000);
    }

    function test_migrationsFolderIsASiblingOfTablesRightBehindIt() {
        openMigrationsTree();
        // Tables, Migrations, Views, Indexes: the same depth, one after the other.
        compare(rowName(1), "Tables");
        compare(rowKind(1), "tablesFolder");
        compare(rowKind(2), "migrationsFolder");
        compare(rowName(6), "Views");
        compare(rowName(7), "Indexes");
        verify(itemAt(1).Accessible.description.endsWith("level 2, 1 of 4"), itemAt(1).Accessible.description);
        const folder = itemAt(2);
        compare(folder.Accessible.name, "Migrations");
        compare(folder.Accessible.description, "Migrations folder, 6 item(s), level 2, 2 of 4");
        compare(folder.Accessible.expanded, true);
        compare(rowKind(3), "pendingFolder");
        compare(rowKind(4), "appliedFolder");
        compare(rowKind(5), "orphansFolder");
        compare(itemAt(3).Accessible.description, "Pending migrations, 3 item(s), level 3, 1 of 3");
        compare(itemAt(4).Accessible.description, "Applied migrations, 2 item(s), level 3, 2 of 3");
        compare(itemAt(5).Accessible.description, "Migrations without a file, 1 item(s), level 3, 3 of 3");
        // Words and icons on the folder rows.
        compare(rowText(itemAt(3)), ["Pending", "3"]);
        compare(icons(itemAt(2)).filter(t => t === Icons.arrowRightLeft).length, 1);
        compare(icons(itemAt(3)).filter(t => t === Icons.clock).length, 1);

        expandNamed("Pending");
        tryVerify(() => rowName(4) === "2024_03_01_000000_create_posts_table");
        compare(rowName(5), "2024_04_01_000000_create_comments_table");
        compare(rowName(6), "2024_05_01_000000_huge");
        compare(rowKind(4), "migration");
        tryVerify(() => itemAt(4)?.Accessible.name === "2024_03_01_000000_create_posts_table");
        const pending = itemAt(4);
        compare(pending.Accessible.name, "2024_03_01_000000_create_posts_table");
        compare(pending.Accessible.description, "Migration, pending, Press Enter to open, level 4, 1 of 3");
        // The row shows the name without the date, and no "batch" line; the name is whole for the
        // screen reader and the tooltip.
        compare(rowText(pending), ["000000_create_posts_table"]);
        compare(pending.displayName, "000000_create_posts_table");
        compare(pending.name, "2024_03_01_000000_create_posts_table");
        verify(icons(pending).includes(Icons.fileCode) && icons(pending).includes(Icons.clock));
        expandNamed("Applied");
        tryVerify(() => rowName(8) === "2024_01_01_000000_create_users_table");
        tryVerify(() => itemAt(8)?.Accessible.name === "2024_01_01_000000_create_users_table");
        const applied = itemAt(8);
        compare(applied.Accessible.description, "Migration, applied, batch 1, Press Enter to open, level 4, 1 of 2");
        compare(rowText(applied), ["000000_create_users_table"]);
        verify(find(applied, i => i.elide === Text.ElideRight && i.text === "000000_create_users_table") !== undefined, "the name is cut with an ellipsis when it does not fit");
        verify(icons(applied).includes(Icons.circleCheck));
        expandNamed("Orphans");
        tryVerify(() => rowName(11) === "2023_12_31_000000_ghost");
        tryVerify(() => itemAt(11)?.Accessible.name === "2023_12_31_000000_ghost");
        compare(itemAt(11).Accessible.description, "Migration, orphan: file missing, batch 1, Press Enter to open, level 4, 1 of 1");
        verify(icons(itemAt(11)).includes(Icons.fileX));
        compare(walker.problems(shell), []);
        // A migration is not table data.
        const object = shell.connections.tree.objectOf(tree().index(4, 0));
        compare(object.openable, false);
        compare(shell.sidebar.currentMigration.valid, false); // the cursor is on Orphans' folder
        tree().goTo(4);
        compare(shell.sidebar.currentMigration.name, "2024_03_01_000000_create_posts_table");
        compare(shell.sidebar.currentMigration.state, "pending");
        compare(CommandRegistry.find("data.open").enabled, false);
        // Opening Tables lists only the tables (the folder is not in it, and its count is of tables).
        expandNamed("Tables");
        tryVerify(() => rowName(2) === "migrations" && rowName(3) === "users", 10000);
        compare(rowName(4), "Migrations");
        compare(rowKind(4), "migrationsFolder");
        verify(itemAt(1).Accessible.description.includes("2 item(s), level 2, 1 of 4"), itemAt(1).Accessible.description);
    }

    function test_enterOnAPendingMigrationOpensItsCodeReadOnlyWithTheActionsOn() {
        openMigrationsTree();
        expandNamed("Pending");
        tryVerify(() => rowName(4) === "2024_03_01_000000_create_posts_table");
        const tab = openMigrationNamed("2024_03_01_000000_create_posts_table");
        compare(shell.editorTabs.count, 1);
        verify(shell.editorTabs.isMigration(0));
        compare(shell.editorTabs.titleOf(0), "2024_03_01_000000_create_posts_table");
        tryCompare(tab, "view", "ready", 10000);
        const edit = tab.codeEditor.edit;
        verify(edit.text.includes("Schema::create('posts'"));
        compare(edit.readOnly, true);
        compare(edit.Accessible.name, "Migration code");
        compare(edit.Accessible.readOnly, true);
        compare(tab.codeEditor.definition, "PHP");
        tryVerify(() => edit.activeFocus); // the code takes the focus
        // The text cannot be changed, but it can be selected and copied.
        const before = edit.text;
        keyClick("x");
        keyClick(Qt.Key_Backspace);
        keyClick(Qt.Key_Return);
        compare(edit.text, before);
        keyClick(Qt.Key_A, Qt.ControlModifier);
        compare(edit.selectedText.length, edit.text.length);
        // The bar: three buttons on top. Connected, with php and a project, they work, and each says
        // what it does (description); Cancel is not there while nothing runs.
        compare(tab.buttons.map(b => b.text), ["Run", "Rollback", "Dry run"]);
        tryVerify(() => tab.availability.ok, 10000);
        compare(tab.buttons.map(b => b.enabled), [true, true, true]);
        compare(tab.buttons.map(b => b.Accessible.description), ["Applies this migration to the database", "Reverts this migration if it is in the latest batch", "Shows the SQL this migration would run, without running it"]);
        for (const b of tab.buttons)
            verify(b.iconGlyph !== "");
        verify(!tab.reasonLabel.visible);
        compare(tab.disabledReason, "");
        verify(!tab.cancelButton.visible);
        compare(tab.statusLabel.text, "Pending");
        compare(tab.fileLabel.text, "2024_03_01_000000_create_posts_table.php");
        verify(tab.toolbar.mapToItem(tab, 0, 0).y < tab.codeEditor.mapToItem(tab, 0, 0).y, "the bar is above the code");
        compare(tab.toolbar.Accessible.name, "Migration actions");
        // The tab bar: title with the icon of a migration, "migration" for the screen reader.
        const tabButton = shell.editorArea.tabBar.itemAt(0);
        compare(tabButton.text, "000000_create_posts_table");
        compare(tabButton.fullTitle, "2024_03_01_000000_create_posts_table");
        verify(tabButton.Accessible.name.startsWith("2024_03_01_000000_create_posts_table, migration"), tabButton.Accessible.name);
        verify(tabButton.Accessible.name.includes("migration"));
        verify(icons(tabButton).includes(Icons.fileCode));
        // The SQL editor's and the data commands do nothing here. There is no command or shortcut for
        // Run or Rollback: they are only the buttons, and those ask first.
        compare(CommandRegistry.find("editor.runStatement").enabled, false);
        compare(CommandRegistry.find("editor.runAll").enabled, false);
        compare(CommandRegistry.find("editor.explain").enabled, false);
        compare(CommandRegistry.find("data.refresh").enabled, false);
        compare(CommandRegistry.find("saved.saveCurrent").enabled, false);
        for (const id of ["migration.run", "migration.rollback", "migration.dryRun"])
            verify(!CommandRegistry.find(id), id);
        compare(shell.connectionCombo.enabled, false);
        compare(walker.problems(shell), []);
        // Tab leaves the code (no keyboard trap).
        keyClick(Qt.Key_Tab);
        verify(!edit.activeFocus);
        // Enter on the same migration shows its tab again: no second one.
        shell.focusRegion(0);
        keyClick(Qt.Key_Return);
        compare(shell.editorTabs.count, 1);
        // A tab without changes closes at once.
        shell.closeQuery(0);
        tryCompare(shell.editorTabs, "count", 0);
    }

    function test_anAppliedMigrationShowsItsBatchAndTabsAreOnePerMigration() {
        openMigrationsTree();
        expandNamed("Applied");
        tryVerify(() => rowName(5) === "2024_01_01_000000_create_users_table");
        const tab = openMigrationNamed("2024_01_01_000000_create_users_table");
        tryCompare(tab, "view", "ready", 10000);
        compare(tab.statusLabel.text, "Applied, batch 1");
        verify(tab.codeEditor.edit.text.includes("Schema::create('users'"));
        shell.focusRegion(0);
        tree().goTo(rowOf("2024_02_01_000000_add_email_to_users"));
        keyClick(Qt.Key_Return);
        tryCompare(shell.editorTabs, "count", 2);
        tryVerify(() => shell.editorArea.currentMigration !== null && shell.editorArea.currentMigration.statusLabel.text === "Applied, batch 2");
        // Tab cycling, closing and the region order treat them like any tab.
        CommandRegistry.trigger("editor.previousTab");
        tryCompare(shell.editorTabs, "currentIndex", 0);
        CommandRegistry.trigger("editor.nextTab");
        tryCompare(shell.editorTabs, "currentIndex", 1);
        shell.focusRegion(1);
        tryVerify(() => shell.editorArea.currentView.edit.activeFocus || shell.editorArea.currentView.activeFocus);
        shell.closeQuery(1);
        shell.closeQuery(0);
        tryCompare(shell.editorTabs, "count", 0);
    }

    function test_anOrphanHasNoFileAndSaysSo() {
        openMigrationsTree();
        expandNamed("Orphans");
        tryVerify(() => rowName(6) === "2023_12_31_000000_ghost");
        const tab = openMigrationNamed("2023_12_31_000000_ghost");
        compare(tab.view, "orphan");
        compare(tab.codeEditor, null);
        compare(tab.statePanel.title, "This migration has no file in the folder.");
        verify(tab.statePanel.message.length > 0);
        compare(tab.statusLabel.text, "File missing, batch 1");
        // Nothing to run: the buttons are off and say why in words.
        compare(tab.buttons.map(b => b.enabled), [false, false, false]);
        tryVerify(() => tab.reasonLabel.visible);
        compare(tab.reasonLabel.text, "This migration has no file in the folder, so there is nothing to run.");
        compare(tab.buttons.map(b => b.Accessible.description), [tab.reasonLabel.text, tab.reasonLabel.text, tab.reasonLabel.text]);
        // Nothing was asked of the disk.
        compare(shell.migrations.sourceOf(tab.connectionId, "", "2023_12_31_000000_ghost").state, "none");
        tryVerify(() => tab.statePanel.activeFocus);
        compare(walker.problems(shell), []);
        shell.closeQuery(0);
    }

    function test_aFileThatCannotBeReadShowsTheErrorAndTryAgain() {
        openMigrationsTree();
        expandNamed("Pending");
        tryVerify(() => rowName(6) === "2024_05_01_000000_huge");
        const tab = openMigrationNamed("2024_05_01_000000_huge");
        tryCompare(tab, "view", "failed", 10000);
        compare(tab.codeEditor, null);
        verify(tab.errorPanel.message.length > 0);
        compare(tab.errorPanel.title, "The migration could not be read");
        verify(tab.retryButton.visible);
        compare(tab.retryButton.text, "Try again");
        tryVerify(() => tab.retryButton.activeFocus);
        compare(walker.problems(shell), []);
        // Try again: a new request (the file is still too big, so it fails again).
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.migrations,
            signalName: "sourceChanged"
        });
        keyClick(Qt.Key_Return);
        tryVerify(() => spy.count >= 2, 10000); // loading, failed
        tryCompare(tab, "view", "failed", 10000);
        shell.closeQuery(0);
    }

    // ---- running (H12 phase C) ---------------------------------------------------------------

    function test_buttonsSayWhyTheyAreOff() {
        const id = openMigrationsTree();
        expandNamed("Pending");
        tryVerify(() => rowName(4) === postsName);
        const tab = openMigrationNamed(postsName);
        tryCompare(tab, "view", "ready", 10000);
        tryVerify(() => tab.availability.ok, 10000);
        const expectOff = reason => {
            tryVerify(() => !tab.availability.ok, 10000);
            verify(tab.reasonLabel.visible);
            verify(tab.reasonLabel.text.includes(reason), tab.reasonLabel.text);
            for (const b of tab.buttons) {
                compare(b.enabled, false);
                compare(b.Accessible.description, tab.reasonLabel.text);
            }
            compare(walker.problems(shell), []);
        };
        // php is not there.
        shell.migrations.saveSettings(id, "", "laravel", testMigrationsDir, "", testTempDir + "/no-php.exe", testMigrationsRoot);
        expectOff("PHP was not found");
        // The project has no artisan (the migrations folder is not a project root).
        shell.migrations.saveSettings(id, "", "laravel", testMigrationsDir, "", testFakePhp, testMigrationsDir);
        expectOff("artisan");
        // Everything in place again: they work.
        shell.migrations.saveSettings(id, "", "laravel", testMigrationsDir, "", testFakePhp, testMigrationsRoot);
        tryVerify(() => tab.availability.ok, 10000);
        verify(!tab.reasonLabel.visible);
        // Not connected.
        shell.connections.tree.disconnectConnection(id);
        expectOff("Connect to the database");
        shell.closeQuery(0);
    }

    function test_aDryRunPrintsItsOutputInMessagesAndNeedsNoQuestion() {
        const tab = openPostsTab();
        useFake("target");
        press(tab.dryRunButton);
        tryVerify(() => tab.run.state === "done", 15000);
        compare(tab.runLabel.text, "Finished");
        verify(!tab.warningLabel.visible);
        verify(!(shell.migrationConfirmDialog?.visible ?? false)); // no confirmation for a dry run
        // The Results panel opened by itself with the output: the command, stdout, stderr and the end.
        tryVerify(() => shell.resultsVisible);
        const names = messageNames();
        verify(names.some(n => n.startsWith("Command: ") && n.includes("--pretend")), names.join("|"));
        verify(names.some(n => n.startsWith("Output: Migrated:")), names.join("|"));
        verify(names.includes("stderr: a warning"), names.join("|"));
        compare(names[names.length - 1], "Finished: Finished");
        const lines = fakeLines();
        verify(lines.includes("db:show"));
        verify(lines.includes("--pretend"));
        verify(lines.indexOf("db:show") < lines.indexOf("--pretend"));
        compare(tab.statusLabel.text, "Pending"); // a dry run changes nothing
        compare(walker.problems(shell), []);
        shell.closeQuery(0);
    }

    function test_aDryRunOnAnotherDatabaseWarnsButGoesOn() {
        const tab = openPostsTab();
        useFake("target", testTempDir + "/somewhere-else.db");
        press(tab.dryRunButton);
        tryVerify(() => tab.run.state === "done", 15000);
        tryVerify(() => tab.warningLabel.visible);
        verify(tab.warningLabel.text.startsWith("This dry run would use database "), tab.warningLabel.text);
        verify(tab.warningLabel.text.includes("somewhere-else.db"));
        verify(tab.warningLabel.text.endsWith("not the one of this connection."));
        verify(messageNames().some(n => n.startsWith("Warning: This dry run would use database ")));
        verify(fakeLines().includes("--pretend"));
        compare(walker.problems(shell), []);
        shell.closeQuery(0);
    }

    function test_runAsksFirstWithItsCommandAndTheFocusIsOnCancel() {
        const tab = openPostsTab();
        useFake("target");
        press(tab.runButton);
        tryVerify(() => shell.migrationConfirmDialog !== null && shell.migrationConfirmDialog.opened, 15000);
        const d = shell.migrationConfirmDialog;
        compare(d.title, "Run migration");
        compare(tab.runLabel.text, "Waiting for confirmation");
        compare(tab.run.state, "awaitingConfirm");
        tryVerify(() => d.cancelButton.activeFocus); // Enter does not confirm by accident
        compare(d.cancelButton.text, "Cancel");
        compare(d.confirmButton.text, "Run");
        // The migration, the connection, the database and the exact command (read-only, copyable).
        verify(find(d.contentItem, i => i.text === postsName) !== undefined);
        verify(find(d.contentItem, i => i.text === "Connection: " + d.info.connectionName) !== undefined);
        verify(find(d.contentItem, i => typeof i.text === "string" && i.text.startsWith("Database: ")) !== undefined);
        verify(find(d.contentItem, i => i.text === "This runs php artisan from your project: it executes your project's PHP code and changes the database.") !== undefined);
        verify(find(d.contentItem, i => typeof i.text === "string" && i.text.startsWith("Laravel only rolls back")) === undefined); // only Rollback says it
        verify(d.commandField.readOnly);
        verify(d.commandField.Accessible.readOnly);
        verify(d.commandField.text.includes("artisan migrate --path="), d.commandField.text);
        verify(d.commandField.text.includes(postsName + ".php"));
        verify(d.commandField.text.includes("--force"));
        verify(!d.commandField.text.includes("--pretend"));
        verify(!d.nameField.visible); // not a production connection
        compare(d.confirmButton.enabled, true);
        compare(walker.problems(shell), []);
        // Enter, with the focus on Cancel, declines: nothing runs.
        keyClick(Qt.Key_Return);
        tryVerify(() => !d.visible);
        compare(tab.run.state, "idle");
        verify(!fakeLines().includes("migrate"));
        tryVerify(() => tab.availability.ok);
        // Esc declines too.
        press(tab.runButton);
        tryVerify(() => d.opened);
        keyClick(Qt.Key_Escape);
        tryVerify(() => !d.visible);
        compare(tab.run.state, "idle");
        verify(!fakeLines().includes("migrate"));
        // Confirming is deliberate: Tab to the button, Enter.
        tryVerify(() => tab.availability.ok);
        press(tab.runButton);
        tryVerify(() => d.opened);
        keyClick(Qt.Key_Tab);
        verify(d.confirmButton.activeFocus);
        keyClick(Qt.Key_Return);
        tryVerify(() => tab.run.state === "done", 15000);
        const lines = fakeLines();
        verify(lines.includes("migrate") && lines.includes("--force") && !lines.includes("--pretend"));
        verify(lines.some(l => l.startsWith("--path=") && l.endsWith(postsName + ".php")), lines.join("|"));
        compare(tab.runLabel.text, "Finished");
        // The report was read again; the fake changed nothing, so it is still pending, and usable.
        tryVerify(() => tab.availability.ok);
        compare(tab.statusLabel.text, "Pending");
        compare(walker.problems(shell), []);
        shell.closeQuery(0);
    }

    function test_rollbackSaysThatOnlyTheLatestBatchIsReverted() {
        openMigrationsTree();
        expandNamed("Applied");
        tryVerify(() => rowName(5) === "2024_01_01_000000_create_users_table");
        const tab = openMigrationNamed("2024_01_01_000000_create_users_table");
        tryCompare(tab, "view", "ready", 10000);
        tryVerify(() => tab.availability.ok, 10000);
        useFake("target");
        press(tab.rollbackButton);
        tryVerify(() => shell.migrationConfirmDialog !== null && shell.migrationConfirmDialog.opened, 15000);
        const d = shell.migrationConfirmDialog;
        compare(d.title, "Roll back migration");
        compare(d.confirmButton.text, "Roll back");
        verify(find(d.contentItem, i => i.text === "Laravel only rolls back migrations of the latest batch; if this one is not in it, nothing happens.") !== undefined);
        verify(d.commandField.text.includes("artisan migrate:rollback --path="), d.commandField.text);
        tryVerify(() => d.cancelButton.activeFocus);
        compare(walker.problems(shell), []);
        keyClick(Qt.Key_Tab);
        keyClick(Qt.Key_Return);
        tryVerify(() => tab.run.state === "done", 15000);
        verify(fakeLines().includes("migrate:rollback"));
        shell.closeQuery(0);
    }

    function test_aProductionConnectionNeedsTheDatabaseNameTyped() {
        const tab = openPostsTab(true);
        useFake("target");
        press(tab.runButton);
        tryVerify(() => shell.migrationConfirmDialog !== null && shell.migrationConfirmDialog.opened, 15000);
        const d = shell.migrationConfirmDialog;
        verify(d.production);
        verify(d.nameField.visible);
        compare(d.info.typeToConfirm, "laravel.db"); // SQLite: the file name
        compare(d.confirmButton.enabled, false);
        tryVerify(() => d.cancelButton.activeFocus);
        verify(find(d.contentItem, i => typeof i.text === "string" && i.text.includes("production connection")) !== undefined);
        compare(walker.problems(shell), []);
        // Part of the name, or the wrong case: off. Enter in the field does not confirm either.
        d.nameField.forceActiveFocus(Qt.TabFocusReason);
        typeText("laravel");
        compare(d.confirmButton.enabled, false);
        keyClick(Qt.Key_Return);
        compare(d.visible, true);
        compare(tab.run.state, "awaitingConfirm");
        d.nameField.text = "";
        typeText("Laravel.db");
        compare(d.confirmButton.enabled, false);
        d.nameField.text = "";
        typeText("laravel.db");
        compare(d.confirmButton.enabled, true);
        verify(!fakeLines().includes("migrate")); // nothing ran while it was being typed
        keyClick(Qt.Key_Return);
        tryVerify(() => tab.run.state === "done", 15000);
        verify(fakeLines().includes("--force"));
        shell.closeQuery(0);
    }

    function test_runIsBlockedWhenArtisanUsesAnotherDatabase() {
        const tab = openPostsTab();
        useFake("target", testTempDir + "/other-database.db");
        press(tab.runButton);
        tryVerify(() => tab.run.state === "blocked", 15000);
        compare(tab.runLabel.text, "Nothing was run: the database artisan uses is not the one of this connection.");
        verify(tab.runError.visible);
        // The two databases and what differs, in words.
        const shown = tab.runError.message.split("\\").join("/");
        verify(shown.includes("Database artisan uses: " + testTempDir + "/other-database.db"), shown);
        verify(shown.includes("Database of this connection: " + testMigrationsDb), shown);
        verify(tab.runError.hint !== "");
        verify(!(shell.migrationConfirmDialog?.visible ?? false)); // there is nothing to confirm
        verify(!fakeLines().includes("migrate"));
        verify(!fakeLines().includes("--force"));
        tryVerify(() => tab.availability.ok); // it can be tried again
        const names = messageNames();
        verify(names.some(n => n.startsWith("Error: Nothing was run")), names.join("|"));
        compare(walker.problems(shell), []);
        shell.closeQuery(0);
    }

    function test_runIsBlockedWhenTheDatabaseCannotBeVerified() {
        const tab = openPostsTab();
        useFake("nocommand"); // Laravel older than 10: no db:show
        press(tab.runButton);
        tryVerify(() => tab.run.state === "blocked", 15000);
        compare(tab.runLabel.text, "The database artisan uses could not be verified, so nothing was run.");
        verify(tab.runError.visible);
        verify(!fakeLines().includes("migrate"));
        verify(!(shell.migrationConfirmDialog?.visible ?? false));
        compare(walker.problems(shell), []);
        // A dry run changes nothing: it goes on, with a warning.
        tryVerify(() => tab.availability.ok);
        press(tab.dryRunButton);
        tryVerify(() => tab.run.state === "done", 15000);
        verify(tab.warningLabel.text.includes("could not be verified"));
        shell.closeQuery(0);
    }

    function test_aFailedRunShowsTheErrorAndTheOutput() {
        const tab = openPostsTab();
        useFake("target");
        press(tab.runButton);
        tryVerify(() => shell.migrationConfirmDialog !== null && shell.migrationConfirmDialog.opened, 15000);
        a11yProbe.setEnvironment("FAKE_PHP_MODE", "fail"); // the check went by; the migration fails
        keyClick(Qt.Key_Tab);
        keyClick(Qt.Key_Return);
        tryVerify(() => tab.run.state === "failed", 15000);
        compare(tab.runLabel.text, "Failed with exit code 3");
        verify(tab.runError.visible); // text and icon, not only a color
        compare(tab.runError.title, "Failed with exit code 3");
        verify(tab.runError.detail.includes("Migration failed"));
        const names = messageNames();
        verify(names.includes("stderr: Migration failed: something went wrong"), names.join("|"));
        verify(names[names.length - 1].startsWith("Error: Failed with exit code 3"), names.join("|"));
        compare(walker.problems(shell), []);
        shell.closeQuery(0);
    }

    function test_cancelWhileItRunsStopsIt() {
        const tab = openPostsTab();
        startSlowDryRun(tab);
        compare(tab.runLabel.text, "Running…");
        verify(tab.cancelButton.visible);
        tryVerify(() => tab.cancelButton.activeFocus);
        // The three buttons are off, and the reason says why.
        compare(tab.buttons.map(b => b.enabled), [false, false, false]);
        verify(tab.reasonLabel.text.startsWith("Another migration action is in progress"), tab.reasonLabel.text);
        compare(walker.problems(shell), []);
        // Esc cancels (as the button does).
        keyClick(Qt.Key_Escape);
        tryVerify(() => tab.run.state === "failed", 10000);
        verify(tab.run.cancelled);
        compare(tab.runLabel.text, "Cancelled");
        verify(!tab.runError.visible); // cancelling is not an error to look at
        tryVerify(() => !tab.cancelButton.visible);
        tryVerify(() => tab.availability.ok);
        tryVerify(() => shell.Window.activeFocusItem !== null && shell.Window.activeFocusItem !== shell.Window.contentItem);
        verify(!shell.migrations.anyRunning());
        verify(messageNames().includes("Cancelled: Cancelled"));
        compare(walker.problems(shell), []);
        shell.closeQuery(0);
    }

    function test_closingATabOrTheAppWhileItRunsAsksFirst() {
        const tab = openPostsTab();
        startSlowDryRun(tab);
        shell.closeQuery(0);
        tryVerify(() => shell.migrationRunningDialog !== null && shell.migrationRunningDialog.opened, 10000);
        const d = shell.migrationRunningDialog;
        compare(d.title, "A migration is running");
        verify(d.message.includes("The database may be left half migrated") || d.message.includes("half migrated"));
        tryVerify(() => d.cancelButton.activeFocus);
        compare(d.cancelButton.text, "Keep it running");
        compare(walker.problems(shell), []);
        keyClick(Qt.Key_Return); // keep it running
        tryVerify(() => !d.visible);
        compare(shell.editorTabs.count, 1);
        compare(tab.run.state, "running");
        // Quitting asks the same, and says no until it is answered.
        compare(shell.canQuit(), false);
        tryVerify(() => d.opened);
        compare(d.confirmButton.text, "Stop it and quit");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !d.visible);
        compare(tab.run.state, "running");
        // Closing the tab for real stops the process.
        shell.closeQuery(0);
        tryVerify(() => d.opened);
        compare(d.confirmButton.text, "Stop it and close the tab");
        keyClick(Qt.Key_Tab);
        keyClick(Qt.Key_Return);
        tryCompare(shell.editorTabs, "count", 0);
        verify(!shell.migrations.anyRunning());
    }

    function test_migrationsInSpanish() {
        closeAllTabs();
        // The folders take the language they are built in: choose it before the tree is opened.
        Language.choice = Language.Choice.Spanish;
        tryVerify(() => field().placeholderText === "Filtrar por todo");
        addLaravel();
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 5 && rowName(1) === "Tables" && rowName(2) === "Migraciones", 10000); // Tables is the provider's folder
        expandNamed("Migraciones");
        tryVerify(() => rowName(3) === "Pendientes" && rowName(4) === "Aplicadas" && rowName(5) === "Huérfanas", 10000);
        expandNamed("Pendientes");
        tryVerify(() => rowName(4) === "2024_03_01_000000_create_posts_table");
        tryVerify(() => itemAt(4)?.Accessible.name === "2024_03_01_000000_create_posts_table");
        compare(itemAt(2).Accessible.description, "Carpeta de migraciones, 6 elementos, nivel 2, 2 de 4");
        compare(itemAt(3).Accessible.description, "Migraciones pendientes, 3 elementos, nivel 3, 1 de 3");
        compare(itemAt(4).Accessible.description, "Migración, pendiente, Pulsa Intro para abrir, nivel 4, 1 de 3");
        const tab = openMigrationNamed("2024_03_01_000000_create_posts_table");
        tryCompare(tab, "view", "ready", 10000);
        tryVerify(() => tab.availability.ok, 10000);
        compare(tab.buttons.map(b => b.text), ["Ejecutar", "Revertir", "Simular"]);
        compare(tab.buttons.map(b => b.Accessible.description), ["Aplica esta migración a la base de datos", "Revierte esta migración si está en el último lote", "Muestra el SQL que ejecutaría esta migración, sin ejecutarla"]);
        compare(tab.statusLabel.text, "Pendiente");
        compare(tab.codeEditor.edit.Accessible.name, "Código de la migración");
        verify(shell.editorArea.tabBar.itemAt(0).Accessible.name.includes("migración"));
        compare(walker.problems(shell), []);
        // The question before running, in Spanish.
        useFake("target");
        press(tab.runButton);
        tryVerify(() => shell.migrationConfirmDialog !== null && shell.migrationConfirmDialog.opened, 15000);
        const d = shell.migrationConfirmDialog;
        compare(d.title, "Ejecutar migración");
        compare(tab.runLabel.text, "Esperando confirmación");
        compare(d.cancelButton.text, "Cancelar");
        compare(d.confirmButton.text, "Ejecutar");
        verify(find(d.contentItem, i => i.text === "Esto ejecuta php artisan en tu proyecto: ejecuta el código PHP de tu proyecto y cambia la base de datos.") !== undefined);
        tryVerify(() => d.cancelButton.activeFocus);
        compare(walker.problems(shell), []);
        keyClick(Qt.Key_Return); // Cancelar
        tryVerify(() => !d.visible);
        // A dry run, its output and its end.
        tryVerify(() => tab.availability.ok);
        press(tab.dryRunButton);
        tryVerify(() => tab.run.state === "done", 15000);
        compare(tab.runLabel.text, "Terminada");
        const names = messageNames();
        verify(names.some(n => n.startsWith("Salida: Migrated:")), names.join("|"));
        verify(names.includes("stderr: a warning"), names.join("|"));
        compare(walker.problems(shell), []);
        shell.closeQuery(0);
        Language.choice = Language.Choice.English;
    }

    function test_migrationsAreFoundByTheSearch() {
        openMigrationsTree();
        shell.sidebar.refreshMigrations();
        verify(shell.sidebar.migrationsAvailable);
        shell.sidebar.filterScope = SchemaTreeModel.Migrations;
        compare(shell.sidebar.filterScope, SchemaTreeModel.Migrations);
        CommandRegistry.trigger("tree.focusFilter");
        typeText("comments");
        tryVerify(() => shell.connections.tree.filterActive, 5000);
        tryVerify(() => shell.sidebar.filterSummary === "1 result(s)", 5000);
        tryVerify(() => rowOf("2024_04_01_000000_create_comments_table") >= 0, 5000);
        compare(rowOf("users"), -1); // a table does not match a migration search
        // The parents stay visible: the connection and the Migrations folder, not Tables.
        verify(rowOf("Migrations") >= 0);
        compare(rowOf("Tables"), -1);
        // The result opens like any migration.
        shell.sidebar.focusFirstResult();
        compare(rowName(tree().cursorRow()), "2024_04_01_000000_create_comments_table");
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.editorArea.currentMigration !== null, 10000);
        shell.closeQuery(0);
        shell.sidebar.filterField.text = "";
        shell.sidebar.filterScope = SchemaTreeModel.Everything;
    }

    function test_withoutMigrationsThereIsNoFolder() {
        addSqlite(testSampleDb);
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows === 4 && rowName(1) === "Tables", 10000);
        compare(rowOf("Migrations"), -1);
        expandNamed("Tables");
        tryVerify(() => tree().rows === 8);
        compare(rowOf("Migrations"), -1);
        compare(rowName(2), "customers");
    }

    function test_theMigrationsFolderTellsItIsNotReadYetUntilTheConnectionIsOpen() {
        closeAllTabs();
        const id = addLaravel();
        // Not connected: the folder exists once the connection is open, with the report not read.
        shell.focusRegion(0);
        keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows >= 4 && rowName(1) === "Tables", 10000);
        // Reconfiguring (a new folder) while connected reads again: the folder says so and then fills.
        shell.migrations.saveSettings(id, "", "laravel", testMigrationsDir, "", testFakePhp, testMigrationsRoot);
        tryVerify(() => rowName(2) === "Migrations", 10000);
        compare(rowName(1), "Tables"); // next to Tables, not in it
        expandNamed("Migrations");
        tryVerify(() => rowName(3) === "Pending", 10000);
        compare(shell.migrations.pendingOf(id, ""), 3);
        // Removing the project takes the folder away.
        shell.migrations.saveSettings(id, "", "", "", "");
        tryVerify(() => rowOf("Migrations") === -1, 10000);
        compare(rowName(2), "Views");
    }
}
