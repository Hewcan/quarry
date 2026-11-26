pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// A data tab (H2): the rows of a table opened from the tree, an edit toolbar and the grid. The
// toolbar buttons are icon-only (name, tooltip and shortcut come from the same text) and run the
// same Commands as the shortcuts and the palette (MainWindow). Read-only tabs (views, tables
// without a usable key) say why above the grid. Pending changes are shown in the grid (QuarryGrid)
// and counted in the toolbar: icon and text, never colour alone. The rows group lives in the toolbar
// too (rows to load when the table opens, "Count rows" and the rows loaded or counted): the grid
// of a data tab has no footer button.
//
// H11 phases 2 to 4: the header of the grid sorts (click, or Enter on the header, or the command
// "Sort by current column"); a record panel to the right of the grid (closed by default) shows the
// fields of the current row and the viewers of long values; a foreign key cell can go to the related
// row (the table it points to, filtered by the key), and the band "Filtered from ..." says why a
// table opened with a filter; "Structure" opens the structure tab of the table.
FocusScope {
    id: root

    required property var tabs // EditorTabsModel
    required property int row
    property string tabTitle: ""
    property string connectionId: ""
    property string connectionName: ""
    property bool production: false
    // The QueryState of the tab.
    property var queryState: null
    // ExportController (null in tests that do not export).
    property var exporter: null
    // QueryController and the tab id: sort and filter on the server (H11). Without them the
    // view bar is hidden.
    property var queries: null
    property string tabId: ""
    // StructureController: the foreign keys of the table and "Go to related row" (null in tests
    // that do not need them).
    property var structure: null

    readonly property bool isEditor: false
    readonly property var edits: queryState ? queryState.edits : null
    readonly property var results: queryState ? queryState.results : null
    readonly property alias grid: tableGrid
    // Which view is on show: 0 = Properties, 1 = Data (the default: opening a table shows its rows).
    property int page: 1
    property bool propertiesMade: false
    readonly property alias pageBar: pageBar
    readonly property var propertiesView: propertiesLoader.item
    onPageChanged: {
        if (page === 0)
            propertiesMade = true;
        if (pageBar.currentIndex !== page)
            pageBar.currentIndex = page;
    }
    // What the editor region focuses (the same name the SQL editor uses).
    readonly property Item edit: page === 0 && propertiesView ? propertiesView.edit : tableGrid.body
    readonly property bool editing: tableGrid.editing
    readonly property bool saving: edits?.saving ?? false
    // The table is being read again and the old rows are still on show: they cannot be changed.
    readonly property bool reloading: edits?.reloading ?? false
    readonly property string reloadReason: qsTr("The table is being read again.")
    readonly property alias toolbar: toolbar
    readonly property alias exportButton: exportBtn
    // Left to right, as the toolbar shows them.
    readonly property var buttons: [refreshBtn, applyBtn, discardBtn, insertBtn, duplicateBtn, deleteBtn, exportBtn, recordBtn, structureBtn]
    readonly property alias viewBar: viewBar
    readonly property alias showSqlButton: showSqlBtn
    readonly property alias showSqlDialog: showSqlDialog
    readonly property alias filterField: filterField
    readonly property alias clearFilterButton: clearFilterBtn
    readonly property alias backButton: backBtn
    readonly property alias forwardButton: forwardBtn
    readonly property alias columnMenu: columnMenu
    // The rows group of the toolbar: rows to load, count button, loaded / total text.
    readonly property alias rowsField: rowsField
    readonly property alias countButton: countBtn
    readonly property alias countLabel: countText
    readonly property alias countErrorLabel: countErrorText
    property string lastNotice: ""
    // The record panel (H11 phase 3): open or not (it starts closed, per tab); made the first time it
    // opens and kept.
    property bool recordOpen: false
    property bool recordMade: false
    readonly property var recordPanel: recordLoader.item
    readonly property alias recordButton: recordBtn
    readonly property alias structureButton: structureBtn
    readonly property alias recordSplit: recordSplit
    readonly property alias originRow: originRow
    readonly property alias originLabel: originText
    readonly property alias originClearButton: originClearBtn
    readonly property int defaultRecordWidth: Theme.controlHeight * 12
    // The order and filters the controller holds, and the foreign keys: refreshed when they change.
    property var currentView: ({})
    property bool canGoBack: false
    property bool canGoForward: false
    property bool hasActiveView: false
    property var linkColumns: ({})
    // "Filtered from orders.customer_id = 5" when this table was opened from a foreign key.
    readonly property string filterOrigin: queryState?.filterOrigin ?? ""
    // The column "Go to related row" and "Sort by current column" act on: the field the record panel
    // is on when it has the focus, else the column of the current cell.
    readonly property bool recordFocused: recordPanel !== null && recordOpen && recordPanel.activeFocus
    readonly property int actionColumn: recordFocused && recordPanel.currentColumn >= 0 ? recordPanel.currentColumn : tableGrid.currentColumn
    readonly property bool canGoToRelated: structure !== null && tableGrid.rowCount > 0 && results !== null && linkColumns[results.columnName(actionColumn)] !== undefined
    readonly property bool canSortByColumn: queries !== null && tabId !== "" && tableGrid.rowCount > 0 && loadedColumns > 0 && !saving
    // Why the condition in the filter box was refused or failed on the server ("" = none).
    property string filterError: ""
    // Why "Go to related row" opened nothing, shown in the record panel ("" = nothing wrong).
    property string relatedError: ""

    // The visible columns of the grid, refreshed when the result changes.
    property var columnNames: []

    readonly property int loadedRows: results?.rows ?? 0
    readonly property int loadedColumns: results?.columns ?? 0
    // The total is not known: more rows can be fetched, are being fetched, or reading was cut.
    readonly property bool totalUnknown: results !== null && (results.moreAvailable || results.fetchingAll || results.truncated)
    // "Count rows": a COUNT(*) of the view is running / its exact total (-1: not counted) / why it failed.
    readonly property bool counting: queryState?.counting ?? false
    readonly property real countedTotal: queryState?.countedTotal ?? -1
    readonly property string countError: queryState?.countError ?? ""
    // What the toolbar says about the rows: the exact total once counted, else the rows loaded
    // ("200+" while the server has more).
    readonly property string countSummary: counting ? qsTr("Counting…") : countedTotal >= 0 ? root.format(countedTotal) : totalUnknown ? qsTr("%1+").arg(root.format(loadedRows)) : root.format(loadedRows)
    readonly property string countDescription: counting ? qsTr("Counting the rows…") : countedTotal >= 0 ? root.totalText(countedTotal) : totalUnknown ? qsTr("%1 rows loaded, more available").arg(root.format(loadedRows)) : loadedRows === 1 ? qsTr("1 row loaded") : qsTr("%1 rows loaded").arg(root.format(loadedRows))

    function format(n) {
        return n.toLocaleString(Qt.locale(), "f", 0);
    }

    function totalText(n) {
        return n === 1 ? qsTr("Total: 1 row") : qsTr("Total: %1 rows").arg(format(n));
    }

    function syncRows() {
        rowsField.text = queries ? String(queries.dataPageRows) : "";
    }

    // Enter or leaving the field: a valid number is the new setting (brought into range) and the
    // table is read again with it, unless there are unsaved changes: then it applies to the next load.
    function commitRows() {
        if (!queries)
            return;
        const typed = parseInt(rowsField.text, 10);
        if (isNaN(typed)) {
            syncRows();
            return;
        }
        const rows = queries.clampDataPageRows(typed);
        rowsField.text = String(rows);
        if (rows === queries.dataPageRows)
            return;
        queries.dataPageRows = rows;
        if ((edits?.pendingCount ?? 0) > 0) {
            tableGrid.announce(qsTr("%n row(s) will be loaded the next time the table is read: there are unsaved changes.", "", rows), false);
        } else if (tabId !== "") {
            tableGrid.announce(qsTr("Loading %n row(s)", "", rows), false);
            queries.reload(tabId);
        }
    }

    function toggleCount() {
        if (!queries || tabId === "")
            return;
        if (counting)
            queries.cancelCount(tabId);
        else
            queries.countRows(tabId);
    }

    function refreshColumns() {
        const results = queryState?.results;
        const names = [];
        if (results)
            for (let i = 0; i < results.columnCount(); ++i)
                names.push(results.columnName(i));
        // While a new view loads the grid has no columns yet: keep the old names, or the
        // combo would fall back to "No order".
        if (names.length > 0 && JSON.stringify(names) !== JSON.stringify(columnNames))
            columnNames = names;
    }

    // Applies the SQL condition `where`, keeping the active multi-column sort and (unless
    // `keepFilters` is false) the column filters.
    function applyView(where, keepFilters) {
        if (!queries || tabId === "")
            return false;
        const current = queries.dataViewOf(tabId);
        const view = {
            sorts: current.sorts ?? [],
            where: where,
            filters: keepFilters === false ? [] : (current.filters ?? [])
        };
        filterError = "";
        const ok = queries.setDataView(tabId, view);
        refreshView();
        return ok;
    }

    // The condition that is applied now (not what is typed).
    function appliedWhere() {
        return queries && tabId !== "" ? (queries.dataViewOf(tabId).where ?? "") : "";
    }

    function refreshView() {
        currentView = queries && tabId !== "" ? queries.dataViewOf(tabId) : ({});
        canGoBack = queries !== null && tabId !== "" && queries.canGoBack(tabId);
        canGoForward = queries !== null && tabId !== "" && queries.canGoForward(tabId);
        hasActiveView = queries !== null && tabId !== "" && queries.hasDataView(tabId);
    }

    function refreshLinks() {
        linkColumns = structure && tabId !== "" ? structure.linkColumns(tabId) : ({});
    }

    // "Clear filters" clears every active filter, SQL condition, foreign-key origin and sort,
    // resetting the search completely.
    function clearFilter() {
        if (!queries || tabId === "")
            return;
        filterField.text = "";
        filterError = "";
        queries.setDataView(tabId, {});
        refreshView();
        filterField.forceActiveFocus(Qt.TabFocusReason);
    }

    // Opens the "Show SQL" dialog with the full SQL currently executing or last executed.
    function openShowSql() {
        if (!queries || tabId === "")
            return;
        const info = queries.currentSqlInfoOf(tabId);
        showSqlDialog.openWith(info.sql ?? "", info.parameters ?? []);
    }

    function goBack() {
        if (!queries || tabId === "")
            return false;
        filterError = "";
        const ok = queries.goBack(tabId);
        refreshView();
        if (ok)
            filterField.text = currentView.where ?? "";
        return ok;
    }

    function goForward() {
        if (!queries || tabId === "")
            return false;
        filterError = "";
        const ok = queries.goForward(tabId);
        refreshView();
        if (ok)
            filterField.text = currentView.where ?? "";
        return ok;
    }

    // The band's "Clear": only the foreign-key filter goes; the condition and the order stay.
    function clearOrigin() {
        applyView(appliedWhere(), false);
        tableGrid.body.forceActiveFocus(Qt.TabFocusReason);
    }

    // A header was clicked or Enter was pressed on it: the order of that column goes ascending ->
    // descending -> none on the server. Refused with pending changes (and said).
    function sortBy(name) {
        if (!queries || tabId === "" || name === "")
            return false;
        tableGrid.pendingColumn = columnNames.indexOf(name);
        const ok = queries.cycleSort(tabId, name);
        if (!ok)
            tableGrid.pendingColumn = -1;
        refreshView();
        return ok;
    }

    function previewValue(text) {
        if (text.length <= 24)
            return text;
        return text.slice(0, 23) + "…";
    }

    function openColumnMenu(columnIndex, anchorItem) {
        if (!queries || tabId === "" || !results || columnIndex < 0 || columnIndex >= results.columns)
            return;
        const colName = results.columnName(columnIndex);
        columnMenu.columnName = colName;
        columnMenu.hasColumnFilter = queries.hasColumnFilter(tabId, colName);
        if (tableGrid.rowCount > 0 && edits) {
            const r = tableGrid.currentRow;
            columnMenu.hasCell = true;
            columnMenu.cellIsNull = edits.isNullAt(r, columnIndex);
            columnMenu.cellValue = columnMenu.cellIsNull ? "" : edits.editText(r, columnIndex);
        } else {
            columnMenu.hasCell = false;
            columnMenu.cellIsNull = false;
            columnMenu.cellValue = "";
        }
        const clip = queries.clipboardText();
        columnMenu.clipValue = clip;
        columnMenu.hasClip = clip !== "";
        if (anchorItem)
            columnMenu.popup(anchorItem, 0, anchorItem.height);
        else
            columnMenu.popup();
    }

    // The command: the column of the current cell (or of the field the record panel is on).
    function sortCurrentColumn() {
        if (!canSortByColumn) {
            tableGrid.announce(qsTr("There is no column to sort by."), false);
            return false;
        }
        return sortBy(results.columnName(actionColumn));
    }

    // ---- record panel and foreign keys ----

    function toggleRecord() {
        recordOpen = !recordOpen;
        if (recordOpen) {
            recordMade = true;
            tableGrid.announce(qsTr("Record view opened"), false);
            Qt.callLater(() => {
                if (root.recordPanel)
                    root.recordPanel.focusTarget.forceActiveFocus(Qt.TabFocusReason);
            });
        } else {
            tableGrid.announce(qsTr("Record view closed"), false);
            tableGrid.body.forceActiveFocus(Qt.TabFocusReason);
        }
    }

    function closeRecord() {
        if (recordOpen)
            toggleRecord();
    }

    function resetRecordWidth() {
        recordLoader.T.SplitView.preferredWidth = defaultRecordWidth;
    }

    // "Go to related row" for the cell (or the field) the user is on.
    function goToRelated() {
        if (!structure || !canGoToRelated) {
            tableGrid.announce(qsTr("The current column is not a foreign key."), false);
            return;
        }
        relatedError = "";
        const r = structure.goToRelated(tabId, tableGrid.currentRow, results.columnName(actionColumn));
        if (!r.started)
            relatedError = r.message;
    }

    Connections {
        target: root.queryState

        function onChanged() {
            root.refreshColumns();
            root.refreshView();
            if (root.queryState.state === "done")
                root.filterError = "";
        }
    }

    // An old "Go to related row" error is about another row.
    Connections {
        target: tableGrid

        function onCurrentRowChanged() {
            root.relatedError = "";
        }
    }

    // The foreign keys of the table (read once, when the tab opens) and the end of "Go to related row".
    Connections {
        target: root.structure

        function onChanged(id) {
            if (id === root.tabId)
                root.refreshLinks();
        }

        function onRelatedFailed(id, message) {
            if (id === root.tabId)
                root.relatedError = message;
        }
    }

    Connections {
        target: root.queries

        function onViewFailed(tabId, message) {
            if (tabId === root.tabId)
                root.filterError = message;
        }

        function onViewHistoryChanged(tabId) {
            if (tabId === root.tabId)
                root.refreshView();
        }

        // Another data tab changed the setting.
        function onDataPageRowsChanged() {
            if (!rowsField.activeFocus)
                root.syncRows();
        }
    }

    // The exact total, said once when the count ends (a failure is announced by the controller).
    Connections {
        target: root.queryState

        function onCountChanged() {
            if (!root.counting && root.countedTotal >= 0)
                tableGrid.announce(root.totalText(root.countedTotal), false);
        }
    }

    Component.onCompleted: {
        refreshColumns();
        syncRows();
        refreshView();
        if (structure && tabId !== "") {
            structure.load(tabId);
            refreshLinks();
        }
    }

    function shortcutOf(id) {
        return CommandRegistry.find(id)?.shortcutText ?? "";
    }

    function run(id) {
        CommandRegistry.trigger(id);
    }

    // The read-only reason and the key warning are said once per load, without moving the focus.
    function announceTarget() {
        if (!edits || !edits.active)
            return;
        const text = edits.editable ? edits.keyWarning : edits.readOnlyReason;
        if (text !== "" && text !== lastNotice)
            tableGrid.announce(text, false);
        lastNotice = text;
    }

    Connections {
        target: root.edits

        function onTargetChanged() {
            root.announceTarget();
        }
    }

    // Properties | Data, above everything else. The properties are made the first time they are
    // shown (nothing is read for them before) and kept.
    QTabBar {
        id: pageBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        accessibleName: qsTr("Table views")
        currentIndex: 1
        onCurrentIndexChanged: root.page = currentIndex

        QTabButton {
            text: qsTr("Properties")
        }

        QTabButton {
            text: qsTr("Data")
        }
    }

    Loader {
        id: propertiesLoader

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: pageBar.bottom
        anchors.bottom: parent.bottom
        visible: root.page === 0
        active: root.propertiesMade
        sourceComponent: TablePropertiesView {
            tabId: root.tabId
            tabTitle: root.tabTitle
            structure: root.structure
        }
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: pageBar.bottom
        anchors.bottom: parent.bottom
        visible: root.page === 1
        spacing: 0

        // DBeaver-style filter and navigation toolbar: Show SQL, expanded SQL filter field,
        // Clear filters, and Back / Forward history buttons.
        RowLayout {
            id: viewBar

            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing
            Layout.rightMargin: Theme.spacing
            Layout.topMargin: Theme.spacing
            Layout.bottomMargin: filterErrorRow.visible ? 0 : Theme.spacing
            visible: root.queries !== null && root.tabId !== ""
            spacing: Theme.spacing

            Accessible.role: Accessible.ToolBar
            Accessible.name: qsTr("Filter and navigation")

            IconButton {
                id: showSqlBtn

                accessibleName: qsTr("Show SQL")
                iconGlyph: Icons.fileCode
                enabled: root.queries !== null && root.tabId !== ""
                Accessible.description: qsTr("Shows the full SQL statement currently executing")
                onClicked: root.openShowSql()
            }

            QTextField {
                id: filterField

                Layout.fillWidth: true
                Layout.minimumWidth: Theme.controlHeight * 10
                accessibleName: qsTr("Filter condition")
                placeholderText: qsTr("Filter: a SQL condition, e.g. age > 30 AND city = 'Madrid'")
                enabled: !root.saving
                Accessible.description: root.filterError !== "" ? qsTr("Filter error: %1").arg(root.filterError) : ""
                onAccepted: root.applyView(text)
            }

            IconButton {
                id: clearFilterBtn

                accessibleName: qsTr("Clear filters")
                iconGlyph: Icons.filterX
                enabled: !root.saving && (filterField.text !== "" || root.hasActiveView || root.filterOrigin !== "")
                Accessible.description: qsTr("Clears all active filters and resets the search")
                onClicked: root.clearFilter()
            }

            IconButton {
                id: backBtn

                accessibleName: qsTr("Back")
                iconGlyph: Icons.arrowLeft
                enabled: !root.saving && root.canGoBack
                Accessible.description: qsTr("Go back to the previous search")
                onClicked: root.goBack()
            }

            IconButton {
                id: forwardBtn

                accessibleName: qsTr("Forward")
                iconGlyph: Icons.arrowRight
                enabled: !root.saving && root.canGoForward
                Accessible.description: qsTr("Go forward to a more recent search")
                onClicked: root.goForward()
            }
        }

        // The refused or failed condition, in words with an icon: never the colour alone.
        RowLayout {
            id: filterErrorRow

            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing * 2
            Layout.rightMargin: Theme.spacing * 2
            Layout.bottomMargin: Theme.spacing
            visible: viewBar.visible && root.filterError !== ""
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: Icons.circleAlert
                color: Theme.danger
            }

            Text {
                id: filterErrorText

                Layout.fillWidth: true
                text: qsTr("Filter error: %1").arg(root.filterError)
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }

        // This table was opened from a foreign key: where the filter comes from, with a way to drop it
        // (text and icon, never colour alone).
        RowLayout {
            id: originRow

            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing * 2
            Layout.rightMargin: Theme.spacing * 2
            Layout.bottomMargin: Theme.spacing
            visible: root.filterOrigin !== ""
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignVCenter
                text: Icons.link
            }

            Text {
                id: originText

                Layout.fillWidth: true
                text: root.filterOrigin
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            QButton {
                id: originClearBtn

                text: qsTr("Clear")
                iconGlyph: Icons.x
                enabled: !root.saving
                Accessible.description: qsTr("Removes the filter that came from the foreign key")
                onClicked: root.clearOrigin()
            }
        }

        // Why the tab cannot be edited, or the warning of a table that has no primary key.
        RowLayout {
            id: notice

            readonly property bool readOnly: root.edits !== null && root.edits.active && !root.edits.editable && root.edits.readOnlyReason !== ""
            readonly property bool warning: root.edits !== null && root.edits.editable && root.edits.keyWarning !== ""

            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing * 2
            Layout.rightMargin: Theme.spacing * 2
            Layout.bottomMargin: Theme.spacing
            visible: readOnly || warning
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: notice.readOnly ? Icons.lock : Icons.triangleAlert
                color: notice.readOnly ? Theme.fgDefault : Theme.warning
            }

            Text {
                id: noticeText

                Layout.fillWidth: true
                text: notice.readOnly ? root.edits.readOnlyReason : (root.edits?.keyWarning ?? "")
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }

        // A condition the server rejected is said under the filter box, not here too.
        Loader {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing * 2
            Layout.rightMargin: Theme.spacing * 2
            Layout.bottomMargin: Theme.spacing
            active: (root.edits?.lastError ?? "") !== "" && root.edits.lastError !== root.filterError
            visible: active
            sourceComponent: ErrorPanel {
                title: qsTr("The table could not be read or saved")
                message: root.edits.lastError
            }
        }

        // The grid, with the record panel to its right (closed until asked for).
        QSplitView {
            id: recordSplit

            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal
            accessibleName: qsTr("Resize record view")
            onResized: recordWidthSave.restart()

            Item {
                T.SplitView.fillWidth: true
                T.SplitView.minimumWidth: Theme.controlHeight * 6

                QuarryGrid {
                    id: tableGrid

                    anchors.fill: parent
                    focus: true
                    results: root.queryState.results
                    edits: root.edits
                    running: root.queryState.running
                    sortable: root.queries !== null && root.tabId !== ""
                    transposable: true
                    sortColumnName: root.currentView.sortColumn ?? ""
                    sortDescending: root.currentView.descending ?? false
                    sortKeys: root.currentView.sorts ?? []
                    columnFilters: root.currentView.filters ?? []
                    linkColumns: root.linkColumns
                    onSortRequested: column => root.sortBy(column)
                    onColumnMenuRequested: (columnIndex, anchorItem) => root.openColumnMenu(columnIndex, anchorItem)
                }
            }

            // Made the first time the panel opens, and kept.
            Loader {
                id: recordLoader

                active: root.recordMade
                visible: root.recordOpen
                T.SplitView.preferredWidth: Number(LayoutStore.value("recordPanelWidth", root.defaultRecordWidth))
                T.SplitView.minimumWidth: Theme.controlHeight * 8
                T.SplitView.maximumWidth: Math.max(T.SplitView.minimumWidth, root.width * 0.7)

                sourceComponent: RecordPanel {
                    edits: root.edits
                    row: tableGrid.rowCount > 0 ? tableGrid.currentRow : -1
                    tabId: root.tabId
                    linkColumns: root.linkColumns
                    relatedError: root.relatedError
                    onCloseRequested: root.closeRecord()
                    onFocusGridRequested: tableGrid.body.forceActiveFocus(Qt.TabFocusReason)
                    onGoToRelatedRequested: column => root.goToRelated()
                }
            }
        }

        // Waits for the layout to settle after a resize, then stores the width of the panel.
        Timer {
            id: recordWidthSave

            interval: 300
            onTriggered: {
                if (recordLoader.visible && recordLoader.width > 0)
                    LayoutStore.setValue("recordPanelWidth", Math.round(recordLoader.width));
            }
        }

        // The editing toolbar sits under the table, like the buttons under a DBeaver grid.
        Flow {
            id: toolbar

            Layout.fillWidth: true
            Layout.margins: Theme.spacing
            spacing: Theme.spacing

            Accessible.role: Accessible.ToolBar
            Accessible.name: qsTr("Table editing toolbar")

            IconButton {
                id: refreshBtn

                accessibleName: qsTr("Refresh")
                shortcutText: root.shortcutOf("data.refresh")
                iconGlyph: Icons.refreshCw
                enabled: !(root.queryState?.running ?? false) && !root.saving
                onClicked: root.run("data.refresh")
            }

            IconButton {
                id: applyBtn

                accessibleName: qsTr("Save changes")
                shortcutText: root.shortcutOf("data.apply")
                iconGlyph: Icons.save
                variant: "primary"
                enabled: (root.edits?.pendingCount ?? 0) > 0 && !root.saving && !root.reloading
                Accessible.description: root.reloading ? root.reloadReason : shortcutText
                onClicked: root.run("data.apply")
            }

            IconButton {
                id: discardBtn

                accessibleName: qsTr("Cancel changes")
                shortcutText: root.shortcutOf("data.discard")
                iconGlyph: Icons.x
                enabled: (root.edits?.pendingCount ?? 0) > 0 && !root.saving && !root.reloading
                Accessible.description: root.reloading ? root.reloadReason : shortcutText
                onClicked: root.run("data.discard")
            }

            IconButton {
                id: insertBtn

                accessibleName: qsTr("Insert row")
                shortcutText: root.shortcutOf("data.insertRow")
                iconGlyph: Icons.plus
                enabled: (root.edits?.editable ?? false) && !root.saving && !root.reloading
                Accessible.description: root.reloading ? root.reloadReason : shortcutText
                onClicked: root.run("data.insertRow")
            }

            IconButton {
                id: duplicateBtn

                accessibleName: qsTr("Duplicate row")
                shortcutText: root.shortcutOf("data.duplicateRow")
                iconGlyph: Icons.copy
                enabled: (root.edits?.editable ?? false) && !root.saving && !root.reloading && tableGrid.rowCount > 0
                Accessible.description: root.reloading ? root.reloadReason : shortcutText
                onClicked: root.run("data.duplicateRow")
            }

            IconButton {
                id: deleteBtn

                accessibleName: qsTr("Delete rows")
                shortcutText: root.shortcutOf("data.deleteRows")
                iconGlyph: Icons.trash2
                enabled: (root.edits?.editable ?? false) && !root.saving && !root.reloading
                Accessible.description: root.reloading ? root.reloadReason : shortcutText
                onClicked: root.run("data.deleteRows")
            }

            // The hover goes to the holder: a disabled button gets none, and the reason shows there.
            Item {
                width: exportBtn.width
                height: exportBtn.height

                IconButton {
                    id: exportBtn

                    accessibleName: qsTr("Export…")
                    shortcutText: enabled ? root.shortcutOf("export.open") : ""
                    iconGlyph: Icons.download
                    enabled: (root.exporter?.info.available ?? false) && !(root.exporter?.busy ?? false) && !root.saving
                    // A disabled button cannot take the focus, so the reason is also its description.
                    Accessible.description: enabled ? shortcutText : (root.exporter?.info.reason ?? "")
                    onClicked: root.run("export.open")
                }

                QToolTip {
                    visible: !exportBtn.enabled && exportHover.hovered && text !== ""
                    delay: 500
                    text: root.exporter?.info.reason ?? ""
                }

                HoverHandler {
                    id: exportHover
                }
            }

            IconButton {
                id: recordBtn

                accessibleName: qsTr("Record view")
                shortcutText: root.shortcutOf("data.toggleRecord")
                iconGlyph: Icons.panelRight
                Accessible.checkable: true
                Accessible.checked: root.recordOpen
                Accessible.description: qsTr("Shows the fields of the current row beside the table. %1").arg(shortcutText)
                onClicked: root.run("data.toggleRecord")
            }

            IconButton {
                id: structureBtn

                accessibleName: qsTr("Structure")
                shortcutText: root.shortcutOf("table.showStructure")
                iconGlyph: Icons.tableProperties
                Accessible.description: qsTr("Shows the columns, indexes, foreign keys and DDL of this table. %1").arg(shortcutText)
                onClicked: root.run("table.showStructure")
            }

            Row {
                height: insertBtn.height
                spacing: Theme.spacing

                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.saving || (root.edits?.pendingCount ?? 0) > 0
                    text: root.saving ? Icons.loaderCircle : Icons.pencil
                }

                Text {
                    id: pending

                    anchors.verticalCenter: parent.verticalCenter
                    text: root.saving ? qsTr("Saving…") : (root.edits?.pendingText ?? "")
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            // The rows: how many are loaded when a table opens (a setting), the count button and
            // what is loaded now ("200+" until the end is known, the exact total once counted).
            // One Row, so the three stay together when the toolbar wraps.
            Row {
                id: rowsGroup

                height: insertBtn.height
                spacing: Theme.spacing

                QTextField {
                    id: rowsField

                    anchors.verticalCenter: parent.verticalCenter
                    width: digits.advanceWidth + leftPadding + rightPadding
                    horizontalAlignment: TextInput.AlignRight
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: RegularExpressionValidator {
                        regularExpression: /[0-9]{0,5}/
                    }
                    accessibleName: qsTr("Rows to load")
                    Accessible.description: tipText
                    enabled: root.queries !== null && !root.saving
                    onEditingFinished: root.commitRows()

                    readonly property string tipText: qsTr("Number of rows to load when a table is opened")

                    // Room for the widest value (10 000).
                    TextMetrics {
                        id: digits

                        font: rowsField.font
                        text: "00000"
                    }

                    HoverHandler {
                        id: rowsHover
                    }

                    QToolTip {
                        visible: (rowsHover.hovered || rowsField.activeFocus) && rowsField.enabled
                        delay: 500
                        text: rowsField.tipText
                    }
                }

                IconButton {
                    id: countBtn

                    anchors.verticalCenter: parent.verticalCenter
                    accessibleName: root.counting ? qsTr("Stop counting") : qsTr("Count rows")
                    iconGlyph: root.counting ? Icons.square : Icons.unfoldVertical
                    enabled: root.queries !== null && root.tabId !== "" && !root.saving && (root.counting || (root.loadedColumns > 0 && !(root.queryState?.running ?? false)))
                    Accessible.description: root.counting ? qsTr("Stop counting the rows of the table") : qsTr("Count the rows of the table")
                    onClicked: root.toggleCount()

                    Binding {
                        target: countBtn.tooltip
                        property: "text"
                        value: root.counting ? qsTr("Stop counting the rows of the table") : qsTr("Count the rows of the table")
                    }
                }

                Text {
                    id: countText

                    anchors.verticalCenter: parent.verticalCenter
                    // While the first page is still arriving nothing is known yet.
                    visible: root.counting || (root.loadedColumns > 0 && (!(root.queryState?.running ?? false) || root.totalUnknown))
                    text: root.countSummary
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault

                    Accessible.role: Accessible.StaticText
                    Accessible.name: root.countDescription
                }
            }
        }

        // Why the rows could not be counted, in words with an icon (never the colour alone); the
        // same text is in Messages.
        RowLayout {
            id: countErrorRow

            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing * 2
            Layout.rightMargin: Theme.spacing * 2
            Layout.bottomMargin: Theme.spacing
            visible: root.countError !== ""
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: Icons.circleAlert
                color: Theme.danger
            }

            Text {
                id: countErrorText

                Layout.fillWidth: true
                text: qsTr("The rows could not be counted: %1").arg(root.countError)
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }
    }

    ShowSqlDialog {
        id: showSqlDialog

        copyAction: text => {
            if (root.queries) {
                root.queries.copyTextToClipboard(text);
                tableGrid.announce(qsTr("SQL copied to clipboard"), false);
            }
        }
    }

    QMenu {
        id: columnMenu

        property string columnName: ""
        property bool hasCell: false
        property bool cellIsNull: false
        property string cellValue: ""
        property bool hasClip: false
        property string clipValue: ""
        property bool hasColumnFilter: false

        title: columnName !== "" ? qsTr("Column %1").arg(columnName) : qsTr("Column options")

        QMenuItem {
            text: qsTr("Sort ascending (ASC)")
            iconGlyph: Icons.arrowUpNarrowWide
            enabled: !root.saving && columnMenu.columnName !== ""
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.setColumnSort(root.tabId, columnMenu.columnName, false);
            }
        }

        QMenuItem {
            text: qsTr("Sort descending (DESC)")
            iconGlyph: Icons.arrowDownWideNarrow
            enabled: !root.saving && columnMenu.columnName !== ""
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.setColumnSort(root.tabId, columnMenu.columnName, true);
            }
        }

        QMenuSeparator {}

        QMenuItem {
            text: qsTr("Cell value")
            enabled: false
        }

        QMenuItem {
            text: columnMenu.cellIsNull ? qsTr("= NULL") : columnMenu.hasCell ? qsTr("= '%1'").arg(root.previewValue(columnMenu.cellValue)) : qsTr("= (no cell)")
            iconGlyph: Icons.equal
            enabled: !root.saving && columnMenu.hasCell
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.addColumnFilter(root.tabId, columnMenu.columnName, columnMenu.cellIsNull ? "isNull" : "equals", columnMenu.cellIsNull ? "" : columnMenu.cellValue);
            }
        }

        QMenuItem {
            text: columnMenu.cellIsNull ? qsTr("<> NULL") : columnMenu.hasCell ? qsTr("<> '%1'").arg(root.previewValue(columnMenu.cellValue)) : qsTr("<> (no cell)")
            iconGlyph: Icons.equalNot
            enabled: !root.saving && columnMenu.hasCell
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.addColumnFilter(root.tabId, columnMenu.columnName, columnMenu.cellIsNull ? "isNotNull" : "notEquals", columnMenu.cellIsNull ? "" : columnMenu.cellValue);
            }
        }

        QMenuItem {
            text: !columnMenu.cellIsNull && columnMenu.hasCell ? qsTr("LIKE '%%1%'").arg(root.previewValue(columnMenu.cellValue)) : qsTr("LIKE '%…%'")
            iconGlyph: Icons.search
            enabled: !root.saving && columnMenu.hasCell && !columnMenu.cellIsNull
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.addColumnFilter(root.tabId, columnMenu.columnName, "contains", columnMenu.cellValue);
            }
        }

        QMenuSeparator {}

        QMenuItem {
            text: qsTr("Clipboard")
            iconGlyph: Icons.clipboard
            enabled: false
        }

        QMenuItem {
            text: columnMenu.hasClip ? qsTr("= '%1'").arg(root.previewValue(columnMenu.clipValue)) : qsTr("= (empty)")
            iconGlyph: Icons.equal
            enabled: !root.saving && columnMenu.hasClip
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.addColumnFilter(root.tabId, columnMenu.columnName, "equals", columnMenu.clipValue);
            }
        }

        QMenuItem {
            text: columnMenu.hasClip ? qsTr("<> '%1'").arg(root.previewValue(columnMenu.clipValue)) : qsTr("<> (empty)")
            iconGlyph: Icons.equalNot
            enabled: !root.saving && columnMenu.hasClip
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.addColumnFilter(root.tabId, columnMenu.columnName, "notEquals", columnMenu.clipValue);
            }
        }

        QMenuItem {
            text: columnMenu.hasClip ? qsTr("LIKE '%%1%'").arg(root.previewValue(columnMenu.clipValue)) : qsTr("LIKE '%…%'")
            iconGlyph: Icons.search
            enabled: !root.saving && columnMenu.hasClip
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.addColumnFilter(root.tabId, columnMenu.columnName, "contains", columnMenu.clipValue);
            }
        }

        QMenuSeparator {}

        QMenuItem {
            text: qsTr("Clear column")
            iconGlyph: Icons.filterX
            enabled: !root.saving && columnMenu.hasColumnFilter
            onTriggered: {
                if (root.queries && root.tabId !== "")
                    root.queries.clearColumnFilters(root.tabId, columnMenu.columnName);
            }
        }
    }
}
