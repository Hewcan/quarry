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
    property string lastNotice: ""

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
