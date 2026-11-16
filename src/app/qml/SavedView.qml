pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The Saved tab: saved queries and their folders (one level) as an accessible tree. Keyboard: Up/
// Down/Home/End move, Right expands a folder or goes to its first query, Left collapses or goes
// to the parent folder, Enter opens a query (copies it to the editor) or expands/collapses a
// folder, F2 renames, Delete deletes after asking, Shift+F10 / Menu opens the actions menu (which
// has "Move to…"). Nothing needs the mouse: moving is a dialog, not a drag.
Item {
    id: root

    required property var saved // SavedQueriesModel
    required property var connections // ConnectionController
    property string lastAnnouncement: ""
    // Set after opening a query that belongs to a connection the tab does not use: an offer, not a
    // change. {connectionId, name} or null.
    property var offer: null

    readonly property alias tree: list
    readonly property alias saveButton: saveBtn
    readonly property alias newFolderButton: folderBtn
    readonly property alias openButton: openBtn
    readonly property alias actionsButton: actionsBtn
    readonly property alias nameDialog: nameDlg
    readonly property alias confirmDialog: confirmDlg
    readonly property alias moveDialog: moveDlg
    readonly property alias queryMenu: queryActions
    readonly property alias folderMenu: folderActions
    readonly property alias offerButton: offerBtn
    readonly property Item defaultFocus: list.visible ? list : saveBtn
    readonly property bool hasCurrent: list.currentIndex >= 0 && list.currentIndex < root.saved.count
    readonly property bool currentIsQuery: hasCurrent && current().kind === "query"
    readonly property bool currentIsFolder: hasCurrent && current().kind === "folder"

    signal openRequested(string sql, string connectionId)
    signal saveCurrentRequested
    signal useConnectionRequested(string connectionId)

    function announce(text, assertive) {
        lastAnnouncement = text;
        list.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
    }

    function current() {
        return hasCurrent ? root.saved.get(list.currentIndex) : null;
    }

    // ---- actions -----------------------------------------------------------------------------

    function openCurrent() {
        const q = current();
        if (q && q.kind === "query")
            root.openRequested(q.sql, q.connectionId);
    }

    // Name dialog for a new saved query (the panel passes what the editor holds).
    function askSave(sql, connectionId) {
        nameDlg.mode = "save";
        nameDlg.sql = sql;
        nameDlg.connectionId = connectionId;
        nameDlg.targetId = "";
        nameDlg.fieldLabel = qsTr("Name of the saved query");
        nameDlg.openFor(qsTr("Save query"), "", qsTr("Save"), sql.trim());
    }

    function askNewFolder() {
        nameDlg.mode = "newFolder";
        nameDlg.targetId = "";
        nameDlg.fieldLabel = qsTr("Name of the folder");
        nameDlg.openFor(qsTr("New folder"), "", qsTr("Create"), "");
    }

    function askRename() {
        const item = current();
        if (!item)
            return;
        nameDlg.targetId = item.itemId;
        if (item.kind === "folder") {
            nameDlg.mode = "renameFolder";
            nameDlg.fieldLabel = qsTr("Name of the folder");
            nameDlg.openFor(qsTr("Rename folder"), item.name, qsTr("Rename"), "");
        } else {
            nameDlg.mode = "renameQuery";
            nameDlg.fieldLabel = qsTr("Name of the saved query");
            nameDlg.openFor(qsTr("Rename saved query"), item.name, qsTr("Rename"), "");
        }
    }

    function askMove() {
        const item = current();
        if (!item || item.kind !== "query")
            return;
        moveDlg.targetId = item.itemId;
        moveDlg.openFor(item.name, root.saved.folders(), item.folderId);
    }

    function askDelete() {
        const item = current();
        if (!item)
            return;
        confirmDlg.targetId = item.itemId;
        if (item.kind === "folder") {
            const n = root.saved.queriesIn(item.itemId);
            confirmDlg.isFolder = true;
            confirmDlg.title = qsTr("Delete folder");
            confirmDlg.message = n === 0 ? qsTr("Delete the folder “%1”? It is empty.").arg(item.name) : n === 1 ? qsTr("Delete the folder “%1”? Its query is not deleted: it moves to the top level.").arg(item.name) : qsTr("Delete the folder “%1”? Its %2 queries are not deleted: they move to the top level.").arg(item.name).arg(n);
            confirmDlg.confirmText = qsTr("Delete folder");
        } else {
            confirmDlg.isFolder = false;
            confirmDlg.title = qsTr("Delete saved query");
            confirmDlg.message = qsTr("Delete “%1”? This cannot be undone.").arg(item.name);
            confirmDlg.confirmText = qsTr("Delete query");
        }
        confirmDlg.open();
    }

    function toggleCurrent() {
        if (currentIsFolder)
            root.saved.setExpanded(list.currentIndex, !current().expanded);
    }

    function showMenu(item, x, y) {
        if (currentIsQuery)
            queryActions.popup(item, x, y);
        else if (currentIsFolder)
            folderActions.popup(item, x, y);
    }

    function showOffer(connectionId, name) {
        offer = {
            "connectionId": connectionId,
            "name": name
        };
        announce(qsTr("This query is for connection %1. Press the Use button to switch this tab to it.").arg(name), false);
    }

    function dismissOffer() {
        offer = null;
    }

    // Right / Left on the keyboard.
    function expandOrDescend() {
        const item = current();
        if (!item || item.kind !== "folder")
            return;
        if (!item.expanded)
            root.saved.setExpanded(list.currentIndex, true);
        else if (item.childCount > 0)
            list.currentIndex = list.currentIndex + 1;
    }

    function collapseOrAscend() {
        const item = current();
        if (!item)
            return;
        if (item.kind === "folder") {
            if (item.expanded)
                root.saved.setExpanded(list.currentIndex, false);
        } else if (item.depth === 1) {
            for (let i = list.currentIndex - 1; i >= 0; --i) {
                if (root.saved.get(i).kind === "folder") {
                    list.currentIndex = i;
                    return;
                }
            }
        }
    }

    Connections {
        target: root.saved

        function onAnnouncement(text, assertive) {
            root.announce(text, assertive);
        }

        // Selects what was just saved or created, so the user sees where it went.
        function onQuerySaved(id) {
            Qt.callLater(() => {
                const row = root.saved.indexOfId(id);
                if (row >= 0)
                    list.currentIndex = row;
            });
        }

        function onFolderSaved(id) {
            Qt.callLater(() => {
                const row = root.saved.indexOfId(id);
                if (row >= 0)
                    list.currentIndex = row;
            });
        }

        function onChanged() {
            if (list.currentIndex >= root.saved.count)
                list.currentIndex = root.saved.count - 1;
            else if (list.currentIndex < 0 && root.saved.count > 0)
                list.currentIndex = 0;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing * 2
        spacing: Theme.spacing * 2

        // The offer to use the query's connection: shown, never applied on its own.
        RowLayout {
            Layout.fillWidth: true
            visible: root.offer !== null
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: Icons.info
                color: Theme.fgMuted
            }

            Text {
                Layout.fillWidth: true
                text: root.offer ? qsTr("This query is for connection “%1”.").arg(root.offer.name) : ""
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            QButton {
                id: offerBtn

                text: root.offer ? qsTr("Use %1").arg(root.offer.name) : ""
                onClicked: {
                    const id = root.offer.connectionId;
                    root.offer = null;
                    root.useConnectionRequested(id);
                }
            }

            IconButton {
                accessibleName: qsTr("Dismiss")
                iconGlyph: Icons.x
                onClicked: root.dismissOffer()
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Theme.controlHeight * 4

            ListView {
                id: list

                anchors.fill: parent
                visible: root.saved.count > 0
                clip: true
                model: root.saved
                currentIndex: root.saved.count > 0 ? 0 : -1
                activeFocusOnTab: true
                keyNavigationEnabled: true
                boundsBehavior: Flickable.StopAtBounds
                onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

                Accessible.role: Accessible.Tree
                Accessible.name: qsTr("Saved queries")
                Accessible.description: qsTr("Enter opens a query. Right and Left expand and collapse folders. F2 renames, Delete deletes, Menu key opens more actions.")
                Accessible.focusable: true

                Keys.onReturnPressed: root.currentIsFolder ? root.toggleCurrent() : root.openCurrent()
                Keys.onEnterPressed: root.currentIsFolder ? root.toggleCurrent() : root.openCurrent()
                Keys.onRightPressed: root.expandOrDescend()
                Keys.onLeftPressed: root.collapseOrAscend()
                Keys.onDeletePressed: root.askDelete()
                Keys.onMenuPressed: root.showMenu(list, Theme.spacing * 2, 0)
                Keys.onPressed: event => {
                    // ListView moves with Up/Down only: Home and End are ours.
                    if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                        list.currentIndex = event.key === Qt.Key_Home ? 0 : list.count - 1;
                        event.accepted = true;
                    } else if (event.key === Qt.Key_F2) {
                        root.askRename();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
                        root.showMenu(list, Theme.spacing * 2, 0);
                        event.accepted = true;
                    }
                }

                delegate: Item {
                    id: node

                    required property int index
                    required property string kind
                    required property string itemId
                    required property string name
                    required property string preview
                    required property int depth
                    required property bool expanded
                    required property int childCount
                    required property string connectionName
                    required property int position
                    required property int siblings
                    readonly property bool isFolder: kind === "folder"
                    readonly property bool current: ListView.isCurrentItem
                    readonly property color textColor: current ? Theme.selectionFg : Theme.fgDefault
                    readonly property color mutedColor: current ? Theme.selectionFg : Theme.fgMuted

                    function describe() {
                        const parts = [];
                        if (isFolder)
                            parts.push(qsTr("Folder"), childCount === 1 ? qsTr("1 query") : qsTr("%1 queries").arg(childCount));
                        else
                            parts.push(qsTr("Saved query"), connectionName !== "" ? qsTr("for connection %1").arg(connectionName) : qsTr("any connection"));
                        parts.push(qsTr("level %1").arg(depth + 1), qsTr("%1 of %2").arg(position).arg(siblings));
                        return parts.join(", ");
                    }

                    width: ListView.view.width
                    implicitHeight: Math.max(Theme.controlHeight, content.implicitHeight + Theme.spacing * 2)

                    Accessible.role: Accessible.TreeItem
                    Accessible.name: name
                    Accessible.description: describe()
                    Accessible.expandable: isFolder
                    Accessible.expanded: isFolder && expanded
                    Accessible.selected: current
                    Accessible.focusable: true

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.radius
                        color: node.current ? Theme.selectionBg : hover.hovered ? Theme.bgHover : Qt.alpha(Theme.bgSurface, 0)

                        Rectangle {
                            visible: node.current
                            width: Theme.focusRingWidth
                            height: parent.height
                            color: Theme.accent
                        }
                    }

                    RowLayout {
                        id: content

                        anchors.fill: parent
                        anchors.margins: Theme.spacing
                        anchors.leftMargin: Theme.spacing * 2 + node.depth * Theme.controlHeight
                        spacing: Theme.spacing

                        Icon {
                            Layout.alignment: Qt.AlignTop
                            visible: node.isFolder
                            text: node.expanded ? Icons.chevronDown : Icons.chevronRight
                            color: node.mutedColor
                        }

                        Icon {
                            Layout.alignment: Qt.AlignTop
                            text: node.isFolder ? (node.expanded ? Icons.folderOpen : Icons.folder) : Icons.bookmark
                            color: node.mutedColor
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Text {
                                Layout.fillWidth: true
                                text: node.isFolder ? qsTr("%1 (%2)").arg(node.name).arg(node.childCount) : node.name
                                font.pointSize: Theme.fontSize
                                font.bold: node.isFolder
                                color: node.textColor
                                elide: Text.ElideRight
                                Accessible.ignored: true
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: !node.isFolder
                                text: node.preview
                                font.family: Theme.monoFontFamily
                                font.pointSize: Theme.fontSize
                                color: node.mutedColor
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                Accessible.ignored: true
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: !node.isFolder && node.connectionName !== ""
                                text: qsTr("Connection: %1").arg(node.connectionName)
                                font.pointSize: Theme.fontSize
                                color: node.mutedColor
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
                            list.currentIndex = node.index;
                            list.forceActiveFocus(Qt.MouseFocusReason);
                        }
                        onDoubleTapped: {
                            list.currentIndex = node.index;
                            if (node.isFolder)
                                root.toggleCurrent();
                            else
                                root.openCurrent();
                        }
                    }

                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: eventPoint => {
                            list.currentIndex = node.index;
                            list.forceActiveFocus(Qt.MouseFocusReason);
                            root.showMenu(node, eventPoint.position.x, eventPoint.position.y);
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
                visible: root.saved.count === 0 && !root.saved.loading
                iconGlyph: Icons.bookmark
                title: qsTr("No saved queries yet")
                message: qsTr("Keep the statements you use often. Save the one in the editor, then organise them in folders.")
                focusable: true

                QButton {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Save current query…")
                    onClicked: root.saveCurrentRequested()
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.saved.loading
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
                id: saveBtn

                text: qsTr("Save current query…")
                iconGlyph: Icons.save
                onClicked: root.saveCurrentRequested()
            }

            QButton {
                id: folderBtn

                text: qsTr("New folder…")
                iconGlyph: Icons.folderPlus
                onClicked: root.askNewFolder()
            }

            QButton {
                id: openBtn

                text: qsTr("Open")
                enabled: root.currentIsQuery
                Accessible.description: qsTr("Copies the query to the editor.")
                onClicked: root.openCurrent()
            }

            IconButton {
                id: actionsBtn

                accessibleName: qsTr("More actions")
                iconGlyph: Icons.ellipsis
                enabled: root.hasCurrent
                onClicked: root.showMenu(actionsBtn, 0, actionsBtn.height)
            }
        }
    }

    NameDialog {
        id: nameDlg

        // "save" | "newFolder" | "renameQuery" | "renameFolder"
        property string mode: "save"
        property string targetId: ""
        property string sql: ""
        property string connectionId: ""

        onNamed: name => {
            if (mode === "save")
                root.saved.saveQuery(name, sql, connectionId, "");
            else if (mode === "newFolder")
                root.saved.newFolder(name);
            else if (mode === "renameQuery")
                root.saved.renameQuery(targetId, name);
            else
                root.saved.renameFolder(targetId, name);
        }
    }

    ConfirmActionDialog {
        id: confirmDlg

        property string targetId: ""
        property bool isFolder: false

        onConfirmed: {
            if (isFolder)
                root.saved.removeFolder(targetId);
            else
                root.saved.removeQuery(targetId);
        }
    }

    MoveToDialog {
        id: moveDlg

        property string targetId: ""

        onChosen: folderId => root.saved.moveQuery(targetId, folderId)
    }

    // Each menu runs its action once it has closed, so the dialog it opens remembers the list
    // as the place to return the focus to.
    QMenu {
        id: queryActions

        property string action: ""

        title: qsTr("Saved query actions")
        onClosed: {
            const what = action;
            action = "";
            if (what !== "")
                Qt.callLater(() => {
                    if (what === "open")
                        root.openCurrent();
                    else if (what === "rename")
                        root.askRename();
                    else if (what === "move")
                        root.askMove();
                    else
                        root.askDelete();
                });
        }

        QMenuItem {
            text: qsTr("Open")
            onTriggered: queryActions.action = "open"
        }

        QMenuItem {
            text: qsTr("Rename…")
            onTriggered: queryActions.action = "rename"
        }

        QMenuItem {
            text: qsTr("Move to…")
            onTriggered: queryActions.action = "move"
        }

        QMenuItem {
            text: qsTr("Delete…")
            onTriggered: queryActions.action = "delete"
        }
    }

    QMenu {
        id: folderActions

        property string action: ""

        title: qsTr("Folder actions")
        onClosed: {
            const what = action;
            action = "";
            if (what !== "")
                Qt.callLater(() => {
                    if (what === "toggle")
                        root.toggleCurrent();
                    else if (what === "rename")
                        root.askRename();
                    else
                        root.askDelete();
                });
        }

        QMenuItem {
            text: qsTr("Expand or collapse")
            onTriggered: folderActions.action = "toggle"
        }

        QMenuItem {
            text: qsTr("Rename folder…")
            onTriggered: folderActions.action = "rename"
        }

        QMenuItem {
            text: qsTr("Delete folder…")
            onTriggered: folderActions.action = "delete"
        }
    }
}
