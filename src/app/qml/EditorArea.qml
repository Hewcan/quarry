pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// The content of the Editor region: the tab bar, the query bar (run, cancel, plan), the editors
// (one per tab, created the first time the tab is shown and kept so each keeps its undo history
// and scroll position) and the empty state when no tab is open.
ColumnLayout {
    id: root

    required property var tabs // EditorTabsModel
    required property var queries // QueryController
    required property var connections // ConnectionController
    property string newShortcut: ""
    property var completion: null // CompletionController
    property var exporter: null // ExportController: the data tabs' Export button
    // MigrationController: the migration tabs read their code through it.
    property var migrations: null
    // AiController (H13): the chat panel of each query tab. Optional.
    property var ai: null
    // The command behind the AI button of the query bar.
    property var aiCommand: null
    // The commands behind the query bar's buttons (null in tests that do not have them).
    property var runCommand: null
    property var runAllCommand: null
    property var cancelCommand: null
    property var explainCommand: null
    property var explainAnalyzeCommand: null
    // The last thing sent to the screen reader (tests read it; it is not shown).
    property string lastAnnouncement: ""
    property string errorMessage: ""
    property string errorDetail: ""
    property int editorRevision: 0

    // The view of the current tab (a SqlEditor, a DataTab or a MigrationTab), null when there is none
    // (or it is not created yet).
    readonly property var currentView: {
        const holder = root.editorRevision >= 0 ? editors.itemAt(root.tabs.currentIndex) as Loader : null;
        return holder ? holder.item : null;
    }
    readonly property var currentEditor: currentView && currentView.isEditor ? currentView : null
    readonly property var currentMigration: currentView && currentView.isMigration === true ? currentView : null
    readonly property var currentData: currentView && !currentView.isEditor && currentMigration === null ? currentView : null
    readonly property Item defaultFocus: reviewLoader.item ? reviewLoader.item.focusTarget : currentView ? currentView.edit : emptyView
    readonly property alias tabBar: bar
    readonly property alias emptyState: emptyView
    readonly property alias queryBar: queryBar
    readonly property alias runButton: runBtn
    readonly property alias runAllButton: runAllBtn
    readonly property alias cancelButton: cancelBtn
    readonly property alias explainButton: explainBtn
    readonly property alias explainAnalyzeButton: analyzeBtn
    readonly property alias aiButton: aiBtn

    // ---- the AI assistant (H13 phase C) ----
    // Which tabs have their panel open (by tab id). Never stored: every run starts closed.
    property var chatOpen: ({})
    property int chatOpenRevision: 0
    readonly property string currentTabId: {
        const row = root.tabs.currentIndex;
        return row >= 0 ? root.tabs.idOf(row) : "";
    }
    readonly property bool aiPanelVisible: {
        root.chatOpenRevision;
        return root.currentEditor !== null && root.chatOpen[root.currentTabId] === true;
    }
    // The chat of the current tab, null until its panel was opened once (nothing exists before).
    readonly property var currentChat: {
        if (root.ai === null || root.currentEditor === null)
            return null;
        root.ai.chatCount;
        return root.ai.existingChat(root.currentTabId);
    }
    readonly property alias chatPanelLoader: panelLoader
    readonly property alias reviewLoader: reviewLoader
    readonly property alias chatSplit: split
    readonly property var chatPanel: panelLoader.item
    readonly property var review: reviewLoader.item
    readonly property int defaultChatWidth: Theme.controlHeight * 12
    // The notice of the panel is open the first time it is ever shown, then folded.
    property bool noticeSeenBefore: LayoutStore.value("aiNoticeSeen", false) === true || LayoutStore.value("aiNoticeSeen", false) === "true"

    signal newRequested
    signal closeRequested(int row)
    // The chat panel asks to open the settings: "connection" (the connection's AI options) or "app".
    signal aiSettingsRequested(string kind, string connectionId)

    // Opens or closes the assistant of the current query tab. The panel and its objects are made
    // the first time it opens.
    function toggleChat() {
        if (currentEditor === null || ai === null)
            return;
        setChatOpen(currentTabId, !aiPanelVisible);
    }

    function setChatOpen(tabId, open) {
        if (tabId === "")
            return;
        if (open && ai.chatFor(tabId) === null)
            return;
        const next = Object.assign({}, chatOpen);
        next[tabId] = open;
        chatOpen = next;
        chatOpenRevision++;
        if (open) {
            panelLoader.active = true;
            announce(qsTr("AI assistant opened"));
            Qt.callLater(() => {
                if (root.chatPanel)
                    root.chatPanel.focusTarget.forceActiveFocus(Qt.TabFocusReason);
                // From now on the notice starts folded.
                LayoutStore.setValue("aiNoticeSeen", true);
            });
        } else {
            announce(qsTr("AI assistant closed"));
            focusEditor();
        }
    }

    function closeAllChats() {
        chatOpen = ({});
        chatOpenRevision++;
    }

    function focusEditor() {
        Qt.callLater(() => {
            const target = root.defaultFocus;
            if (target)
                target.forceActiveFocus(Qt.OtherFocusReason);
        });
    }

    function resetChatWidth() {
        panelLoader.T.SplitView.preferredWidth = defaultChatWidth;
    }

    function announce(text, assertive) {
        lastAnnouncement = text;
        root.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    spacing: 0

    Connections {
        target: root.tabs

        function onFailed(message, detail) {
            root.errorMessage = message;
            root.errorDetail = detail;
            root.announce(message, true);
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: false // its fill-height child would otherwise make the whole row grow
        spacing: 0

        QTabBar {
            id: bar

            Layout.fillWidth: true
            accessibleName: qsTr("Editor tabs")
            currentIndex: root.tabs.currentIndex
            onCurrentIndexChanged: {
                if (currentIndex >= 0 && currentIndex !== root.tabs.currentIndex)
                    root.tabs.currentIndex = currentIndex;
            }

            Repeater {
                model: root.tabs

                QTabButton {
                    id: tab

                    required property int index
                    required property string title
                    // A migration's name without its date prefix.
                    required property string displayTitle
                    required property string connectionName
                    required property int colorIndex
                    required property bool production
                    required property string tabKind

                    text: displayTitle
                    fullTitle: title
                    closable: true
                    fitContent: true
                    tintColor: tint.fill
                    tintInk: tint.ink
                    Accessible.name: title + (tabKind === "data" ? ", " + qsTr("table data") : tabKind === "migration" ? ", " + qsTr("migration") : "") + (connectionName !== "" ? ", " + connectionName : "") + (production ? ", " + qsTr("production") : "")
                    onCloseRequested: root.closeRequested(index)

                    // The color of the tab's connection is its background.
                    ConnectionTint {
                        id: tint

                        colorIndex: tab.colorIndex
                    }

                    // A migration is told apart by its icon (the name says "migration" as well).
                    leading: Icon {
                        visible: tab.tabKind === "migration"
                        text: Icons.fileCode
                        color: tab.tinted ? tint.ink : Theme.fgMuted
                    }

                    trailing: ProductionBadge {
                        visible: tab.production
                        framed: tab.tinted
                    }
                }
            }
        }

        Rectangle {
            Layout.fillHeight: true
            implicitWidth: newButton.implicitWidth + Theme.spacing
            color: Theme.bgSurface

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.borderDefault
            }

            IconButton {
                id: newButton

                anchors.centerIn: parent
                accessibleName: qsTr("New query")
                shortcutText: root.newShortcut
                iconGlyph: Icons.plus
                onClicked: root.newRequested()
            }
        }
    }

    // The query bar, right above the editor: run, run all, cancel and the query plan (UX.md "Plan
    // de consulta (H9)"). Only query tabs have them: a data or a migration tab has no statement to
    // run. The connection chooser lives in the application toolbar (MainWindow); the color of the
    // connection is the tab's background and PRODUCTION is in the tab and here, next to the buttons.
    // A Flow: at 200% zoom the badge drops to a second line instead of leaving the window.
    Flow {
        id: queryBar

        Layout.fillWidth: true
        Layout.margins: Theme.spacing
        visible: root.currentEditor !== null || connectionBadge.visible
        spacing: Theme.spacing

        Accessible.role: Accessible.ToolBar
        Accessible.name: qsTr("Query toolbar")

        QueryButton {
            id: runBtn

            command: root.runCommand
            iconGlyph: Icons.play
        }

        QueryButton {
            id: runAllBtn

            command: root.runAllCommand
            iconGlyph: Icons.listVideo
        }

        QueryButton {
            id: cancelBtn

            command: root.cancelCommand
            iconGlyph: Icons.square
        }

        IconButton {
            id: aiBtn

            visible: root.currentEditor !== null && root.ai !== null
            accessibleName: root.aiCommand?.title ?? ""
            shortcutText: root.aiCommand?.shortcutText ?? ""
            iconGlyph: Icons.sparkles
            enabled: root.aiCommand?.enabled ?? false
            Accessible.checkable: true
            Accessible.checked: root.aiPanelVisible
            onClicked: CommandRegistry.trigger(root.aiCommand.commandId)
        }

        IconButton {
            id: explainBtn

            visible: root.currentEditor !== null
            accessibleName: root.explainCommand?.title ?? ""
            shortcutText: enabled ? root.explainCommand?.shortcutText ?? "" : ""
            iconGlyph: Icons.listTree
            enabled: root.explainCommand?.enabled ?? false
            Accessible.description: enabled ? shortcutText : qsTr("Not available: this connection's engine cannot show a query plan.")
            onClicked: CommandRegistry.trigger(root.explainCommand.commandId)
        }

        IconButton {
            id: analyzeBtn

            visible: root.currentEditor !== null
            accessibleName: root.explainAnalyzeCommand?.title ?? ""
            shortcutText: enabled ? root.explainAnalyzeCommand?.shortcutText ?? "" : ""
            iconGlyph: Icons.gauge
            enabled: root.explainAnalyzeCommand?.enabled ?? false
            Accessible.description: enabled ? shortcutText : qsTr("Not available: this connection's engine cannot show a query plan.")
            onClicked: CommandRegistry.trigger(root.explainAnalyzeCommand.commandId)
        }

        ProductionBadge {
            id: connectionBadge

            height: runBtn.height
            visible: root.currentView?.production ?? false
        }
    }

    // A button for a Command of the editor: name, tooltip shortcut and enabled state follow it.
    component QueryButton: IconButton {
        property var command: null

        visible: root.currentEditor !== null
        accessibleName: command?.title ?? ""
        shortcutText: command?.shortcutText ?? ""
        enabled: command?.enabled ?? false
        onClicked: CommandRegistry.trigger(command.commandId)
    }

    // A tab could not be stored: said in plain language, with the driver text below.
    Loader {
        Layout.fillWidth: true
        Layout.margins: Theme.spacing * 2
        active: root.errorMessage !== ""
        visible: active
        sourceComponent: ErrorPanel {
            title: qsTr("Your queries could not be saved")
            message: root.errorMessage
            detail: root.errorDetail
        }
    }

    // The editors, with the assistant of the current tab to their right (closed until asked).
    QSplitView {
        id: split

        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: root.tabs.count > 0
        orientation: Qt.Horizontal
        accessibleName: qsTr("Resize AI assistant")

        onResized: widthSave.restart()

        Item {
            id: stackHost

            T.SplitView.fillWidth: true
            T.SplitView.minimumWidth: Theme.controlHeight * 6

            StackLayout {
                id: stack

                anchors.fill: parent
                // While the proposal of Claude is under review the editor under it takes no keys.
                enabled: reviewLoader.item === null
                currentIndex: root.tabs.currentIndex

                Repeater {
                    id: editors

                    model: root.tabs

                    onItemAdded: root.editorRevision++
                    onItemRemoved: root.editorRevision++

                    Loader {
                        id: holder

                        required property int index
                        required property string tabId
                        required property string title
                        required property string connectionId
                        required property string connectionName
                        required property string tabKind
                        required property string definition
                        required property bool production
                        property bool shown: false

                        // Sized from the stack itself: a Loader that turns active after the stack's last
                        // layout pass would otherwise stay 0x0 until the next one (seen on Linux CI).
                        width: stack.width
                        height: stack.height
                        active: shown || StackLayout.isCurrentItem
                        onItemChanged: root.editorRevision++
                        StackLayout.onIsCurrentItemChanged: {
                            if (StackLayout.isCurrentItem)
                                shown = true;
                        }

                        sourceComponent: holder.tabKind === "data" ? dataComponent : holder.tabKind === "migration" ? migrationComponent : editorComponent

                        Component {
                            id: dataComponent

                            DataTab {
                                anchors.fill: parent
                                tabs: root.tabs
                                row: holder.index
                                queryState: root.queries.stateOf(holder.tabId)
                                tabTitle: holder.title
                                connectionId: holder.connectionId
                                connectionName: holder.connectionName
                                production: holder.production
                                exporter: root.exporter
                                queries: root.queries
                                tabId: holder.tabId
                            }
                        }

                        Component {
                            id: migrationComponent

                            MigrationTab {
                                anchors.fill: parent
                                tabs: root.tabs
                                row: holder.index
                                migrations: root.migrations
                                queries: root.queries
                                tabId: holder.tabId
                                tabTitle: holder.title
                                connectionId: holder.connectionId
                                database: root.tabs.migrationDatabaseOf(holder.index)
                                connectionName: holder.connectionName
                                production: holder.production
                            }
                        }

                        Component {
                            id: editorComponent

                            SqlEditor {
                                anchors.fill: parent
                                tabs: root.tabs
                                row: holder.index
                                queryState: root.queries.stateOf(holder.tabId)
                                tabTitle: holder.title
                                connectionId: holder.connectionId
                                connectionName: holder.connectionName
                                definition: holder.definition
                                production: holder.production
                                completion: root.completion
                                placeholder: qsTr("Write SQL here. %1 runs the current statement.").arg(CommandRegistry.displayShortcut("Ctrl+Enter"))
                            }
                        }
                    }
                }
            }

            // The review of Claude's proposal covers the editor of the tab; Apply writes into it.
            Loader {
                id: reviewLoader

                anchors.fill: parent
                active: (root.currentChat?.diff ?? null) !== null
                sourceComponent: AiDiffReview {
                    chat: root.currentChat
                    currentText: root.currentEditor ? root.currentEditor.edit.text : ""
                    tabTitle: root.currentEditor?.tabTitle ?? ""
                    onAnnounce: (text, assertive) => root.announce(text, assertive)
                    onApplyRequested: text => {
                        const editor = root.currentEditor;
                        const chat = root.currentChat;
                        if (editor === null || chat === null)
                            return;
                        editor.applyText(text);
                        chat.diffApplied();
                        root.announce(qsTr("Changes applied to the script. Nothing was run."));
                        root.focusEditor();
                    }
                    onDiscardRequested: {
                        const chat = root.currentChat;
                        if (chat)
                            chat.discardDiff();
                        root.announce(qsTr("Review discarded. The script was not changed."));
                        root.focusEditor();
                    }
                }
            }
        }

        // Made the first time the panel opens, and kept (the chats of the tabs live in AiController).
        Loader {
            id: panelLoader

            active: false
            visible: root.aiPanelVisible
            T.SplitView.preferredWidth: Number(LayoutStore.value("aiChatWidth", root.defaultChatWidth))
            T.SplitView.minimumWidth: Theme.controlHeight * 8
            T.SplitView.maximumWidth: Math.max(T.SplitView.minimumWidth, root.width * 0.7)

            sourceComponent: AiChatPanel {
                chat: root.currentChat
                ai: root.ai
                noticeOpen: !root.noticeSeenBefore
                onCloseRequested: root.setChatOpen(root.currentTabId, false)
                onFocusEditorRequested: root.focusEditor()
                onSettingsRequested: kind => root.aiSettingsRequested(kind, root.currentChat?.connectionId ?? "")
                onReviewRequested: {
                    if (root.review)
                        root.review.focusTarget.forceActiveFocus(Qt.TabFocusReason);
                }
            }
        }
    }

    // Waits for the layout to settle after a resize, then stores the width of the panel.
    Timer {
        id: widthSave

        interval: 300
        onTriggered: {
            if (panelLoader.visible && panelLoader.width > 0)
                LayoutStore.setValue("aiChatWidth", Math.round(panelLoader.width));
        }
    }

    EmptyState {
        id: emptyView

        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: root.tabs.count === 0
        focusable: true
        iconGlyph: Icons.fileCode
        title: qsTr("No query open")
        message: qsTr("Write SQL here. %1 runs the current statement.").arg(CommandRegistry.displayShortcut("Ctrl+Enter"))

        QButton {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("New query")
            iconGlyph: Icons.plus
            variant: "primary"
            onClicked: root.newRequested()
        }
    }
}
