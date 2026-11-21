pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// A migration tab (H12 phase B): the code of one migration file, read-only, with its actions on top.
// Run, Rollback and Dry run are shown but off in this phase, with the reason in words (under the
// bar, in the tooltip and in the accessible description); the state of the migration and the file
// name are text with an icon. The code is read on the worker thread (MigrationController): while it
// loads the tab says so, when it fails it says why and offers "Try again", and a migration whose
// file is not in the folder (an orphan) says that instead of asking for code.
FocusScope {
    id: root

    required property var tabs // EditorTabsModel
    required property int row
    // MigrationController
    required property var migrations
    // The name of the migration (the tab's title).
    property string tabTitle: ""
    property string connectionId: ""
    property string connectionName: ""
    property bool production: false

    readonly property bool isEditor: false
    readonly property bool isMigration: true
    // {state: "loading" | "ready" | "failed" | "none", text, message, detail, file, definition}
    property var source: ({
            "state": "none",
            "text": "",
            "message": "",
            "detail": "",
            "file": "",
            "definition": "PHP"
        })
    // {found, state: "pending" | "applied" | "orphan", batch}
    property var entry: ({
            "found": false
        })
    readonly property bool orphan: entry.found && entry.state === "orphan"
    // What the view is showing: "orphan" | "loading" | "ready" | "failed".
    readonly property string view: orphan ? "orphan" : source.state === "ready" ? "ready" : source.state === "failed" ? "failed" : "loading"
    readonly property string statusText: !entry.found ? qsTr("State unknown") : entry.state === "applied" ? (entry.batch >= 0 ? qsTr("Applied, batch %1").arg(entry.batch) : qsTr("Applied")) : entry.state === "orphan" ? (entry.batch >= 0 ? qsTr("File missing, batch %1").arg(entry.batch) : qsTr("File missing")) : qsTr("Pending")
    readonly property string statusIcon: !entry.found ? Icons.info : entry.state === "applied" ? Icons.circleCheck : entry.state === "orphan" ? Icons.fileX : Icons.clock
    readonly property string disabledReason: qsTr("Running migrations arrives in the next step")
    // What the editor region focuses (the same name the SQL editor and the data tab use).
    readonly property Item edit: view === "ready" ? (code.item?.edit ?? stateless) : view === "failed" ? retryButton : stateless
    readonly property var codeEditor: code.item
    readonly property alias toolbar: actions
    readonly property alias runButton: runBtn
    readonly property alias rollbackButton: rollbackBtn
    readonly property alias dryRunButton: dryRunBtn
    readonly property alias retryButton: retryButton
    readonly property alias statusLabel: statusLine
    readonly property alias fileLabel: fileLine
    readonly property alias reasonLabel: reasonLine
    readonly property alias statePanel: stateless
    readonly property alias errorPanel: errorPanel
    // Left to right, as the bar shows them.
    readonly property var buttons: [runBtn, rollbackBtn, dryRunBtn]
    property string lastAnnouncement: ""

    function refreshSource() {
        source = migrations.sourceOf(connectionId, tabTitle);
    }

    function refreshEntry() {
        entry = migrations.entryOf(connectionId, tabTitle);
    }

    function load() {
        migrations.readSource(connectionId, tabTitle);
    }

    function say(text, assertive) {
        lastAnnouncement = text;
        root.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    Component.onCompleted: {
        refreshEntry();
        refreshSource();
        if (!orphan)
            load();
    }

    Component.onDestruction: {
        if (migrations)
            migrations.releaseSource(connectionId, tabTitle);
    }

    // The focus was on what is gone (the loading text, the error): the new content takes it.
    onViewChanged: {
        const lost = !root.Window.activeFocusItem || root.Window.activeFocusItem === root.Window.contentItem;
        if (lost || root.activeFocus)
            Qt.callLater(() => root.edit.forceActiveFocus(Qt.OtherFocusReason));
    }

    Connections {
        target: root.migrations

        function onSourceChanged(id, name) {
            if (id !== root.connectionId || name !== root.tabTitle)
                return;
            const before = root.source.state;
            root.refreshSource();
            if (before !== root.source.state && root.source.state === "failed")
                root.say(root.source.message, true);
            else if (before !== root.source.state && root.source.state === "ready")
                root.say(qsTr("%1 loaded").arg(root.tabTitle), false);
        }

        function onReportsChanged() {
            root.refreshEntry();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bgCanvas
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The bar: the actions on top of everything, then the state and the file, then why the
        // actions are off.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: bar.implicitHeight + Theme.spacing * 2
            color: Theme.bgSurface

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.borderDefault
            }

            ColumnLayout {
                id: bar

                anchors.fill: parent
                anchors.margins: Theme.spacing
                spacing: Theme.spacing

                Flow {
                    id: actions

                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Accessible.role: Accessible.ToolBar
                    Accessible.name: qsTr("Migration actions")

                    component ActionButton: QButton {
                        enabled: false
                        Accessible.description: root.disabledReason

                        QToolTip {
                            visible: parent.hovered
                            delay: 500
                            text: root.disabledReason
                        }
                    }

                    ActionButton {
                        id: runBtn

                        text: qsTr("Run")
                        iconGlyph: Icons.play
                    }

                    ActionButton {
                        id: rollbackBtn

                        text: qsTr("Rollback")
                        iconGlyph: Icons.undo2
                    }

                    ActionButton {
                        id: dryRunBtn

                        text: qsTr("Dry run")
                        iconGlyph: Icons.flaskConical
                    }
                }

                // The state (icon and words) and the file.
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.spacing * 3

                    Row {
                        spacing: Theme.spacing

                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.statusIcon
                            color: Theme.fgDefault
                        }

                        Text {
                            id: statusLine

                            anchors.verticalCenter: parent.verticalCenter
                            text: root.statusText
                            font.pointSize: Theme.fontSize
                            font.bold: true
                            color: Theme.fgDefault

                            Accessible.role: Accessible.StaticText
                            Accessible.name: qsTr("State: %1").arg(text)
                        }
                    }

                    Row {
                        spacing: Theme.spacing

                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            text: Icons.fileCode
                            color: Theme.fgMuted
                        }

                        Text {
                            id: fileLine

                            anchors.verticalCenter: parent.verticalCenter
                            text: root.source.file !== "" ? root.source.file : root.tabTitle
                            font.family: Theme.monoFontFamily
                            font.pointSize: Theme.fontSize
                            color: Theme.fgMuted

                            Accessible.role: Accessible.StaticText
                            Accessible.name: qsTr("File: %1").arg(text)
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Icon {
                        Layout.alignment: Qt.AlignTop
                        text: Icons.info
                        color: Theme.fgMuted
                    }

                    Text {
                        id: reasonLine

                        Layout.fillWidth: true
                        text: root.disabledReason
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap

                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Run, Rollback and Dry run are off. %1").arg(text)
                    }
                }
            }
        }

        // The code of the migration, once read.
        Loader {
            id: code

            Layout.fillWidth: true
            Layout.fillHeight: true
            active: root.view === "ready"
            visible: active
            focus: true
            sourceComponent: SqlEditor {
                anchors.fill: parent
                tabs: root.tabs
                row: root.row
                readOnly: true
                sourceText: root.source.text
                definition: root.source.definition
                tabTitle: root.tabTitle
                connectionId: root.connectionId
                connectionName: root.connectionName
                production: root.production
                accessibleName: qsTr("Migration code")
                accessibleDescription: qsTr("%1, read only").arg(root.source.file !== "" ? root.source.file : root.tabTitle)
            }
        }

        // Reading, or an orphan: a message with nothing to do.
        EmptyState {
            id: stateless

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.view === "loading" || root.view === "orphan"
            focusable: visible
            iconGlyph: root.view === "orphan" ? Icons.fileX : Icons.loaderCircle
            title: root.view === "orphan" ? qsTr("This migration has no file in the folder.") : qsTr("Loading…")
            message: root.view === "orphan" ? qsTr("Its name is in the migrations table, but the file is not in the migrations folder: it may have been deleted or renamed. There is no code to show.") : qsTr("Reading the migration file.")
        }

        // The file could not be read.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacing * 2
            visible: root.view === "failed"
            spacing: Theme.spacing * 2

            ErrorPanel {
                id: errorPanel

                Layout.fillWidth: true
                title: qsTr("The migration could not be read")
                message: root.source.message
                detail: root.source.detail
            }

            QButton {
                id: retryButton

                text: qsTr("Try again")
                iconGlyph: Icons.rotateCcw
                onClicked: root.load()
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }
}
