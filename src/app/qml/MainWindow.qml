pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import QtQml
import Quarry.Controls

// The application frame: toolbar, sidebar | (editor / results), status bar. Every action is a
// Command in CommandRegistry; the shortcuts, the menu and the palette all read that registry.
Item {
    id: shell

    // Regions in F6 order. Hidden ones are skipped.
    readonly property list<Item> regions: [sidebarRegion, editorRegion, resultsRegion, statusRegion]
    readonly property list<Command> ownCommands: [cmdPalette, cmdSettings, cmdNewConnection, cmdOpenSqlite, cmdEditConnection, cmdDuplicateConnection, cmdDeleteConnection, cmdDisconnect, cmdSidebar, cmdResults, cmdResetLayout, cmdZoomIn, cmdZoomOut, cmdZoomReset, cmdFocusSidebar, cmdFocusEditor, cmdFocusResults, cmdNextRegion, cmdPreviousRegion, cmdNewTab, cmdCloseTab, cmdNextTab, cmdPreviousTab, cmdRunStatement, cmdRunAll, cmdCancelQuery, cmdSuggest, cmdRefreshMetadata, cmdOpenData, cmdInsertRow, cmdDeleteRows, cmdSetNull, cmdUndo, cmdRedo, cmdPreview, cmdApply, cmdDiscard, cmdRefresh, cmdHistory, cmdSavedShow, cmdSaveQuery, cmdClearHistory, cmdExport]
    // Overlays are created on first use (and preloaded when idle) to keep them off the startup
    // path; each is null until then.
    readonly property Item overlayItem: T.Overlay.overlay
    readonly property var paletteDialog: paletteLoader.item
    readonly property var settingsDialog: settingsLoader.item
    readonly property var connectionDialog: connectionLoader.item
    readonly property var deleteDialog: deleteLoader.item
    readonly property var closeTabDialog: closeTabLoader.item
    readonly property var confirmRunDialog: confirmRunLoader.item
    readonly property var queryPasswordDialog: queryPasswordLoader.item
    readonly property var sshSecretDialog: sshSecretLoader.item
    readonly property var hostKeyDialog: hostKeyLoader.item
    readonly property var previewDialog: previewLoader.item
    readonly property var applyConfirmDialog: applyConfirmLoader.item
    readonly property var discardDialog: discardLoader.item
    readonly property var exportDialog: exportLoader.item
    readonly property alias exporter: exportCtl
    readonly property var filePicker: pickerLoader.item
    readonly property alias connections: connectionsCtl
    readonly property alias sidebar: sidebar
    readonly property alias editorTabs: tabsModel
    readonly property alias historyModel: historyModel
    readonly property alias savedModel: savedModel
    // The History / Saved panel, under the schema tree: null until it is shown for the first time.
    readonly property var library: libraryLoader.item
    readonly property var historyOptOutDialog: historyOptOutLoader.item
    readonly property alias editorArea: editorArea
    readonly property alias queries: queryCtl
    readonly property alias completion: completionCtl
    readonly property alias resultsPanel: resultsPanel
    readonly property alias statusItem: statusItem
    readonly property var appMenu: menuLoader.item
    // Where the connections live; empty = the application's own. Tests use a temporary one.
    property string databasePath: ""
    property string providerDirectory: ""
    property bool sidebarVisible: shell.flag("sidebarVisible", true)
    property bool resultsVisible: shell.flag("resultsVisible", true)
    // Not remembered between runs: the panel starts closed so it adds nothing to the startup.
    property bool libraryVisible: false
    // Bumped when a connection changes, so the status bar re-reads "history off".
    property int connectionsRevision: 0
    readonly property bool tabHistoryOff: {
        shell.connectionsRevision;
        const id = editorArea.currentView?.connectionId ?? "";
        const row = id === "" ? -1 : connectionsCtl.model.indexOf(id);
        return row >= 0 && connectionsCtl.model.get(row).recordHistory === false;
    }
    readonly property string selectedSummary: sidebar.hasCurrent ? qsTr("Selected connection: %1").arg(sidebar.currentName) + (sidebar.currentProduction ? ", " + qsTr("production") : "") : ""
    readonly property string cursorSummary: editorArea.currentEditor ? qsTr("Ln %1, Col %2").arg(editorArea.currentEditor.line).arg(editorArea.currentEditor.column) : ""
    readonly property int defaultSidebarWidth: Theme.controlHeight * 10
    readonly property int defaultResultsHeight: Theme.controlHeight * 8

    // QSettings (INI) gives booleans back as strings.
    function flag(key, fallback) {
        const v = LayoutStore.value(key, fallback);
        return v === true || v === "true";
    }

    function showOverlay(loader) {
        loader.active = true;
        loader.item.open();
    }

    function openConnectionDialog(id, copied) {
        connectionLoader.active = true;
        connectionLoader.item.openFor(id, copied === true);
    }

    function askDelete(id) {
        const row = connectionsCtl.model.indexOf(id);
        if (row < 0)
            return;
        const c = connectionsCtl.model.get(row);
        deleteLoader.active = true;
        deleteLoader.item.openFor(id, c.name, c.production);
    }


    // ---- data tabs ---------------------------------------------------------------------------

    readonly property var currentEdits: queryCtl.current.edits
    readonly property bool dataEditable: editorArea.currentData !== null && currentEdits.editable && !currentEdits.saving
    readonly property bool dataPending: editorArea.currentData !== null && currentEdits.pendingCount > 0 && !currentEdits.saving
    // Set once the user agreed to lose the pending changes at quit.
    property bool quitConfirmed: false
    // Where the focus was when the panel opened: Esc brings it back there.
    property Item libraryReturn: null

    function openData(object) {
        if (!object || !object.openable)
            return;
        const row = tabsModel.openData(object.connectionId, object.parts, object.kind);
        if (row < 0)
            return;
        editorArea.errorMessage = "";
        editorArea.announce(qsTr("%1 opened, table data").arg(object.name));
        Qt.callLater(() => editorRegion.focusDefault());
    }

    // A cell being edited is committed before anything reads the pending changes.
    function commitCell() {
        const data = editorArea.currentData;
        if (data && data.grid.editing)
            data.grid.finishEdit(true, 0, 0);
    }

    function askApply(request) {
        applyConfirmLoader.active = true;
        applyConfirmLoader.item.openFor(request);
    }

    function previewChanges() {
        commitCell();
        const id = tabsModel.idOf(tabsModel.currentIndex);
        const list = queryCtl.previewStatements(id);
        if (list.length === 0) {
            editorArea.announce(currentEdits.lastError !== "" ? currentEdits.lastError : qsTr("There are no changes to preview."), true);
            return;
        }
        previewLoader.active = true;
        previewLoader.item.openFor(id, list);
    }

    function applyChanges() {
        commitCell();
        queryCtl.apply(tabsModel.idOf(tabsModel.currentIndex));
    }

    // Runs `action` for the data tab: at once when nothing is pending, else after asking.
    function askDiscard(action, tabId) {
        const n = queryCtl.pendingChanges(tabId);
        if (n === 0) {
            runDiscard(action, tabId);
            return;
        }
        discardLoader.active = true;
        discardLoader.item.openFor(action, tabId, queryCtl.stateOf(tabId) ? tabTitleOf(tabId) : "", n);
    }

    function tabTitleOf(tabId) {
        for (let i = 0; i < tabsModel.count; ++i)
            if (tabsModel.idOf(i) === tabId)
                return tabsModel.titleOf(i);
        return "";
    }

    function runDiscard(action, tabId) {
        if (action === "close") {
            closeQueryNow(tabId);
        } else if (action === "refresh") {
            queryCtl.reload(tabId);
            editorArea.announce(qsTr("Refreshing %1").arg(tabTitleOf(tabId)));
        } else if (action === "discard") {
            const edits = queryCtl.stateOf(tabId)?.edits;
            if (edits) {
                edits.discardAll();
                edits.lastError = "";
                editorArea.announce(qsTr("Changes discarded"));
            }
        } else if (action === "quit") {
            quitConfirmed = true;
            Window.window.close();
        }
    }

    // Window.onClosing asks this: false keeps the window open while the question is shown.
    function canQuit() {
        const n = queryCtl.pendingTotal();
        if (quitConfirmed || n === 0)
            return true;
        discardLoader.active = true;
        discardLoader.item.openFor("quit", "", "", n);
        return false;
    }

    // ---- history and saved queries -----------------------------------------------------------

    // Shows the panel on a tab (0 History, 1 Saved) and moves the focus into it.
    function showLibrary(tab) {
        if (!libraryVisible)
            libraryReturn = Window.activeFocusItem;
        sidebarVisible = true;
        libraryLoader.active = true;
        libraryVisible = true;
        library.showTab(tab);
        Qt.callLater(() => {
            if (library)
                library.defaultFocus.forceActiveFocus(Qt.TabFocusReason);
        });
    }

    function hideLibrary() {
        if (!libraryVisible)
            return;
        const inside = isInside(Window.activeFocusItem, libraryLoader);
        library?.closeDialogs();
        const back = libraryReturn;
        libraryReturn = null;
        libraryVisible = false;
        if (inside || regionOf(Window.activeFocusItem) < 0) {
            if (back && back.visible)
                back.forceActiveFocus(Qt.TabFocusReason);
            else
                focusRegion(1);
        }
    }

    // The command: shows the tab, or closes the panel when the focus is already on that tab.
    function toggleLibrary(tab) {
        if (libraryVisible && library && library.tabIndex === tab && isInside(Window.activeFocusItem, libraryLoader))
            hideLibrary();
        else
            showLibrary(tab);
    }

    function isInside(item, ancestor) {
        for (let p = item; p; p = p.parent)
            if (p === ancestor)
                return true;
        return false;
    }

    function connectionNameOf(id) {
        const row = id === "" ? -1 : connectionsCtl.model.indexOf(id);
        return row >= 0 ? connectionsCtl.model.get(row).name : "";
    }

    // One statement ends in a semicolon, so a later one never glues to it (when the last line has
    // a comment the text is left alone: a semicolon there would be part of the comment).
    function terminated(sql) {
        const t = sql.replace(/\s+$/, "");
        const lastLine = t.substring(t.lastIndexOf("\n") + 1);
        return t === "" || t.endsWith(";") || lastLine.includes("--") ? t : t + ";";
    }

    // Adds `sql` at the end of the editor, after what is there, and puts the cursor on it. The text
    // that was there is kept; a semicolon is added after it if it lacks one.
    function appendSql(editor, sql) {
        const edit = editor.edit;
        const text = edit.text;
        const end = text.replace(/\s+$/, "").length;
        if (end === 0) {
            edit.remove(0, edit.length);
        } else {
            const kept = text.substring(0, end);
            if (!kept.endsWith(";") && !kept.substring(kept.lastIndexOf("\n") + 1).includes("--"))
                edit.insert(end, ";");
            edit.insert(edit.length, "\n\n");
        }
        const start = edit.length;
        edit.insert(start, terminated(sql));
        edit.cursorPosition = start;
    }

    // History "Copy to editor" and "Run", and saved query "Open". Run on a tab of another
    // connection would run the statement somewhere the user did not mean: it goes to a new tab
    // of the statement's own connection instead. Returns the editor, null when there is none.
    function insertSql(sql, connectionId, run) {
        let editor = editorArea.currentEditor;
        const own = editor ? editorArea.currentView.connectionId : "";
        const known = connectionId !== "" && connectionsCtl.model.indexOf(connectionId) >= 0;
        const other = run && editor && own !== "" && known && own !== connectionId;
        if (!editor || other) {
            tabsModel.openTab(known ? connectionId : (sidebar.hasCurrent ? sidebar.currentId : ""));
            editorArea.errorMessage = "";
            editor = editorArea.currentEditor;
        }
        if (!editor) {
            editorArea.announce(qsTr("No query is open."), true);
            return null;
        }
        if (run && known && tabsModel.connectionIdOf(tabsModel.currentIndex) === "")
            tabsModel.setConnection(tabsModel.currentIndex, connectionId);
        appendSql(editor, sql);
        if (run) {
            shell.runQuery(false);
        } else {
            editorArea.announce(qsTr("Copied to %1").arg(tabsModel.titleOf(tabsModel.currentIndex)));
            editorRegion.focusDefault();
        }
        return editor;
    }

    function openSaved(sql, connectionId) {
        if (!insertSql(sql, connectionId, false))
            return;
        const name = connectionNameOf(connectionId);
        // A query for a connection the tab does not have: offered, never forced.
        if (name !== "" && tabsModel.connectionIdOf(tabsModel.currentIndex) === "")
            library?.savedView.showOffer(connectionId, name);
    }

    function useConnection(connectionId) {
        const row = tabsModel.currentIndex;
        if (row < 0 || connectionId === "")
            return;
        tabsModel.setConnection(row, connectionId);
        editorArea.announce(qsTr("%1 now uses connection %2").arg(tabsModel.titleOf(row)).arg(connectionNameOf(connectionId)));
    }

    // "Save current query": the selection, else the statement under the cursor, else all the text.
    function saveCurrentQuery() {
        const editor = editorArea.currentEditor;
        if (!editor) {
            editorArea.announce(qsTr("Open a query to save it."), true);
            return;
        }
        const edit = editor.edit;
        const row = tabsModel.currentIndex;
        let sql = edit.selectedText.replace(/\u2029/g, "\n").trim();
        if (sql === "") {
            const st = tabsModel.statementNow(row, edit.cursorPosition);
            if (st.index >= 0)
                sql = edit.text.substring(st.begin, st.end).trim();
        }
        if (sql === "")
            sql = edit.text.trim();
        if (sql === "") {
            editorArea.announce(qsTr("There is nothing to save."), true);
            return;
        }
        showLibrary(1);
        library.savedView.askSave(sql, tabsModel.connectionIdOf(row));
    }

    function askClearHistory() {
        showLibrary(0);
        library.historyView.askClear();
    }

    // The connection stopped recording history: does the history it already has go too?
    function askDeleteHistory(id, name) {
        historyOptOutLoader.active = true;
        historyOptOutLoader.item.openFor(id, name);
    }

    // ---- export (H5) -------------------------------------------------------------------------

    // The dialog is built the first time it is used.
    function openExport() {
        if (!exportCtl.info.available || exportCtl.busy)
            return;
        commitCell();
        exportLoader.active = true;
        exportLoader.item.openFor(tabsModel.idOf(tabsModel.currentIndex));
    }

    // ---- editor tabs -------------------------------------------------------------------------

    function confirmRun(request) {
        confirmRunLoader.active = true;
        confirmRunLoader.item.openFor(request);
    }

    function askQueryPassword(id, name, retry) {
        queryPasswordLoader.active = true;
        queryPasswordLoader.item.openFor(id, name, retry);
    }

    // The SSH tunnel's prompts, for every place that connects (tree, tabs, test): one dialog each,
    // answered through the shared preflight.
    function askSshSecret(id, name, passphrase, retry) {
        sshSecretLoader.active = true;
        sshSecretLoader.item.openFor(id, name, retry, passphrase ? "sshPassphrase" : "sshPassword");
    }

    function askHostKey(host, port, keyType, fingerprint) {
        hostKeyLoader.active = true;
        hostKeyLoader.item.openFor(host, port, keyType, fingerprint);
    }

    function showHostKeyChanged(host, port, keyType, known, received) {
        hostKeyLoader.active = true;
        hostKeyLoader.item.openMismatch(host, port, keyType, known, received);
    }

    function openQuery() {
        // The connection of the current tree node, else the one of the current tab (the model).
        const row = tabsModel.openTab(sidebar.hasCurrent ? sidebar.currentId : "");
        editorArea.errorMessage = "";
        const name = tabsModel.titleOf(row);
        const connection = editorArea.currentEditor?.connectionName ?? "";
        editorArea.announce(connection !== "" ? qsTr("%1 opened, connection %2").arg(name).arg(connection) : qsTr("%1 opened, no connection").arg(name));
        editorRegion.focusDefault();
    }

    // Closes the tab at once, or after confirming when it has text.
    function closeQuery(row) {
        if (row < 0 || row >= tabsModel.count)
            return;
        if (tabsModel.isData(row)) {
            askDiscard("close", tabsModel.idOf(row));
        } else if (tabsModel.hasText(row)) {
            closeTabLoader.active = true;
            closeTabLoader.item.openFor(tabsModel.idOf(row), tabsModel.titleOf(row));
        } else {
            closeQueryNow(tabsModel.idOf(row));
        }
    }

    function closeQueryNow(tabId) {
        let row = -1;
        for (let i = 0; i < tabsModel.count; ++i)
            if (tabsModel.idOf(i) === tabId)
                row = i;
        if (row < 0)
            return;
        const name = tabsModel.titleOf(row);
        const inEditor = regionOf(Window.activeFocusItem) === 1;
        tabsModel.closeTab(row);
        editorArea.errorMessage = "";
        editorArea.announce(qsTr("%1 closed").arg(name));
        // The focus was in the tab that is gone (or in the dialog that closed it): the editor
        // that is now current takes it.
        if (inEditor || regionOf(Window.activeFocusItem) < 0)
            Qt.callLater(() => editorRegion.focusDefault());
    }

    function cycleQuery(step) {
        tabsModel.cycle(step);
        if (tabsModel.count > 0)
            editorArea.announce(qsTr("%1, tab %2 of %3").arg(tabsModel.titleOf(tabsModel.currentIndex)).arg(tabsModel.currentIndex + 1).arg(tabsModel.count));
    }

    // Marks what runs and runs it: QueryController hooks tabsModel.runRequested, which `run`
    // emits, and announces the start. Only a plan that cannot run is said here.
    function runQuery(all) {
        const editor = editorArea.currentEditor;
        const plan = tabsModel.run(tabsModel.currentIndex, all, editor ? editor.edit.cursorPosition : 0);
        if (!plan.ok)
            editorArea.announce(plan.message);
        else if (editor)
            editor.mark(plan.ranges);
        return plan;
    }

    // Mod+Space: the suggestions of the current editor.
    function showSuggestions() {
        const editor = editorArea.currentEditor;
        if (editor)
            editor.suggest();
        else
            editorArea.announce(qsTr("No query is open."));
    }

    // Drops what autocompletion cached about the connection of the current tab.
    function refreshMetadata() {
        const id = tabsModel.connectionIdOf(tabsModel.currentIndex);
        if (id === "")
            return;
        completionCtl.invalidate(id);
        editorArea.announce(qsTr("Metadata refreshed"));
    }

    // The palette's "@": shows a table or view in the tree and moves the focus there.
    function revealObject(connectionId, parts) {
        sidebarVisible = true;
        connectionsCtl.tree.reveal(connectionId, parts);
    }

    // Esc in the editor or the results region cancels a running query; otherwise it is left alone.
    function cancelOnEscape(event) {
        event.accepted = queryCtl.current.running;
        if (event.accepted)
            queryCtl.cancelCurrent();
    }

    function regionOf(item) {
        for (let p = item; p; p = p.parent) {
            const i = regions.indexOf(p);
            if (i >= 0)
                return i;
        }
        return -1;
    }

    function focusRegion(index) {
        if (index === 0)
            sidebarVisible = true;
        else if (index === 2)
            resultsVisible = true;
        regions[index].focusDefault();
    }

    function cycleRegion(step) {
        const n = regions.length;
        const current = regionOf(Window.activeFocusItem);
        const from = current >= 0 ? current : (step > 0 ? -1 : 0);
        for (let k = 1; k <= n; ++k) {
            const j = ((from + step * k) % n + n) % n;
            if (regions[j].visible) {
                focusRegion(j);
                return;
            }
        }
    }

    // A pane that is about to disappear must not take the focus with it.
    function leaveRegion(index) {
        if (regionOf(Window.activeFocusItem) === index)
            focusRegion(1);
    }

    function toggleSidebar() {
        if (sidebarVisible)
            leaveRegion(0);
        sidebarVisible = !sidebarVisible;
    }

    function toggleResults() {
        if (resultsVisible)
            leaveRegion(2);
        resultsVisible = !resultsVisible;
    }

    function resetLayout() {
        hideLibrary();
        saveLayout.stop();
        for (const key of ["sidebarVisible", "resultsVisible", "sidebarWidth", "resultsHeight"])
            LayoutStore.remove(key);
        sidebarVisible = true;
        resultsVisible = true;
        sidebarRegion.T.SplitView.preferredWidth = defaultSidebarWidth;
        resultsRegion.T.SplitView.preferredHeight = defaultResultsHeight;
    }

    onSidebarVisibleChanged: LayoutStore.setValue("sidebarVisible", sidebarVisible)
    onResultsVisibleChanged: LayoutStore.setValue("resultsVisible", resultsVisible)

    Component.onCompleted: {
        for (const c of ownCommands)
            CommandRegistry.registerCommand(c);
    }
    Component.onDestruction: {
        tabsModel.flush();
        for (const c of ownCommands)
            CommandRegistry.unregisterCommand(c.commandId);
    }

    // Waits for the layout to settle after a resize, then stores the sizes of the visible panes.
    Timer {
        id: saveLayout

        interval: 300
        onTriggered: {
            if (sidebarRegion.visible)
                LayoutStore.setValue("sidebarWidth", Math.round(sidebarRegion.width));
            if (resultsRegion.visible)
                LayoutStore.setValue("resultsHeight", Math.round(resultsRegion.height));
        }
    }

    // ---- connections -------------------------------------------------------------------------

    // Before the ConnectionController: the sessions' code lives in its plugins, and children are
    // destroyed in creation order.
    QueryController {
        id: queryCtl

        tabs: tabsModel
        history: historyModel

        onAnnouncement: (text, assertive) => editorArea.announce(text, assertive)
        onConfirmationRequested: request => shell.confirmRun(request)
        onPasswordRequested: (id, name, retry) => shell.askQueryPassword(id, name, retry)
        onApplyConfirmationRequested: request => shell.askApply(request)
        // CREATE, ALTER, DROP: the tables autocompletion knows are stale.
        onSchemaChanged: connectionId => completionCtl.invalidate(connectionId)
    }

    // Before the ConnectionController, like QueryController: its session lives in the plugins.
    ExportController {
        id: exportCtl

        queries: queryCtl
    }

    ConnectionController {
        id: connectionsCtl

        databasePath: shell.databasePath
        providerDirectory: shell.providerDirectory

        onConnectionSaved: id => sidebar.selectId(id)
        // The copy has no password or SSH secret (they are never copied): when it needs one, the
        // edit dialog opens and says so.
        onConnectionDuplicated: id => {
            sidebar.selectId(id);
            const copy = connectionsCtl.model.get(connectionsCtl.model.indexOf(id));
            if (copy.passwordMode === 1 || copy.sshSecretSaved)
                shell.openConnectionDialog(id, true);
        }
        onConnectionRemoved: Qt.callLater(() => {
            if (shell.regionOf(shell.Window.activeFocusItem) < 0)
                shell.focusRegion(0);
        })
    }

    EditorTabsModel {
        id: tabsModel

        connections: connectionsCtl
        databasePath: shell.databasePath
    }

    // The stores are created when first used (the first statement, or the first time the panel
    // opens), never at startup.
    HistoryModel {
        id: historyModel

        connections: connectionsCtl
        databasePath: shell.databasePath
        // A failed write must not interrupt: it is said politely and the list stays as it is.
        onFailed: (message, detail) => editorArea.announce(message, false)
    }

    SavedQueriesModel {
        id: savedModel

        connections: connectionsCtl
        databasePath: shell.databasePath
    }

    Connections {
        target: connectionsCtl

        function onHistoryDisabled(id, name) {
            Qt.callLater(() => shell.askDeleteHistory(id, name));
        }
    }

    Connections {
        target: connectionsCtl.model

        function onDataChanged() {
            shell.connectionsRevision++;
        }

        function onModelReset() {
            shell.connectionsRevision++;
        }
    }

    // Autocompletion: a cache per connection, filled on first use (nothing is read before).
    CompletionController {
        id: completionCtl

        tabs: tabsModel
    }

    // The provider scan and the store threads start after the first frame, never before it.
    Connections {
        target: shell.Window.window
        enabled: !connectionsCtl.started

        function onFrameSwapped() {
            connectionsCtl.start();
            tabsModel.start();
        }
    }

    // Typed text is saved a second after the last edit; this covers quitting inside that second.
    Connections {
        target: Qt.application

        function onAboutToQuit() {
            tabsModel.flush();
        }
    }

    // ---- commands ----------------------------------------------------------------------------

    Command {
        id: cmdPalette

        commandId: "palette.open"
        category: qsTr("General")
        title: qsTr("Command palette")
        shortcut: "Ctrl+K"
        alternateShortcut: "Ctrl+Shift+P"
        trackRecent: false // opening the palette must not push itself to the top of it
        onTriggered: shell.showOverlay(paletteLoader)
    }

    Command {
        id: cmdSettings

        commandId: "settings.open"
        category: qsTr("General")
        title: qsTr("Settings")
        shortcut: "Ctrl+,"
        onTriggered: shell.showOverlay(settingsLoader)
    }

    Command {
        id: cmdNewConnection

        commandId: "connection.new"
        category: qsTr("Connection")
        title: qsTr("New connection")
        shortcut: "Ctrl+Shift+N"
        onTriggered: shell.openConnectionDialog("", false)
    }

    Command {
        id: cmdOpenSqlite

        commandId: "connection.openSqlite"
        category: qsTr("Connection")
        title: qsTr("Open SQLite file")
        shortcut: "Ctrl+O"
        onTriggered: shell.showOverlay(pickerLoader)
    }

    Command {
        id: cmdEditConnection

        commandId: "connection.edit"
        category: qsTr("Connection")
        title: qsTr("Edit connection")
        enabled: sidebar.hasCurrent
        onTriggered: shell.openConnectionDialog(sidebar.currentId, false)
    }

    Command {
        id: cmdDuplicateConnection

        commandId: "connection.duplicate"
        category: qsTr("Connection")
        title: qsTr("Duplicate connection")
        enabled: sidebar.hasCurrent
        onTriggered: connectionsCtl.duplicate(sidebar.currentId)
    }

    Command {
        id: cmdDeleteConnection

        commandId: "connection.delete"
        category: qsTr("Connection")
        title: qsTr("Delete connection")
        enabled: sidebar.hasCurrent
        onTriggered: shell.askDelete(sidebar.currentId)
    }

    Command {
        id: cmdDisconnect

        commandId: "connection.disconnect"
        category: qsTr("Connection")
        title: qsTr("Disconnect")
        enabled: sidebar.currentConnected
        onTriggered: connectionsCtl.tree.disconnectConnection(sidebar.currentId)
    }

    Command {
        id: cmdSidebar

        commandId: "view.toggleSidebar"
        category: qsTr("View")
        title: qsTr("Toggle sidebar")
        shortcut: "Ctrl+B"
        checkable: true
        checked: shell.sidebarVisible
        onTriggered: shell.toggleSidebar()
    }

    Command {
        id: cmdResults

        commandId: "view.toggleResults"
        category: qsTr("View")
        title: qsTr("Toggle results panel")
        shortcut: "Ctrl+J"
        checkable: true
        checked: shell.resultsVisible
        onTriggered: shell.toggleResults()
    }

    Command {
        id: cmdResetLayout

        commandId: "view.resetLayout"
        category: qsTr("View")
        title: qsTr("Reset layout")
        onTriggered: shell.resetLayout()
    }

    Command {
        id: cmdZoomIn

        commandId: "view.zoomIn"
        category: qsTr("View")
        title: qsTr("Zoom in")
        shortcut: "Ctrl+="
        alternateShortcut: "Ctrl++"
        onTriggered: Theme.zoomIn()
    }

    Command {
        id: cmdZoomOut

        commandId: "view.zoomOut"
        category: qsTr("View")
        title: qsTr("Zoom out")
        shortcut: "Ctrl+-"
        onTriggered: Theme.zoomOut()
    }

    Command {
        id: cmdZoomReset

        commandId: "view.zoomReset"
        category: qsTr("View")
        title: qsTr("Reset zoom")
        shortcut: "Ctrl+0"
        onTriggered: Theme.resetZoom()
    }

    Command {
        id: cmdFocusSidebar

        commandId: "focus.sidebar"
        category: qsTr("Navigate")
        title: qsTr("Go to sidebar")
        shortcut: "Ctrl+1"
        onTriggered: shell.focusRegion(0)
    }

    Command {
        id: cmdFocusEditor

        commandId: "focus.editor"
        category: qsTr("Navigate")
        title: qsTr("Go to editor")
        shortcut: "Ctrl+2"
        onTriggered: shell.focusRegion(1)
    }

    Command {
        id: cmdFocusResults

        commandId: "focus.results"
        category: qsTr("Navigate")
        title: qsTr("Go to results")
        shortcut: "Ctrl+3"
        onTriggered: shell.focusRegion(2)
    }

    Command {
        id: cmdNextRegion

        commandId: "focus.nextRegion"
        category: qsTr("Navigate")
        title: qsTr("Next region")
        shortcut: "F6"
        onTriggered: shell.cycleRegion(1)
    }

    Command {
        id: cmdPreviousRegion

        commandId: "focus.previousRegion"
        category: qsTr("Navigate")
        title: qsTr("Previous region")
        shortcut: "Shift+F6"
        onTriggered: shell.cycleRegion(-1)
    }

    Command {
        id: cmdNewTab

        commandId: "editor.newTab"
        category: qsTr("Editor")
        title: qsTr("New query")
        shortcut: "Ctrl+T"
        onTriggered: shell.openQuery()
    }

    Command {
        id: cmdCloseTab

        commandId: "editor.closeTab"
        category: qsTr("Editor")
        title: qsTr("Close query")
        shortcut: "Ctrl+W"
        onTriggered: shell.closeQuery(tabsModel.currentIndex)
    }

    Command {
        id: cmdNextTab

        commandId: "editor.nextTab"
        category: qsTr("Editor")
        title: qsTr("Next query")
        shortcut: "Ctrl+Tab"
        onTriggered: shell.cycleQuery(1)
    }

    Command {
        id: cmdPreviousTab

        commandId: "editor.previousTab"
        category: qsTr("Editor")
        title: qsTr("Previous query")
        shortcut: "Ctrl+Shift+Tab"
        // Keyboards report Shift+Tab as Backtab.
        alternateShortcut: "Ctrl+Shift+Backtab"
        onTriggered: shell.cycleQuery(-1)
    }

    Command {
        id: cmdRunStatement

        commandId: "editor.runStatement"
        category: qsTr("Editor")
        title: qsTr("Run current statement")
        enabled: editorArea.currentData === null
        shortcut: "Ctrl+Enter"
        // The main Enter key is Return; Enter is the keypad's.
        alternateShortcut: "Ctrl+Return"
        onTriggered: shell.runQuery(false)
    }

    Command {
        id: cmdRunAll

        commandId: "editor.runAll"
        category: qsTr("Editor")
        title: qsTr("Run all statements")
        enabled: editorArea.currentData === null
        shortcut: "Ctrl+Shift+Enter"
        alternateShortcut: "Ctrl+Shift+Return"
        onTriggered: shell.runQuery(true)
    }

    Command {
        id: cmdCancelQuery

        commandId: "query.cancel"
        category: qsTr("Editor")
        title: qsTr("Cancel query")
        shortcut: "Ctrl+."
        enabled: queryCtl.current.running
        onTriggered: queryCtl.cancelCurrent()
    }


    Command {
        id: cmdSuggest

        commandId: "editor.suggest"
        category: qsTr("Editor")
        title: qsTr("Show suggestions")
        // Not in the results grid (Ctrl+Space selects a column there) nor in the sidebar.
        enabled: editorArea.currentData === null && [1, -1].includes(shell.regionOf(shell.Window.activeFocusItem))
        // Cmd+Space belongs to Spotlight on macOS: there it is Control+Space.
        shortcut: Qt.platform.os === "osx" ? "Meta+Space" : "Ctrl+Space"
        onTriggered: shell.showSuggestions()
    }

    Command {
        id: cmdRefreshMetadata

        commandId: "connection.refreshMetadata"
        category: qsTr("Connection")
        title: qsTr("Refresh metadata")
        enabled: editorArea.currentView !== null && editorArea.currentView.connectionId !== ""
        onTriggered: shell.refreshMetadata()
    }

    // ---- history and saved queries (H4) ------------------------------------------------------

    Command {
        id: cmdHistory

        commandId: "history.show"
        category: qsTr("History")
        title: qsTr("Show history")
        shortcut: "Ctrl+Shift+H"
        onTriggered: shell.toggleLibrary(0)
    }

    Command {
        id: cmdSavedShow

        commandId: "saved.show"
        category: qsTr("History")
        title: qsTr("Show saved queries")
        shortcut: "Ctrl+Shift+L"
        onTriggered: shell.toggleLibrary(1)
    }

    Command {
        id: cmdSaveQuery

        commandId: "saved.saveCurrent"
        category: qsTr("History")
        title: qsTr("Save current query")
        shortcut: "Ctrl+Shift+S"
        enabled: editorArea.currentEditor !== null
        onTriggered: shell.saveCurrentQuery()
    }

    Command {
        id: cmdClearHistory

        commandId: "history.clear"
        category: qsTr("History")
        title: qsTr("Clear history")
        onTriggered: shell.askClearHistory()
    }

    // ---- export (H5) -------------------------------------------------------------------------

    Command {
        id: cmdExport

        commandId: "export.open"
        category: qsTr("Export")
        title: qsTr("Export…")
        shortcut: "Ctrl+Shift+E"
        enabled: exportCtl.info.available && !exportCtl.busy
        onTriggered: shell.openExport()
    }

    // ---- data tabs (H2) ----------------------------------------------------------------------

    Command {
        id: cmdOpenData

        commandId: "data.open"
        category: qsTr("Data")
        title: qsTr("Open data")
        enabled: sidebar.currentObject.openable
        onTriggered: shell.openData(sidebar.currentObject)
    }

    Command {
        id: cmdInsertRow

        commandId: "data.insertRow"
        category: qsTr("Data")
        title: qsTr("Insert row")
        shortcut: "Ctrl+I"
        alternateShortcut: "Alt+Ins"
        enabled: shell.dataEditable && !shell.currentEdits.cellEditing
        onTriggered: editorArea.currentData?.grid.insertRow()
    }

    Command {
        id: cmdDeleteRows

        commandId: "data.deleteRows"
        category: qsTr("Data")
        title: qsTr("Delete selected rows")
        shortcut: "Ctrl+Delete"
        enabled: shell.dataEditable && !shell.currentEdits.cellEditing
        onTriggered: editorArea.currentData?.grid.deleteSelected()
    }

    Command {
        id: cmdSetNull

        commandId: "data.setNull"
        category: qsTr("Data")
        title: qsTr("Set to NULL")
        shortcut: "Ctrl+Shift+Backspace"
        enabled: shell.dataEditable && !shell.currentEdits.cellEditing
        onTriggered: editorArea.currentData?.grid.setNull()
    }

    Command {
        id: cmdUndo

        commandId: "data.undo"
        category: qsTr("Data")
        title: qsTr("Undo change")
        shortcut: "Ctrl+Z"
        // While a cell is being edited, Ctrl+Z belongs to its text.
        enabled: shell.dataEditable && shell.currentEdits.canUndo && !shell.currentEdits.cellEditing
        onTriggered: editorArea.currentData?.grid.undo()
    }

    Command {
        id: cmdRedo

        commandId: "data.redo"
        category: qsTr("Data")
        title: qsTr("Redo change")
        shortcut: "Ctrl+Shift+Z"
        alternateShortcut: "Ctrl+Y"
        enabled: shell.dataEditable && shell.currentEdits.canRedo && !shell.currentEdits.cellEditing
        onTriggered: editorArea.currentData?.grid.redo()
    }

    Command {
        id: cmdPreview

        commandId: "data.preview"
        category: qsTr("Data")
        title: qsTr("Preview SQL")
        shortcut: "Ctrl+Shift+D"
        enabled: shell.dataPending
        onTriggered: shell.previewChanges()
    }

    Command {
        id: cmdApply

        commandId: "data.apply"
        category: qsTr("Data")
        title: qsTr("Save changes")
        shortcut: "Ctrl+S"
        enabled: shell.dataPending
        onTriggered: shell.applyChanges()
    }

    Command {
        id: cmdDiscard

        commandId: "data.discard"
        category: qsTr("Data")
        title: qsTr("Discard changes")
        enabled: shell.dataPending
        onTriggered: shell.askDiscard("discard", tabsModel.idOf(tabsModel.currentIndex))
    }

    Command {
        id: cmdRefresh

        commandId: "data.refresh"
        category: qsTr("Data")
        title: qsTr("Refresh data")
        shortcut: "F5"
        enabled: editorArea.currentData !== null && !queryCtl.current.running && !shell.currentEdits.saving
        onTriggered: shell.askDiscard("refresh", tabsModel.idOf(tabsModel.currentIndex))
    }

    Instantiator {
        model: CommandRegistry.commands

        delegate: Shortcut {
            required property var modelData

            sequences: modelData.sequences
            enabled: modelData.enabled
            onActivated: CommandRegistry.trigger(modelData.commandId)
        }
    }

    // The table or view the palette chose: its branch opens in the tree and takes the focus.
    Connections {
        target: connectionsCtl.tree

        function onRevealed(target, ancestors) {
            sidebar.revealIndex(target, ancestors);
        }
    }

    // ---- layout ------------------------------------------------------------------------------

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: toolbar.implicitHeight + Theme.spacing * 2
            color: Theme.bgSurface

            Accessible.role: Accessible.ToolBar
            Accessible.name: qsTr("Toolbar")

            // Bottom edge, so the toolbar is delimited without relying on the fill color.
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.borderDefault
            }

            RowLayout {
                id: toolbar

                anchors.fill: parent
                anchors.margins: Theme.spacing
                spacing: Theme.spacing

                IconButton {
                    id: menuButton

                    accessibleName: qsTr("Menu")
                    iconGlyph: Icons.menu
                    onClicked: {
                        menuLoader.active = true;
                        (menuLoader.item as QMenu).popup(menuButton, 0, menuButton.height);
                    }
                }

                IconButton {
                    accessibleName: cmdSidebar.title
                    shortcutText: cmdSidebar.shortcutText
                    iconGlyph: Icons.panelLeft
                    onClicked: CommandRegistry.trigger(cmdSidebar.commandId)
                }

                IconButton {
                    accessibleName: cmdResults.title
                    shortcutText: cmdResults.shortcutText
                    iconGlyph: Icons.panelBottom
                    onClicked: CommandRegistry.trigger(cmdResults.commandId)
                }

                IconButton {
                    accessibleName: cmdResetLayout.title
                    iconGlyph: Icons.rotateCcw
                    onClicked: CommandRegistry.trigger(cmdResetLayout.commandId)
                }

                Item {
                    Layout.fillWidth: true
                }

                IconButton {
                    accessibleName: cmdPalette.title
                    shortcutText: cmdPalette.shortcutText
                    iconGlyph: Icons.command
                    onClicked: CommandRegistry.trigger(cmdPalette.commandId)
                }
            }
        }

        QSplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal
            accessibleName: qsTr("Resize sidebar")

            onResized: saveLayout.restart()

            Region {
                id: sidebarRegion

                regionName: qsTr("Sidebar")
                defaultFocus: sidebar.defaultFocus
                visible: shell.sidebarVisible
                T.SplitView.preferredWidth: Number(LayoutStore.value("sidebarWidth", shell.defaultSidebarWidth))
                T.SplitView.minimumWidth: Theme.controlHeight * 6
                T.SplitView.maximumWidth: Math.max(T.SplitView.minimumWidth, shell.width * 0.6)

                // The tree on top; the History / Saved panel below it while it is open.
                QSplitView {
                    anchors.fill: parent
                    orientation: Qt.Vertical
                    accessibleName: qsTr("Resize history panel")

                    ConnectionList {
                        id: sidebar

                        T.SplitView.fillHeight: true
                        T.SplitView.minimumHeight: Theme.controlHeight * 5
                        connections: connectionsCtl
                        newShortcut: cmdNewConnection.shortcutText
                        onNewRequested: CommandRegistry.trigger(cmdNewConnection.commandId)
                        onOpenSqliteRequested: CommandRegistry.trigger(cmdOpenSqlite.commandId)
                        onEditRequested: id => shell.openConnectionDialog(id, false)
                        onDuplicateRequested: id => connectionsCtl.duplicate(id)
                        onDeleteRequested: id => shell.askDelete(id)
                        onOpenDataRequested: object => shell.openData(object)
                    }

                    Loader {
                        id: libraryLoader

                        active: false
                        visible: shell.libraryVisible
                        T.SplitView.preferredHeight: Math.round(sidebarRegion.height * 0.55)
                        T.SplitView.minimumHeight: Theme.controlHeight * 8

                        sourceComponent: LibraryPanel {
                            history: historyModel
                            saved: savedModel
                            connections: connectionsCtl
                            activeConnectionId: editorArea.currentView?.connectionId ?? ""
                            onCopyRequested: (sql, connectionId) => shell.insertSql(sql, connectionId, false)
                            onRunRequested: (sql, connectionId) => shell.insertSql(sql, connectionId, true)
                            onOpenSavedRequested: (sql, connectionId) => shell.openSaved(sql, connectionId)
                            onSaveCurrentRequested: shell.saveCurrentQuery()
                            onUseConnectionRequested: id => shell.useConnection(id)
                            onCloseRequested: shell.hideLibrary()
                            Keys.onEscapePressed: shell.hideLibrary()
                        }
                    }
                }
            }

            QSplitView {
                T.SplitView.fillWidth: true
                T.SplitView.minimumWidth: Theme.controlHeight * 8
                orientation: Qt.Vertical
                accessibleName: qsTr("Resize results panel")

                onResized: saveLayout.restart()

                Region {
                    id: editorRegion

                    regionName: qsTr("Editor")
                    color: Theme.bgCanvas
                    defaultFocus: editorArea.defaultFocus
                    T.SplitView.fillHeight: true
                    T.SplitView.minimumHeight: Theme.controlHeight * 4
                    Keys.onEscapePressed: event => shell.cancelOnEscape(event)

                    EditorArea {
                        id: editorArea

                        anchors.fill: parent
                        tabs: tabsModel
                        queries: queryCtl
                        connections: connectionsCtl
                        completion: completionCtl
                        exporter: exportCtl
                        newShortcut: cmdNewTab.shortcutText
                        onNewRequested: CommandRegistry.trigger(cmdNewTab.commandId)
                        onCloseRequested: row => shell.closeQuery(row)
                    }
                }

                Region {
                    id: resultsRegion

                    regionName: qsTr("Results panel")
                    color: Theme.bgCanvas
                    defaultFocus: resultsPanel.focusTarget
                    visible: shell.resultsVisible
                    Keys.onEscapePressed: event => shell.cancelOnEscape(event)
                    T.SplitView.preferredHeight: Number(LayoutStore.value("resultsHeight", shell.defaultResultsHeight))
                    T.SplitView.minimumHeight: Theme.controlHeight * 4

                    ResultsPanel {
                        id: resultsPanel

                        anchors.fill: parent
                        queries: queryCtl
                        exporter: exportCtl
                    }
                }
            }
        }

        Region {
            id: statusRegion

            readonly property bool queryShown: queryCtl.current.state !== "idle"

            Layout.fillWidth: true
            implicitHeight: statusColumn.implicitHeight + 1
            regionName: qsTr("Status bar")
            defaultFocus: statusItem

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.borderDefault
            }

            ColumnLayout {
                id: statusColumn

                anchors.fill: parent
                anchors.topMargin: 1
                spacing: 0

                // The last or running query of the current tab. Its own row, so "Running… 00:03" and
                // Cancel never squeeze the connection and cursor texts (200% zoom).
                RowLayout {
                    id: queryRow

                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing * 2
                    Layout.rightMargin: Theme.spacing * 2
                    visible: statusRegion.queryShown
                    spacing: Theme.spacing

                    Icon {
                        text: queryCtl.current.state === "running" ? Icons.loaderCircle : queryCtl.current.state === "done" ? Icons.circleCheck : queryCtl.current.state === "failed" ? Icons.circleAlert : Icons.info
                        color: queryCtl.current.state === "failed" ? Theme.danger : Theme.fgMuted
                    }

                    Text {
                        id: queryText

                        Layout.fillWidth: true
                        text: queryCtl.current.statusText
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                    }

                    QButton {
                        id: cancelQueryButton

                        visible: queryCtl.current.running
                        text: qsTr("Cancel")
                        iconGlyph: Icons.square
                        Accessible.name: qsTr("Cancel query")
                        Accessible.description: cmdCancelQuery.shortcutText
                        onClicked: CommandRegistry.trigger(cmdCancelQuery.commandId)
                    }
                }

                // Focusable so keyboard and screen-reader users can read the status.
                Item {
                    id: statusItem

                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(Theme.controlHeight, statusText.implicitHeight + Theme.spacing * 2)
                    activeFocusOnTab: true

                    Accessible.role: Accessible.StaticText
                    Accessible.name: (statusRegion.queryShown ? queryCtl.current.statusText + ". " : "") + statusText.text + (sidebar.hasCurrent ? ". " + shell.selectedSummary : "") + (shell.tabHistoryOff ? ". " + qsTr("History is off for this connection") : "") + (shell.cursorSummary !== "" ? ". " + shell.cursorSummary : "")
                    Accessible.focusable: true

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacing * 2
                        anchors.rightMargin: Theme.spacing * 2
                        spacing: Theme.spacing

                        Icon {
                            text: sidebar.currentConnected ? Icons.plug : Icons.unplug
                            color: Theme.fgMuted
                        }

                        Text {
                            id: statusText

                            Layout.fillWidth: true
                            text: sidebar.currentConnected ? qsTr("Connected") : qsTr("Not connected")
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            wrapMode: Text.WordWrap
                        }

                        // The connection of the selected node.
                        Text {
                            visible: sidebar.hasCurrent
                            text: qsTr("Selected: %1").arg(sidebar.currentName)
                            font.pointSize: Theme.fontSize
                            color: Theme.fgMuted
                        }

                        ProductionBadge {
                            visible: sidebar.hasCurrent && sidebar.currentProduction
                        }

                        // The connection of the current tab does not record history: icon and words.
                        RowLayout {
                            visible: shell.tabHistoryOff
                            spacing: Theme.spacing

                            Icon {
                                text: Icons.eyeOff
                                color: Theme.fgMuted
                            }

                            Text {
                                text: qsTr("History off")
                                font.pointSize: Theme.fontSize
                                color: Theme.fgMuted
                                Accessible.ignored: true
                            }
                        }

                        // The cursor of the current editor.
                        Text {
                            // Never cut: the other status items give way first.
                            Layout.minimumWidth: implicitWidth
                            Layout.preferredWidth: implicitWidth
                            visible: shell.cursorSummary !== ""
                            text: shell.cursorSummary
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                        }
                    }

                    FocusRing {
                        shown: statusItem.activeFocus
                        inset: true
                    }
                }
            }
        }
    }

    // ---- overlays ----------------------------------------------------------------------------

    Loader {
        id: paletteLoader

        active: false
        sourceComponent: CommandPalette {
            completion: completionCtl
            // The tables of the connection of the current tab.
            connectionId: editorArea.currentView?.connectionId ?? ""
            onObjectChosen: (id, parts) => shell.revealObject(id, parts)
        }
    }

    Loader {
        id: settingsLoader

        active: false
        sourceComponent: SettingsDialog {
            completion: completionCtl
        }
    }

    Loader {
        id: connectionLoader

        active: false
        sourceComponent: ConnectionDialog {
            connections: connectionsCtl
        }
    }

    Loader {
        id: deleteLoader

        active: false
        sourceComponent: DeleteConnectionDialog {
            onDeleteConfirmed: id => connectionsCtl.remove(id)
        }
    }

    Loader {
        id: closeTabLoader

        active: false
        sourceComponent: CloseTabDialog {
            onCloseConfirmed: tabId => shell.closeQueryNow(tabId)
        }
    }

    Loader {
        id: confirmRunLoader

        active: false
        sourceComponent: ConfirmRunDialog {
            onConfirmed: tabId => queryCtl.confirm(tabId)
            onDeclined: tabId => queryCtl.decline(tabId)
        }
    }


    Loader {
        id: previewLoader

        active: false
        sourceComponent: PreviewSqlDialog {
            onSaveRequested: tabId => queryCtl.apply(tabId)
        }
    }

    Loader {
        id: applyConfirmLoader

        active: false
        sourceComponent: ApplyConfirmDialog {
            onConfirmed: (tabId, typedName) => queryCtl.confirmApply(tabId, typedName)
            onDeclined: tabId => queryCtl.declineApply(tabId)
        }
    }

    Loader {
        id: exportLoader

        active: false
        sourceComponent: ExportDialog {
            controller: exportCtl
        }
    }

    Loader {
        id: discardLoader

        active: false
        sourceComponent: DiscardChangesDialog {
            onDiscardConfirmed: (action, tabId) => shell.runDiscard(action, tabId)
        }
    }

    // The privacy question after "Don't save query history" was turned on for a connection.
    Loader {
        id: historyOptOutLoader

        active: false
        sourceComponent: ConfirmActionDialog {
            property string connectionId: ""

            function openFor(id, name) {
                connectionId = id;
                title = qsTr("Query history");
                message = qsTr("“%1” no longer saves query history. Delete the history it already has too?").arg(name);
                confirmText = qsTr("Delete history");
                cancelText = qsTr("Keep history");
                open();
            }

            onConfirmed: historyModel.clearConnection(connectionId)
        }
    }

    // The password of an "Ask every time" connection, asked on the first run of a tab.
    Loader {
        id: queryPasswordLoader

        active: false
        sourceComponent: PasswordDialog {
            onSubmitted: (id, password) => queryCtl.providePassword(id, password)
            onDismissed: id => queryCtl.cancelPassword(id)
        }
    }

    // The SSH tunnel's secret ("Ask every time") and the server's fingerprint.
    Loader {
        id: sshSecretLoader

        active: false
        sourceComponent: PasswordDialog {
            onSubmitted: (id, secret) => connectionsCtl.preflight.provideSecret(id, secret)
            onDismissed: id => connectionsCtl.preflight.cancelSecret(id)
        }
    }

    Loader {
        id: hostKeyLoader

        active: false
        sourceComponent: HostKeyDialog {
            onTrusted: remember => connectionsCtl.preflight.trustHostKey(remember)
            onDeclined: connectionsCtl.preflight.cancelHostKey()
        }
    }

    Connections {
        target: connectionsCtl.preflight

        function onSecretRequested(connectionId, name, passphrase, retry) {
            shell.askSshSecret(connectionId, name, passphrase, retry);
        }

        function onHostKeyRequested(host, port, keyType, fingerprint) {
            shell.askHostKey(host, port, keyType, fingerprint);
        }

        function onHostKeyMismatch(host, port, keyType, known, received) {
            shell.showHostKeyChanged(host, port, keyType, known, received);
        }
    }

    // "Open SQLite file": the platform file dialog, created on first use.
    Loader {
        id: pickerLoader

        // By URL, not as a type: naming FilePicker here would load QtQuick.Dialogs at startup.
        active: false
        source: "FilePicker.qml"
    }

    Connections {
        target: pickerLoader.item

        function onAccepted() {
            // qmllint disable missing-property
            connectionsCtl.openSqliteFile(pickerLoader.item["selectedFile"]);
            // qmllint enable missing-property
        }
    }

    Loader {
        id: menuLoader

        active: false
        sourceComponent: QMenu {
            id: appMenu

            // The chosen command runs once the menu is closed, so its focus change sticks.
            property string pendingId: ""

            title: qsTr("Menu")
            onClosed: {
                if (pendingId !== "") {
                    const id = pendingId;
                    pendingId = "";
                    Qt.callLater(() => CommandRegistry.trigger(id));
                }
            }

            Instantiator {
                model: CommandRegistry.commands

                delegate: QMenuItem {
                    required property var modelData

                    text: modelData.title
                    shortcutText: modelData.shortcutText
                    enabled: modelData.enabled
                    checkable: modelData.checkable
                    checked: modelData.checked
                    onTriggered: appMenu.pendingId = modelData.commandId
                }

                onObjectAdded: (index, object) => appMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => appMenu.removeItem(object)
            }
        }
    }

    // Idle preload, so the first Ctrl+K or Ctrl+, opens within a frame.
    Timer {
        interval: 1500
        running: true
        onTriggered: {
            paletteLoader.active = true;
            settingsLoader.active = true;
        }
    }
}
