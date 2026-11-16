pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The History tab: a search field, the filters (this connection, only errors), the list of
// executed statements (newest first) and the actions on the current one. Keyboard: Tab goes
// search, filters, list, buttons; in the list Up/Down/Home/End move, Enter copies the statement
// to the editor, Delete removes the entry, Mod+C copies its SQL and Shift+F10 / Menu opens the
// menu (which also has Run). Nothing here runs a statement unless "Run" is chosen.
Item {
    id: root

    required property var history // HistoryModel
    required property var connections // ConnectionController
    // The connection of the current editor tab ("" = none): what "Only this connection" means.
    property string activeConnectionId: ""
    property string lastAnnouncement: ""

    readonly property alias entries: list
    readonly property alias searchField: search
    readonly property alias onlyConnectionBox: connectionBox
    readonly property alias onlyErrorsBox: errorsBox
    readonly property alias clearButton: clearBtn
    readonly property alias clearConfirm: clearDialog
    readonly property alias actionsMenu: actionMenu
    readonly property Item defaultFocus: search
    readonly property bool hasCurrent: list.currentIndex >= 0 && list.currentIndex < root.history.count
    readonly property bool filtered: root.history.text.trim() !== "" || root.history.connectionFilter !== "" || root.history.onlyErrors
    // Name of the connection the filter shows, "" for all of them.
    readonly property string scopeName: connectionName(root.history.connectionFilter)

    signal copyRequested(string sql, string connectionId)
    signal runRequested(string sql, string connectionId)

    function connectionName(id) {
        if (id === "")
            return "";
        const row = connections.model.indexOf(id);
        return row >= 0 ? connections.model.get(row).name : "";
    }

    function announce(text, assertive) {
        lastAnnouncement = text;
        list.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    function current() {
        return hasCurrent ? root.history.get(list.currentIndex) : null;
    }

    function copyEntry() {
        const e = current();
        if (e)
            root.copyRequested(e.sql, e.connectionId);
    }

    function runEntry() {
        const e = current();
        if (e)
            root.runRequested(e.sql, e.connectionId);
    }

    // The clipboard has no QML API: a hidden text item does it.
    function copySql() {
        const e = current();
        if (!e)
            return;
        clipboard.text = e.sql;
        clipboard.selectAll();
        clipboard.copy();
        clipboard.deselect();
        announce(qsTr("SQL copied"), false);
    }

    function deleteEntry() {
        if (!hasCurrent)
            return;
        const row = list.currentIndex;
        root.history.remove(row);
    }

    function askClear() {
        const name = scopeName;
        clearDialog.title = qsTr("Clear history");
        clearDialog.message = name !== "" ? qsTr("Delete all the history of “%1”? This cannot be undone.").arg(name) : qsTr("Delete the history of all connections? This cannot be undone.");
        clearDialog.confirmText = name !== "" ? qsTr("Clear history of “%1”").arg(name) : qsTr("Clear all history");
        clearDialog.open();
    }

    function showMenu(item, x, y) {
        if (hasCurrent)
            actionMenu.popup(item, x, y);
    }

    onActiveConnectionIdChanged: {
        // "Only this connection" follows the tab; a tab without connection keeps the last one.
        if (connectionBox.checked && activeConnectionId !== "")
            root.history.connectionFilter = activeConnectionId;
    }

    Connections {
        target: root.history

        function onAnnouncement(text, assertive) {
            root.announce(text, assertive);
        }

        // The list was rebuilt: keep a valid current row.
        function onCountChanged() {
            if (list.currentIndex >= root.history.count)
                list.currentIndex = root.history.count - 1;
            else if (list.currentIndex < 0 && root.history.count > 0)
                list.currentIndex = 0;
        }
    }

    TextEdit {
        id: clipboard

        visible: false
        Accessible.ignored: true
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing * 2
        spacing: Theme.spacing * 2

        QTextField {
            id: search

            Layout.fillWidth: true
            accessibleName: qsTr("Search history")
            placeholderText: qsTr("Search history")
            text: root.history.text
            onTextEdited: root.history.text = text
            // Down moves into the results without leaving the keyboard.
            Keys.onDownPressed: event => {
                if (list.count > 0) {
                    list.forceActiveFocus(Qt.TabFocusReason);
                    event.accepted = true;
                }
            }
        }

        // Flow: at 200 % the two filters wrap instead of being cut.
        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing * 2

            QCheckBox {
                id: connectionBox

                text: qsTr("Only this connection")
                // Without a connection in the tab there is nothing to filter by (unless it is on).
                enabled: root.activeConnectionId !== "" || checked
                checked: root.history.connectionFilter !== ""
                Accessible.description: root.activeConnectionId !== "" ? root.connectionName(root.activeConnectionId) : qsTr("The current tab has no connection")
                onToggled: root.history.connectionFilter = checked ? root.activeConnectionId : ""
            }

            QCheckBox {
                id: errorsBox

                text: qsTr("Only errors")
                checked: root.history.onlyErrors
                onToggled: root.history.onlyErrors = checked
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.scopeName !== ""
            text: qsTr("Showing the history of “%1”.").arg(root.scopeName)
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Theme.controlHeight * 4

            ListView {
                id: list

                anchors.fill: parent
                visible: root.history.count > 0
                clip: true
                model: root.history
                currentIndex: root.history.count > 0 ? 0 : -1
                activeFocusOnTab: true
                keyNavigationEnabled: true
                boundsBehavior: Flickable.StopAtBounds
                onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
                // Near the end the next page is read (the "Load more" button does the same).
                onAtYEndChanged: {
                    if (atYEnd && root.history.hasMore && !root.history.loading)
                        root.history.loadMore();
                }

                Accessible.role: Accessible.List
                Accessible.name: qsTr("History")
                Accessible.description: qsTr("Enter copies the statement to the editor. Delete removes it. Menu key opens more actions.")
                Accessible.focusable: true

                Keys.onReturnPressed: root.copyEntry()
                Keys.onEnterPressed: root.copyEntry()
                Keys.onDeletePressed: root.deleteEntry()
                Keys.onMenuPressed: root.showMenu(list, Theme.spacing * 2, 0)
                Keys.onPressed: event => {
                    // ListView moves with Up/Down only: Home and End are ours.
                    if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                        list.currentIndex = event.key === Qt.Key_Home ? 0 : list.count - 1;
                        event.accepted = true;
                    } else if (event.matches(StandardKey.Copy)) {
                        root.copySql();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
                        root.showMenu(list, Theme.spacing * 2, 0);
                        event.accepted = true;
                    }
                }

                delegate: Item {
                    id: entry

                    required property int index
                    required property string preview
                    required property string connectionName
                    required property string when
                    required property string detail
                    required property bool success
                    required property string errorMessage
                    readonly property bool current: ListView.isCurrentItem
                    readonly property color textColor: current ? Theme.selectionFg : Theme.fgDefault
                    readonly property color mutedColor: current ? Theme.selectionFg : Theme.fgMuted

                    width: ListView.view.width
                    implicitHeight: Math.max(Theme.controlHeight, content.implicitHeight + Theme.spacing * 2)

                    Accessible.role: Accessible.ListItem
                    Accessible.name: preview
                    Accessible.description: [success ? qsTr("Succeeded") : "", detail, connectionName, when, success ? "" : errorMessage, qsTr("%1 of %2").arg(index + 1).arg(root.history.count)].filter(s => s !== "").join(", ")
                    Accessible.selected: current
                    Accessible.focusable: true

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.radius
                        color: entry.current ? Theme.selectionBg : hover.hovered ? Theme.bgHover : Qt.alpha(Theme.bgSurface, 0)

                        // The current row is marked by a bar as well as by its fill.
                        Rectangle {
                            visible: entry.current
                            width: Theme.focusRingWidth
                            height: parent.height
                            color: Theme.accent
                        }
                    }

                    RowLayout {
                        id: content

                        anchors.fill: parent
                        anchors.margins: Theme.spacing
                        anchors.leftMargin: Theme.spacing * 2
                        spacing: Theme.spacing

                        // Succeeded or failed is said by the icon and by the words in `detail`.
                        Icon {
                            Layout.alignment: Qt.AlignTop
                            text: entry.success ? Icons.circleCheck : Icons.circleAlert
                            color: entry.success ? entry.mutedColor : (entry.current ? Theme.selectionFg : Theme.danger)
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Text {
                                Layout.fillWidth: true
                                text: entry.preview
                                font.family: Theme.monoFontFamily
                                font.pointSize: Theme.fontSize
                                color: entry.textColor
                                elide: Text.ElideRight
                                maximumLineCount: 2
                                wrapMode: Text.WrapAnywhere
                                Accessible.ignored: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: entry.detail
                                font.pointSize: Theme.fontSize
                                color: entry.mutedColor
                                wrapMode: Text.WordWrap
                                Accessible.ignored: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: [entry.connectionName, entry.when].filter(s => s !== "").join(" · ")
                                font.pointSize: Theme.fontSize
                                color: entry.mutedColor
                                wrapMode: Text.WordWrap
                                Accessible.ignored: true
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: !entry.success && entry.errorMessage !== ""
                                text: entry.errorMessage
                                font.pointSize: Theme.fontSize
                                color: entry.current ? Theme.selectionFg : Theme.danger
                                wrapMode: Text.WordWrap
                                Accessible.ignored: true
                            }
                        }
                    }

                    HoverHandler {
                        id: hover
                    }

                    TapHandler {
                        onTapped: {
                            list.currentIndex = entry.index;
                            list.forceActiveFocus(Qt.MouseFocusReason);
                        }
                        onDoubleTapped: {
                            list.currentIndex = entry.index;
                            root.copyEntry();
                        }
                    }

                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: eventPoint => {
                            list.currentIndex = entry.index;
                            list.forceActiveFocus(Qt.MouseFocusReason);
                            root.showMenu(entry, eventPoint.position.x, eventPoint.position.y);
                        }
                    }
                }

                FocusRing {
                    shown: list.activeFocus
                    inset: true
                }
            }

            EmptyState {
                anchors.fill: parent
                visible: root.history.count === 0 && !root.history.loading
                iconGlyph: Icons.history
                title: root.filtered ? qsTr("No matching history") : qsTr("No history yet")
                message: root.filtered ? qsTr("No statement matches the search and filters. Change them to see more.") : qsTr("Statements you run appear here, with their duration and result.")
                focusable: true
            }
        }

        // Loading is a state, said in words (and announced by the model when it ends).
        Text {
            Layout.fillWidth: true
            visible: root.history.loading
            text: qsTr("Loading…")
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing

            QButton {
                id: copyBtn

                text: qsTr("Copy to editor")
                enabled: root.hasCurrent
                Accessible.description: qsTr("Adds the statement at the end of the current query. Does not run it.")
                onClicked: root.copyEntry()
            }

            QButton {
                id: runBtn

                text: qsTr("Run")
                iconGlyph: Icons.play
                enabled: root.hasCurrent
                Accessible.description: qsTr("Adds the statement to the editor and runs it.")
                onClicked: root.runEntry()
            }

            QButton {
                id: copySqlBtn

                text: qsTr("Copy SQL")
                iconGlyph: Icons.copy
                enabled: root.hasCurrent
                onClicked: root.copySql()
            }

            QButton {
                id: deleteBtn

                text: qsTr("Delete entry")
                iconGlyph: Icons.trash2
                enabled: root.hasCurrent
                onClicked: root.deleteEntry()
            }

            QButton {
                id: moreBtn

                visible: root.history.hasMore
                text: qsTr("Load more")
                enabled: !root.history.loading
                onClicked: root.history.loadMore()
            }

            QButton {
                id: clearBtn

                text: qsTr("Clear history…")
                enabled: root.history.count > 0 || root.filtered
                onClicked: root.askClear()
            }
        }
    }

    ConfirmActionDialog {
        id: clearDialog

        onConfirmed: root.history.clear()
    }

    QMenu {
        id: actionMenu

        // The chosen action runs once the menu has closed, so the focus change it causes sticks.
        property string action: ""

        title: qsTr("History actions")
        onClosed: {
            const what = action;
            action = "";
            if (what !== "")
                Qt.callLater(() => {
                    if (what === "copy")
                        root.copyEntry();
                    else if (what === "run")
                        root.runEntry();
                    else if (what === "sql")
                        root.copySql();
                    else
                        root.deleteEntry();
                });
        }

        QMenuItem {
            text: qsTr("Copy to editor")
            onTriggered: actionMenu.action = "copy"
        }

        QMenuItem {
            text: qsTr("Run")
            onTriggered: actionMenu.action = "run"
        }

        QMenuItem {
            text: qsTr("Copy SQL")
            onTriggered: actionMenu.action = "sql"
        }

        QMenuItem {
            text: qsTr("Delete entry")
            onTriggered: actionMenu.action = "delete"
        }
    }
}
