pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The sidebar content: one tree. The saved connections are its roots; expanding one connects and
// lists its schema lazily (database, folders, tables, columns...). Keyboard: Up/Down move, Right
// expands or goes to the first child, Left collapses or goes to the parent, Home/End, Enter
// toggles (Enter on a table, view or materialized view opens its data), letters jump to the next node starting with them, Delete deletes a connection after
// confirming, Shift+F10 / Menu / right click open the context menu. Collapsing does not
// disconnect; "Disconnect" does. There is one Tab stop for the whole tree (roving selection).
Item {
    id: root

    required property var connections // ConnectionController
    property string newShortcut: ""
    // The last thing sent to the screen reader (tests read it; it is not shown).
    property string lastAnnouncement: ""
    // Bumped when the data of a node changes, so the "current" properties below re-read it.
    property int revision: 0

    readonly property alias tree: treeLoader.item
    readonly property alias newButton: newBtn
    readonly property alias menu: actionMenu
    readonly property var passwordDialog: passwordLoader.item
    // Always read from the selection model: it holds a persistent index, so it never goes stale
    // when nodes are replaced (a copy kept in a property would).
    readonly property bool hasCurrent: tree !== null && tree.selectionModel.currentIndex.valid
    // The connection the current node belongs to.
    readonly property string currentId: {
        root.revision;
        return tree ? connections.tree.connectionIdOf(tree.selectionModel.currentIndex) : "";
    }
    readonly property string currentName: rootRole(SchemaTreeModel.NameRole, "")
    readonly property bool currentProduction: rootRole(SchemaTreeModel.ProductionRole, false)
    readonly property bool currentConnected: hasCurrent && connections.tree.connectedIds.includes(currentId)
    // {openable, connectionId, parts, kind, name} of the current node: a table, view or
    // materialized view can be opened as a data tab (Enter, or the Open data command).
    readonly property var currentObject: tree ? connections.tree.objectOf(tree.selectionModel.currentIndex) : ({
            "openable": false
        })
    // The migration of the current node ({valid: false} when it is not a migration leaf).
    readonly property var currentMigration: tree ? connections.tree.migrationOf(tree.selectionModel.currentIndex) : ({
            "valid": false
        })
    // First thing F6 / Mod+1 should land on.
    readonly property Item defaultFocus: tree ?? (newBtn.visible ? newBtn : headerNew)

    // ---- tree search (H12 phase A) ----
    // Why the box filters (a SchemaTreeModel.FilterScope); remembered between sessions, the text is not.
    property int filterScope: Math.max(0, Math.min(SchemaTreeModel.Migrations, Number(LayoutStore.value("treeFilterScope", SchemaTreeModel.Everything)) || 0))
    readonly property alias filterField: filterInput
    readonly property alias filterMenu: scopeMenu
    readonly property alias filterButton: scopeButton
    // There is a tree to search.
    readonly property bool filterAvailable: treeLoader.active
    // Some connection has a migrations folder configured (the option is off otherwise).
    property bool migrationsAvailable: false
    // The view shows the filtered tree (set after the model has reset).
    property bool filterOn: false
    // The nodes that were expanded before the search began: the tree goes back to them.
    property var savedExpansion: []
    // The current node and its ancestors, as keys, kept across the model reset that follows.
    property var keptKeys: []
    property string lastFilterAnnouncement: ""
    readonly property string filterSummary: summaryText()

    signal newRequested
    signal openSqliteRequested
    signal editRequested(string id)
    signal duplicateRequested(string id)
    signal deleteRequested(string id)
    signal openDataRequested(var object)
    // A migration leaf of the Migrations folder: {valid, connectionId, name, state, batch}.
    signal openMigrationRequested(var migration)

    // MigrationController (optional): pending migrations next to each linked connection.
    property var migrations: null

    function pendingOf(id) {
        if (migrations === null)
            return -1;
        const n = migrations.pendingCounts[id];
        return n === undefined ? -1 : n;
    }

    function rootRole(role, fallback) {
        root.revision;
        const idx = tree ? tree.selectionModel.currentIndex : null;
        return idx?.valid ? connections.tree.data(connections.tree.connectionOf(idx), role) : fallback;
    }

    function selectId(id) {
        const idx = connections.tree.indexOfConnection(id);
        if (!idx.valid || !tree)
            return;
        tree.selectionModel.setCurrentIndex(idx, ItemSelectionModel.ClearAndSelect);
        const row = tree.rowAtIndex(idx);
        if (row >= 0)
            tree.positionViewAtRow(row, TableView.Contain);
    }

    // Shows a node that was found by SchemaTreeModel::reveal(): expands the branches above it,
    // makes it current and gives the tree the focus.
    function revealIndex(target, ancestors) {
        if (!tree)
            return;
        for (const a of ancestors) {
            const r = tree.rowAtIndex(a);
            if (r >= 0 && !tree.isExpanded(r))
                tree.expand(r);
        }
        // The view counts its rows on the next layout pass: goTo() would still see the old count.
        tree.forceLayout();
        const row = tree.rowAtIndex(target);
        if (row < 0)
            return;
        tree.selectionModel.setCurrentIndex(target, ItemSelectionModel.ClearAndSelect);
        tree.positionViewAtRow(row, TableView.Contain);
        tree.forceActiveFocus(Qt.TabFocusReason);
        announce(qsTr("%1 selected in the tree").arg(connections.tree.data(target, SchemaTreeModel.NameRole)), false);
    }

    function popupMenu(item, index) {
        actionMenu.targetId = connections.tree.connectionIdOf(index);
        actionMenu.popup(item, Theme.spacing * 2, item === root.tree ? 0 : item.height);
    }

    function announce(text, assertive) {
        lastAnnouncement = text;
        (tree ?? root).Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    // ---- tree search ----

    function scopeLabel(scope) {
        switch (scope) {
        case SchemaTreeModel.Connections:
            return qsTr("Filter by connections");
        case SchemaTreeModel.Databases:
            return qsTr("Filter by databases");
        case SchemaTreeModel.Tables:
            return qsTr("Filter by tables and indexes");
        case SchemaTreeModel.Views:
            return qsTr("Filter by views");
        case SchemaTreeModel.Users:
            return qsTr("Filter by users");
        case SchemaTreeModel.Migrations:
            return qsTr("Filter by migrations");
        default:
            return qsTr("Filter by everything");
        }
    }

    function summaryText() {
        const t = connections.tree;
        if (!t.filterActive)
            return "";
        return t.filterIncomplete ? qsTr("%n result(s); connect or expand to search more", "", t.filterCount) : qsTr("%n result(s)", "", t.filterCount);
    }

    function refreshMigrations() {
        let any = false;
        if (migrations !== null) {
            const model = connections.model;
            for (let i = 0; i < model.count && !any; ++i)
                any = migrations.isConfigured(model.get(i).connectionId);
        }
        migrationsAvailable = any;
        if (!any && filterScope === SchemaTreeModel.Migrations)
            filterScope = SchemaTreeModel.Everything;
    }

    function focusFilter() {
        if (!filterAvailable) {
            defaultFocus.forceActiveFocus(Qt.TabFocusReason);
            return;
        }
        filterInput.forceActiveFocus(Qt.TabFocusReason);
        filterInput.selectAll();
    }

    // Sends the box to the model: the text after the typing delay, a new scope at once.
    function applyFilter() {
        const t = connections.tree;
        if (tree && filterInput.text.trim() !== "" && !t.filterActive && !filterOn)
            savedExpansion = expandedKeys();
        t.setFilter(filterInput.text, filterScope);
    }

    // The nodes that are open now.
    function expandedKeys() {
        const keys = [];
        if (!tree)
            return keys;
        for (let r = 0; r < tree.rows; ++r)
            if (tree.isExpanded(r))
                keys.push(connections.tree.keyOf(tree.index(r, 0)));
        return keys;
    }

    // The keys of a node and of everything above it.
    function chainKeys(idx) {
        const t = connections.tree;
        if (!idx.valid)
            return [];
        const keys = t.ancestorsOf(idx).map(a => t.keyOf(a));
        keys.push(t.keyOf(idx));
        return keys;
    }

    // Opens `expandKeys` and the branches above `chain`, and makes the last of `chain` current again
    // unless something chose a current node meanwhile. Only nodes already loaded are opened.
    function reopen(expandKeys, chain) {
        const t = connections.tree;
        const open = expandKeys.concat(chain.slice(0, -1));
        const found = t.indexesOfKeys(open.concat(chain.slice(-1)));
        const last = chain.length > 0 ? chain[chain.length - 1] : "";
        let current = null;
        for (const idx of found) {
            const key = t.keyOf(idx);
            if (key === last)
                current = idx;
            if (!open.includes(key))
                continue;
            const r = tree.rowAtIndex(idx);
            if (r >= 0 && !tree.isExpanded(r) && t.hasChildren(idx) && !t.canFetchMore(idx))
                tree.expand(r);
        }
        tree.forceLayout();
        if (current !== null && !tree.selectionModel.currentIndex.valid) {
            const row = tree.rowAtIndex(current);
            if (row >= 0) {
                tree.selectionModel.setCurrentIndex(current, ItemSelectionModel.ClearAndSelect);
                tree.positionViewAtRow(row, TableView.Contain);
            }
        }
    }

    // A reset of the model (a new search, or the tree changing under one) has collapsed the view.
    function afterReset() {
        if (!tree) { // the tree went with its last connection: nothing to give back
            filterOn = false;
            savedExpansion = [];
            return;
        }
        const t = connections.tree;
        if (t.filterActive) {
            filterOn = true;
            for (const idx of t.filterExpansion()) {
                const r = tree.rowAtIndex(idx);
                if (r >= 0 && !tree.isExpanded(r))
                    tree.expand(r);
            }
            reopen([], keptKeys);
        } else if (filterOn) {
            filterOn = false;
            lastFilterAnnouncement = "";
            reopen(savedExpansion, keptKeys);
            savedExpansion = [];
        }
    }

    // First result: the first node the search matched, not the connection above it.
    function focusFirstResult() {
        if (!tree || tree.rows === 0)
            return;
        const first = connections.tree.firstMatch();
        const row = first.valid ? tree.rowAtIndex(first) : 0;
        tree.forceActiveFocus(Qt.TabFocusReason);
        tree.goTo(row >= 0 ? row : 0);
    }

    Timer {
        id: filterTimer

        interval: 150
        onTriggered: root.applyFilter()
    }

    onFilterScopeChanged: {
        LayoutStore.setValue("treeFilterScope", filterScope);
        filterTimer.stop();
        applyFilter();
    }

    // The last connection went: the box goes with the tree, and so does the search.
    onFilterAvailableChanged: {
        if (!filterAvailable)
            filterInput.text = "";
    }

    Component.onCompleted: refreshMigrations()

    Connections {
        target: root.migrations
        ignoreUnknownSignals: true

        function onCountsChanged() {
            root.refreshMigrations();
        }

        function onConnectionsChanged() {
            root.refreshMigrations();
        }

        function onFolderChanged() {
            root.refreshMigrations();
        }

        function onSettingsChanged() {
            root.refreshMigrations();
        }
    }

    Connections {
        target: root.connections.model

        function onCountChanged() {
            root.refreshMigrations();
        }
    }

    Connections {
        target: root.connections.tree

        function onAnnouncement(text, assertive) {
            root.announce(text, assertive);
        }

        // reveal() dropped the search: the box follows.
        function onFilterCleared() {
            filterTimer.stop();
            filterInput.text = "";
        }

        // Polite, once per result: the model also says "changed" when a load finishes under a search.
        function onFilterChanged() {
            const text = root.summaryText();
            if (text === "")
                return;
            if (text !== root.lastFilterAnnouncement) {
                root.lastFilterAnnouncement = text;
                root.announce(text, false);
            }
        }

        function onModelAboutToBeReset() {
            root.keptKeys = root.tree ? root.chainKeys(root.tree.selectionModel.currentIndex) : [];
        }

        // Disconnected, or the password prompt was dismissed: show the branch collapsed.
        function onCollapseRequested(index) {
            const row = root.tree ? root.tree.rowAtIndex(index) : -1;
            if (row >= 0 && root.tree.isExpanded(row))
                root.tree.collapseRow(row);
        }

        function onPasswordRequested(id, name, retry) {
            passwordLoader.active = true;
            passwordLoader.item.openFor(id, name, retry);
        }

        function onDataChanged() {
            root.revision++;
        }

        // After the view has reset itself too (its slot may run after this one).
        function onModelReset() {
            root.revision++;
            Qt.callLater(root.afterReset);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing * 2
        spacing: Theme.spacing * 2

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Heading {
                Layout.fillWidth: true
                level: 2
                text: qsTr("Connections")
            }

            IconButton {
                id: headerNew

                accessibleName: qsTr("New connection")
                shortcutText: root.newShortcut
                iconGlyph: Icons.plus
                onClicked: root.newRequested()
            }
        }

        // Load failure, or a failed delete / duplicate / open.
        ColumnLayout {
            id: banner

            readonly property string message: root.connections.errorMessage !== "" ? root.connections.errorMessage : root.connections.model.lastError
            readonly property string detail: root.connections.errorMessage !== "" ? root.connections.errorDetail : root.connections.model.lastErrorDetail

            Layout.fillWidth: true
            visible: message !== ""
            spacing: Theme.spacing

            onMessageChanged: {
                if (message !== "")
                    banner.Accessible.announce(message, Accessible.Assertive);
            }

            // Created when there is something to say: it keeps its text engine off the startup path.
            Loader {
                Layout.fillWidth: true
                active: banner.visible
                sourceComponent: ErrorPanel {
                    title: root.connections.errorMessage !== "" ? qsTr("Something went wrong") : qsTr("Could not load the connections")
                    message: banner.message
                    detail: banner.detail
                }
            }

            RowLayout {
                spacing: Theme.spacing

                QButton {
                    visible: root.connections.errorMessage === ""
                    text: qsTr("Try again")
                    onClicked: root.connections.model.start()
                }

                QButton {
                    visible: root.connections.errorMessage !== ""
                    text: qsTr("Dismiss")
                    onClicked: root.connections.clearError()
                }
            }
        }

        // Loading: shown instead of the empty state so the first frame does not claim "nothing".
        RowLayout {
            Layout.fillWidth: true
            visible: root.connections.model.loading
            spacing: Theme.spacing

            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Loading connections…")

            Icon {
                id: spinner

                text: Icons.loaderCircle
                color: Theme.fgMuted

                RotationAnimator on rotation {
                    running: spinner.visible && Theme.animationDuration > 0
                    from: 0
                    to: 360
                    duration: 1000
                    loops: Animation.Infinite
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Loading connections…")
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }
        }

        EmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.connections.model.loading && root.connections.model.count === 0 && root.connections.model.lastError === ""
            iconGlyph: Icons.database
            title: qsTr("No connections yet")
            message: qsTr("Add a connection to browse its tables and run queries.")

            QButton {
                id: newBtn

                Layout.alignment: Qt.AlignHCenter
                text: qsTr("New connection")
                variant: "primary"
                iconGlyph: Icons.plus
                onClicked: root.newRequested()
            }

            QButton {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Open SQLite file")
                iconGlyph: Icons.folderOpen
                onClicked: root.openSqliteRequested()
            }
        }

        // The search: the button says what is searched, the field's placeholder repeats it. Tab goes
        // to the button, the field, then the tree.
        ColumnLayout {
            id: filterBox

            Layout.fillWidth: true
            visible: root.filterAvailable
            spacing: Theme.spacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing

                IconButton {
                    id: scopeButton

                    accessibleName: qsTr("Search options")
                    iconGlyph: Icons.listFilter
                    Accessible.description: root.scopeLabel(root.filterScope)
                    Accessible.expandable: true
                    Accessible.expanded: scopeMenu.visible
                    onClicked: scopeMenu.popup(scopeButton, 0, scopeButton.height)
                }

                QTextField {
                    id: filterInput

                    Layout.fillWidth: true
                    accessibleName: qsTr("Search the tree")
                    placeholderText: root.scopeLabel(root.filterScope)
                    Accessible.description: root.scopeLabel(root.filterScope)
                    onTextChanged: {
                        if (text === "") {
                            // Clearing is not typing: show the whole tree at once.
                            filterTimer.stop();
                            root.applyFilter();
                        } else {
                            filterTimer.restart();
                        }
                    }
                    Keys.onEscapePressed: event => {
                        event.accepted = true;
                        if (text !== "")
                            text = "";
                        else if (root.tree)
                            root.tree.forceActiveFocus(Qt.TabFocusReason);
                    }
                    Keys.onReturnPressed: root.focusFirstResult()
                    Keys.onEnterPressed: root.focusFirstResult()
                    Keys.onDownPressed: root.focusFirstResult()
                }

                IconButton {
                    id: clearButton

                    visible: filterInput.text !== ""
                    accessibleName: qsTr("Clear search")
                    iconGlyph: Icons.x
                    focusPolicy: Qt.NoFocus // Esc does it from the keyboard
                    onClicked: {
                        filterInput.text = "";
                        filterInput.forceActiveFocus(Qt.MouseFocusReason);
                    }
                }
            }

            // Visible text as well as an announcement: shown once the search has been applied.
            Text {
                id: summaryLine

                Layout.fillWidth: true
                visible: root.filterSummary !== ""
                text: root.filterSummary
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }

        EmptyState {
            id: noMatches

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.filterAvailable && root.connections.tree.filterActive && root.connections.tree.filterCount === 0
            iconGlyph: Icons.search
            title: qsTr("No matches")
            message: qsTr("Nothing in the tree matches. Connect or expand a connection to search more.")

            QButton {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Clear search")
                onClicked: {
                    filterInput.text = "";
                    filterInput.forceActiveFocus(Qt.MouseFocusReason);
                }
            }
        }

        // Created when the first connection exists: nothing of the tree (TreeView, delegates)
        // is loaded on the startup path, which only ever shows the empty state.
        Loader {
            id: treeLoader

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !noMatches.visible
            active: root.connections.model.count > 0

            sourceComponent: TreeView {
                id: treeView

                // Type-ahead state: what has been typed in the last second.
                property string typed: ""

                anchors.fill: parent
                clip: true
                model: root.connections.tree
                selectionModel: ItemSelectionModel {
                    model: treeView.model
                }
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                // Everything below is done by hand, so the keys do exactly what UX.md says.
                keyNavigationEnabled: false
                pointerNavigationEnabled: false
                editTriggers: TableView.NoEditTriggers
                columnWidthProvider: () => treeView.width
                onWidthChanged: forceLayout()

                Accessible.role: Accessible.Tree
                Accessible.name: qsTr("Connections")
                Accessible.focusable: true

                function cursorRow() {
                    return selectionModel.currentIndex.valid ? rowAtIndex(selectionModel.currentIndex) : -1;
                }

                function goTo(row) {
                    if (row < 0 || row >= rows)
                        return;
                    selectionModel.setCurrentIndex(index(row, 0), ItemSelectionModel.ClearAndSelect);
                    positionViewAtRow(row, TableView.Contain);
                }

                function roleAt(row, role) {
                    return root.connections.tree.data(index(row, 0), role);
                }

                function parentRow(row) {
                    const d = depth(row);
                    for (let r = row - 1; r >= 0; --r)
                        if (depth(r) < d)
                            return r;
                    return -1;
                }

                function hasChildNodes(row) {
                    return root.connections.tree.hasChildren(index(row, 0));
                }

                // Expands and, for a node not loaded yet, asks the model for its children.
                function expandRow(row) {
                    if (isExpanded(row))
                        return;
                    expand(row);
                    const i = index(row, 0);
                    if (root.connections.tree.canFetchMore(i))
                        root.connections.tree.fetchMore(i);
                }

                // Keeps the current node visible: if it was inside the branch, the branch takes it.
                function collapseRow(row) {
                    collapse(row);
                    if (cursorRow() < 0)
                        goTo(row);
                }

                function toggleRow(row) {
                    if (isExpanded(row))
                        collapseRow(row);
                    else
                        expandRow(row);
                }

                function activateRow(row) {
                    const object = root.connections.tree.objectOf(index(row, 0));
                    const migration = root.connections.tree.migrationOf(index(row, 0));
                    if (roleAt(row, SchemaTreeModel.KindRole) === "error")
                        root.connections.tree.retry(index(row, 0));
                    else if (object.openable)
                        root.openDataRequested(object);
                    else if (migration.valid)
                        root.openMigrationRequested(migration);
                    else if (hasChildNodes(row))
                        toggleRow(row);
                }

                function typeAhead(ch) {
                    typed += ch;
                    typeTimer.restart();
                    const prefix = typed.toLowerCase();
                    const cur = cursorRow();
                    // Typing on keeps the current node while it still matches.
                    const first = typed.length > 1 ? Math.max(cur, 0) : cur + 1;
                    for (let k = 0; k < rows; ++k) {
                        const r = (first + k) % rows;
                        if (String(roleAt(r, SchemaTreeModel.NameRole)).toLowerCase().startsWith(prefix)) {
                            const kind = roleAt(r, SchemaTreeModel.KindRole);
                            if (kind !== "loading" && kind !== "error" && kind !== "empty") {
                                goTo(r);
                                return;
                            }
                        }
                    }
                }

                function popupAtCurrent() {
                    const idx = selectionModel.currentIndex;
                    if (!idx.valid)
                        return;
                    root.popupMenu(itemAtIndex(idx) ?? treeView, idx);
                }

                onActiveFocusChanged: {
                    if (activeFocus && !selectionModel.currentIndex.valid && rows > 0)
                        goTo(0);
                }

                Keys.onReturnPressed: if (cursorRow() >= 0) activateRow(cursorRow())
                Keys.onEnterPressed: if (cursorRow() >= 0) activateRow(cursorRow())
                Keys.onMenuPressed: popupAtCurrent()
                Keys.onDeletePressed: {
                    const row = cursorRow();
                    if (row >= 0 && roleAt(row, SchemaTreeModel.KindRole) === "connection")
                        root.deleteRequested(roleAt(row, SchemaTreeModel.ConnectionIdRole));
                }
                Keys.onPressed: event => {
                    const row = cursorRow();
                    const page = Math.max(1, Math.floor(height / Theme.controlHeight) - 1);
                    event.accepted = true;
                    switch (event.key) {
                    case Qt.Key_Escape:
                        // Esc drops the search, the way it does in its field.
                        if (root.connections.tree.filterActive) {
                            filterInput.text = "";
                            return;
                        }
                        break;
                    case Qt.Key_Down:
                        goTo(Math.min(row + 1, rows - 1));
                        return;
                    case Qt.Key_Up:
                        goTo(Math.max(row - 1, 0));
                        return;
                    case Qt.Key_PageDown:
                        goTo(Math.min(Math.max(row, 0) + page, rows - 1));
                        return;
                    case Qt.Key_PageUp:
                        goTo(Math.max(row - page, 0));
                        return;
                    case Qt.Key_Home:
                        goTo(0);
                        return;
                    case Qt.Key_End:
                        goTo(rows - 1);
                        return;
                    case Qt.Key_Right:
                        if (row < 0)
                            goTo(0);
                        else if (hasChildNodes(row) && !isExpanded(row))
                            expandRow(row);
                        else if (isExpanded(row) && row + 1 < rows && depth(row + 1) > depth(row))
                            goTo(row + 1);
                        return;
                    case Qt.Key_Left:
                        if (row >= 0 && isExpanded(row))
                            collapseRow(row);
                        else if (row >= 0)
                            goTo(parentRow(row));
                        return;
                    case Qt.Key_F10:
                        if (event.modifiers & Qt.ShiftModifier) {
                            popupAtCurrent();
                            return;
                        }
                        break;
                    case Qt.Key_C:
                        // Copies the technical details of an error row.
                        if ((event.modifiers & Qt.ControlModifier) && row >= 0 && roleAt(row, SchemaTreeModel.KindRole) === "error") {
                            // qmllint disable missing-property
                            itemAtIndex(selectionModel.currentIndex)?.copyDetails();
                            // qmllint enable missing-property
                            return;
                        }
                        break;
                    case Qt.Key_Space:
                        if (typed === "" && row >= 0 && roleAt(row, SchemaTreeModel.KindRole) === "error") {
                            // qmllint disable missing-property
                            itemAtIndex(selectionModel.currentIndex)?.toggleDetails();
                            // qmllint enable missing-property
                            return;
                        }
                        break;
                    }
                    // Printable character without Ctrl/Alt/Meta: type-ahead.
                    if (!(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) && event.text.length === 1 && event.text >= " ") {
                        typeAhead(event.text);
                        return;
                    }
                    event.accepted = false;
                }

                Timer {
                    id: typeTimer

                    interval: 1000
                    onTriggered: treeView.typed = ""
                }

                delegate: Item {
                    id: node

                    required property TreeView treeView
                    required property bool isTreeNode
                    required property bool expanded
                    required property bool hasChildren
                    required property int depth
                    required property int row
                    required property int column
                    required property bool current
                    required property string name
                    required property string kind
                    required property string detail
                    required property string connectionId
                    required property string providerName
                    required property string summary
                    required property var colorIndex
                    required property string colorName
                    required property var production
                    required property bool connected
                    required property int position
                    required property int setSize
                    required property string migrationState
                    required property int batch

                    readonly property bool isConnection: kind === "connection"
                    // Folders of the object tree: the secondary text is how many items they hold.
                    readonly property bool isFolder: kind === "folder" || kind.endsWith("Folder")
                    readonly property bool isError: kind === "error"
                    // A migration leaf: its state in words (also on the row, with an icon).
                    readonly property string stateText: migrationState === "applied" ? (batch >= 0 ? qsTr("applied, batch %1").arg(batch) : qsTr("applied")) : migrationState === "orphan" ? (batch >= 0 ? qsTr("orphan: file missing, batch %1").arg(batch) : qsTr("orphan: file missing")) : migrationState === "pending" ? qsTr("pending") : ""
                    readonly property string stateIcon: migrationState === "applied" ? Icons.circleCheck : migrationState === "orphan" ? Icons.fileX : Icons.clock
                    readonly property bool isLoading: kind === "loading"
                    readonly property bool expandable: isTreeNode && hasChildren
                    readonly property bool hasColor: colorIndex !== undefined && colorIndex >= 0
                    // The connection's color is the background of its row (and of its tabs).
                    readonly property bool tinted: isConnection && hasColor
                    property bool detailsOpen: false
                    readonly property int pendingMigrations: isConnection ? root.pendingOf(connectionId) : -1
                    readonly property string pendingText: pendingMigrations > 0 ? qsTr("%n pending migration(s)", "", pendingMigrations) : ""
                    readonly property color textColor: tinted ? tint.ink : current ? Theme.selectionFg : Theme.fgDefault
                    readonly property color mutedColor: tinted ? tint.ink : current ? Theme.selectionFg : Theme.fgMuted
                    // The secondary line of a connection: the file or the database, not the engine.
                    readonly property string connectionDetail: summary !== name ? summary : ""

                    function toggleDetails() {
                        detailsOpen = !detailsOpen;
                    }

                    function copyDetails() {
                        clipboard.selectAll();
                        clipboard.copy();
                        clipboard.deselect();
                    }

                    function iconFor(k) {
                        switch (k) {
                        case "connection":
                        case "database":
                            return Icons.database;
                        case "schema":
                            return Icons.layers;
                        case "folder":
                        case "otherFolder":
                            return expanded ? Icons.folderOpen : Icons.folder;
                        case "databasesFolder":
                            return Icons.database;
                        case "usersFolder":
                            return Icons.users;
                        case "tablesFolder":
                            return Icons.table;
                        case "viewsFolder":
                            return Icons.eye;
                        case "indexesFolder":
                            return Icons.listTree;
                        case "migrationsFolder":
                            return Icons.arrowRightLeft;
                        case "pendingFolder":
                            return Icons.clock;
                        case "appliedFolder":
                            return Icons.circleCheck;
                        case "orphansFolder":
                            return Icons.fileX;
                        case "migration":
                            return Icons.fileCode;
                        case "user":
                            return Icons.userRound;
                        case "table":
                            return Icons.table;
                        case "view":
                        case "materializedView":
                            return Icons.eye;
                        case "column":
                            return Icons.columns3;
                        case "index":
                            return Icons.hash;
                        case "foreignKey":
                            return Icons.link;
                        case "function":
                        case "procedure":
                            return Icons.squareFunction;
                        default:
                            return Icons.braces;
                        }
                    }

                    // The icons are decorative: the kind is also said in words.
                    function kindLabel(k) {
                        switch (k) {
                        case "database":
                            return qsTr("Database");
                        case "schema":
                            return qsTr("Schema");
                        case "table":
                            return qsTr("Table");
                        case "view":
                            return qsTr("View");
                        case "materializedView":
                            return qsTr("Materialized view");
                        case "column":
                            return qsTr("Column");
                        case "index":
                            return qsTr("Index");
                        case "foreignKey":
                            return qsTr("Foreign key");
                        case "function":
                            return qsTr("Function");
                        case "procedure":
                            return qsTr("Procedure");
                        case "sequence":
                            return qsTr("Sequence");
                        case "trigger":
                            return qsTr("Trigger");
                        case "user":
                            return qsTr("User");
                        case "migration":
                            return qsTr("Migration");
                        case "migrationsFolder":
                            return qsTr("Migrations folder");
                        case "pendingFolder":
                            return qsTr("Pending migrations");
                        case "appliedFolder":
                            return qsTr("Applied migrations");
                        case "orphansFolder":
                            return qsTr("Migrations without a file");
                        case "error":
                            return qsTr("Error");
                        default:
                            return isFolder ? qsTr("Folder") : "";
                        }
                    }

                    function describe() {
                        const parts = [];
                        if (isConnection)
                            parts.push(connected ? qsTr("Connected") : qsTr("Not connected"), summary, pendingText);
                        else if (isFolder)
                            parts.push(kindLabel(kind), detail !== "" ? qsTr("%n item(s)", "", Number(detail)) : "");
                        else if (kind === "migration")
                            parts.push(kindLabel(kind), stateText, qsTr("Press Enter to open"));
                        else
                            parts.push(kindLabel(kind), detail);
                        parts.push(qsTr("level %1").arg(depth + 1), qsTr("%1 of %2").arg(position).arg(setSize));
                        if (isError)
                            parts.push(qsTr("Press Enter to retry"));
                        return parts.filter(s => s !== "").join(", ");
                    }

                    implicitHeight: Math.max(Theme.controlHeight, content.implicitHeight + Theme.spacing * 2)

                    TableView.onReused: detailsOpen = false

                    Accessible.role: Accessible.TreeItem
                    Accessible.name: isConnection ? [name, providerName, colorName, production ? qsTr("production") : ""].filter(s => s !== "").join(", ") : name
                    Accessible.description: describe()
                    Accessible.expandable: expandable
                    Accessible.expanded: expanded
                    Accessible.selected: current
                    Accessible.focusable: true
                    Accessible.focused: current && node.treeView.activeFocus

                    ConnectionTint {
                        id: tint

                        colorIndex: node.isConnection && node.hasColor ? node.colorIndex : -1
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.radius
                        color: node.tinted ? tint.fill : node.current ? Theme.selectionBg : hover.hovered ? Theme.bgHover : Qt.alpha(Theme.bgSurface, 0)
                        // On a color the selected row has a frame as well as the bar.
                        border.width: node.tinted && node.current ? Theme.focusRingWidth : 0
                        border.color: tint.ink

                        // Hover on a colored row: a veil of the readable color.
                        Rectangle {
                            anchors.fill: parent
                            radius: parent.radius
                            visible: node.tinted && hover.hovered && !node.current
                            color: Qt.alpha(tint.ink, 0.16)
                        }

                        // The current row is marked by a bar as well as by its fill.
                        Rectangle {
                            visible: node.current
                            width: node.tinted ? Theme.focusRingWidth * 2 : Theme.focusRingWidth
                            height: parent.height
                            color: node.tinted ? tint.ink : Theme.accent
                        }
                    }

                    RowLayout {
                        id: content

                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacing + node.depth * Theme.spacing * 4
                        anchors.rightMargin: Theme.spacing * 2
                        spacing: Theme.spacing

                        // The expander. Decorative: the expanded state is on the item itself.
                        Item {
                            id: expander

                            Layout.alignment: Qt.AlignTop
                            Layout.preferredWidth: Theme.spacing * 6
                            Layout.preferredHeight: Math.max(Theme.controlHeight - Theme.spacing * 2, Theme.spacing * 6)

                            Icon {
                                anchors.centerIn: parent
                                visible: node.expandable
                                text: node.expanded ? Icons.chevronDown : Icons.chevronRight
                                color: node.mutedColor
                            }
                        }

                        Icon {
                            id: kindIcon

                            Layout.alignment: Qt.AlignTop
                            Layout.preferredHeight: expander.Layout.preferredHeight
                            visible: node.kind !== "empty"
                            // A connection shows its state, not its kind: a plug with a check when connected,
                            // a cut plug when not (the words are in the row's name and the tooltip).
                            text: node.isLoading ? Icons.loaderCircle : node.isError ? Icons.circleAlert : node.isConnection ? (node.connected ? Icons.plug : Icons.unplug) : node.iconFor(node.kind)
                            color: node.isError && !node.current ? Theme.danger : node.mutedColor

                            RotationAnimator on rotation {
                                running: node.isLoading && kindIcon.visible && Theme.animationDuration > 0
                                from: 0
                                to: 360
                                duration: 1000
                                loops: Animation.Infinite
                            }

                            // The check of a connected connection, in the corner of the plug.
                            Icon {
                                objectName: "connectedCheck"
                                visible: node.isConnection && node.connected
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: Theme.spacing / 2
                                font.pointSize: Theme.fontSize * 0.75
                                font.bold: true
                                text: Icons.check
                                color: kindIcon.color
                            }

                            HoverHandler {
                                id: stateHover

                                enabled: node.isConnection
                            }

                            QToolTip {
                                visible: node.isConnection && stateHover.hovered
                                delay: 500
                                text: node.connected ? qsTr("Connected") : qsTr("Not connected")
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            spacing: 0

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing

                                Text {
                                    Layout.fillWidth: true
                                    Layout.topMargin: Math.max(0, (expander.Layout.preferredHeight - implicitHeight) / 2)
                                    text: node.name
                                    font.pointSize: Theme.fontSize
                                    font.bold: node.isConnection
                                    font.italic: node.kind === "empty"
                                    color: node.isError && !node.current ? Theme.danger : node.kind === "empty" || node.isLoading ? node.mutedColor : node.textColor
                                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                    Accessible.ignored: true // the item itself carries name and description
                                }

                                // The state of a migration, as an icon next to its name (the words are in the row's description).
                                Icon {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.topMargin: Math.max(0, (expander.Layout.preferredHeight - implicitHeight) / 2)
                                    visible: node.kind === "migration"
                                    text: node.stateIcon
                                    color: node.mutedColor
                                }

                                // How many items a folder holds, once it is known.
                                Text {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.topMargin: Math.max(0, (expander.Layout.preferredHeight - implicitHeight) / 2)
                                    visible: node.isFolder && node.detail !== ""
                                    text: node.detail
                                    font.pointSize: Theme.fontSize
                                    color: node.mutedColor
                                    Accessible.ignored: true // in the item's description
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: text !== ""
                                text: node.isConnection ? node.connectionDetail : node.isError || node.isFolder ? "" : node.detail
                                font.pointSize: Theme.fontSize
                                color: node.mutedColor
                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                Accessible.ignored: true
                            }

                            ProductionBadge {
                                visible: node.isConnection && node.production === true
                                framed: node.tinted
                            }

                            // Pending migrations of the linked project (H12): text, not only color.
                            Text {
                                visible: node.pendingText !== ""
                                text: node.pendingText
                                font.pointSize: Theme.fontSize
                                font.bold: true
                                color: node.textColor
                                Accessible.ignored: true // already in the row's description
                            }

                            // Error row: Retry and the collapsible technical details. Enter retries and
                            // Space opens the details from the keyboard; the buttons are for the pointer.
                            RowLayout {
                                visible: node.isError
                                spacing: Theme.spacing

                                QButton {
                                    text: qsTr("Retry")
                                    focusPolicy: Qt.NoFocus
                                    onClicked: {
                                        node.treeView.selectionModel.setCurrentIndex(node.treeView.index(node.row, 0), ItemSelectionModel.ClearAndSelect);
                                        root.connections.tree.retry(node.treeView.index(node.row, 0));
                                    }
                                }

                                QButton {
                                    visible: node.detail !== ""
                                    variant: "flat"
                                    text: qsTr("Technical details")
                                    iconGlyph: node.detailsOpen ? Icons.chevronDown : Icons.chevronRight
                                    focusPolicy: Qt.NoFocus
                                    onClicked: node.toggleDetails()

                                    Accessible.expandable: true
                                    Accessible.expanded: node.detailsOpen
                                }
                            }

                            TextEdit {
                                id: clipboard

                                Layout.fillWidth: true
                                visible: node.isError && node.detailsOpen && node.detail !== ""
                                readOnly: true
                                selectByMouse: true
                                text: node.detail
                                wrapMode: TextEdit.WrapAnywhere
                                font.family: Theme.monoFontFamily
                                font.pointSize: Theme.fontSize
                                color: node.textColor
                                selectionColor: Theme.selectionFg
                                selectedTextColor: Theme.selectionBg

                                Accessible.role: Accessible.StaticText
                                Accessible.name: qsTr("Technical details")
                                Accessible.description: text
                            }
                        }
                    }

                    HoverHandler {
                        id: hover
                    }

                    TapHandler {
                        onTapped: point => {
                            node.treeView.selectionModel.setCurrentIndex(node.treeView.index(node.row, 0), ItemSelectionModel.ClearAndSelect);
                            node.treeView.forceActiveFocus(Qt.MouseFocusReason);
                            if (node.expandable && point.position.x < expander.x + expander.width + content.anchors.leftMargin)
                                // qmllint disable missing-property
                                root.tree.toggleRow(node.row);
                                // qmllint enable missing-property
                        }
                        onDoubleTapped: point => {
                            // The same as Enter: a table or view opens its data, anything else expands or collapses.
                            if (point.position.x >= expander.x + expander.width + content.anchors.leftMargin)
                                // qmllint disable missing-property
                                root.tree.activateRow(node.row);
                                // qmllint enable missing-property
                        }
                    }

                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            const idx = node.treeView.index(node.row, 0);
                            node.treeView.selectionModel.setCurrentIndex(idx, ItemSelectionModel.ClearAndSelect);
                            node.treeView.forceActiveFocus(Qt.MouseFocusReason);
                            root.popupMenu(node, idx);
                        }
                    }
                }

                FocusRing {
                    parent: treeView // not the flickable's content item
                    shown: treeView.activeFocus
                    inset: true
                }
            }
        }
    }

    Loader {
        id: passwordLoader

        active: false
        sourceComponent: PasswordDialog {
            onSubmitted: (id, password) => root.connections.tree.providePassword(id, password)
            onDismissed: id => root.connections.tree.cancelPassword(id)
        }
    }

    // One option of the search menu: a radio item (exactly one is checked).
    component ScopeItem: QMenuItem {
        required property int scope

        text: root.scopeLabel(scope)
        checkable: true
        autoExclusive: true
        checked: root.filterScope === scope
        onTriggered: root.filterScope = scope
    }

    QMenu {
        id: scopeMenu

        title: qsTr("Search options")
        onAboutToShow: root.refreshMigrations()

        ScopeItem {
            scope: SchemaTreeModel.Everything
        }

        ScopeItem {
            scope: SchemaTreeModel.Connections
        }

        ScopeItem {
            scope: SchemaTreeModel.Databases
        }

        ScopeItem {
            scope: SchemaTreeModel.Tables
        }

        ScopeItem {
            scope: SchemaTreeModel.Views
        }

        ScopeItem {
            scope: SchemaTreeModel.Users
        }

        // Off, with the reason in words (also its description), while no connection has a project.
        ScopeItem {
            scope: SchemaTreeModel.Migrations
            enabled: root.migrationsAvailable
            Accessible.description: enabled ? "" : qsTr("No connection has migrations configured")
        }

        QMenuItem {
            visible: !root.migrationsAvailable
            height: visible ? implicitHeight : 0
            enabled: false
            // Empty while it is not needed, so it does not widen the menu.
            text: root.migrationsAvailable ? "" : qsTr("No connection has migrations configured")
        }
    }

    QMenu {
        id: actionMenu

        // The chosen action runs once the menu has closed, so the dialog it opens remembers the
        // tree (not the menu) as the place to return the focus to. The actions are about the
        // connection the node belongs to.
        property string targetId: ""
        property string action: ""

        title: qsTr("Connection actions")
        onClosed: {
            const id = targetId;
            const what = action;
            action = "";
            if (what !== "")
                Qt.callLater(() => {
                    if (what === "edit")
                        root.editRequested(id);
                    else if (what === "duplicate")
                        root.duplicateRequested(id);
                    else if (what === "disconnect")
                        root.connections.tree.disconnectConnection(id);
                    else
                        root.deleteRequested(id);
                });
        }

        QMenuItem {
            text: qsTr("Edit")
            onTriggered: actionMenu.action = "edit"
        }

        QMenuItem {
            text: qsTr("Duplicate")
            onTriggered: actionMenu.action = "duplicate"
        }

        QMenuItem {
            text: qsTr("Disconnect")
            enabled: root.connections.tree.connectedIds.includes(actionMenu.targetId)
            onTriggered: actionMenu.action = "disconnect"
        }

        QMenuItem {
            text: qsTr("Delete")
            onTriggered: actionMenu.action = "delete"
        }
    }
}
