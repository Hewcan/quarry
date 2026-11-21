pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// A migration tab (H12 phases B and C): the code of one migration file, read-only, with its actions
// on top. Run, Rollback and Dry run call `php artisan` through MigrationController: when one cannot,
// the reason is in words (under the bar, in the tooltip and in the accessible description). Run and
// Rollback ask first in a dialog (MainWindow opens it when the controller says "awaitingConfirm");
// nothing here starts them without that. While something runs the buttons are off and Cancel is
// shown; the output goes to the Messages of the Results panel (the tab's QueryState); the result is
// text with an icon, never only a color. The state of the migration and the file name are text
// with an icon too. The code is read on the worker thread: while it loads the tab says so, when it
// fails it says why and offers "Try again", and a migration whose file is not in the folder (an
// orphan) says that instead of asking for code.
FocusScope {
    id: root

    required property var tabs // EditorTabsModel
    required property int row
    // MigrationController
    required property var migrations
    // QueryController and the id of this tab: the Messages of its QueryState take the output.
    property var queries: null
    property string tabId: ""
    // The name of the migration (the tab's title).
    property string tabTitle: ""
    property string connectionId: ""
    // The key of the migration settings of this tab: "" = the connection's own database.
    property string database: ""
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
    // {state: "idle" | "checking" | "awaitingConfirm" | "running" | "done" | "failed" | "blocked",
    // action, message, detail, warning, why, exitCode, cancelled, target, connection}
    property var run: ({
            "state": "idle",
            "action": "",
            "message": "",
            "detail": "",
            "warning": "",
            "why": "",
            "exitCode": -1,
            "cancelled": false,
            "target": {
                "known": false
            },
            "connection": ({})
        })
    // {ok, reason}: whether the buttons can start something, and if not why.
    property var availability: ({
            "ok": false,
            "reason": ""
        })
    readonly property bool orphan: entry.found && entry.state === "orphan"
    // What the view is showing: "orphan" | "loading" | "ready" | "failed".
    readonly property string view: orphan ? "orphan" : source.state === "ready" ? "ready" : source.state === "failed" ? "failed" : "loading"
    readonly property string statusText: !entry.found ? qsTr("State unknown") : entry.state === "applied" ? (entry.batch >= 0 ? qsTr("Applied, batch %1").arg(entry.batch) : qsTr("Applied")) : entry.state === "orphan" ? (entry.batch >= 0 ? qsTr("File missing, batch %1").arg(entry.batch) : qsTr("File missing")) : qsTr("Pending")
    readonly property string statusIcon: !entry.found ? Icons.info : entry.state === "applied" ? Icons.circleCheck : entry.state === "orphan" ? Icons.fileX : Icons.clock
    readonly property string disabledReason: availability.ok ? "" : availability.reason
    // The check or the process is going: Cancel is shown, and Esc cancels.
    readonly property bool running: run.state === "checking" || run.state === "running"
    readonly property string runIcon: running ? Icons.loaderCircle : run.state === "awaitingConfirm" ? Icons.info : run.state === "done" ? Icons.circleCheck : Icons.circleAlert
    // What the editor region focuses (the same name the SQL editor and the data tab use).
    readonly property Item edit: view === "ready" ? (code.item?.edit ?? stateless) : view === "failed" ? retryButton : stateless
    readonly property var codeEditor: code.item
    readonly property alias toolbar: actions
    readonly property alias runButton: runBtn
    readonly property alias rollbackButton: rollbackBtn
    readonly property alias dryRunButton: dryRunBtn
    readonly property alias cancelButton: cancelBtn
    readonly property alias retryButton: retryButton
    readonly property alias statusLabel: statusLine
    readonly property alias fileLabel: fileLine
    readonly property alias reasonLabel: reasonLine
    readonly property alias runLabel: runLine
    readonly property alias warningLabel: warningLine
    readonly property alias runError: runErrorPanel
    readonly property alias statePanel: stateless
    readonly property alias errorPanel: errorPanel
    // Left to right, as the bar shows them.
    readonly property var buttons: [runBtn, rollbackBtn, dryRunBtn]
    property string lastAnnouncement: ""

    function refreshSource() {
        source = migrations.sourceOf(connectionId, database, tabTitle);
    }

    function refreshEntry() {
        entry = migrations.entryOf(connectionId, database, tabTitle);
    }

    function refreshRun() {
        run = migrations.runOf(connectionId, database, tabTitle);
        availability = migrations.canRun(connectionId, database, tabTitle);
    }

    function load() {
        migrations.readSource(connectionId, database, tabTitle);
    }

    // The three buttons. Run and Rollback only start the check: the controller asks MainWindow to
    // confirm before anything runs.
    function begin(action) {
        if (!migrations.start(connectionId, database, tabTitle, action))
            say(availability.reason, true);
    }

    function cancelRun() {
        migrations.cancel(connectionId, database, tabTitle);
    }

    function say(text, assertive) {
        lastAnnouncement = text;
        root.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    // After a run: the first button that works, else the code.
    function giveBackFocus() {
        const active = root.Window.activeFocusItem;
        const lost = !active || active === root.Window.contentItem || active === cancelBtn;
        if (lost)
            (availability.ok ? runBtn : root.edit).forceActiveFocus(Qt.OtherFocusReason);
    }

    Component.onCompleted: {
        if (queries !== null && tabId !== "")
            migrations.setMessages(connectionId, database, tabTitle, queries.stateOf(tabId).messages);
        refreshEntry();
        refreshSource();
        refreshRun();
        if (!orphan)
            load();
    }

    Component.onDestruction: {
        if (migrations)
            migrations.releaseSource(connectionId, database, tabTitle);
    }

    // The focus was on what is gone (the loading text, the error): the new content takes it.
    onViewChanged: {
        const lost = !root.Window.activeFocusItem || root.Window.activeFocusItem === root.Window.contentItem;
        if (lost || root.activeFocus)
            Qt.callLater(() => root.edit.forceActiveFocus(Qt.OtherFocusReason));
    }

    Connections {
        target: root.migrations

        function onSourceChanged(id, db, name) {
            if (id !== root.connectionId || db !== root.database || name !== root.tabTitle)
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
            root.refreshRun();
        }

        function onSettingsChanged() {
            root.refreshRun();
        }

        function onAvailabilityChanged() {
            root.refreshRun();
        }

        function onRunChanged(id, db, name) {
            if (id !== root.connectionId || db !== root.database || name !== root.tabTitle)
                return;
            const before = root.run.state;
            root.refreshRun();
            const now = root.run.state;
            if (before === now)
                return;
            const wasGoing = before === "checking" || before === "running";
            // The button that had the focus is off now: Cancel takes it, and gives it back at the end.
            if (root.running && !wasGoing)
                Qt.callLater(() => {
                    if (cancelBtn.visible)
                        cancelBtn.forceActiveFocus(Qt.OtherFocusReason);
                });
            else if (!root.running && wasGoing)
                Qt.callLater(root.giveBackFocus);
            if (now === "failed" || now === "blocked")
                root.say(root.run.message, true);
            else if (now !== "idle")
                root.say(root.run.message, false);
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bgCanvas
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The bar: the actions on top of everything, then the state and the file, then what the
        // action is doing, then why the actions are off.
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

                    // What it does while it can, why it does not while it cannot (tooltip and
                    // accessible description).
                    component ActionButton: QButton {
                        id: action

                        property string actionId: ""
                        property string doing: ""

                        enabled: root.availability.ok
                        Accessible.description: enabled ? doing : root.availability.reason
                        onClicked: root.begin(actionId)

                        QToolTip {
                            visible: action.hovered
                            delay: 500
                            text: action.enabled ? action.doing : root.availability.reason
                        }
                    }

                    ActionButton {
                        id: runBtn

                        text: qsTr("Run")
                        iconGlyph: Icons.play
                        actionId: "run"
                        doing: qsTr("Applies this migration to the database")
                    }

                    ActionButton {
                        id: rollbackBtn

                        text: qsTr("Rollback")
                        iconGlyph: Icons.undo2
                        actionId: "rollback"
                        doing: qsTr("Reverts this migration if it is in the latest batch")
                    }

                    ActionButton {
                        id: dryRunBtn

                        text: qsTr("Dry run")
                        iconGlyph: Icons.flaskConical
                        actionId: "dryRun"
                        doing: qsTr("Shows the SQL this migration would run, without running it")
                    }

                    // Only while something runs. Esc does the same.
                    QButton {
                        id: cancelBtn

                        visible: root.running
                        text: qsTr("Cancel")
                        iconGlyph: Icons.square
                        onClicked: root.cancelRun()
                        Accessible.description: root.run.state === "running" && root.run.action !== "dryRun" ? qsTr("Stops the process. The database may be left half migrated.") : qsTr("Stops the process")
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

                // What the last action is doing or did: icon and words.
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.run.state !== "idle"
                    spacing: Theme.spacing

                    Icon {
                        Layout.alignment: Qt.AlignTop
                        text: root.runIcon
                        color: root.run.state === "done" ? Theme.success : root.run.state === "failed" || root.run.state === "blocked" ? Theme.danger : Theme.fgDefault
                    }

                    Text {
                        id: runLine

                        Layout.fillWidth: true
                        text: root.run.message
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap

                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Action: %1").arg(text)
                    }
                }

                // A dry run on another database than the connection's: a warning, not a stop.
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.run.warning !== ""
                    spacing: Theme.spacing

                    Icon {
                        Layout.alignment: Qt.AlignTop
                        text: Icons.triangleAlert
                        color: Theme.warning
                    }

                    Text {
                        id: warningLine

                        Layout.fillWidth: true
                        text: root.run.warning
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap

                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Warning: %1").arg(text)
                    }
                }

                // Blocked (the two databases are shown) or failed (the output is in Messages).
                ErrorPanel {
                    id: runErrorPanel

                    readonly property bool twoDatabases: root.run.state === "blocked" && root.run.target.known

                    Layout.fillWidth: true
                    visible: root.run.state === "blocked" || (root.run.state === "failed" && !root.run.cancelled)
                    title: root.run.message
                    message: twoDatabases ? qsTr("Database artisan uses: %1 on host %2").arg(root.run.target.database).arg(root.run.target.host) + "\n" + qsTr("Database of this connection: %1 on host %2").arg(root.run.connection.database ?? "").arg(root.run.connection.host ?? "") : qsTr("The output is in the Messages panel.")
                    hint: twoDatabases ? root.run.why : ""
                    detail: twoDatabases ? "" : root.run.detail
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.disabledReason !== ""
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
