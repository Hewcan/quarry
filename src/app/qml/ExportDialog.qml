pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// "Export..." (H5, UX.md "Exportacion"): format, the options that format has, which rows, and the
// file. Then the same dialog shows the progress (rows written, Cancel) and the result. Esc cancels
// a running export and closes the dialog once it has ended. The writing runs on the Exporter's
// thread; nothing here waits for it. Announcements are only the start, the end, a cancellation and
// an error: the counter on screen updates all the time but is never announced.
QDialog {
    id: root

    required property var controller // ExportController
    property string tabId: ""
    property var info: ({})
    property int formatIndex: 0
    property string startError: ""
    property string lastAnnouncement: ""
    readonly property var formats: root.controller.formats()
    readonly property string format: root.formats[root.formatIndex]?.name ?? ""
    readonly property string stateName: root.controller.state
    readonly property bool formShown: root.stateName === "idle"
    readonly property bool busy: root.controller.busy
    readonly property bool isCsv: root.formatIndex === 0
    readonly property bool isTsv: root.formatIndex === 1
    readonly property bool isSql: root.formatIndex === 4
    readonly property bool isMarkdown: root.formatIndex === 5
    readonly property bool hasHeaderOption: root.isCsv || root.isTsv || root.isMarkdown
    readonly property var scopes: root.buildScopes(root.info)
    readonly property string scope: root.scopes[scopeCombo.currentIndex]?.value ?? "current"
    readonly property alias formatList: formatList
    readonly property alias delimiterCombo: delimiterCombo
    readonly property alias headerBox: headerBox
    readonly property alias bomBox: bomBox
    readonly property alias tableField: tableField
    readonly property alias rowsField: rowsField
    readonly property alias scopeCombo: scopeCombo
    readonly property alias pathField: pathField
    readonly property alias browseButton: browseBtn
    readonly property alias exportButton: exportBtn
    readonly property alias cancelButton: cancelBtn
    readonly property alias stopButton: stopBtn
    readonly property alias closeButton: closeBtn
    readonly property alias backButton: backBtn
    readonly property alias folderButton: folderBtn
    readonly property alias replaceDialog: replaceDialog
    readonly property alias errorPanel: failure
    readonly property alias progressLabel: progressText
    readonly property alias flick: flick
    readonly property alias picker: pickerLoader

    function buildScopes(i) {
        const list = [];
        const rows = Number(i.loadedRows ?? 0).toLocaleString(Qt.locale(), "f", 0);
        if (i.currentAllowed)
            list.push({
                text: qsTr("Current result (rows loaded: %1)").arg(rows),
                value: "current"
            });
        if (i.queryAllowed)
            list.push({
                text: qsTr("Run the query again (all rows)"),
                value: "query"
            });
        return list;
    }

    function announce(text, assertive) {
        lastAnnouncement = text;
        body.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    // Opens the form for a tab. An export still running (it cannot be: the dialog is modal) keeps
    // showing its progress.
    function openFor(id) {
        tabId = id;
        info = controller.infoFor(id);
        controller.reset();
        const saved = Number(LayoutStore.value("exportFormat", 0));
        formatIndex = saved >= 0 && saved < formats.length ? saved : 0;
        startError = "";
        pathField.text = controller.defaultPath(id, formatIndex);
        tableField.text = info.defaultTable ?? "";
        rowsField.text = "1";
        delimiterCombo.currentIndex = 0;
        headerBox.checked = true;
        bomBox.checked = false;
        scopeCombo.currentIndex = 0;
        open();
    }

    function submit() {
        startError = "";
        if (!root.formShown)
            return;
        const path = pathField.text.trim();
        if (controller.fileExists(path)) {
            replaceDialog.message = qsTr("“%1” already exists. The old file is only replaced when the export finishes without errors.").arg(path);
            replaceDialog.open();
        } else {
            begin();
        }
    }

    function begin() {
        const delimiters = [",", ";", "\t", "|"];
        const error = controller.start({
            tabId: tabId,
            scope: root.scope,
            format: formatIndex,
            filePath: pathField.text,
            delimiter: delimiters[delimiterCombo.currentIndex] ?? ",",
            header: headerBox.checked,
            bom: bomBox.checked,
            tableName: tableField.text,
            rowsPerInsert: parseInt(rowsField.text) || 1
        });
        startError = error;
        if (error !== "") {
            announce(error, true);
            return;
        }
        LayoutStore.setValue("exportFormat", formatIndex);
        Qt.callLater(root.focusProgress);
    }

    function focusProgress() {
        if (root.busy)
            stopBtn.forceActiveFocus(Qt.TabFocusReason);
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

    function chooseFile() {
        pickerLoader.active = true;
        const picker = pickerLoader.item;
        const f = formats[formatIndex];
        picker.nameFilters = [f.filter, qsTr("All files (*)")];
        picker.defaultSuffix = f.extension;
        picker.currentFolder = controller.folderUrl(pathField.text);
        picker.selectedFile = controller.fileUrl(pathField.text);
        picker.open();
    }

    title: qsTr("Export")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)
    // While it runs, Esc is the Cancel key (UX.md: the same key everywhere), not "close".
    closePolicy: root.busy ? T.Popup.NoAutoClose : T.Popup.CloseOnEscape

    onFormatIndexChanged: {
        if (pathField.text !== "")
            pathField.text = controller.retarget(pathField.text, formatIndex);
    }
    onOpened: {
        if (root.formShown)
            formatList.forceActiveFocus(Qt.TabFocusReason);
    }
    onClosed: {
        if (root.busy)
            controller.cancel();
    }

    // Done, failed or cancelled: the buttons of the form are gone, so the focus moves to the one
    // that continues. The announcement itself comes from the controller.
    Connections {
        target: root.controller

        function onAnnouncement(text, assertive) {
            root.announce(text, assertive);
        }

        function onStateChanged() {
            if (!root.visible)
                return;
            Qt.callLater(() => {
                if (root.stateName === "done")
                    closeBtn.forceActiveFocus(Qt.TabFocusReason);
                else if (root.stateName === "failed" || root.stateName === "cancelled")
                    backBtn.forceActiveFocus(Qt.TabFocusReason);
                else if (root.stateName === "running" || root.stateName === "preparing")
                    root.focusProgress();
            });
        }
    }

    Connections {
        target: flick.Window.window

        function onActiveFocusItemChanged() {
            root.reveal(flick.Window.window.activeFocusItem);
        }
    }

    // The file picker is built the first time it is needed.
    Loader {
        id: pickerLoader

        active: false
        source: "ExportFilePicker.qml"
    }

    Connections {
        target: pickerLoader.item

        function onAccepted() {
            // qmllint disable missing-property
            pathField.text = root.controller.toLocalPath(pickerLoader.item["selectedFile"]);
            // qmllint enable missing-property
            pathField.forceActiveFocus(Qt.TabFocusReason);
        }
    }

    ConfirmActionDialog {
        id: replaceDialog

        title: qsTr("Replace file?")
        confirmText: qsTr("Replace")
        onConfirmed: root.begin()
    }

    contentItem: ColumnLayout {
        id: body

        spacing: Theme.spacing * 2

        Keys.onEscapePressed: event => {
            // Cancels what runs; once it has ended the event goes on and the dialog closes.
            if (root.busy) {
                root.controller.cancel();
                event.accepted = true;
            } else {
                event.accepted = false;
            }
        }

        // ---- the form -----------------------------------------------------------------------

        Flickable {
            id: flick

            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 22
            Layout.preferredHeight: Math.min(form.implicitHeight, Math.max(Theme.controlHeight * 6, (root.parent ? root.parent.height : 600) - Theme.controlHeight * 8))
            visible: root.formShown
            contentWidth: width
            contentHeight: form.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: form

                width: flick.width
                spacing: Theme.spacing * 2

                Text {
                    Layout.fillWidth: true
                    text: root.info.connectionName ? qsTr("Export the result of “%1” (connection %2).").arg(root.info.tabTitle ?? "").arg(root.info.connectionName) : qsTr("Export the result of “%1”.").arg(root.info.tabTitle ?? "")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap
                }

                Text {
                    id: formatLabel

                    text: qsTr("Format")
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                }

                // One choice: Up and Down move it, Home and End jump.
                ListView {
                    id: formatList

                    readonly property var hints: [qsTr("For spreadsheets"), qsTr("Tab-separated text"), qsTr("One array of objects"), qsTr("One object per line"), qsTr("INSERT statements"), qsTr("A table in text")]

                    Layout.fillWidth: true
                    Layout.preferredHeight: contentHeight
                    interactive: false
                    model: root.formats
                    currentIndex: root.formatIndex
                    onCurrentIndexChanged: {
                        if (currentIndex >= 0)
                            root.formatIndex = currentIndex;
                    }
                    activeFocusOnTab: true
                    keyNavigationEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_Home) {
                            currentIndex = 0;
                            event.accepted = true;
                        } else if (event.key === Qt.Key_End) {
                            currentIndex = count - 1;
                            event.accepted = true;
                        }
                    }

                    Accessible.role: Accessible.List
                    Accessible.name: formatLabel.text
                    Accessible.focusable: true

                    delegate: Rectangle {
                        id: formatRow

                        required property var modelData
                        required property int index

                        width: ListView.view.width
                        height: Theme.controlHeight
                        color: ListView.isCurrentItem ? Theme.selectionBg : Qt.alpha(Theme.bgRaised, 0)

                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData.name + ", " + formatList.hints[index]
                        Accessible.selected: ListView.isCurrentItem

                        // The selection is the check mark and the accent bar, not only the fill.
                        Rectangle {
                            visible: formatRow.ListView.isCurrentItem
                            width: Theme.focusRingWidth
                            height: parent.height
                            color: Theme.accent
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacing * 2
                            anchors.rightMargin: Theme.spacing * 2
                            spacing: Theme.spacing * 2

                            Icon {
                                text: Icons.check
                                color: Theme.selectionFg
                                opacity: formatRow.ListView.isCurrentItem ? 1 : 0
                            }

                            Text {
                                text: formatRow.modelData.name
                                font.pointSize: Theme.fontSize
                                font.bold: formatRow.ListView.isCurrentItem
                                color: formatRow.ListView.isCurrentItem ? Theme.selectionFg : Theme.fgDefault
                            }

                            Text {
                                Layout.fillWidth: true
                                text: formatList.hints[formatRow.index]
                                font.pointSize: Theme.fontSize
                                color: formatRow.ListView.isCurrentItem ? Theme.selectionFg : Theme.fgMuted
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                formatList.currentIndex = formatRow.index;
                                formatList.forceActiveFocus(Qt.MouseFocusReason);
                            }
                        }
                    }

                    FocusRing {
                        shown: formatList.activeFocus
                        inset: true
                    }
                }

                // Only the options the chosen format has.
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.spacing * 3
                    rowSpacing: Theme.spacing * 2

                    Text {
                        id: delimiterLabel

                        visible: root.isCsv
                        text: qsTr("Separator")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QComboBox {
                        id: delimiterCombo

                        Layout.fillWidth: true
                        visible: root.isCsv
                        accessibleName: delimiterLabel.text
                        model: [qsTr("Comma (,)"), qsTr("Semicolon (;)"), qsTr("Tab"), qsTr("Vertical bar (|)")]
                    }

                    Text {
                        visible: root.hasHeaderOption
                        text: qsTr("Column names")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QCheckBox {
                        id: headerBox

                        visible: root.hasHeaderOption
                        text: qsTr("First row holds the column names")
                        checked: true
                    }

                    Text {
                        visible: root.isCsv || root.isTsv
                        text: qsTr("Encoding")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QCheckBox {
                        id: bomBox

                        visible: root.isCsv || root.isTsv
                        text: qsTr("Add a UTF-8 byte order mark (BOM)")
                    }

                    Text {
                        id: tableLabel

                        visible: root.isSql
                        text: qsTr("Table name")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QTextField {
                        id: tableField

                        Layout.fillWidth: true
                        visible: root.isSql
                        accessibleName: tableLabel.text
                    }

                    Text {
                        id: rowsLabel

                        visible: root.isSql
                        text: qsTr("Rows per INSERT (1 to 1000)")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                    }

                    QTextField {
                        id: rowsField

                        Layout.fillWidth: true
                        visible: root.isSql
                        accessibleName: rowsLabel.text
                        // 1 to 1000: IntValidator would accept 5000 as an intermediate value.
                        validator: RegularExpressionValidator {
                            regularExpression: /^(1000|[1-9][0-9]{0,2})?$/
                        }
                        inputMethodHints: Qt.ImhDigitsOnly
                    }
                }

                Text {
                    id: scopeLabel

                    text: qsTr("Rows to export")
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                }

                QComboBox {
                    id: scopeCombo

                    Layout.fillWidth: true
                    accessibleName: scopeLabel.text
                    model: root.scopes
                    textRole: "text"
                    enabled: root.scopes.length > 1
                }

                // Why a choice is missing, and what the current result does not hold.
                Text {
                    id: scopeNote

                    readonly property string reason: root.scope === "query" ? "" : root.scopes.length === 0 ? (root.info.reason ?? "") : (root.info.queryReason ?? "")
                    readonly property string more: root.scope === "current" && root.info.moreAvailable ? qsTr("More rows exist than the grid has loaded. Only the loaded rows are exported; choose “Run the query again” for all of them.") : ""
                    readonly property string pending: (root.info.pendingChanges ?? 0) > 0 ? qsTr("Changes that are not saved yet are not exported.") : ""

                    Layout.fillWidth: true
                    visible: text !== ""
                    text: [reason, more, pending].filter(s => s !== "").join(" ")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                Text {
                    id: pathLabel

                    text: qsTr("File")
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    QTextField {
                        id: pathField

                        Layout.fillWidth: true
                        accessibleName: pathLabel.text
                        onAccepted: root.submit()
                    }

                    QButton {
                        id: browseBtn

                        text: qsTr("Browse…")
                        iconGlyph: Icons.folderOpen
                        onClicked: root.chooseFile()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.startError !== ""
                    spacing: Theme.spacing

                    Icon {
                        Layout.alignment: Qt.AlignTop
                        text: Icons.circleAlert
                        color: Theme.danger
                    }

                    Text {
                        id: startErrorText

                        Layout.fillWidth: true
                        text: root.startError
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.danger
                        wrapMode: Text.WordWrap

                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }
            }
        }

        // ---- progress and result --------------------------------------------------------------

        ColumnLayout {
            id: progress

            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 22
            visible: !root.formShown
            spacing: Theme.spacing * 2

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                visible: root.stateName === "preparing" || root.stateName === "running" || root.stateName === "done" || root.stateName === "cancelled"

                Icon {
                    Layout.alignment: Qt.AlignTop
                    text: root.stateName === "done" ? Icons.circleCheck : root.stateName === "cancelled" ? Icons.info : Icons.loaderCircle
                    color: root.stateName === "done" ? Theme.success : Theme.fgMuted
                }

                // The name stays the same while the count changes, and the count is not an
                // announcement: a screen reader reads it when the user goes there.
                Text {
                    id: progressText

                    Layout.fillWidth: true
                    text: root.stateName === "preparing" ? qsTr("Preparing…") : root.stateName === "done" ? root.controller.resultText : root.stateName === "cancelled" ? qsTr("Export cancelled. The file was not written.") : root.controller.cancelling ? qsTr("Cancelling… %1").arg(root.controller.progressText) : root.controller.progressText
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                    wrapMode: Text.WrapAnywhere

                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Export progress")
                    Accessible.description: text
                }
            }

            ErrorPanel {
                id: failure

                Layout.fillWidth: true
                visible: root.stateName === "failed"
                title: qsTr("The export failed")
                message: root.controller.errorMessage
                hint: qsTr("No file was written. An existing file with that name was left as it was.")
                detail: root.controller.errorDetail
            }
        }

        // ---- buttons --------------------------------------------------------------------------

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: cancelBtn

                visible: root.formShown
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            QButton {
                id: exportBtn

                visible: root.formShown
                text: qsTr("Export")
                variant: "primary"
                enabled: root.scopes.length > 0
                Accessible.description: enabled ? "" : (root.info.reason ?? "")
                onClicked: root.submit()
            }

            QButton {
                id: stopBtn

                visible: root.busy
                enabled: !root.controller.cancelling
                text: qsTr("Cancel export")
                onClicked: root.controller.cancel()
            }

            QButton {
                id: folderBtn

                visible: root.stateName === "done"
                text: qsTr("Show in folder")
                iconGlyph: Icons.folderOpen
                onClicked: root.controller.showInFolder()
            }

            QButton {
                id: backBtn

                visible: root.stateName === "failed" || root.stateName === "cancelled"
                text: qsTr("Back")
                onClicked: {
                    root.controller.reset();
                    Qt.callLater(() => pathField.forceActiveFocus(Qt.TabFocusReason));
                }
            }

            QButton {
                id: closeBtn

                visible: !root.formShown && !root.busy
                text: qsTr("Close")
                variant: root.stateName === "done" ? "primary" : "default"
                onClicked: root.close()
            }
        }
    }
}
