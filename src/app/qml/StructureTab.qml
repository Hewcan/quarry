pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// A structure tab (H11 phase 4): the columns, indexes, foreign keys and DDL of one table, view or
// materialized view, read-only. Opened from the tree ("Show structure") or from a data tab. It reads
// through StructureController (Session::describe on the tree's metadata session, asynchronous): while
// it reads it says "Loading...", when it fails it says why and offers "Try again", and a table
// without columns says that.
//
// Four sections under a title, top to bottom: Columns (name, type, NOT NULL / nullable, default and
// the marks "Primary key" and "Foreign key to t.c", all as text), Indexes (name, columns, "unique"),
// Foreign keys (name, columns, the table they point to as a button that opens its structure, and a
// "Data" button) and DDL (reconstructed by Quarry from the structure: it says so, and what it does
// not include, in a visible note; "Copy DDL" copies it; read-only SQL editor with highlighting).
// The page scrolls with the keys (Up, Down, PageUp, PageDown) when it has the focus.
FocusScope {
    id: root

    required property var tabs // EditorTabsModel
    required property int row
    // StructureController
    required property var structure
    property string tabId: ""
    property string tabTitle: ""
    property string connectionId: ""
    property string connectionName: ""
    property bool production: false

    readonly property bool isEditor: false
    readonly property bool isStructure: true
    // StructureController.infoOf()
    property var info: ({
            "state": "idle",
            "message": "",
            "detail": "",
            "isView": false,
            "empty": false,
            "name": "",
            "columns": [],
            "indexes": [],
            "foreignKeys": [],
            "ddl": ""
        })
    // What the view is showing: "loading" | "failed" | "empty" | "ready".
    readonly property string view: info.state === "ready" ? (info.empty ? "empty" : "ready") : info.state === "failed" ? "failed" : "loading"
    // What the editor region focuses (the same name the SQL editor and the data tab use).
    readonly property Item edit: view === "ready" ? scroller : view === "failed" ? retryButton : stateless
    readonly property alias scroller: scroller
    readonly property alias retryButton: retryButton
    readonly property alias statePanel: stateless
    readonly property alias errorPanel: errorPanel
    readonly property alias titleLabel: titleText
    readonly property alias dataButton: dataBtn
    readonly property alias copyDdlButton: copyDdl
    readonly property alias ddlNote: ddlNote
    readonly property alias ddlEditor: ddlLoader.item
    readonly property alias columnsSection: columnsHead
    readonly property alias indexesSection: indexesHead
    readonly property alias foreignKeysSection: fkHead
    readonly property alias ddlSection: ddlHead
    readonly property alias columnRows: columnRepeater
    readonly property alias indexRows: indexRepeater
    readonly property alias foreignKeyRows: fkRepeater
    property string lastAnnouncement: ""

    // A table or view of this connection: the tree's own shape, for MainWindow.
    signal openTableRequested(var object, string target)

    function say(text, assertive) {
        lastAnnouncement = text;
        root.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    function refresh() {
        const before = info.state;
        info = structure.infoOf(tabId);
        if (before !== info.state && info.state === "ready")
            say(qsTr("Structure of %1 loaded").arg(info.name), false);
        else if (before !== info.state && info.state === "failed")
            say(info.message, true);
    }

    function load() {
        structure.reload(tabId);
        refresh();
    }

    function ownObject() {
        return {
            "connectionId": connectionId,
            "parts": tabs.objectPartsOf(row),
            "kind": tabs.objectKindOf(row),
            "name": tabTitle,
            "openable": true
        };
    }

    function scrollBy(dy) {
        scroller.contentY = Math.max(0, Math.min(scroller.contentHeight - scroller.height, scroller.contentY + dy));
    }

    // A control that takes the focus (Tab) is scrolled into view: the page is longer than the window.
    function revealFocus() {
        const item = root.Window.activeFocusItem;
        if (!item || !scroller.visible)
            return;
        let inside = false;
        for (let p = item; p; p = p.parent) {
            if (p === scroller.contentItem) {
                inside = true;
                break;
            }
        }
        if (!inside)
            return;
        const top = item.mapToItem(scroller.contentItem, 0, 0).y;
        const bottom = top + item.height;
        const margin = Theme.spacing * 2;
        if (top < scroller.contentY || item.height > scroller.height)
            scroller.contentY = Math.max(0, top - margin);
        else if (bottom > scroller.contentY + scroller.height)
            scroller.contentY = Math.min(scroller.contentHeight - scroller.height, bottom - scroller.height + margin);
    }

    Connections {
        target: root.Window.window

        function onActiveFocusItemChanged() {
            root.revealFocus();
        }
    }

    Component.onCompleted: {
        structure.load(tabId);
        refresh();
    }

    // The focus was on what is gone (the loading text, the error): the new content takes it.
    onViewChanged: {
        const lost = !root.Window.activeFocusItem || root.Window.activeFocusItem === root.Window.contentItem;
        if (lost || root.activeFocus)
            Qt.callLater(() => root.edit.forceActiveFocus(Qt.OtherFocusReason));
    }

    Connections {
        target: root.structure

        function onChanged(id) {
            if (id === root.tabId)
                root.refresh();
        }
    }

    // A table of the page: the cells of a row are laid out in proportion (`weights`), and wrap.
    component HeaderCell: Text {
        Layout.fillWidth: true
        font.pointSize: Theme.fontSize
        font.bold: true
        color: Theme.fgMuted
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        Accessible.ignored: true
    }

    component BodyCell: Text {
        Layout.fillWidth: true
        font.pointSize: Theme.fontSize
        color: Theme.fgDefault
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        textFormat: Text.PlainText
        Accessible.ignored: true
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bgCanvas
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The title and what to do next with the table.
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

            Flow {
                id: bar

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.spacing * 2
                anchors.rightMargin: Theme.spacing * 2
                spacing: Theme.spacing * 2

                Accessible.role: Accessible.ToolBar
                Accessible.name: qsTr("Structure actions")

                Row {
                    height: dataBtn.height
                    spacing: Theme.spacing

                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        text: Icons.tableProperties
                        color: Theme.fgMuted
                    }

                    Text {
                        id: titleText

                        anchors.verticalCenter: parent.verticalCenter
                        text: root.tabTitle !== "" ? qsTr("Structure of %1").arg(root.tabTitle) : qsTr("Structure")
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.fgDefault

                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }

                QButton {
                    id: dataBtn

                    text: qsTr("Open data")
                    iconGlyph: Icons.table
                    Accessible.description: qsTr("Opens the rows of this table")
                    onClicked: root.openTableRequested(root.ownObject(), "data")
                }
            }
        }

        // The page, once read.
        Flickable {
            id: scroller

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.view === "ready"
            activeFocusOnTab: true
            clip: true
            contentWidth: width
            contentHeight: page.implicitHeight + Theme.spacing * 4
            boundsBehavior: Flickable.StopAtBounds

            Accessible.role: Accessible.Pane
            Accessible.name: qsTr("Structure of %1").arg(root.tabTitle)
            Accessible.description: qsTr("Columns, indexes, foreign keys and DDL. Up, Down, PageUp and PageDown scroll.")
            Accessible.focusable: true

            T.ScrollBar.vertical: QScrollBar {}

            Keys.onPressed: event => {
                const line = Theme.controlHeight;
                event.accepted = true;
                if (event.key === Qt.Key_Down)
                    root.scrollBy(line);
                else if (event.key === Qt.Key_Up)
                    root.scrollBy(-line);
                else if (event.key === Qt.Key_PageDown)
                    root.scrollBy(scroller.height - line);
                else if (event.key === Qt.Key_PageUp)
                    root.scrollBy(line - scroller.height);
                else if (event.key === Qt.Key_Home)
                    root.scrollBy(-scroller.contentHeight);
                else if (event.key === Qt.Key_End)
                    root.scrollBy(scroller.contentHeight);
                else
                    event.accepted = false;
            }

            ColumnLayout {
                id: page

                x: Theme.spacing * 2
                y: Theme.spacing * 2
                width: scroller.width - Theme.spacing * 6
                spacing: Theme.spacing * 3

                // ---- Columns ----
                Heading {
                    id: columnsHead

                    Layout.fillWidth: true
                    level: 2
                    text: qsTr("Columns")
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Accessible.role: Accessible.Table
                    Accessible.name: qsTr("Columns")
                    Accessible.description: qsTr("%1 columns. Each row: name, type, NOT NULL or nullable, default, keys.").arg(root.info.columns.length)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing * 2

                        HeaderCell {
                            Layout.preferredWidth: 0
                            Layout.horizontalStretchFactor: 22
                            text: qsTr("Name")
                        }

                        HeaderCell {
                            Layout.preferredWidth: 0
                            Layout.horizontalStretchFactor: 16
                            text: qsTr("Type")
                        }

                        HeaderCell {
                            Layout.preferredWidth: 0
                            Layout.horizontalStretchFactor: 12
                            text: qsTr("Null")
                        }

                        HeaderCell {
                            Layout.preferredWidth: 0
                            Layout.horizontalStretchFactor: 20
                            text: qsTr("Default")
                        }

                        HeaderCell {
                            Layout.preferredWidth: 0
                            Layout.horizontalStretchFactor: 30
                            text: qsTr("Keys")
                        }
                    }

                    Repeater {
                        id: columnRepeater

                        model: root.info.columns

                        delegate: Item {
                            id: col

                            required property var modelData

                            Layout.fillWidth: true
                            implicitHeight: columnRow.implicitHeight + Theme.spacing * 2

                            Accessible.role: Accessible.ListItem
                            Accessible.name: qsTr("%1, %2, %3, %4%5").arg(col.modelData.name).arg(col.modelData.type).arg(col.modelData.nullableText).arg(col.modelData.defaultText !== "" ? qsTr("default %1").arg(col.modelData.defaultText) : qsTr("no default")).arg(col.modelData.marks !== "" ? ", " + col.modelData.marks : "")

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: Theme.borderDefault
                            }

                            RowLayout {
                                id: columnRow

                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.spacing * 2

                                BodyCell {
                                    Layout.preferredWidth: 0
                                    Layout.horizontalStretchFactor: 22
                                    text: col.modelData.name
                                    font.bold: col.modelData.primaryKey
                                }

                                BodyCell {
                                    Layout.preferredWidth: 0
                                    Layout.horizontalStretchFactor: 16
                                    text: col.modelData.type
                                }

                                BodyCell {
                                    Layout.preferredWidth: 0
                                    Layout.horizontalStretchFactor: 12
                                    text: col.modelData.nullableText
                                }

                                BodyCell {
                                    Layout.preferredWidth: 0
                                    Layout.horizontalStretchFactor: 20
                                    text: col.modelData.defaultText !== "" ? col.modelData.defaultText : "—"
                                    color: col.modelData.defaultText !== "" ? Theme.fgDefault : Theme.fgMuted
                                    font.family: col.modelData.defaultText !== "" ? Theme.monoFontFamily : Qt.application.font.family
                                }

                                BodyCell {
                                    Layout.preferredWidth: 0
                                    Layout.horizontalStretchFactor: 30
                                    text: col.modelData.marks !== "" ? col.modelData.marks : "—"
                                    color: col.modelData.marks !== "" ? Theme.fgDefault : Theme.fgMuted
                                }
                            }
                        }
                    }
                }

                // ---- Indexes ----
                Heading {
                    id: indexesHead

                    Layout.fillWidth: true
                    level: 2
                    text: qsTr("Indexes")
                }

                Text {
                    Layout.fillWidth: true
                    visible: root.info.indexes.length === 0
                    text: qsTr("This table has no indexes.")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.info.indexes.length > 0
                    spacing: 0

                    Accessible.role: Accessible.Table
                    Accessible.name: qsTr("Indexes")
                    Accessible.description: qsTr("%1 indexes. Each row: name, columns, unique or not.").arg(root.info.indexes.length)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing * 2

                        HeaderCell {
                            Layout.preferredWidth: 0
                            Layout.horizontalStretchFactor: 35
                            text: qsTr("Name")
                        }

                        HeaderCell {
                            Layout.preferredWidth: 0
                            Layout.horizontalStretchFactor: 45
                            text: qsTr("Columns")
                        }

                        HeaderCell {
                            Layout.preferredWidth: 0
                            Layout.horizontalStretchFactor: 20
                            text: qsTr("Unique")
                        }
                    }

                    Repeater {
                        id: indexRepeater

                        model: root.info.indexes

                        delegate: Item {
                            id: idx

                            required property var modelData

                            Layout.fillWidth: true
                            implicitHeight: indexRow.implicitHeight + Theme.spacing * 2

                            Accessible.role: Accessible.ListItem
                            Accessible.name: qsTr("%1, columns %2, %3").arg(idx.modelData.name).arg(idx.modelData.columns).arg(idx.modelData.uniqueText)

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: Theme.borderDefault
                            }

                            RowLayout {
                                id: indexRow

                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.spacing * 2

                                BodyCell {
                                    Layout.preferredWidth: 0
                                    Layout.horizontalStretchFactor: 35
                                    text: idx.modelData.name
                                }

                                BodyCell {
                                    Layout.preferredWidth: 0
                                    Layout.horizontalStretchFactor: 45
                                    text: idx.modelData.columns
                                }

                                BodyCell {
                                    Layout.preferredWidth: 0
                                    Layout.horizontalStretchFactor: 20
                                    text: idx.modelData.uniqueText
                                    font.bold: idx.modelData.unique
                                }
                            }
                        }
                    }
                }

                // ---- Foreign keys ----
                Heading {
                    id: fkHead

                    Layout.fillWidth: true
                    level: 2
                    text: qsTr("Foreign keys")
                }

                Text {
                    Layout.fillWidth: true
                    visible: root.info.foreignKeys.length === 0
                    text: qsTr("This table has no foreign keys.")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.info.foreignKeys.length > 0
                    spacing: 0

                    Accessible.role: Accessible.Table
                    Accessible.name: qsTr("Foreign keys")
                    Accessible.description: qsTr("%1 foreign keys. Each row: name, columns, the table and columns they point to.").arg(root.info.foreignKeys.length)

                    Repeater {
                        id: fkRepeater

                        model: root.info.foreignKeys

                        delegate: Item {
                            id: key

                            required property var modelData

                            Layout.fillWidth: true
                            implicitHeight: keyRow.implicitHeight + Theme.spacing * 2

                            Accessible.role: Accessible.ListItem
                            Accessible.name: qsTr("%1: %2").arg(key.modelData.name !== "" ? key.modelData.name : qsTr("foreign key")).arg(key.modelData.text)

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: Theme.borderDefault
                            }

                            // name, "columns -> table (columns)", then the table as a button and "Data".
                            Flow {
                                id: keyRow

                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.spacing * 2

                                Text {
                                    text: key.modelData.name !== "" ? key.modelData.name : qsTr("(unnamed)")
                                    font.pointSize: Theme.fontSize
                                    font.bold: true
                                    color: Theme.fgDefault
                                    Accessible.ignored: true
                                }

                                Text {
                                    text: qsTr("%1 references").arg(key.modelData.columns)
                                    font.pointSize: Theme.fontSize
                                    color: Theme.fgDefault
                                    Accessible.ignored: true
                                }

                                QButton {
                                    variant: "flat"
                                    text: key.modelData.table
                                    iconGlyph: Icons.tableProperties
                                    enabled: key.modelData.openable
                                    Accessible.name: qsTr("Open the structure of %1").arg(key.modelData.table)
                                    Accessible.description: qsTr("Opens the structure of the table this foreign key points to")
                                    onClicked: root.openTableRequested(root.objectOf(key.modelData), "structure")
                                }

                                Text {
                                    text: qsTr("(%1)").arg(key.modelData.refColumns)
                                    font.pointSize: Theme.fontSize
                                    color: Theme.fgDefault
                                    Accessible.ignored: true
                                }

                                QButton {
                                    variant: "flat"
                                    text: qsTr("Data")
                                    iconGlyph: Icons.table
                                    enabled: key.modelData.openable
                                    Accessible.name: qsTr("Open the data of %1").arg(key.modelData.table)
                                    onClicked: root.openTableRequested(root.objectOf(key.modelData), "data")
                                }
                            }
                        }
                    }
                }

                // ---- DDL ----
                Heading {
                    id: ddlHead

                    Layout.fillWidth: true
                    level: 2
                    text: qsTr("DDL")
                }

                // Reconstructed, not read from the server: said where it is read.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Icon {
                        Layout.alignment: Qt.AlignTop
                        text: Icons.triangleAlert
                        color: Theme.warning
                    }

                    Text {
                        id: ddlNote

                        Layout.fillWidth: true
                        text: qsTr("This DDL is rebuilt by Quarry from the structure above, not read from the server. It does not include CHECK constraints, triggers or engine options, so do not run it as an exact copy of the table.")
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        wrapMode: Text.WordWrap

                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }

                QButton {
                    id: copyDdl

                    text: qsTr("Copy DDL")
                    iconGlyph: Icons.copy
                    Accessible.description: qsTr("Copies the DDL to the clipboard")
                    onClicked: {
                        ValueViewer.copyText(root.info.ddl);
                        root.say(qsTr("DDL copied"), false);
                    }
                }

                Loader {
                    id: ddlLoader

                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.controlHeight * 14
                    active: root.view === "ready"
                    sourceComponent: SqlEditor {
                        tabs: root.tabs
                        row: root.row
                        readOnly: true
                        sourceText: root.info.ddl
                        definition: root.tabs.definitionOf(root.row)
                        tabTitle: root.tabTitle
                        connectionId: root.connectionId
                        connectionName: root.connectionName
                        production: root.production
                        accessibleName: qsTr("DDL")
                        accessibleDescription: qsTr("Reconstructed DDL of %1, read only").arg(root.tabTitle)
                    }
                }
            }
        }

        // Reading, or a table without columns: a message with nothing to do.
        EmptyState {
            id: stateless

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.view === "loading" || root.view === "empty"
            focusable: visible
            iconGlyph: root.view === "empty" ? Icons.table : Icons.loaderCircle
            title: root.view === "empty" ? qsTr("This table has no columns.") : qsTr("Loading…")
            message: root.view === "empty" ? qsTr("The database returned no columns for it, so there is nothing to show.") : qsTr("Reading the structure of the table.")
        }

        // The structure could not be read.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacing * 2
            visible: root.view === "failed"
            spacing: Theme.spacing * 2

            ErrorPanel {
                id: errorPanel

                Layout.fillWidth: true
                title: qsTr("The structure could not be read")
                message: root.info.message
                detail: root.info.detail
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

    // The tree-shaped object of a foreign key's target, for the shell.
    function objectOf(key) {
        return {
            "connectionId": connectionId,
            "parts": key.parts,
            "kind": key.kind,
            "name": key.table,
            "openable": key.openable
        };
    }
}
