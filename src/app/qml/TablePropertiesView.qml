pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// The "Properties" page of a data tab: a header (name, table type, description and, on the engines
// that have it, strict typing) and, below, the list of sections on the left and the selected
// section on the right. Read-only. The metadata comes from StructureController (Session::describe on
// the tree's metadata session, never on the UI thread) and is read the first time the page is shown.
//
// Keyboard: Tab goes header -> sections -> the selected section's table; in the section list and in
// the tables Up / Down / Home / End / PageUp / PageDown move; every row is a ListItem whose name is
// all its cells as text (nothing depends on colour). The tables are ListViews, so a table with
// hundreds of columns only makes the rows on screen.
FocusScope {
    id: root

    property string tabId: ""
    property string tabTitle: ""
    // StructureController (null in tests that do not need it).
    property var structure: null

    property var info: ({
            "state": "idle",
            "message": "",
            "detail": "",
            "name": "",
            "tableType": "",
            "description": "",
            "strictSupported": false,
            "strict": false,
            "empty": false,
            "columns": [],
            "keys": [],
            "indexes": [],
            "foreignKeys": [],
            "ddl": ""
        })
    readonly property string view: info.state === "ready" ? "ready" : info.state === "failed" ? "failed" : "loading"
    readonly property Item edit: view === "ready" ? sectionList : view === "failed" ? retryButton : stateless
    readonly property alias sectionList: sectionList
    readonly property alias sectionTable: sectionTable
    readonly property alias retryButton: retryButton
    readonly property alias strictBox: strictBox
    readonly property alias typeLabel: typeText
    readonly property alias descriptionLabel: descriptionText
    readonly property alias copyDdlButton: copyDdl
    readonly property alias ddlText: ddlText
    readonly property string currentSection: sections[sectionList.currentIndex]?.key ?? "columns"
    property string lastAnnouncement: ""

    readonly property var sections: [
        {
            "key": "columns",
            "title": qsTr("Columns")
        },
        {
            "key": "keys",
            "title": qsTr("Keys")
        },
        {
            "key": "foreignKeys",
            "title": qsTr("Foreign key columns")
        },
        {
            "key": "indexes",
            "title": qsTr("Indexes")
        },
        {
            "key": "references",
            "title": qsTr("References")
        },
        {
            "key": "triggers",
            "title": qsTr("Triggers")
        },
        {
            "key": "ddl",
            "title": qsTr("DDL")
        }
    ]

    // What each table section shows: its columns (title, field of the row, share of the width).
    readonly property var specs: ({
            "columns": [
                {
                    "title": qsTr("Name"),
                    "field": "name",
                    "weight": 18
                },
                {
                    "title": qsTr("Position"),
                    "field": "position",
                    "weight": 7
                },
                {
                    "title": qsTr("Data type"),
                    "field": "type",
                    "weight": 14
                },
                {
                    "title": qsTr("Length"),
                    "field": "length",
                    "weight": 7
                },
                {
                    "title": qsTr("Not null"),
                    "field": "notNullText",
                    "weight": 8
                },
                {
                    "title": qsTr("Auto increment"),
                    "field": "autoIncrementText",
                    "weight": 9
                },
                {
                    "title": qsTr("Default"),
                    "field": "defaultText",
                    "weight": 16
                },
                {
                    "title": qsTr("Description"),
                    "field": "description",
                    "weight": 21
                }
            ],
            "keys": [
                {
                    "title": qsTr("Name"),
                    "field": "name",
                    "weight": 30
                },
                {
                    "title": qsTr("Kind"),
                    "field": "kind",
                    "weight": 20
                },
                {
                    "title": qsTr("Columns"),
                    "field": "columns",
                    "weight": 50
                }
            ],
            "foreignKeys": [
                {
                    "title": qsTr("Name"),
                    "field": "name",
                    "weight": 25
                },
                {
                    "title": qsTr("Columns"),
                    "field": "columns",
                    "weight": 25
                },
                {
                    "title": qsTr("Referenced table"),
                    "field": "table",
                    "weight": 25
                },
                {
                    "title": qsTr("Referenced columns"),
                    "field": "refColumns",
                    "weight": 25
                }
            ],
            "indexes": [
                {
                    "title": qsTr("Name"),
                    "field": "name",
                    "weight": 35
                },
                {
                    "title": qsTr("Columns"),
                    "field": "columns",
                    "weight": 45
                },
                {
                    "title": qsTr("Unique"),
                    "field": "uniqueText",
                    "weight": 20
                }
            ]
        })

    readonly property var currentSpec: specs[currentSection] ?? []
    readonly property var currentRows: {
        if (currentSection === "columns")
            return info.columns.map(c => Object.assign({
                        "notNullText": c.nullable ? qsTr("no") : qsTr("yes")
                    }, c));
        return specs[currentSection] !== undefined ? (info[currentSection] ?? []) : [];
    }
    readonly property string emptyText: {
        switch (currentSection) {
        case "columns":
            return qsTr("This table has no columns.");
        case "keys":
            return qsTr("This table has no primary or unique keys.");
        case "foreignKeys":
            return qsTr("This table has no foreign keys.");
        case "indexes":
            return qsTr("This table has no indexes.");
        case "references":
            return qsTr("The tables that reference this one are not available for this connection yet.");
        case "triggers":
            return qsTr("The triggers of this table are not available for this connection yet.");
        }
        return "";
    }

    function say(text, assertive) {
        lastAnnouncement = text;
        root.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    function refresh() {
        if (!structure)
            return;
        const before = info.state;
        info = structure.infoOf(tabId);
        if (before !== info.state && info.state === "ready")
            say(qsTr("Properties of %1 loaded").arg(info.name), false);
        else if (before !== info.state && info.state === "failed")
            say(info.message, true);
    }

    // Home / End / PageUp / PageDown in a list (Up and Down are the ListView's own).
    function jump(list, event) {
        const page = Math.max(1, Math.floor(list.height / Theme.controlHeight) - 1);
        let to = -1;
        if (event.key === Qt.Key_Home)
            to = 0;
        else if (event.key === Qt.Key_End)
            to = list.count - 1;
        else if (event.key === Qt.Key_PageDown)
            to = Math.min(list.count - 1, list.currentIndex + page);
        else if (event.key === Qt.Key_PageUp)
            to = Math.max(0, list.currentIndex - page);
        if (to < 0)
            return;
        list.currentIndex = to;
        list.positionViewAtIndex(to, ListView.Contain);
        event.accepted = true;
    }

    function rowText(rowData) {
        const parts = [];
        for (const c of currentSpec) {
            const v = rowData[c.field];
            parts.push(c.title + ": " + (v === undefined || v === "" ? qsTr("none") : v));
        }
        return parts.join(", ");
    }

    Component.onCompleted: {
        if (structure && tabId !== "")
            structure.load(tabId);
        refresh();
    }

    Connections {
        target: root.structure

        function onChanged(id) {
            if (id === root.tabId)
                root.refresh();
        }
    }

    component Cell: Text {
        Layout.fillWidth: true
        Layout.preferredWidth: 0
        font.pointSize: Theme.fontSize
        color: Theme.fgDefault
        elide: Text.ElideRight
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

        // ---- header ----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: header.implicitHeight + Theme.spacing * 2
            color: Theme.bgSurface

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.borderDefault
            }

            Flow {
                id: header

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.spacing * 2
                anchors.rightMargin: Theme.spacing * 2
                spacing: Theme.spacing * 3

                Heading {
                    level: 2
                    text: root.info.name !== "" ? root.info.name : root.tabTitle
                }

                Text {
                    id: typeText

                    text: qsTr("Type: %1").arg(root.info.tableType !== "" ? root.info.tableType : "—")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                Text {
                    id: descriptionText

                    text: root.info.description !== "" ? qsTr("Description: %1").arg(root.info.description) : qsTr("Description: none")
                    font.pointSize: Theme.fontSize
                    color: root.info.description !== "" ? Theme.fgDefault : Theme.fgMuted
                    textFormat: Text.PlainText
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                // Read-only: it says how the table was made, it does not change it.
                QCheckBox {
                    id: strictBox

                    visible: root.info.strictSupported
                    enabled: false
                    checked: root.info.strict
                    text: qsTr("Strict typing")
                    Accessible.description: root.info.strict ? qsTr("The table is strict") : qsTr("The table is not strict")
                }
            }
        }

        // ---- sections and their content ----
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.view === "ready"
            spacing: 0

            ListView {
                id: sectionList

                Layout.fillHeight: true
                Layout.preferredWidth: Theme.controlHeight * 7
                model: root.sections
                activeFocusOnTab: true
                keyNavigationEnabled: true
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Accessible.role: Accessible.List
                Accessible.name: qsTr("Sections")
                Accessible.description: qsTr("Up and Down choose the section shown on the right.")

                Keys.onPressed: event => root.jump(sectionList, event)

                onCurrentIndexChanged: {
                    if (activeFocus)
                        root.say(root.sections[currentIndex].title, false);
                }

                delegate: Rectangle {
                    id: sectionItem

                    required property var modelData
                    required property int index
                    readonly property bool current: ListView.isCurrentItem

                    width: ListView.view.width
                    height: Theme.controlHeight
                    color: current ? Theme.selectionBg : "transparent"

                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.title
                    Accessible.selected: current
                    Accessible.selectable: true

                    // Selected is said by the marker and bold text too, not colour alone.
                    Rectangle {
                        width: Theme.spacing / 2
                        height: parent.height
                        visible: sectionItem.current
                        color: Theme.accent
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.spacing * 2
                        width: parent.width - Theme.spacing * 3
                        text: sectionItem.modelData.title
                        font.pointSize: Theme.fontSize
                        font.bold: sectionItem.current
                        color: Theme.fgDefault
                        elide: Text.ElideRight
                    }

                    FocusRing {
                        anchors.fill: parent
                        inset: true
                        shown: sectionItem.current && sectionList.activeFocus
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            sectionList.currentIndex = sectionItem.index;
                            sectionList.forceActiveFocus(Qt.MouseFocusReason);
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 1
                color: Theme.borderDefault
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.spacing * 2
                spacing: Theme.spacing

                Heading {
                    Layout.fillWidth: true
                    level: 3
                    text: root.sections[sectionList.currentIndex]?.title ?? ""
                }

                Text {
                    Layout.fillWidth: true
                    visible: root.currentSection !== "ddl" && root.currentRows.length === 0
                    text: root.emptyText
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WordWrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                // Header of the table section.
                RowLayout {
                    Layout.fillWidth: true
                    visible: sectionTable.visible
                    spacing: Theme.spacing * 2

                    Repeater {
                        model: root.currentSpec

                        Cell {
                            required property var modelData

                            Layout.horizontalStretchFactor: modelData.weight
                            text: modelData.title
                            font.bold: true
                            color: Theme.fgMuted
                        }
                    }
                }

                ListView {
                    id: sectionTable

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.currentSpec.length > 0 && root.currentRows.length > 0
                    model: root.currentRows
                    activeFocusOnTab: visible
                    keyNavigationEnabled: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    reuseItems: true

                    Accessible.role: Accessible.Table
                    Accessible.name: root.sections[sectionList.currentIndex]?.title ?? ""
                    Accessible.description: qsTr("%1 rows. Up and Down move between them.").arg(count)

                    T.ScrollBar.vertical: QScrollBar {}

                    Keys.onPressed: event => root.jump(sectionTable, event)

                    delegate: Rectangle {
                        id: rowItem

                        required property var modelData
                        required property int index
                        readonly property bool current: ListView.isCurrentItem

                        width: ListView.view.width
                        height: Theme.controlHeight
                        color: current ? Theme.selectionBg : index % 2 === 1 ? Theme.bgSurface : "transparent"

                        Accessible.role: Accessible.ListItem
                        Accessible.name: root.rowText(modelData)
                        Accessible.selected: current

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacing
                            anchors.rightMargin: Theme.spacing
                            spacing: Theme.spacing * 2

                            Repeater {
                                model: root.currentSpec

                                Cell {
                                    required property var modelData
                                    readonly property var value: rowItem.modelData[modelData.field]
                                    readonly property bool none: value === undefined || value === ""

                                    Layout.horizontalStretchFactor: modelData.weight
                                    text: none ? "—" : String(value)
                                    color: none ? Theme.fgMuted : Theme.fgDefault
                                    font.bold: modelData.field === "name" && (rowItem.modelData.primaryKey ?? false)
                                }
                            }
                        }

                        FocusRing {
                            anchors.fill: parent
                            inset: true
                            shown: rowItem.current && sectionTable.activeFocus
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                sectionTable.currentIndex = rowItem.index;
                                sectionTable.forceActiveFocus(Qt.MouseFocusReason);
                            }
                        }
                    }
                }

                // ---- DDL ----
                Text {
                    Layout.fillWidth: true
                    visible: root.currentSection === "ddl"
                    text: qsTr("Reconstructed by Quarry from the structure: it does not include CHECK constraints, triggers or engine options.")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WordWrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                QButton {
                    id: copyDdl

                    visible: root.currentSection === "ddl"
                    text: qsTr("Copy DDL")
                    iconGlyph: Icons.copy
                    onClicked: {
                        ValueViewer.copyText(root.info.ddl);
                        root.say(qsTr("DDL copied"), false);
                    }
                }

                Flickable {
                    id: ddlScroller

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.currentSection === "ddl"
                    clip: true
                    contentWidth: ddlText.implicitWidth
                    contentHeight: ddlText.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds

                    T.ScrollBar.vertical: QScrollBar {}
                    T.ScrollBar.horizontal: QScrollBar {}

                    TextEdit {
                        id: ddlText

                        readOnly: true
                        selectByKeyboard: true
                        selectByMouse: true
                        activeFocusOnTab: ddlScroller.visible
                        text: root.info.ddl
                        textFormat: TextEdit.PlainText
                        font.family: Theme.monoFontFamily
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        selectionColor: Theme.selectionBg
                        selectedTextColor: Theme.selectionFg

                        Accessible.role: Accessible.EditableText
                        Accessible.name: qsTr("DDL")
                        Accessible.description: qsTr("Reconstructed DDL of %1, read only").arg(root.info.name)

                        FocusRing {
                            anchors.fill: parent
                            shown: ddlText.activeFocus
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                    visible: !sectionTable.visible && root.currentSection !== "ddl"
                }
            }
        }

        EmptyState {
            id: stateless

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.view === "loading"
            focusable: visible
            iconGlyph: Icons.loaderCircle
            title: qsTr("Loading…")
            message: qsTr("Reading the properties of the table.")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacing * 2
            visible: root.view === "failed"
            spacing: Theme.spacing * 2

            ErrorPanel {
                Layout.fillWidth: true
                title: qsTr("The properties could not be read")
                message: root.info.message
                detail: root.info.detail
            }

            QButton {
                id: retryButton

                text: qsTr("Try again")
                iconGlyph: Icons.rotateCcw
                onClicked: {
                    root.structure.reload(root.tabId);
                    root.refresh();
                }
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }
}
