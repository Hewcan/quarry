import QtQuick
import QtTest
import Quarry.Controls

// Structure tabs (H11 phase 4): "Show structure" from the tree's menu, the command and the data tab's
// button; the four sections (columns, indexes, foreign keys, DDL), loading / error with "Try again" /
// no columns, "Copy DDL", the links to the tables a key points to, one tab per table, closing without a
// question, the tab in the cycle of tabs and regions, the editor commands off, the accessibility walker,
// 200 % zoom and Spanish. Temporary databases and the real SQLite plugin; no keychain.
TestCase {
    id: testCase

    property var shell: null
    property int fileCounter: 0

    name: "Structure"
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

    function model() {
        return shell.connections.model;
    }

    function tree() {
        return shell.sidebar.tree;
    }

    function view() {
        return shell.editorArea.currentStructure;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function waitForViews() {
        tryVerify(() => tree() === null ? model().count === 0 : tree().rows >= model().count, 20000);
    }

    function addSqlite() {
        const spy = createTemporaryObject(spyComp, testCase, {
            target: shell.connections,
            signalName: "connectionSaved"
        });
        shell.connections.openSqliteFile(Qt.url("file:///" + testTempDir + "/structure-" + (++fileCounter) + ".db"));
        tryCompare(spy, "count", 1);
        waitForViews();
        return spy.signalArguments[0][0];
    }

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

    // customers, orders (a foreign key, a unique index, a default), a view, and an empty database table.
    function shopDb() {
        const id = addSqlite();
        setup("CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT NOT NULL); " + "CREATE TABLE orders(id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES customers(id), note TEXT DEFAULT 'none'); " + "CREATE UNIQUE INDEX orders_note ON orders(note); " + "CREATE VIEW order_notes AS SELECT note FROM orders; " + "INSERT INTO customers VALUES (1,'Ana');");
        return id;
    }

    // Expands the connection down to the folder and selects `table` in it.
    function selectInTree(table) {
        shell.focusRegion(0);
        keyClick(Qt.Key_Home);
        if (tree().rows === 1)
            keyClick(Qt.Key_Right);
        tryVerify(() => tree().rows >= 3 && tree().roleAt(1, SchemaTreeModel.NameRole) === "Tables", 10000);
        const folder = table.indexOf("_notes") >= 0 ? "Views" : "Tables";
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
        for (let r = 0; r < tree().rows; ++r) {
            if (tree().roleAt(r, SchemaTreeModel.NameRole) === table) {
                tree().goTo(r);
                break;
            }
        }
        compare(tree().roleAt(tree().cursorRow(), SchemaTreeModel.NameRole), table);
    }

    function openStructure(table) {
        selectInTree(table);
        CommandRegistry.trigger("table.showStructure");
        tryVerify(() => view() !== null && view().visible && view().tabTitle === table, 10000);
    }

    function waitReady() {
        tryVerify(() => view().info.state === "ready", 15000);
        tryVerify(() => view().ddlEditor !== null && view().ddlEditor.visible, 10000);
    }

    function closeOverlays() {
        for (const d of [shell.discardDialog, shell.closeTabDialog, shell.connectionDialog, shell.deleteDialog, shell.paletteDialog, shell.exportDialog]) {
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        }
        if (shell.sidebar.menu.visible)
            shell.sidebar.menu.close();
    }

    function removeAll() {
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
            databasePath: testTempDir + "/structure.db",
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
        removeAll();
    }

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

    // ---- opening -----------------------------------------------------------------------------

    function test_theCommandOpensTheStructureAndEnterStillOpensTheData() {
        shopDb();
        selectInTree("orders");
        const command = CommandRegistry.find("table.showStructure");
        compare(command.title, "Show structure");
        compare(command.shortcutText, CommandRegistry.displayShortcut("Ctrl+Shift+U"));
        tryVerify(() => command.enabled);
        // Enter opens the data, as always.
        keyClick(Qt.Key_Return);
        tryVerify(() => tabs().isData(tabs().currentIndex));
        verify(shell.editorArea.currentStructure === null);
        // The command from the tree.
        shell.focusRegion(0);
        CommandRegistry.trigger("table.showStructure");
        tryVerify(() => view() !== null && view().visible && tabs().isStructure(tabs().currentIndex), 10000);
        compare(tabs().titleOf(tabs().currentIndex), "orders");
        verify(shell.editorArea.currentData === null, "a structure tab is not a data tab");
        compare(view().titleLabel.text, "Structure of orders");
        // One per table and connection: the command shows it instead of opening another.
        const count = tabs().count;
        tabs().currentIndex = 0;
        tryVerify(() => shell.editorArea.currentData !== null);
        CommandRegistry.trigger("table.showStructure"); // the current tab is the data tab of orders
        tryVerify(() => shell.editorArea.currentStructure !== null);
        compare(tabs().count, count);
        // From the data tab's button.
        tabs().currentIndex = 0;
        tryVerify(() => shell.editorArea.currentData !== null);
        const button = shell.editorArea.currentData.structureButton;
        compare(button.accessibleName, "Structure");
        compare(button.shortcutText, command.shortcutText);
        mouseClick(button);
        tryVerify(() => shell.editorArea.currentStructure !== null);
        compare(tabs().count, count);
    }

    // The data tab's own "Properties" page: Data is shown first; Properties has the header and the
    // sections, all with the keyboard.
    function test_theDataTabHasAPropertiesPage() {
        shopDb();
        selectInTree("orders");
        keyClick(Qt.Key_Return);
        tryVerify(() => shell.editorArea.currentData !== null && shell.editorArea.currentData.visible, 10000);
        const data = shell.editorArea.currentData;
        compare(data.page, 1, "opening a table shows its rows");
        compare(data.pageBar.count, 2);
        compare(data.pageBar.itemAt(0).text, "Properties");
        compare(data.pageBar.itemAt(1).text, "Data");
        verify(data.propertiesView === null, "nothing is made before the page is shown");
        mouseClick(data.pageBar.itemAt(0));
        tryCompare(data, "page", 0);
        tryVerify(() => data.propertiesView !== null && data.propertiesView.visible, 10000);
        const props = data.propertiesView;
        tryVerify(() => props.info.state === "ready", 15000);
        compare(props.typeLabel.text, "Type: TABLE");
        compare(props.descriptionLabel.text, "Description: none");
        verify(!props.strictBox.visible, "SQLite has no strict flag in the metadata yet");
        compare(props.currentSection, "columns");
        compare(props.sectionTable.count, 3);
        const first = props.sectionTable.itemAtIndex(0);
        verify(first.Accessible.name.indexOf("Name: id") === 0, first.Accessible.name);
        verify(first.Accessible.name.indexOf("Auto increment: yes") > 0, first.Accessible.name);
        // The keyboard: the section list chooses what is on the right.
        props.sectionList.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        compare(props.currentSection, "keys");
        tryCompare(props.sectionTable, "count", 2);
        keyClick(Qt.Key_Down);
        compare(props.currentSection, "foreignKeys");
        tryCompare(props.sectionTable, "count", 1);
        keyClick(Qt.Key_End);
        compare(props.currentSection, "ddl");
        verify(props.ddlText.visible);
        verify(props.ddlText.text.indexOf("CREATE TABLE") >= 0);
        compare(walker.problems(shell), []);
        // Back to the rows.
        mouseClick(data.pageBar.itemAt(1));
        tryCompare(data, "page", 1);
        verify(data.grid.visible);
        verify(!props.visible);
    }

    function test_theTreeMenuHasShowStructureForTablesAndViews() {
        shopDb();
        selectInTree("orders");
        const menu = shell.sidebar.menu;
        shell.sidebar.popupMenu(tree(), tree().selectionModel.currentIndex);
        tryVerify(() => menu.opened);
        const items = walker.findAll(menu.contentItem, i => i.text === "Show structure");
        verify(items.length >= 1, "the menu has Show structure");
        verify(items[0].visible);
        verify(items[0].enabled);
        items[0].triggered();
        menu.close();
        tryVerify(() => view() !== null && view().tabTitle === "orders", 10000);

        // A view too.
        selectInTree("order_notes");
        shell.sidebar.popupMenu(tree(), tree().selectionModel.currentIndex);
        tryVerify(() => menu.opened);
        walker.findAll(menu.contentItem, i => i.text === "Show structure")[0].triggered();
        menu.close();
        tryVerify(() => view() !== null && view().tabTitle === "order_notes", 10000);
        waitReady();
        compare(view().info.isView, true);
        verify(view().ddlEditor.sourceText.indexOf("definition of a view is not available") >= 0, view().ddlEditor.sourceText);

    }

    // ---- the page ----------------------------------------------------------------------------

    function test_theSectionsShowColumnsIndexesForeignKeysAndTheDdl() {
        shopDb();
        openStructure("orders");
        waitReady();
        const v = view();
        // Four sections, in this order, with headings.
        compare(v.columnsSection.text, "Columns");
        compare(v.indexesSection.text, "Indexes");
        compare(v.foreignKeysSection.text, "Foreign keys");
        compare(v.ddlSection.text, "DDL");
        for (const h of [v.columnsSection, v.indexesSection, v.foreignKeysSection, v.ddlSection])
            compare(h.Accessible.role, Accessible.Heading);
        tryVerify(() => v.columnRows.count === 3);
        tryVerify(() => v.columnsSection.y < v.indexesSection.y && v.indexesSection.y < v.foreignKeysSection.y && v.foreignKeysSection.y < v.ddlSection.y);

        // Columns: everything as text (not only color or icon).
        const row = i => v.columnRows.itemAt(i);
        const texts = i => walker.findAll(row(i), t => t.text !== undefined && t.text !== "").map(t => t.text);
        compare(texts(0)[0], "id");
        verify(texts(0).includes("Primary key"), JSON.stringify(texts(0)));
        compare(texts(1)[0], "customer_id");
        verify(texts(1).includes("INTEGER"));
        verify(texts(1).includes("nullable"));
        verify(texts(1).includes("Foreign key to customers.id"), JSON.stringify(texts(1)));
        verify(texts(2).includes("'none'"), JSON.stringify(texts(2)));
        verify(row(0).Accessible.name.indexOf("Primary key") >= 0, row(0).Accessible.name);
        compare(row(1).Accessible.role, Accessible.ListItem);

        // Indexes.
        tryVerify(() => v.indexRows.count === 1);
        const index = walker.findAll(v.indexRows.itemAt(0), t => t.text !== undefined && t.text !== "").map(t => t.text);
        compare(index, ["orders_note", "note", "unique"]);

        // Foreign keys: the table is a button that opens its structure, and "Data" opens its rows.
        tryVerify(() => v.foreignKeyRows.count === 1);
        const key = v.foreignKeyRows.itemAt(0);
        verify(key.Accessible.name.indexOf("customer_id references customers (id)") >= 0, key.Accessible.name);
        const buttons = walker.findAll(key, b => b.iconGlyph !== undefined && b.text !== undefined && b.text !== "");
        compare(buttons.map(b => b.text), ["customers", "Data"]);

        // DDL: the note comes first and says what is missing; the code is read-only SQL.
        verify(v.ddlNote.visible);
        verify(v.ddlNote.text.indexOf("rebuilt") >= 0 && v.ddlNote.text.indexOf("CHECK") >= 0, v.ddlNote.text);
        const ddl = v.ddlEditor;
        compare(ddl.readOnly, true);
        verify(ddl.sourceText.startsWith("-- Reconstructed by Quarry"), ddl.sourceText);
        verify(ddl.sourceText.indexOf("CREATE TABLE") >= 0 && ddl.sourceText.indexOf("CREATE UNIQUE INDEX") >= 0);
        compare(ddl.accessibleName, "DDL");
        compare(walker.problems(shell), []);
    }

    function test_copyDdlPutsTheWholeDdlOnTheClipboard() {
        shopDb();
        openStructure("orders");
        waitReady();
        a11yProbe.setClipboardText("before");
        const v = view();
        compare(v.copyDdlButton.text, "Copy DDL");
        v.copyDdlButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        compare(a11yProbe.clipboardText(), v.info.ddl);
        verify(a11yProbe.clipboardText().startsWith("-- Reconstructed by Quarry"));
        tryVerify(() => v.lastAnnouncement === "DDL copied");
    }

    function test_theLinkOfAForeignKeyOpensTheOtherTable() {
        shopDb();
        openStructure("orders");
        waitReady();
        const key = view().foreignKeyRows.itemAt(0);
        const buttons = walker.findAll(key, b => b.iconGlyph !== undefined && b.text !== undefined && b.text !== "");
        const count = tabs().count;
        tryVerify(() => buttons[0].mapToItem(view(), 0, 0).x > 0); // the Flow has placed it
        // Tab to it: the page scrolls to the focused control, so the click lands on it.
        buttons[0].forceActiveFocus(Qt.TabFocusReason);
        tryVerify(() => {
            const p = buttons[0].mapToItem(view().scroller, 0, 0);
            return p.y >= 0 && p.y + buttons[0].height <= view().scroller.height;
        });
        mouseClick(buttons[0]);
        tryVerify(() => view() !== null && view().tabTitle === "customers" && tabs().count === count + 1, 10000);
        waitReady();
        compare(view().columnRows.count, 2);
        // The same link again shows that tab; "Data" opens its rows.
        tabs().currentIndex = tabs().count - 2;
        tryVerify(() => view().tabTitle === "orders");
        const dataButton = walker.findAll(view().foreignKeyRows.itemAt(0), b => b.iconGlyph !== undefined && b.text === "Data")[0];
        tryVerify(() => dataButton.mapToItem(view(), 0, 0).x > 0);
        dataButton.forceActiveFocus(Qt.TabFocusReason);
        tryVerify(() => {
            const p = dataButton.mapToItem(view().scroller, 0, 0);
            return p.y >= 0 && p.y + dataButton.height <= view().scroller.height;
        });
        mouseClick(dataButton);
        tryVerify(() => shell.editorArea.currentData !== null && shell.editorArea.currentData.tabTitle === "customers", 10000);
    }

    // ---- states ------------------------------------------------------------------------------

    function test_loadingErrorTryAgainAndNoColumns() {
        shopDb();
        openStructure("orders");
        waitReady();
        const v = view();

        // Loading.
        v.info = {
            "state": "loading",
            "message": "",
            "detail": "",
            "isView": false,
            "empty": false,
            "name": "orders",
            "columns": [],
            "indexes": [],
            "foreignKeys": [],
            "ddl": ""
        };
        compare(v.view, "loading");
        tryVerify(() => v.statePanel.visible);
        compare(v.statePanel.title, "Loading…");
        verify(!v.scroller.visible);
        verify(v.edit === v.statePanel);
        compare(v.statePanel.Accessible.name.indexOf("Loading…"), 0);
        compare(walker.problems(shell), []);

        // A failure: why, in words, and "Try again", which reads it again.
        v.info = {
            "state": "failed",
            "message": "The table is not reachable.",
            "detail": "driver text",
            "isView": false,
            "empty": false,
            "name": "orders",
            "columns": [],
            "indexes": [],
            "foreignKeys": [],
            "ddl": ""
        };
        tryVerify(() => v.errorPanel.visible && v.retryButton.visible);
        compare(v.errorPanel.title, "The structure could not be read");
        compare(v.errorPanel.message, "The table is not reachable.");
        compare(v.retryButton.text, "Try again");
        tryVerify(() => v.retryButton.activeFocus, 5000, "Try again takes the focus");
        compare(walker.problems(shell), []);
        keyClick(Qt.Key_Return);
        tryVerify(() => v.info.state === "ready", 15000);
        tryVerify(() => v.scroller.visible);
        compare(v.columnRows.count, 3);

        // No columns.
        v.info = {
            "state": "ready",
            "message": "",
            "detail": "",
            "isView": false,
            "empty": true,
            "name": "orders",
            "columns": [],
            "indexes": [],
            "foreignKeys": [],
            "ddl": ""
        };
        compare(v.view, "empty");
        tryVerify(() => v.statePanel.visible);
        compare(v.statePanel.title, "This table has no columns.");
    }

    function test_aTableThatIsGoneFailsWithTryAgain() {
        shopDb();
        const id = model().get(0).connectionId;
        verify(tabs().openStructure(id, ["main", "vanished"], "table") >= 0);
        tryVerify(() => view() !== null && view().visible, 10000);
        tryVerify(() => view().info.state === "failed", 15000);
        verify(view().info.message.indexOf("vanished") >= 0, view().info.message);
        tryVerify(() => view().retryButton.visible);
        compare(shell.editorArea.lastAnnouncement.indexOf("vanished") >= 0 || view().lastAnnouncement.indexOf("vanished") >= 0, true);
        compare(walker.problems(shell), []);
    }

    // ---- as a tab ----------------------------------------------------------------------------

    function test_itIsATabLikeTheOthers() {
        shopDb();
        openStructure("orders");
        waitReady();
        const count = tabs().count;
        // The editor and the data commands are off, and so is Export.
        for (const c of ["editor.runStatement", "editor.runAll", "editor.suggest", "data.refresh", "data.insertRow", "data.toggleRecord", "data.goToRelated", "data.sortByCurrentColumn", "data.apply"])
            verify(!CommandRegistry.find(c).enabled, c + " is off in a structure tab");
        verify(!shell.exporter.info.available);
        verify(shell.exporter.info.reason.indexOf("structure") >= 0, shell.exporter.info.reason);
        verify(!shell.connectionCombo.enabled);
        compare(shell.connectionCombo.Accessible.description, "The connection of a structure tab cannot be changed.");
        // The query bar has no query buttons.
        verify(!shell.editorArea.runButton.visible);
        // The tab says what it is.
        const bar = shell.editorArea.tabBar;
        const tabButton = find(bar, i => i.Accessible.name !== undefined && i.Accessible.name.indexOf("table structure") >= 0);
        verify(tabButton !== undefined, "the tab bar says 'table structure'");
        verify(find(tabButton, i => i.text === Icons.tableProperties) !== undefined, "and shows an icon");

        // F6 and Ctrl+Tab reach it; the focus lands on the page.
        shell.focusRegion(0);
        shell.focusRegion(1);
        tryVerify(() => view().edit.activeFocus);
        compare(view().edit, view().scroller);
        shell.focusRegion(0);
        keyClick(Qt.Key_Tab, Qt.ControlModifier);
        keyClick(Qt.Key_Tab, Qt.ControlModifier | Qt.ShiftModifier);
        tryVerify(() => view() !== null);
        // The page scrolls with the keys.
        view().scroller.forceActiveFocus(Qt.TabFocusReason);
        const y0 = view().scroller.contentY;
        keyClick(Qt.Key_End);
        tryVerify(() => view().scroller.contentY >= y0);
        keyClick(Qt.Key_Home);
        tryCompare(view().scroller, "contentY", 0);

        // It closes without asking, with Mod+W and with the tab's button.
        keyClick(Qt.Key_W, Qt.ControlModifier);
        tryVerify(() => tabs().count === count - 1);
        verify(shell.closeTabDialog === null || !shell.closeTabDialog.visible);
        compare(shell.discardDialog === null || !shell.discardDialog.visible, true);
        // It is never stored: nothing of it is in the tab store.
        tabs().flush();
    }

    function test_walkerAndTwoHundredPercentZoomAndSpanish() {
        shopDb();
        openStructure("orders");
        waitReady();
        compare(walker.problems(shell), []);
        const rowHeight = view().columnRows.itemAt(0).height;
        Theme.zoom = 2.0;
        tryVerify(() => view().columnRows.itemAt(0).height > rowHeight);
        // Re-laid out: the page is no wider than the tab, and Copy DDL and the title are inside it.
        for (const item of [view().copyDdlButton, view().dataButton]) {
            tryVerify(() => item.mapToItem(view(), item.width, 0).x <= view().width + 1, 5000, item.text + " is inside the tab at 200 %");
        }
        verify(view().scroller.contentHeight > view().scroller.height, "it scrolls");
        compare(walker.problems(shell), []);
        Theme.mode = Theme.Mode.HighContrast;
        compare(walker.problems(shell), []);
        Theme.resetZoom();
        Theme.mode = Theme.Mode.Light;

        Language.choice = Language.Choice.Spanish;
        compare(view().columnsSection.text, "Columnas");
        compare(view().indexesSection.text, "Índices");
        compare(view().foreignKeysSection.text, "Claves foráneas");
        compare(view().ddlSection.text, "DDL");
        compare(view().titleLabel.text, "Estructura de orders");
        compare(view().copyDdlButton.text, "Copiar DDL");
        compare(view().dataButton.text, "Abrir los datos");
        verify(view().ddlNote.text.indexOf("CHECK") >= 0);
        compare(CommandRegistry.find("table.showStructure").title, "Mostrar estructura");
        compare(walker.problems(shell), []);
        Language.choice = Language.Choice.English;
    }
}
