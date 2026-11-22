pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// The record view of a data tab (H11 phase 3): a panel to the right of the grid with the fields of
// the CURRENT row as "column, type, value", following the row while you move in the grid. NULL is
// the word in italics, a pending change says "modified" with a pencil (text and icon, never colour
// alone). The value is the whole one (the grid cuts it), up to a limit: a longer one ends in "…" and
// is read with View or taken whole with Copy. Long text, JSON, XML and BLOB values get a viewer in
// the same panel (View, or Enter on the field): read-only, with Copy of the whole value, a wrap
// switch for text and, for a BLOB, a hex + ASCII dump of the first 64 KiB and its size. The panel
// only shows: editing stays in the grid.
//
// Keys (the field list is one tab stop): Up / Down / Home / End move between fields, Enter opens
// the viewer of the field, Ctrl+C copies its whole value, Esc goes back to the grid (or, in a
// viewer, back to the fields).
FocusScope {
    id: root

    // The TableEditModel of the tab.
    required property var edits
    // The current row of the grid, -1 = none.
    property int row: -1
    property string tabId: ""
    // {columnName: "Foreign key to customers.id"}: the columns that can go to a related row.
    property var linkColumns: ({})
    // Why the last "Go to related row" opened nothing ("" = nothing wrong).
    property string relatedError: ""
    property string lastAnnouncement: ""
    // A viewer is open.
    property bool viewing: false
    property var fields: []
    // The viewer's content (TableEditModel.fieldView) and the column it shows.
    property var view: ({
            "ok": false,
            "kind": "",
            "text": "",
            "notice": "",
            "size": "",
            "name": "",
            "type": ""
        })
    property int viewColumn: -1
    property bool wrap: true

    readonly property alias list: list
    readonly property alias closeButton: closeBtn
    readonly property alias backButton: backBtn
    readonly property alias copyButton: copyBtn
    readonly property alias wrapBox: wrapBox
    readonly property alias viewerText: viewerText
    readonly property alias emptyLabel: emptyText
    readonly property alias errorLabel: errorText
    readonly property alias noticeLabel: noticeText
    readonly property alias titleLabel: titleText
    // The field the list is on (a column of the grid), -1 = none.
    readonly property int currentColumn: list.currentIndex >= 0 && list.currentIndex < fields.length ? list.currentIndex : -1
    // What the grid-side code focuses when the panel opens.
    readonly property Item focusTarget: viewing ? viewerText : list

    signal closeRequested
    // Esc in the field list: the grid takes the focus back.
    signal focusGridRequested
    signal goToRelatedRequested(int column)

    function say(text, assertive) {
        lastAnnouncement = text;
        root.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    function kindLabel(kind) {
        return kind === "json" ? qsTr("JSON") : kind === "xml" ? qsTr("XML") : kind === "blob" ? qsTr("Binary data") : qsTr("Long text");
    }

    // Text shown for a field: the value, the word NULL, or what stands for it.
    function valueText(f) {
        if (f.isNull)
            return qsTr("NULL");
        if (f.kind === "blob")
            return qsTr("Binary data, %1").arg(ValueViewer.size(f.size));
        if (f.text === "")
            return qsTr("(empty)");
        return f.cut ? f.text + "…" : f.text;
    }

    function fieldName(f) {
        let text = qsTr("%1, %2: %3").arg(f.name).arg(f.type !== "" ? f.type : qsTr("unknown type")).arg(valueText(f));
        if (f.cut)
            text += ", " + qsTr("value cut, use View or Copy for the whole value");
        if (f.state !== "")
            text += ", " + f.state;
        if (f.kind !== "")
            text += ", " + qsTr("has a viewer");
        const link = linkColumns[f.name];
        if (link !== undefined)
            text += ", " + link;
        return text;
    }

    function refresh() {
        const n = edits ? edits.rows : 0;
        const next = edits && row >= 0 && row < n ? edits.recordFields(row) : [];
        fields = next;
        if (list.currentIndex >= next.length)
            list.currentIndex = next.length - 1;
        else if (list.currentIndex < 0 && next.length > 0)
            list.currentIndex = 0;
        if (viewing) {
            if (next.length === 0 || viewColumn >= next.length)
                closeView(false);
            else
                view = edits.fieldView(row, viewColumn);
        }
    }

    function openView(column) {
        if (!edits || row < 0 || column < 0 || column >= fields.length)
            return;
        const f = fields[column];
        if (f.kind === "") {
            say(qsTr("%1 is shown in full: it has no viewer.").arg(f.name), false);
            return;
        }
        viewColumn = column;
        view = edits.fieldView(row, column);
        viewing = true;
        say(qsTr("%1, %2 viewer").arg(f.name).arg(kindLabel(view.kind)), false);
        Qt.callLater(() => viewerText.forceActiveFocus(Qt.TabFocusReason));
    }

    function closeView(refocus) {
        viewing = false;
        if (refocus !== false) {
            say(qsTr("Back to the fields"), false);
            Qt.callLater(() => list.forceActiveFocus(Qt.TabFocusReason));
        }
    }

    // The whole value of a field on the clipboard.
    function copyField(column) {
        if (!edits || row < 0 || column < 0 || column >= fields.length)
            return;
        edits.copyField(row, column);
        say(qsTr("Copied the whole value of %1").arg(fields[column].name), false);
    }

    onRowChanged: Qt.callLater(refresh)
    onEditsChanged: Qt.callLater(refresh)
    Component.onCompleted: refresh()

    // The words of the fields come from C++ ("modified"): they are made again in the new language.
    Connections {
        target: Language

        function onChoiceChanged() {
            Qt.callLater(root.refresh);
        }
    }

    // Pending changes, new rows and rows that arrive: the fields follow (coalesced).
    Connections {
        target: root.edits

        function onPendingChanged() {
            Qt.callLater(root.refresh);
        }

        function onRowsChanged() {
            Qt.callLater(root.refresh);
        }

        function onDataChanged() {
            Qt.callLater(root.refresh);
        }

        function onModelReset() {
            Qt.callLater(root.refresh);
        }
    }

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Record view")

    Rectangle {
        anchors.fill: parent
        color: Theme.bgCanvas
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Title: the row, and the button that closes the panel.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: head.implicitHeight + Theme.spacing * 2
            color: Theme.bgSurface

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.borderDefault
            }

            RowLayout {
                id: head

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.spacing * 2
                anchors.rightMargin: Theme.spacing
                spacing: Theme.spacing

                Text {
                    id: titleText

                    Layout.fillWidth: true
                    text: root.row >= 0 && root.edits && root.row < root.edits.rows ? qsTr("Record: %1").arg(root.edits.rowLabel(root.row)) : qsTr("Record")
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                    elide: Text.ElideRight

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                IconButton {
                    id: closeBtn

                    accessibleName: qsTr("Close record view")
                    iconGlyph: Icons.x
                    onClicked: root.closeRequested()
                }
            }
        }

        // Nothing to show: no current row.
        Text {
            id: emptyText

            Layout.fillWidth: true
            Layout.margins: Theme.spacing * 2
            visible: root.fields.length === 0
            text: qsTr("There is no current row. Choose a row in the table to see its fields.")
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WordWrap

            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        // The fields.
        ListView {
            id: list

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.viewing && root.fields.length > 0
            model: root.fields
            clip: true
            focus: true
            activeFocusOnTab: true
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            currentIndex: 0

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Record fields")
            Accessible.description: qsTr("Up and Down choose a field, Enter opens its viewer, Ctrl+C copies its whole value.")
            Accessible.focusable: true

            T.ScrollBar.vertical: QScrollBar {}

            Keys.onPressed: event => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    root.openView(list.currentIndex);
                    event.accepted = true;
                } else if (event.matches(StandardKey.Copy)) {
                    root.copyField(list.currentIndex);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Escape) {
                    root.focusGridRequested();
                    event.accepted = true;
                }
            }

            delegate: Item {
                id: field

                required property var modelData
                required property int index
                readonly property bool current: ListView.isCurrentItem
                readonly property color ink: current ? Theme.selectionFg : Theme.fgDefault
                readonly property color softInk: current ? Theme.selectionFg : Theme.fgMuted
                readonly property bool linked: root.linkColumns[modelData.name] !== undefined

                width: ListView.view.width
                implicitHeight: body.implicitHeight + Theme.spacing * 2
                height: implicitHeight

                Accessible.role: Accessible.ListItem
                Accessible.name: root.fieldName(modelData)
                Accessible.selectable: true
                Accessible.selected: current

                Rectangle {
                    anchors.fill: parent
                    color: field.current ? Theme.selectionBg : "transparent"
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.borderDefault
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        list.currentIndex = field.index;
                        list.forceActiveFocus(Qt.MouseFocusReason);
                    }
                }

                ColumnLayout {
                    id: body

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.spacing * 2
                    anchors.topMargin: Theme.spacing
                    spacing: Theme.spacing / 2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing

                        Text {
                            text: field.modelData.name
                            font.pointSize: Theme.fontSize
                            font.bold: true
                            color: field.ink
                            elide: Text.ElideRight
                            Layout.maximumWidth: Math.max(Theme.controlHeight * 2, body.width * 0.6)
                            Accessible.ignored: true
                        }

                        Text {
                            Layout.fillWidth: true
                            text: field.modelData.type
                            font.pointSize: Theme.fontSize
                            color: field.softInk
                            elide: Text.ElideRight
                            Accessible.ignored: true
                        }

                        // A pending change: icon and word.
                        Row {
                            visible: field.modelData.state !== ""
                            spacing: Theme.spacing / 2
                            Accessible.ignored: true

                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                text: Icons.pencil
                                color: field.ink
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: field.modelData.state
                                font.pointSize: Theme.fontSize
                                font.bold: true
                                color: field.ink
                                Accessible.ignored: true
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.valueText(field.modelData)
                        font.pointSize: Theme.fontSize
                        font.italic: field.modelData.isNull
                        font.family: field.modelData.kind === "json" || field.modelData.kind === "xml" ? Theme.monoFontFamily : Qt.application.font.family
                        color: field.modelData.isNull || field.modelData.text === "" && field.modelData.kind !== "blob" ? field.softInk : field.ink
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        textFormat: Text.PlainText
                        maximumLineCount: 6
                        elide: Text.ElideRight
                        Accessible.ignored: true
                    }

                    Flow {
                        Layout.fillWidth: true
                        visible: field.modelData.kind !== "" || field.linked
                        spacing: Theme.spacing

                        // Keyboard: Enter on the field; the button is for the pointer.
                        QButton {
                            visible: field.modelData.kind !== ""
                            text: qsTr("View")
                            iconGlyph: Icons.eye
                            focusPolicy: Qt.NoFocus
                            Accessible.name: qsTr("View %1").arg(field.modelData.name)
                            onClicked: {
                                list.currentIndex = field.index;
                                root.openView(field.index);
                            }
                        }

                        QButton {
                            visible: field.linked
                            text: qsTr("Go to related row")
                            iconGlyph: Icons.link
                            focusPolicy: Qt.NoFocus
                            Accessible.name: qsTr("Go to related row of %1").arg(field.modelData.name)
                            Accessible.description: root.linkColumns[field.modelData.name] ?? ""
                            onClicked: {
                                list.currentIndex = field.index;
                                root.goToRelatedRequested(field.index);
                            }
                        }
                    }
                }
            }

            FocusRing {
                shown: list.activeFocus
                inset: true
            }
        }

        // The viewer of one value.
        ColumnLayout {
            id: viewer

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.viewing
            spacing: Theme.spacing

            // Esc from any control of the viewer (the buttons too) goes back to the fields.
            Keys.onEscapePressed: root.closeView()

            Flow {
                Layout.fillWidth: true
                Layout.margins: Theme.spacing
                spacing: Theme.spacing

                Accessible.role: Accessible.ToolBar
                Accessible.name: qsTr("Viewer actions")

                QButton {
                    id: backBtn

                    text: qsTr("Back to fields")
                    iconGlyph: Icons.arrowLeft
                    onClicked: root.closeView()
                }

                QButton {
                    id: copyBtn

                    text: qsTr("Copy")
                    iconGlyph: Icons.copy
                    Accessible.description: qsTr("Copies the whole value, not only what is shown")
                    onClicked: root.copyField(root.viewColumn)
                }

                QCheckBox {
                    id: wrapBox

                    visible: root.view.kind !== "blob"
                    text: qsTr("Wrap lines")
                    checked: root.wrap
                    onToggled: root.wrap = checked
                }
            }

            Text {
                id: viewerTitle

                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing * 2
                Layout.rightMargin: Theme.spacing * 2
                text: qsTr("%1: %2 (%3)").arg(root.view.name).arg(root.kindLabel(root.view.kind)).arg(root.view.type)
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            Text {
                id: sizeText

                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing * 2
                Layout.rightMargin: Theme.spacing * 2
                visible: root.view.size !== ""
                text: qsTr("Size: %1").arg(root.view.size)
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            // Not valid JSON, cut, too big to format...: in words, with an icon.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing * 2
                Layout.rightMargin: Theme.spacing * 2
                visible: root.view.notice !== ""
                spacing: Theme.spacing

                Icon {
                    Layout.alignment: Qt.AlignTop
                    text: root.view.ok ? Icons.info : Icons.triangleAlert
                    color: root.view.ok ? Theme.fgMuted : Theme.warning
                }

                Text {
                    id: noticeText

                    Layout.fillWidth: true
                    text: root.view.notice
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.spacing

                Rectangle {
                    anchors.fill: parent
                    color: Theme.bgSurface
                    border.width: 1
                    border.color: Theme.borderStrong
                }

                Flickable {
                    id: flick

                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    contentWidth: root.wrap && root.view.kind !== "blob" ? width : Math.max(width, viewerText.contentWidth + viewerText.leftPadding * 2)
                    contentHeight: Math.max(height, viewerText.contentHeight + viewerText.topPadding * 2)
                    boundsBehavior: Flickable.StopAtBounds

                    T.ScrollBar.vertical: QScrollBar {}
                    T.ScrollBar.horizontal: QScrollBar {}

                    TextEdit {
                        id: viewerText

                        function scrollBy(dx, dy) {
                            flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + dy));
                            flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, flick.contentX + dx));
                        }

                        width: root.wrap && root.view.kind !== "blob" ? flick.width : Math.max(flick.width, contentWidth + leftPadding * 2)
                        topPadding: Theme.spacing
                        leftPadding: Theme.spacing * 2
                        rightPadding: Theme.spacing * 2
                        text: root.view.text
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        wrapMode: root.wrap && root.view.kind !== "blob" ? TextEdit.WrapAtWordBoundaryOrAnywhere : TextEdit.NoWrap
                        textFormat: TextEdit.PlainText
                        font.family: Theme.monoFontFamily
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        selectionColor: Theme.selectionBg
                        selectedTextColor: Theme.selectionFg
                        activeFocusOnTab: true

                        // A read-only TextEdit has no caret to move, so the keys scroll the text.
                        Keys.onPressed: event => {
                            const line = Math.max(1, cursorRectangle.height);
                            event.accepted = true;
                            if (event.matches(StandardKey.SelectAll)) {
                                selectAll();
                            } else if (event.matches(StandardKey.Copy)) {
                                if (selectedText !== "")
                                    copy();
                                else
                                    root.copyField(root.viewColumn);
                            } else if (event.key === Qt.Key_Down) {
                                scrollBy(0, line);
                            } else if (event.key === Qt.Key_Up) {
                                scrollBy(0, -line);
                            } else if (event.key === Qt.Key_Right) {
                                scrollBy(line * 4, 0);
                            } else if (event.key === Qt.Key_Left) {
                                scrollBy(-line * 4, 0);
                            } else if (event.key === Qt.Key_PageDown) {
                                scrollBy(0, flick.height - line);
                            } else if (event.key === Qt.Key_PageUp) {
                                scrollBy(0, line - flick.height);
                            } else if (event.key === Qt.Key_Home) {
                                scrollBy(-flick.contentWidth, -flick.contentHeight);
                            } else if (event.key === Qt.Key_End) {
                                scrollBy(0, flick.contentHeight);
                            } else if (event.key === Qt.Key_Escape) {
                                root.closeView();
                            } else {
                                event.accepted = false;
                            }
                        }

                        Accessible.role: Accessible.EditableText
                        Accessible.name: qsTr("%1 viewer, %2").arg(root.kindLabel(root.view.kind)).arg(root.view.name)
                        Accessible.description: qsTr("Read only. Up and Down scroll, Ctrl+C copies the whole value, Esc goes back to the fields.")
                        Accessible.readOnly: true
                        Accessible.multiLine: true
                        Accessible.focusable: true
                    }
                }

                FocusRing {
                    shown: viewerText.activeFocus
                    inset: true
                }
            }
        }

        // "Go to related row" could not open the table: in words, with an icon.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacing * 2
            visible: root.relatedError !== ""
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: Icons.circleAlert
                color: Theme.danger
            }

            Text {
                id: errorText

                Layout.fillWidth: true
                text: root.relatedError
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }
    }
}
