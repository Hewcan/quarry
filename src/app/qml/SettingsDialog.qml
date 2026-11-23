pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Every change applies at once (Theme persists its own settings). Esc closes it.
QDialog {
    id: dialog

    // Combo bound to a setting by index. Translating the model resets a combo, so it re-binds.
    component OptionCombo: QComboBox {
        id: combo

        property int value: 0

        signal picked(int index)

        Layout.fillWidth: true
        currentIndex: value
        onModelChanged: currentIndex = Qt.binding(() => combo.value)
        onActivated: index => picked(index)
    }

    // One native tool of "Database tools" (H14): its path, Browse and Test, and the result of the
    // test (icon and text, also announced).
    component ToolRow: ColumnLayout {
        id: tool

        required property string toolName // "pg_dump" | "pg_restore" | "psql"
        required property string path
        property var result: null
        readonly property alias pathField: field
        readonly property alias testButton: test
        readonly property alias browseButton: browse
        readonly property alias statusText: status

        signal pathEdited(string text)
        signal browseRequested

        function runTest() {
            pathEdited(field.text);
            result = dialog.dump.testTool(toolName);
            const text = result.ok ? qsTr("%1 found: %2").arg(toolName).arg(result.message) : result.message;
            dialog.lastAnnouncement = text;
            status.Accessible.announce(text, result.ok ? Accessible.Polite : Accessible.Assertive);
        }

        Layout.fillWidth: true
        spacing: Theme.spacing

        Text {
            id: label

            Layout.fillWidth: true
            text: tool.toolName
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            QTextField {
                id: field

                Layout.fillWidth: true
                text: tool.path
                accessibleName: label.text
                placeholderText: qsTr("Empty: look for it automatically")
                onEditingFinished: tool.pathEdited(text)
                onAccepted: tool.runTest()

                Accessible.description: qsTr("Leave it empty to look in the PATH and the usual install folders.")
            }

            QButton {
                id: browse

                text: qsTr("Browse…")
                onClicked: tool.browseRequested()

                Accessible.name: qsTr("Browse… (%1)").arg(label.text)
            }

            QButton {
                id: test

                text: qsTr("Test")
                onClicked: tool.runTest()

                Accessible.name: qsTr("Test (%1)").arg(label.text)
                Accessible.description: qsTr("Looks for the program. Nothing is run.")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: tool.result !== null
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: tool.result && tool.result.ok ? Icons.circleCheck : Icons.circleAlert
                color: tool.result && tool.result.ok ? Theme.success : Theme.danger
            }

            Text {
                id: status

                Layout.fillWidth: true
                text: tool.result ? (tool.result.ok ? qsTr("Found: %1").arg(tool.result.message) : tool.result.message) : ""
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                wrapMode: Text.WrapAnywhere

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }
    }

    // CompletionController: the "Suggest while typing" setting.
    property var completion: null
    // AiController (H13): where Claude Code is. Optional: without it the section is hidden.
    property var ai: null
    // DumpController (H14): where pg_dump, pg_restore and psql are. Optional: without it the
    // section is hidden.
    property var dump: null
    // Which field the file picker is filling: "cli" or the tool's name.
    property string pickFor: "cli"
    property bool detailsOpen: false
    // The last thing sent to the screen reader (tests read it; it is not shown).
    property string lastAnnouncement: ""

    readonly property string cliState: ai ? ai.cliState : "unknown"
    readonly property bool cliShown: ai !== null && cliState !== "unknown"
    readonly property alias cliPathField: cliPathField
    readonly property alias cliTestButton: cliTest
    readonly property alias cliBrowseButton: cliBrowse
    readonly property alias cliStatusText: cliStatusText
    readonly property alias cliDetailsButton: cliDetailsButton
    readonly property alias aiSection: aiSection
    readonly property alias keepLogBox: keepLogBox
    readonly property alias logPathText: logPathText
    readonly property alias openLogFolderButton: openLogFolder
    readonly property alias toolsSection: toolsSection
    readonly property alias pgDumpRow: pgDumpRow
    readonly property alias pgRestoreRow: pgRestoreRow
    readonly property alias psqlRow: psqlRow

    // What is typed in the field is the path (trimmed by the controller) before anything runs.
    function applyCliPath() {
        if (ai && ai.cliPath !== cliPathField.text.trim())
            ai.cliPath = cliPathField.text;
    }

    function runCliTest() {
        if (!ai || ai.cliState === "checking")
            return;
        applyCliPath();
        detailsOpen = false;
        ai.testCli();
    }

    // Keeps the focused control inside the scrolled area.
    function reveal(item) {
        if (!item)
            return;
        let inside = false;
        for (let p = item; p; p = p.parent)
            if (p === flick.contentItem) {
                inside = true;
                break;
            }
        if (!inside)
            return;
        const top = item.mapToItem(flick.contentItem, 0, 0).y;
        const bottom = top + item.height;
        if (top < flick.contentY)
            flick.contentY = Math.max(0, top - Theme.spacing);
        else if (bottom > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.contentHeight - flick.height, bottom - flick.height + Theme.spacing);
    }

    function pickTool(name) {
        pickFor = name;
        pickerLoader.active = true;
        // qmllint disable missing-property
        pickerLoader.item["purpose"] = "pgtool";
        // qmllint enable missing-property
        pickerLoader.item.open();
    }

    function pickCli() {
        pickFor = "cli";
        pickerLoader.active = true;
        // qmllint disable missing-property
        pickerLoader.item["purpose"] = "claude";
        // qmllint enable missing-property
        pickerLoader.item.open();
    }

    title: qsTr("Settings")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onOpened: themeCombo.forceActiveFocus(Qt.TabFocusReason)
    // A test in flight is dropped with the dialog: its answer would have nowhere to go.
    onAboutToHide: {
        if (ai)
            ai.cancelCli();
    }

    // Created on first use: the platform file dialog is not needed to show the settings. By URL,
    // not as a type: naming FilePicker would load QtQuick.Dialogs with the dialog.
    Loader {
        id: pickerLoader

        active: false
        source: "FilePicker.qml"
    }

    Connections {
        target: pickerLoader.item

        function onAccepted() {
            // qmllint disable missing-property
            if (dialog.pickFor === "cli")
                dialog.ai.cliPath = dialog.ai.toLocalPath(pickerLoader.item["selectedFile"]);
            else if (dialog.pickFor === "pg_dump")
                dialog.dump.pgDumpPath = dialog.dump.toLocalPath(pickerLoader.item["selectedFile"]);
            else if (dialog.pickFor === "pg_restore")
                dialog.dump.pgRestorePath = dialog.dump.toLocalPath(pickerLoader.item["selectedFile"]);
            else if (dialog.pickFor === "psql")
                dialog.dump.psqlPath = dialog.dump.toLocalPath(pickerLoader.item["selectedFile"]);
            // qmllint enable missing-property
        }
    }

    Connections {
        target: flick.Window.window

        function onActiveFocusItemChanged() {
            dialog.reveal(flick.Window.window.activeFocusItem);
        }
    }

    Connections {
        target: dialog.ai
        enabled: dialog.visible

        function onCliChanged() {
            const state = dialog.ai.cliState;
            if (state === "unknown")
                return;
            dialog.lastAnnouncement = dialog.ai.cliMessage;
            cliStatusText.Accessible.announce(dialog.ai.cliMessage, state === "ok" || state === "checking" ? Accessible.Polite : Accessible.Assertive);
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        // Scrolls when the settings are taller than the window (200 % text size): Close stays in view.
        Flickable {
            id: flick

            Layout.fillWidth: true
            Layout.preferredWidth: form.implicitWidth
            Layout.preferredHeight: Math.min(form.implicitHeight, Math.max(Theme.controlHeight * 6, (dialog.parent ? dialog.parent.height : 600) - Theme.controlHeight * 8))
            contentWidth: width
            contentHeight: form.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: form

                width: flick.width
                spacing: Theme.spacing * 3

                GridLayout {
                    columns: 2
                    columnSpacing: Theme.spacing * 3
                    rowSpacing: Theme.spacing * 2

                    Text {
                        id: themeLabel

                        text: qsTr("Theme")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    OptionCombo {
                        id: themeCombo

                        accessibleName: themeLabel.text
                        model: [qsTr("System"), qsTr("Light"), qsTr("Dark"), qsTr("High contrast")]
                        value: Theme.mode
                        onPicked: index => Theme.mode = index
                    }

                    Text {
                        id: densityLabel

                        text: qsTr("Density")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    OptionCombo {
                        accessibleName: densityLabel.text
                        model: [qsTr("Compact"), qsTr("Comfortable")]
                        value: Theme.density
                        onPicked: index => Theme.density = index
                    }

                    Text {
                        id: zoomLabel

                        text: qsTr("Text size")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        spacing: Theme.spacing

                        IconButton {
                            accessibleName: qsTr("Decrease text size")
                            iconGlyph: Icons.minus
                            onClicked: Theme.zoomOut()
                        }

                        Text {
                            text: qsTr("%1 %").arg(Math.round(Theme.zoom * 100))
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            horizontalAlignment: Text.AlignHCenter
                            Layout.minimumWidth: Theme.controlHeight * 2

                            Accessible.role: Accessible.StaticText
                            Accessible.name: zoomLabel.text + ": " + text
                        }

                        IconButton {
                            accessibleName: qsTr("Increase text size")
                            iconGlyph: Icons.plus
                            onClicked: Theme.zoomIn()
                        }

                        QButton {
                            text: qsTr("Reset")
                            onClicked: Theme.resetZoom()
                        }
                    }

                    Text {
                        id: animationsLabel

                        text: qsTr("Animations")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    OptionCombo {
                        accessibleName: animationsLabel.text
                        model: [qsTr("System"), qsTr("On"), qsTr("Off")]
                        value: Theme.animations
                        onPicked: index => Theme.animations = index
                    }

                    Text {
                        id: languageLabel

                        text: qsTr("Language")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    OptionCombo {
                        accessibleName: languageLabel.text
                        model: [qsTr("System"), "English", "Español"]
                        value: Language.choice
                        onPicked: index => Language.choice = index
                    }

                    QCheckBox {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: qsTr("Suggest while typing")
                        enabled: dialog.completion !== null
                        checked: dialog.completion ? dialog.completion.autoOpen : true
                        onToggled: dialog.completion.autoOpen = checked
                    }
                }

                // H13: where the user's own Claude Code is. Nothing here runs until Test is pressed.
                ColumnLayout {
                    id: aiSection

                    Layout.fillWidth: true
                    Layout.preferredWidth: Theme.controlHeight * 18
                    visible: dialog.ai !== null
                    spacing: Theme.spacing

                    Heading {
                        Layout.fillWidth: true
                        level: 3
                        text: qsTr("AI assistant")
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Uses the Claude Code you already have installed. Nothing is sent by Quarry itself; Claude Code sends your request to Anthropic. Usage counts against your Claude subscription or your API key.")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        id: cliLabel

                        Layout.fillWidth: true
                        text: qsTr("Claude Code executable")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing

                        QTextField {
                            id: cliPathField

                            Layout.fillWidth: true
                            text: dialog.ai ? dialog.ai.cliPath : ""
                            accessibleName: cliLabel.text
                            placeholderText: qsTr("Empty: look for it automatically")
                            onEditingFinished: dialog.applyCliPath()
                            onAccepted: dialog.runCliTest()

                            Accessible.description: qsTr("Leave it empty to look for Claude Code in the usual places.")
                        }

                        QButton {
                            id: cliBrowse

                            text: qsTr("Browse…")
                            onClicked: dialog.pickCli()

                            Accessible.name: qsTr("Browse… (%1)").arg(cliLabel.text)
                        }

                        QButton {
                            id: cliTest

                            text: qsTr("Test")
                            enabled: dialog.ai !== null && dialog.ai.cliState !== "checking"
                            onClicked: dialog.runCliTest()

                            Accessible.description: qsTr("Runs Claude Code with --version, which does not use your quota.")
                        }
                    }

                    // The result: icon and text (never color alone), also announced when it arrives.
                    RowLayout {
                        id: cliStatus

                        Layout.fillWidth: true
                        visible: dialog.cliShown
                        spacing: Theme.spacing

                        Accessible.role: Accessible.StaticText
                        Accessible.name: cliStatusText.text

                        Icon {
                            id: cliStatusIcon

                            Layout.alignment: Qt.AlignTop
                            text: dialog.cliState === "ok" ? Icons.circleCheck : dialog.cliState === "checking" ? Icons.loaderCircle : dialog.cliState === "incompatible" ? Icons.triangleAlert : Icons.circleAlert
                            color: dialog.cliState === "ok" ? Theme.success : dialog.cliState === "checking" ? Theme.fgMuted : dialog.cliState === "incompatible" ? Theme.warning : Theme.danger

                            RotationAnimator on rotation {
                                running: dialog.cliState === "checking" && Theme.animationDuration > 0
                                from: 0
                                to: 360
                                duration: 1000
                                loops: Animation.Infinite
                            }
                        }

                        Text {
                            id: cliStatusText

                            Layout.fillWidth: true
                            text: dialog.ai ? dialog.ai.cliMessage : ""
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            wrapMode: Text.WrapAnywhere
                        }
                    }

                    // "Details" of a failure: collapsed, selectable and copyable.
                    QButton {
                        id: cliDetailsButton

                        visible: dialog.cliShown && dialog.ai.cliDetail !== ""
                        variant: "flat"
                        text: qsTr("Details")
                        iconGlyph: dialog.detailsOpen ? Icons.chevronDown : Icons.chevronRight
                        onClicked: dialog.detailsOpen = !dialog.detailsOpen

                        Accessible.expandable: true
                        Accessible.expanded: dialog.detailsOpen
                    }

                    TextEdit {
                        id: cliDetailsText

                        Layout.fillWidth: true
                        visible: cliDetailsButton.visible && dialog.detailsOpen
                        activeFocusOnTab: visible
                        readOnly: true
                        selectByMouse: true
                        text: dialog.ai ? dialog.ai.cliDetail : ""
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
                            shown: cliDetailsText.activeFocus
                        }
                    }

                    QButton {
                        id: cliCopyDetails

                        visible: cliDetailsText.visible
                        text: qsTr("Copy details")
                        onClicked: {
                            cliDetailsText.selectAll();
                            cliDetailsText.copy();
                            cliDetailsText.deselect();
                        }
                    }

                    // The local record of what each request sent: the same text the chat's
                    // "Context sent" panel shows, nothing else. Off unless the user turns it on.
                    QCheckBox {
                        id: keepLogBox

                        Layout.fillWidth: true
                        text: qsTr("Keep a local log of what is sent")
                        enabled: dialog.ai !== null
                        checked: dialog.ai ? dialog.ai.keepLog : false
                        onToggled: dialog.ai.keepLog = checked

                        Accessible.description: qsTr("Writes what each request sends to a file on this computer. Off by default.")
                    }

                    Text {
                        id: logPathText

                        Layout.fillWidth: true
                        visible: keepLogBox.checked
                        text: qsTr("Log file: %1").arg(dialog.ai ? dialog.ai.logFile : "")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WrapAnywhere
                    }

                    QButton {
                        id: openLogFolder

                        visible: keepLogBox.checked
                        text: qsTr("Open log folder")
                        onClicked: dialog.ai.openLogFolder()
                    }
                }

                // H14: the native tools of Dump and Restore. Nothing runs from here: Test only
                // looks for the program.
                ColumnLayout {
                    id: toolsSection

                    Layout.fillWidth: true
                    Layout.preferredWidth: Theme.controlHeight * 18
                    visible: dialog.dump !== null
                    spacing: Theme.spacing

                    Heading {
                        Layout.fillWidth: true
                        level: 3
                        text: qsTr("Database tools")
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Dump and Restore of PostgreSQL use the programs that come with it. Leave a path empty to look for it in the PATH and the usual install folders.")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WordWrap
                    }

                    ToolRow {
                        id: pgDumpRow

                        toolName: "pg_dump"
                        path: dialog.dump ? dialog.dump.pgDumpPath : ""
                        onPathEdited: text => dialog.dump.pgDumpPath = text
                        onBrowseRequested: dialog.pickTool("pg_dump")
                    }

                    ToolRow {
                        id: pgRestoreRow

                        toolName: "pg_restore"
                        path: dialog.dump ? dialog.dump.pgRestorePath : ""
                        onPathEdited: text => dialog.dump.pgRestorePath = text
                        onBrowseRequested: dialog.pickTool("pg_restore")
                    }

                    ToolRow {
                        id: psqlRow

                        toolName: "psql"
                        path: dialog.dump ? dialog.dump.psqlPath : ""
                        onPathEdited: text => dialog.dump.psqlPath = text
                        onBrowseRequested: dialog.pickTool("psql")
                    }
                }
            }
        }

        QButton {
            text: qsTr("Close")
            variant: "primary"
            Layout.alignment: Qt.AlignRight
            onClicked: dialog.close()
        }
    }
}
