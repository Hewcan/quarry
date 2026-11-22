pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The AI assistant of one query tab, to the right of its editor (H13 phase C, UX.md "Asistente de
// IA"). It talks to an AiChatController (`chat`, the one of the tab) and shows: the fixed notice
// about Claude Code and your subscription, the conversation (kept in memory only), the status of the
// request, what was sent ("Context sent") and the field to ask. It never runs SQL: the proposals
// of Claude go to the review view over the editor.
FocusScope {
    id: root

    // AiChatController of the current tab, null while there is none.
    property var chat: null
    // AiController: the log of what is sent.
    property var ai: null
    // The notice is open the first time the panel is ever shown (the caller sets it).
    property bool noticeOpen: true
    property bool contextOpen: false
    property bool detailsOpen: false
    // The last thing sent to the screen reader (tests read it; it is not shown).
    property string lastAnnouncement: ""

    readonly property Item focusTarget: input
    readonly property alias input: input
    readonly property alias sendButton: sendBtn
    readonly property alias cancelButton: cancelBtn
    readonly property alias clearButton: clearBtn
    readonly property alias closeButton: closeBtn
    readonly property alias noticeButton: noticeBtn
    readonly property alias noticeText: noticeText
    readonly property alias contextButton: contextBtn
    readonly property alias contextBody: contextBody
    readonly property alias messageList: messages
    readonly property alias statusCard: statusCard
    readonly property alias statusText: statusText
    readonly property alias detailsButton: detailsBtn
    readonly property alias detailsText: detailsEdit
    readonly property alias settingsButton: settingsBtn
    readonly property alias reviewButton: reviewBtn
    readonly property alias loginText: loginEdit
    readonly property alias notSentText: notSentText
    readonly property alias unavailableBox: unavailableBox
    readonly property alias unavailableText: unavailableLabel
    readonly property alias unavailableSettingsButton: unavailableBtn

    signal closeRequested
    // What opens: "connection" (the connection's AI settings) or "app" (the Settings dialog).
    signal settingsRequested(string kind)
    signal reviewRequested
    signal focusEditorRequested

    function say(text, assertive) {
        lastAnnouncement = text;
        root.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    function submit() {
        if (!chat || !chat.canSend || input.text.trim() === "")
            return;
        if (chat.send(input.text)) {
            input.text = "";
            chat.draft = "";
        }
    }

    function copyOf(text) {
        clip.text = text;
        clip.selectAll();
        clip.copy();
        clip.deselect();
    }

    function openSettings() {
        root.settingsRequested(chat && chat.unavailableKind === "off" ? "connection" : "app");
    }

    onChatChanged: {
        if (input.text !== (chat ? chat.draft : ""))
            input.text = chat ? chat.draft : "";
        detailsOpen = false;
    }

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("AI assistant")

    Connections {
        target: root.chat

        function onAnnouncement(text, assertive) {
            root.say(text, assertive);
        }

        function onErrorChanged() {
            root.detailsOpen = false;
        }
    }

    // Only to put text on the clipboard.
    TextEdit {
        id: clip

        visible: false
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bgSurface
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing
        spacing: Theme.spacing

        // ---- header ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Icon {
                text: Icons.sparkles
                color: Theme.accent
            }

            Heading {
                Layout.fillWidth: true
                level: 3
                text: qsTr("AI assistant")
            }

            IconButton {
                id: closeBtn

                accessibleName: qsTr("Close AI assistant")
                iconGlyph: Icons.x
                onClicked: root.closeRequested()
            }
        }

        // The notice: open the first time, then folded (the button brings it back).
        QButton {
            id: noticeBtn

            Layout.fillWidth: true
            variant: "flat"
            text: qsTr("What is sent, and who pays")
            iconGlyph: root.noticeOpen ? Icons.chevronDown : Icons.chevronRight
            onClicked: root.noticeOpen = !root.noticeOpen

            Accessible.expandable: true
            Accessible.expanded: root.noticeOpen
        }

        Text {
            id: noticeText

            Layout.fillWidth: true
            visible: root.noticeOpen
            text: qsTr("Claude Code runs on your own installation: your request and the structure you allow are sent to Anthropic through it, and use your Claude subscription or API key. Quarry sends nothing by itself.")
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap

            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("This conversation is kept in memory only: it is lost when you close this query.")
            font.pointSize: Theme.fontSize * 0.9
            color: Theme.fgMuted
            wrapMode: Text.WordWrap
        }

        // ---- the conversation ----
        ListView {
            id: messages

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Theme.controlHeight * 3
            clip: true
            model: root.chat ? root.chat.messages : null
            activeFocusOnTab: true
            spacing: Theme.spacing
            boundsBehavior: Flickable.StopAtBounds
            onCountChanged: {
                currentIndex = count - 1;
                positionViewAtEnd();
            }

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Conversation")
            Accessible.focusable: true

            Keys.onUpPressed: event => {
                if (messages.currentIndex > 0)
                    messages.currentIndex--;
                event.accepted = true;
            }
            Keys.onDownPressed: event => {
                if (messages.currentIndex < messages.count - 1)
                    messages.currentIndex++;
                event.accepted = true;
            }
            Keys.onPressed: event => {
                if (event.key === Qt.Key_C && (event.modifiers & Qt.ControlModifier) && messages.currentItem) {
                    root.copyOf(messages.currentItem.text);
                    event.accepted = true;
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - Theme.spacing * 4
                visible: messages.count === 0
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Ask Claude about this script. Claude sees the structure of the tables it uses, never their rows.")
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
                Accessible.ignored: true
            }

            delegate: Rectangle {
                id: message

                required property int index
                required property string role
                required property string author
                required property string text

                width: messages.width
                implicitHeight: body.implicitHeight + Theme.spacing * 2
                radius: Theme.radius
                color: role === "user" ? Theme.bgRaised : Theme.bgCanvas
                border.width: ListView.isCurrent && messages.activeFocus ? Theme.focusRingWidth : 1
                border.color: ListView.isCurrent && messages.activeFocus ? Theme.focusRing : Theme.borderDefault

                Accessible.role: Accessible.ListItem
                Accessible.name: author + ": " + text

                ColumnLayout {
                    id: body

                    x: Theme.spacing
                    y: Theme.spacing
                    width: parent.width - Theme.spacing * 2
                    spacing: Theme.spacing / 2

                    // The author is a word, never only a color or a side.
                    Text {
                        Layout.fillWidth: true
                        text: message.author
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.fgDefault
                        Accessible.ignored: true
                    }

                    Text {
                        Layout.fillWidth: true
                        text: message.text
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WrapAnywhere
                        textFormat: Text.PlainText
                        Accessible.ignored: true
                    }
                }
            }
        }

        // ---- not available: the reason, and where to fix it ----
        ColumnLayout {
            id: unavailableBox

            Layout.fillWidth: true
            visible: root.chat !== null && root.chat.unavailableKind !== ""
            spacing: Theme.spacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing

                Icon {
                    Layout.alignment: Qt.AlignTop
                    text: Icons.circleAlert
                    color: Theme.warning
                }

                Text {
                    id: unavailableLabel

                    Layout.fillWidth: true
                    text: root.chat ? root.chat.unavailableText : ""
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            QButton {
                id: unavailableBtn

                visible: root.chat !== null && root.chat.unavailableKind !== "noConnection"
                text: root.chat && root.chat.unavailableKind === "off" ? qsTr("Open connection settings") : qsTr("Open settings")
                iconGlyph: Icons.settings
                onClicked: root.openSettings()
            }
        }

        // ---- the status of the request ----
        Rectangle {
            id: statusCard

            Layout.fillWidth: true
            visible: root.chat !== null && root.chat.state !== "idle" && root.chat.unavailableKind === "" && root.chat.state !== "off" && root.chat.state !== "needsCli" && root.chat.state !== "noConnection"
            implicitHeight: statusColumn.implicitHeight + Theme.spacing * 2
            radius: Theme.radius
            color: Theme.bgCanvas
            border.width: 1
            border.color: root.chat && root.chat.state === "error" ? Theme.danger : Theme.borderDefault

            ColumnLayout {
                id: statusColumn

                x: Theme.spacing
                y: Theme.spacing
                width: parent.width - Theme.spacing * 2
                spacing: Theme.spacing

                RowLayout {
                    id: statusRow

                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Accessible.role: Accessible.StaticText
                    Accessible.name: statusText.text

                    Icon {
                        Layout.alignment: Qt.AlignTop
                        text: root.chat === null ? "" : root.chat.busy ? Icons.loaderCircle : root.chat.state === "error" ? Icons.circleAlert : root.chat.state === "done" ? Icons.circleCheck : Icons.info
                        color: root.chat === null ? Theme.fgMuted : root.chat.state === "error" ? Theme.danger : root.chat.state === "done" ? Theme.success : Theme.fgMuted

                        RotationAnimator on rotation {
                            running: root.chat !== null && root.chat.busy && Theme.animationDuration > 0
                            from: 0
                            to: 360
                            duration: 1000
                            loops: Animation.Infinite
                        }
                    }

                    Text {
                        id: statusText

                        Layout.fillWidth: true
                        text: root.chat ? root.chat.stateText : ""
                        font.pointSize: Theme.fontSize
                        font.bold: root.chat !== null && root.chat.state === "error"
                        color: root.chat !== null && root.chat.state === "error" ? Theme.danger : Theme.fgDefault
                        wrapMode: Text.WordWrap
                    }
                }

                // How to sign in: copyable.
                Text {
                    Layout.fillWidth: true
                    visible: loginEdit.visible
                    text: qsTr("Run this command in a terminal:")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.chat !== null && root.chat.state === "error" && root.chat.errorHint !== ""
                    spacing: Theme.spacing

                    TextEdit {
                        id: loginEdit

                        Layout.fillWidth: true
                        visible: parent.visible
                        activeFocusOnTab: visible
                        readOnly: true
                        selectByMouse: true
                        text: root.chat ? root.chat.errorHint : ""
                        wrapMode: TextEdit.WrapAnywhere
                        font.family: Theme.monoFontFamily
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        selectionColor: Theme.selectionBg
                        selectedTextColor: Theme.selectionFg

                        Accessible.role: Accessible.EditableText
                        Accessible.name: qsTr("Sign-in command")
                        Accessible.readOnly: true
                        Accessible.focusable: true

                        FocusRing {
                            shown: loginEdit.activeFocus
                        }
                    }

                    QButton {
                        text: qsTr("Copy")
                        iconGlyph: Icons.copy
                        onClicked: root.copyOf(loginEdit.text)

                        Accessible.name: qsTr("Copy the sign-in command")
                    }
                }

                QButton {
                    id: settingsBtn

                    visible: root.chat !== null && root.chat.state === "error" && root.chat.errorAction === "settings"
                    text: qsTr("Open settings")
                    iconGlyph: Icons.settings
                    onClicked: root.settingsRequested("app")
                }

                // The CLI's own text, sanitized: collapsed, selectable, copyable.
                QButton {
                    id: detailsBtn

                    visible: root.chat !== null && root.chat.state === "error" && root.chat.errorDetail !== ""
                    variant: "flat"
                    text: qsTr("Details")
                    iconGlyph: root.detailsOpen ? Icons.chevronDown : Icons.chevronRight
                    onClicked: root.detailsOpen = !root.detailsOpen

                    Accessible.expandable: true
                    Accessible.expanded: root.detailsOpen
                }

                TextEdit {
                    id: detailsEdit

                    Layout.fillWidth: true
                    visible: detailsBtn.visible && root.detailsOpen
                    activeFocusOnTab: visible
                    readOnly: true
                    selectByMouse: true
                    text: root.chat ? root.chat.errorDetail : ""
                    wrapMode: TextEdit.WrapAnywhere
                    font.family: Theme.monoFontFamily
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    selectionColor: Theme.selectionBg
                    selectedTextColor: Theme.selectionFg

                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Details")
                    Accessible.readOnly: true
                    Accessible.focusable: true

                    FocusRing {
                        shown: detailsEdit.activeFocus
                    }
                }

                QButton {
                    visible: detailsEdit.visible
                    text: qsTr("Copy details")
                    onClicked: root.copyOf(detailsEdit.text)
                }

                QButton {
                    id: reviewBtn

                    visible: root.chat !== null && root.chat.state === "done" && root.chat.diff !== null
                    text: qsTr("Review changes")
                    iconGlyph: Icons.fileDiff
                    onClicked: root.reviewRequested()

                    Accessible.description: qsTr("Goes to the review of the proposal, over the editor")
                }
            }
        }

        // ---- what was sent ----
        QButton {
            id: contextBtn

            Layout.fillWidth: true
            variant: "flat"
            text: qsTr("Context sent") + (root.chat && root.chat.contexts.count > 0 ? " (" + root.chat.contexts.count + ")" : "")
            iconGlyph: root.contextOpen ? Icons.chevronDown : Icons.chevronRight
            onClicked: root.contextOpen = !root.contextOpen

            Accessible.expandable: true
            Accessible.expanded: root.contextOpen
            Accessible.name: qsTr("Context sent")
        }

        Flickable {
            id: contextBody

            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contextColumn.implicitHeight, Theme.controlHeight * 9)
            visible: root.contextOpen
            clip: true
            contentWidth: width
            contentHeight: contextColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: contextColumn

                width: contextBody.width
                spacing: Theme.spacing

                Text {
                    id: notSentText

                    Layout.fillWidth: true
                    text: qsTr("Not sent: rows, results or connection details.")
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                Text {
                    Layout.fillWidth: true
                    visible: root.chat === null || root.chat.contexts.count === 0
                    text: qsTr("Nothing was sent yet. Each request will be listed here with exactly what it sent.")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible: root.ai !== null && root.ai.keepLog
                    text: qsTr("A copy of each request is kept in a local log: %1").arg(root.ai ? root.ai.logFile : "")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WrapAnywhere
                }

                QButton {
                    visible: root.ai !== null && root.ai.keepLog
                    text: qsTr("Open log folder")
                    onClicked: root.ai.openLogFolder()
                }

                Repeater {
                    model: root.chat ? root.chat.contexts.count : 0

                    ColumnLayout {
                        id: entryBox

                        required property int index
                        // Newest first.
                        readonly property int row: root.chat.contexts.count - 1 - index
                        readonly property var entry: root.chat.contexts.get(row)

                        Layout.fillWidth: true
                        spacing: Theme.spacing / 2

                        Heading {
                            Layout.fillWidth: true
                            level: 3
                            text: qsTr("Request %1").arg(entryBox.row + 1)
                        }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Tables (%1): %2").arg(entryBox.entry.tables.length).arg(entryBox.entry.tablesText !== "" ? entryBox.entry.tablesText : qsTr("none"))
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            wrapMode: Text.WrapAnywhere
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: entryBox.entry.note !== ""
                            text: entryBox.entry.note
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            Layout.fillWidth: true
                            text: entryBox.entry.historyCount === 1 ? qsTr("1 earlier message sent") : qsTr("%1 earlier messages sent").arg(entryBox.entry.historyCount)
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            wrapMode: Text.WordWrap
                        }

                        ContextBlock {
                            label: qsTr("Table structure (DDL)")
                            body: entryBox.entry.ddl
                        }

                        ContextBlock {
                            label: qsTr("Script, with literals masked as set for the connection")
                            body: entryBox.entry.script
                        }

                        ContextBlock {
                            label: qsTr("Error of the last run, sanitized")
                            body: entryBox.entry.errorText
                            visible: entryBox.entry.errorText !== ""
                        }

                        QButton {
                            text: qsTr("Copy what was sent")
                            iconGlyph: Icons.copy
                            onClicked: root.copyOf(root.chat.contextText(entryBox.row))

                            Accessible.name: qsTr("Copy what request %1 sent").arg(entryBox.row + 1)
                        }
                    }
                }
            }
        }

        // ---- the field ----
        Text {
            id: askLabel

            Layout.fillWidth: true
            text: qsTr("Ask Claude")
            font.pointSize: Theme.fontSize
            font.bold: true
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        Rectangle {
            id: inputFrame

            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight * 3
            color: Theme.bgCanvas
            radius: Theme.radius
            border.width: 1
            border.color: Theme.borderStrong
            opacity: input.enabled ? 1 : 0.6

            Flickable {
                id: inputFlick

                anchors.fill: parent
                anchors.margins: Theme.spacing
                clip: true
                contentWidth: width
                contentHeight: input.contentHeight
                boundsBehavior: Flickable.StopAtBounds

                TextEdit {
                    id: input

                    width: inputFlick.width
                    height: Math.max(inputFlick.height, contentHeight)
                    enabled: root.chat !== null && root.chat.unavailableKind === ""
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    selectionColor: Theme.selectionBg
                    selectedTextColor: Theme.selectionFg
                    selectByMouse: true
                    activeFocusOnTab: true
                    persistentSelection: true

                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Ask Claude")
                    Accessible.description: qsTr("Enter adds a line. %1 sends.").arg(CommandRegistry.displayShortcut("Ctrl+Enter"))
                    Accessible.focusable: true
                    Accessible.multiLine: true

                    onTextChanged: {
                        if (root.chat && root.chat.draft !== text)
                            root.chat.draft = text;
                    }
                    onCursorRectangleChanged: {
                        const r = cursorRectangle;
                        if (r.y < inputFlick.contentY)
                            inputFlick.contentY = r.y;
                        else if (r.y + r.height > inputFlick.contentY + inputFlick.height)
                            inputFlick.contentY = r.y + r.height - inputFlick.height;
                    }

                    // Ctrl+Enter is also "run the statement" of the window: here it sends.
                    Keys.onShortcutOverride: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier))
                            event.accepted = true;
                    }
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) {
                            root.submit();
                            event.accepted = true;
                        }
                    }
                    // Tab leaves the field (WCAG 2.1.2: no keyboard trap).
                    Keys.onTabPressed: event => {
                        const next = input.nextItemInFocusChain(true);
                        if (next)
                            next.forceActiveFocus(Qt.TabFocusReason);
                        event.accepted = true;
                    }
                    Keys.onBacktabPressed: event => {
                        const prev = input.nextItemInFocusChain(false);
                        if (prev)
                            prev.forceActiveFocus(Qt.BacktabFocusReason);
                        event.accepted = true;
                    }

                    Text {
                        visible: input.text === "" && input.preeditText === ""
                        text: qsTr("Ask Claude about this script…")
                        font: input.font
                        color: Theme.fgMuted
                        Accessible.ignored: true
                    }
                }
            }

            FocusRing {
                shown: input.activeFocus
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing

            QButton {
                id: sendBtn

                visible: root.chat === null || !root.chat.busy
                text: qsTr("Send")
                variant: "primary"
                iconGlyph: Icons.send
                enabled: root.chat !== null && root.chat.canSend && input.text.trim() !== ""
                onClicked: root.submit()

                Accessible.description: CommandRegistry.displayShortcut("Ctrl+Enter")
            }

            QButton {
                id: cancelBtn

                visible: root.chat !== null && root.chat.busy
                text: qsTr("Cancel")
                iconGlyph: Icons.square
                onClicked: root.chat.cancel()

                Accessible.description: qsTr("Stops the request. Esc does the same.")
            }

            QButton {
                id: clearBtn

                text: qsTr("Clear chat")
                iconGlyph: Icons.trash2
                enabled: root.chat !== null && (root.chat.messages.count > 0 || root.chat.contexts.count > 0 || root.chat.state === "error")
                onClicked: root.chat.clearChat()

                Accessible.description: qsTr("Forgets the conversation and the list of what was sent")
            }
        }
    }

    // One labelled, selectable block of what was sent.
    component ContextBlock: ColumnLayout {
        id: blockRoot

        property string label: ""
        property string body: ""

        Layout.fillWidth: true
        spacing: Theme.spacing / 2

        Text {
            Layout.fillWidth: true
            text: blockRoot.label
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WordWrap
        }

        TextEdit {
            id: blockText

            Layout.fillWidth: true
            activeFocusOnTab: visible
            readOnly: true
            selectByMouse: true
            text: blockRoot.body !== "" ? blockRoot.body : qsTr("(empty)")
            wrapMode: TextEdit.WrapAnywhere
            font.family: Theme.monoFontFamily
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            selectionColor: Theme.selectionBg
            selectedTextColor: Theme.selectionFg

            Accessible.role: Accessible.EditableText
            Accessible.name: blockRoot.label
            Accessible.readOnly: true
            Accessible.focusable: true
            Accessible.multiLine: true

            FocusRing {
                shown: blockText.activeFocus
            }
        }
    }

    Keys.onEscapePressed: event => {
        if (root.chat && root.chat.busy)
            root.chat.cancel();
        else
            root.focusEditorRequested();
        event.accepted = true;
    }
}
