pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// A data tab (H2): the rows of a table opened from the tree, an edit toolbar and the grid. The
// toolbar buttons are icon-only (name, tooltip and shortcut come from the same text) and run the
// same Commands as the shortcuts and the palette (MainWindow). Read-only tabs (views, tables
// without a usable key) say why above the grid. Pending changes are shown in the grid (QuarryGrid)
// and counted in the toolbar: icon and text, never colour alone. The row count, "Fetch all" and the
// total live in the toolbar too: the grid of a data tab has no footer button.
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

    readonly property bool isEditor: false
    readonly property var edits: queryState ? queryState.edits : null
    readonly property var results: queryState ? queryState.results : null
    readonly property alias grid: tableGrid
    // What the editor region focuses (the same name the SQL editor uses).
    readonly property Item edit: tableGrid.body
    readonly property bool editing: tableGrid.editing
    readonly property bool saving: edits?.saving ?? false
    readonly property alias toolbar: toolbar
    readonly property alias exportButton: exportBtn
    readonly property alias fetchButton: fetchBtn
    // Left to right, as the toolbar shows them.
    readonly property var buttons: [refreshBtn, applyBtn, discardBtn, insertBtn, duplicateBtn, deleteBtn, exportBtn]
    readonly property alias viewBar: viewBar
    readonly property alias filterField: filterField
    readonly property alias clearFilterButton: clearFilterBtn
    readonly property alias rowsLabel: rowsText
    readonly property alias totalLabel: totalText
    property string lastNotice: ""
    // Why the condition in the filter box was refused or failed on the server ("" = none).
    property string filterError: ""

    // The visible columns of the grid, refreshed when the result changes.
    property var columnNames: []

    readonly property int loadedRows: results?.rows ?? 0
    // The total is not known: more rows can be fetched, are being fetched, or reading was cut.
    readonly property bool totalUnknown: results !== null && (results.moreAvailable || results.fetchingAll || results.truncated)
    readonly property string rowsSummary: loadedRows === 1 ? qsTr("1 row") : qsTr("%1 rows").arg(root.format(loadedRows))
    readonly property string totalSummary: totalUnknown ? qsTr("%1+").arg(root.format(loadedRows)) : root.format(loadedRows)

    function format(n) {
        return n.toLocaleString(Qt.locale(), "f", 0);
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

    // The order of the combos and the SQL condition `where`, sent to the server. The column filters
    // the controller holds (the foreign-key navigation will set them) are passed through.
    function applyView(where) {
        if (!queries || tabId === "")
            return false;
        const current = queries.dataViewOf(tabId);
        const view = {
            sortColumn: "",
            descending: false,
            where: where,
            filters: current.filters ?? []
        };
        if (sortCombo.currentIndex > 0) {
            view.sortColumn = columnNames[sortCombo.currentIndex - 1];
            view.descending = directionCombo.currentIndex === 1;
        }
        filterError = "";
        return queries.setDataView(tabId, view);
    }

    // The condition that is applied now (not what is typed).
    function appliedWhere() {
        return queries && tabId !== "" ? (queries.dataViewOf(tabId).where ?? "") : "";
    }

    function changeSort() {
        if (!applyView(appliedWhere()))
            syncSort();
    }

    // The combos show the order the controller holds (after a refused change).
    function syncSort() {
        const view = queries && tabId !== "" ? queries.dataViewOf(tabId) : ({});
        const at = columnNames.indexOf(view.sortColumn ?? "");
        sortCombo.currentIndex = (view.sortColumn ?? "") !== "" && at >= 0 ? at + 1 : 0;
        directionCombo.currentIndex = view.descending ? 1 : 0;
    }

    function clearFilter() {
        filterField.text = "";
        applyView("");
        filterField.forceActiveFocus(Qt.TabFocusReason);
    }

    Connections {
        target: root.queryState

        function onChanged() {
            root.refreshColumns();
            if (root.queryState.state === "done")
                root.filterError = "";
        }
    }

    Connections {
        target: root.queries

        function onViewFailed(tabId, message) {
            if (tabId === root.tabId)
                root.filterError = message;
        }
    }

    Component.onCompleted: refreshColumns()

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

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Sort and filter, run on the server (H11). The filter is one SQL condition, as in DBeaver:
        // Enter applies it. The order applies when a combo changes.
        Flow {
            id: viewBar

            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing
            Layout.rightMargin: Theme.spacing
            Layout.bottomMargin: filterErrorRow.visible ? 0 : Theme.spacing
            visible: root.queries !== null && root.tabId !== ""
            spacing: Theme.spacing

            Accessible.role: Accessible.ToolBar
            Accessible.name: qsTr("Sort and filter")

            QComboBox {
                id: sortCombo

                accessibleName: qsTr("Sort by")
                model: [qsTr("No order")].concat(root.columnNames)
                enabled: !root.saving
                onActivated: root.changeSort()
            }

            QComboBox {
                id: directionCombo

                accessibleName: qsTr("Sort direction")
                enabled: sortCombo.currentIndex > 0 && !root.saving
                model: [qsTr("Ascending"), qsTr("Descending")]
                onActivated: root.changeSort()
            }

            Row {
                spacing: Theme.spacing

                QTextField {
                    id: filterField

                    width: Math.max(Theme.controlHeight * 6, Math.min(Theme.spacing * 80, viewBar.width - (clearFilterBtn.visible ? clearFilterBtn.width + Theme.spacing : 0)))
                    accessibleName: qsTr("Filter condition")
                    placeholderText: qsTr("Filter: a SQL condition, e.g. age > 30 AND city = 'Madrid'")
                    enabled: !root.saving
                    Accessible.description: root.filterError !== "" ? qsTr("Filter error: %1").arg(root.filterError) : ""
                    onAccepted: root.applyView(text)
                }

                IconButton {
                    id: clearFilterBtn

                    visible: filterField.text !== ""
                    accessibleName: qsTr("Clear filter")
                    iconGlyph: Icons.x
                    enabled: !root.saving
                    onClicked: root.clearFilter()
                }
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

        QuarryGrid {
            id: tableGrid

            Layout.fillWidth: true
            Layout.fillHeight: true
            focus: true
            results: root.queryState.results
            edits: root.edits
            running: root.queryState.running
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
                enabled: (root.edits?.pendingCount ?? 0) > 0 && !root.saving
                onClicked: root.run("data.apply")
            }

            IconButton {
                id: discardBtn

                accessibleName: qsTr("Cancel changes")
                shortcutText: root.shortcutOf("data.discard")
                iconGlyph: Icons.x
                enabled: (root.edits?.pendingCount ?? 0) > 0 && !root.saving
                onClicked: root.run("data.discard")
            }

            IconButton {
                id: insertBtn

                accessibleName: qsTr("Insert row")
                shortcutText: root.shortcutOf("data.insertRow")
                iconGlyph: Icons.plus
                enabled: (root.edits?.editable ?? false) && !root.saving
                onClicked: root.run("data.insertRow")
            }

            IconButton {
                id: duplicateBtn

                accessibleName: qsTr("Duplicate row")
                shortcutText: root.shortcutOf("data.duplicateRow")
                iconGlyph: Icons.copy
                enabled: (root.edits?.editable ?? false) && !root.saving && tableGrid.rowCount > 0
                onClicked: root.run("data.duplicateRow")
            }

            IconButton {
                id: deleteBtn

                accessibleName: qsTr("Delete rows")
                shortcutText: root.shortcutOf("data.deleteRows")
                iconGlyph: Icons.trash2
                enabled: (root.edits?.editable ?? false) && !root.saving
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

            // The rows read so far, "Fetch all" and the total: "200+" until the end is known.
            Text {
                id: rowsText

                height: insertBtn.height
                verticalAlignment: Text.AlignVCenter
                text: root.rowsSummary
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            QButton {
                id: fetchBtn

                visible: (root.results?.moreAvailable ?? false) || (root.results?.fetchingAll ?? false)
                text: root.results?.fetchingAll ? qsTr("Stop fetching") : qsTr("Fetch all")
                enabled: !root.saving
                onClicked: {
                    if (root.results.fetchingAll)
                        root.results.cancelFetchAll();
                    else
                        root.results.fetchAll();
                }
            }

            Text {
                id: totalText

                height: insertBtn.height
                verticalAlignment: Text.AlignVCenter
                // While the first page is still arriving nothing is known yet.
                visible: root.results !== null && root.results.columns > 0 && (!(root.queryState?.running ?? false) || root.totalUnknown)
                text: root.totalSummary
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault

                Accessible.role: Accessible.StaticText
                Accessible.name: root.totalUnknown ? qsTr("At least %1 rows in total").arg(root.format(root.loadedRows)) : qsTr("Total: %1 rows").arg(root.format(root.loadedRows))
            }
        }
    }
}
