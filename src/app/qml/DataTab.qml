pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// A data tab (H2): the rows of a table opened from the tree, an edit toolbar and the grid. The
// toolbar buttons run the same Commands as the shortcuts and the palette (MainWindow). Read-only
// tabs (views, tables without a usable key) say why above the grid. Pending changes are shown in
// the grid (QuarryGrid) and counted in the toolbar: icon and text, never colour alone.
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
    readonly property alias grid: tableGrid
    // What the editor region focuses (the same name the SQL editor uses).
    readonly property Item edit: tableGrid.body
    readonly property bool editing: tableGrid.editing
    readonly property bool saving: edits?.saving ?? false
    readonly property alias toolbar: toolbar
    readonly property alias exportButton: exportBtn
    readonly property var buttons: [insertBtn, deleteBtn, nullBtn, undoBtn, redoBtn, previewBtn, applyBtn, discardBtn, refreshBtn]
    readonly property alias viewBar: viewBar
    property string lastNotice: ""

    // The visible columns of the grid, refreshed when the result changes.
    property var columnNames: []
    readonly property var operators: queries ? queries.filterOperators() : []

    function refreshColumns() {
        const results = queryState?.results;
        const names = [];
        if (results)
            for (let i = 0; i < results.columnCount(); ++i)
                names.push(results.columnName(i));
        // While a new view loads the grid has no columns yet: keep the old names, or the
        // combos would fall back to "No order" / "No filter".
        if (names.length > 0 && JSON.stringify(names) !== JSON.stringify(columnNames))
            columnNames = names;
    }

    // Sends the bar's sort and filter to the server. `clear` drops both.
    function applyView(clear) {
        if (!queries || tabId === "")
            return false;
        const view = {
            sortColumn: "",
            descending: false,
            filters: []
        };
        if (!clear) {
            if (sortCombo.currentIndex > 0) {
                view.sortColumn = columnNames[sortCombo.currentIndex - 1];
                view.descending = directionCombo.currentIndex === 1;
            }
            if (filterColumnCombo.currentIndex > 0 && operatorCombo.currentIndex >= 0) {
                const op = operators[operatorCombo.currentIndex];
                view.filters.push({
                    column: columnNames[filterColumnCombo.currentIndex - 1],
                    op: op.op,
                    value: op.needsValue ? filterValue.text : ""
                });
            }
        }
        const ok = queries.setDataView(tabId, view);
        if (ok && clear) {
            sortCombo.currentIndex = 0;
            directionCombo.currentIndex = 0;
            filterColumnCombo.currentIndex = 0;
            operatorCombo.currentIndex = 0;
            filterValue.text = "";
        }
        return ok;
    }

    // "Sorted by a, descending. Filter: b contains x" ("" without a view): the bar's summary.
    function viewSummary() {
        const view = queries && tabId !== "" ? queries.dataViewOf(tabId) : ({});
        const parts = [];
        if ((view.sortColumn ?? "") !== "")
            parts.push(view.descending ? qsTr("Sorted by %1, descending").arg(view.sortColumn) : qsTr("Sorted by %1, ascending").arg(view.sortColumn));
        for (const f of (view.filters ?? [])) {
            const op = operators.find(o => o.op === f.op);
            const label = op ? op.label : f.op;
            parts.push(op && op.needsValue ? qsTr("Filter: %1 %2 \"%3\"").arg(f.column).arg(label).arg(f.value) : qsTr("Filter: %1 %2").arg(f.column).arg(label));
        }
        return parts.join(". ");
    }

    property string activeView: ""

    Connections {
        target: root.queryState

        function onChanged() {
            root.refreshColumns();
            root.activeView = root.viewSummary();
        }
    }

    Component.onCompleted: {
        refreshColumns();
        activeView = viewSummary();
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

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Flow {
            id: toolbar

            Layout.fillWidth: true
            Layout.margins: Theme.spacing
            spacing: Theme.spacing

            Accessible.role: Accessible.ToolBar
            Accessible.name: qsTr("Table editing toolbar")

            QButton {
                id: insertBtn

                text: qsTr("Insert row")
                iconGlyph: Icons.plus
                enabled: (root.edits?.editable ?? false) && !root.saving
                Accessible.description: root.shortcutOf("data.insertRow")
                onClicked: root.run("data.insertRow")
            }

            QButton {
                id: deleteBtn

                text: qsTr("Delete rows")
                iconGlyph: Icons.trash2
                enabled: (root.edits?.editable ?? false) && !root.saving
                Accessible.description: root.shortcutOf("data.deleteRows")
                onClicked: root.run("data.deleteRows")
            }

            QButton {
                id: nullBtn

                text: qsTr("Set to NULL")
                iconGlyph: Icons.minus
                enabled: (root.edits?.editable ?? false) && !root.saving
                Accessible.description: root.shortcutOf("data.setNull")
                onClicked: root.run("data.setNull")
            }

            QButton {
                id: undoBtn

                text: qsTr("Undo")
                iconGlyph: Icons.undo2
                enabled: (root.edits?.canUndo ?? false) && !root.saving
                Accessible.description: root.shortcutOf("data.undo")
                onClicked: root.run("data.undo")
            }

            QButton {
                id: redoBtn

                text: qsTr("Redo")
                iconGlyph: Icons.redo2
                enabled: (root.edits?.canRedo ?? false) && !root.saving
                Accessible.description: root.shortcutOf("data.redo")
                onClicked: root.run("data.redo")
            }

            QButton {
                id: previewBtn

                text: qsTr("Preview SQL")
                iconGlyph: Icons.fileDiff
                enabled: (root.edits?.pendingCount ?? 0) > 0 && !root.saving
                Accessible.description: root.shortcutOf("data.preview")
                onClicked: root.run("data.preview")
            }

            QButton {
                id: applyBtn

                text: qsTr("Save changes")
                iconGlyph: Icons.save
                variant: "primary"
                enabled: (root.edits?.pendingCount ?? 0) > 0 && !root.saving
                Accessible.description: root.shortcutOf("data.apply")
                onClicked: root.run("data.apply")
            }

            QButton {
                id: discardBtn

                text: qsTr("Discard changes")
                iconGlyph: Icons.x
                enabled: (root.edits?.pendingCount ?? 0) > 0 && !root.saving
                Accessible.description: root.shortcutOf("data.discard")
                onClicked: root.run("data.discard")
            }

            QButton {
                id: refreshBtn

                text: qsTr("Refresh")
                iconGlyph: Icons.rotateCcw
                enabled: !(root.queryState?.running ?? false) && !root.saving
                Accessible.description: root.shortcutOf("data.refresh")
                onClicked: root.run("data.refresh")
            }

            // The hover goes to the holder: a disabled button gets none, and the reason shows there.
            Item {
                width: exportBtn.width
                height: exportBtn.height

                QButton {
                    id: exportBtn

                    text: qsTr("Export…")
                    iconGlyph: Icons.download
                    enabled: (root.exporter?.info.available ?? false) && !(root.exporter?.busy ?? false) && !root.saving
                    // A disabled button cannot take the focus, so the reason is also its description.
                    Accessible.description: enabled ? root.shortcutOf("export.open") : (root.exporter?.info.reason ?? "")
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
        }

        // Sort and filter, run on the server (H11). Enter in the value field applies; the run is
        // cancelled like a query (Esc / Mod+.).
        Flow {
            id: viewBar

            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing
            Layout.rightMargin: Theme.spacing
            Layout.bottomMargin: Theme.spacing
            visible: root.queries !== null && root.tabId !== ""
            spacing: Theme.spacing

            Accessible.role: Accessible.ToolBar
            Accessible.name: qsTr("Sort and filter")

            QComboBox {
                id: sortCombo

                accessibleName: qsTr("Sort by")
                model: [qsTr("No order")].concat(root.columnNames)
            }

            QComboBox {
                id: directionCombo

                accessibleName: qsTr("Sort direction")
                enabled: sortCombo.currentIndex > 0
                model: [qsTr("Ascending"), qsTr("Descending")]
            }

            QComboBox {
                id: filterColumnCombo

                accessibleName: qsTr("Filter column")
                model: [qsTr("No filter")].concat(root.columnNames)
            }

            QComboBox {
                id: operatorCombo

                accessibleName: qsTr("Filter condition")
                enabled: filterColumnCombo.currentIndex > 0
                model: root.operators
                textRole: "label"
            }

            QTextField {
                id: filterValue

                readonly property bool needed: operatorCombo.currentIndex >= 0 && (root.operators[operatorCombo.currentIndex]?.needsValue ?? false)

                width: Theme.spacing * 24
                accessibleName: qsTr("Filter value")
                enabled: filterColumnCombo.currentIndex > 0 && needed
                onAccepted: root.applyView(false)
            }

            QButton {
                id: applyViewBtn

                text: qsTr("Apply")
                iconGlyph: Icons.search
                enabled: !root.saving
                onClicked: root.applyView(false)
            }

            QButton {
                id: clearViewBtn

                text: qsTr("Clear")
                iconGlyph: Icons.x
                enabled: !root.saving && root.activeView !== ""
                onClicked: root.applyView(true)
            }

            Text {
                id: viewText

                height: applyViewBtn.height
                verticalAlignment: Text.AlignVCenter
                visible: text !== ""
                text: root.activeView
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault

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

        Loader {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing * 2
            Layout.rightMargin: Theme.spacing * 2
            Layout.bottomMargin: Theme.spacing
            active: (root.edits?.lastError ?? "") !== ""
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
    }
}
