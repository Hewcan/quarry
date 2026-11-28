pragma ComponentBehavior: Bound
import QtQuick
import Quarry.Controls

// The application's menu bar, built from CommandRegistry: every command appears in exactly one
// menu, chosen by the prefix of its commandId (the category is a translated string, so it cannot
// be a key). A prefix not listed in `menuOfPrefix` lands in "Other". Titles, shortcuts, enabled and
// checked states are the live properties of the Command.
// What does not fit the width goes into a trailing "More" menu, as submenus; nothing is clipped.
QMenuBar {
    id: bar

    // commandId prefix -> menu key. General is split by id: the palette is navigation, settings
    // belongs with the view options and only About is help.
    readonly property var menuOfPrefix: ({
            "connection": "connection",
            "editor": "editor",
            "query": "editor",
            "ai": "editor",
            "data": "data",
            "table": "data",
            "export": "data",
            "history": "history",
            "saved": "history",
            "palette": "navigate",
            "focus": "navigate",
            "tree": "navigate",
            "view": "view",
            "settings": "view",
            "about": "help",
            "update": "help"
        })
    // Menu order, and for each menu its sections (a separator between them) in order. A command
    // of the menu that is not listed goes into a last section, in registration order.
    readonly property var menuOrder: ["connection", "editor", "data", "history", "navigate", "view", "help", "other"]
    readonly property var sectionsOf: ({
            "connection": [["connection.new", "connection.openSqlite"], ["connection.edit", "connection.duplicate", "connection.delete"], ["connection.disconnect", "connection.refreshMetadata"]],
            "editor": [["editor.newTab", "editor.closeTab", "editor.nextTab", "editor.previousTab"], ["editor.runStatement", "editor.runAll", "query.cancel"], ["editor.explain", "editor.explainAnalyze"], ["editor.suggest"], ["ai.toggleChat"]],
            "data": [["data.open", "data.refresh"], ["data.toggleRecord", "data.goToRelated", "data.sortByCurrentColumn", "table.showStructure"], ["data.insertRow", "data.duplicateRow", "data.deleteRows", "data.setNull"], ["data.undo", "data.redo"], ["data.preview", "data.apply", "data.discard"], ["export.open"]],
            "history": [["history.show", "saved.show"], ["saved.saveCurrent"], ["history.clear"]],
            "navigate": [["palette.open"], ["focus.sidebar", "focus.editor", "focus.results"], ["tree.focusFilter"], ["focus.nextRegion", "focus.previousRegion"]],
            "view": [["view.toggleSidebar", "view.toggleResults", "view.resetLayout"], ["view.zoomIn", "view.zoomOut", "view.zoomReset"], ["settings.open"]],
            "help": [["update.check"], ["about.open"]]
        })

    // [{ key, sections: [[Command]] }], in menu order; only menus with commands.
    property var groups: []
    readonly property int groupCount: groups.length
    // How many of the group menus fit; the others are in "More".
    readonly property int fitCount: {
        const n = bar.groupCount;
        const widthOf = i => bar.itemAt(i)?.implicitWidth ?? 0;
        if (bar.count <= n)
            return n;
        let total = 0;
        for (let i = 0; i < n; ++i)
            total += widthOf(i);
        if (total <= bar.width)
            return n;
        let used = widthOf(n);
        let k = 0;
        while (k < n && used + widthOf(k) <= bar.width) {
            used += widthOf(k);
            ++k;
        }
        return k;
    }
    readonly property bool overflowing: fitCount < groupCount
    // Where the focus was when the first menu opened; it goes back there once all are closed.
    property Item returnItem: null
    // The chosen command runs once the menu is closed, so its focus change sticks.
    property string pendingId: ""
    property bool dirty: false
    property var topMenus: []

    function titleOf(key) {
        switch (key) {
        case "connection":
            return qsTr("Connection");
        case "editor":
            return qsTr("Editor");
        case "data":
            return qsTr("Data");
        case "history":
            return qsTr("History");
        case "navigate":
            return qsTr("Navigate");
        case "view":
            return qsTr("View");
        case "help":
            return qsTr("Help");
        default:
            return qsTr("Other");
        }
    }

    function menuKeyOf(commandId) {
        return menuOfPrefix[commandId.split(".")[0]] ?? "other";
    }

    function buildGroups() {
        const byMenu = {};
        for (const c of CommandRegistry.commands) {
            const key = menuKeyOf(c.commandId);
            if (byMenu[key] === undefined)
                byMenu[key] = [];
            byMenu[key].push(c);
        }
        const out = [];
        for (const key of menuOrder) {
            const cmds = byMenu[key];
            if (cmds === undefined)
                continue;
            const sections = [];
            const placed = [];
            for (const ids of sectionsOf[key] ?? []) {
                const section = ids.map(id => cmds.find(c => c.commandId === id)).filter(c => c !== undefined && !placed.includes(c));
                placed.push(...section);
                if (section.length > 0)
                    sections.push(section);
            }
            const rest = cmds.filter(c => !placed.includes(c));
            if (rest.length > 0)
                sections.push(rest);
            out.push({
                key: key,
                sections: sections
            });
        }
        return out;
    }

    function anyOpen() {
        return topMenus.some(m => m.visible);
    }

    function rebuild() {
        if (anyOpen()) {
            dirty = true;
            return;
        }
        dirty = false;
        while (bar.count > 0)
            bar.removeMenu(bar.menuAt(bar.count - 1));
        topMenus = [];
        groups = buildGroups();
        const menus = [];
        for (let i = 0; i < groups.length; ++i) {
            const m = menuComp.createObject(bar, {
                group: groups[i],
                position: i
            });
            bar.addMenu(m);
            menus.push(m);
        }
        const more = menuComp.createObject(bar, {
            isMore: true,
            position: groups.length
        });
        bar.addMenu(more);
        menus.push(more);
        topMenus = menus;
        refreshMore();
    }

    // The overflow menu holds the hidden groups as submenus.
    function refreshMore() {
        const more = topMenus[topMenus.length - 1];
        if (!more)
            return;
        while (more.count > 0)
            more.removeItem(more.itemAt(0));
        for (let i = fitCount; i < groups.length; ++i)
            more.addMenu(menuComp.createObject(more, {
                group: groups[i],
                top: false
            }));
    }

    // Menu `position` is shown when it fits; the "More" item only when something overflowed.
    function shows(menu) {
        return menu.isMore ? overflowing : menu.position < fitCount;
    }

    function noteFocus() {
        if (returnItem === null)
            returnItem = bar.Window.window?.activeFocusItem ?? null;
    }

    function menuClosed() {
        Qt.callLater(bar.finish);
    }

    function finish() {
        if (anyOpen())
            return;
        const id = pendingId;
        pendingId = "";
        const back = returnItem;
        returnItem = null;
        // First the focus, then the command: the command may move it somewhere else on purpose.
        if (back !== null && back.visible)
            back.forceActiveFocus(Qt.OtherFocusReason);
        if (id !== "")
            CommandRegistry.trigger(id);
        if (dirty)
            rebuild();
    }

    // F10: opens the first menu; the arrow keys then walk the bar.
    function activate() {
        for (let i = 0; i < bar.count; ++i) {
            const item = bar.itemAt(i);
            if (item.visible && item.enabled) {
                noteFocus();
                item.triggered();
                return true;
            }
        }
        return false;
    }

    onFitCountChanged: Qt.callLater(bar.refreshMore)

    delegate: QMenuBarItem {
        id: entry

        visible: entry.menu === null || bar.shows(entry.menu)
        enabled: visible
    }

    Component.onCompleted: rebuild()

    Connections {
        target: CommandRegistry

        function onCommandsChanged() {
            Qt.callLater(bar.rebuild); // one rebuild for the burst of registrations at startup
        }
    }

    Component {
        id: separatorComp

        QMenuSeparator {}
    }

    Component {
        id: itemComp

        QMenuItem {
            id: item

            required property var command

            text: command.title
            shortcutText: command.shortcutText
            enabled: command.enabled
            checkable: command.checkable
            checked: command.checked
            onTriggered: bar.pendingId = command.commandId
        }
    }

    Component {
        id: menuComp

        QMenu {
            id: menu

            property var group: null
            property int position: 0
            property bool isMore: false
            // False for the submenus inside "More": the bar's focus handling belongs to the top ones.
            property bool top: true
            // The items of a group menu are made when it first opens: they are not on the startup path.
            property bool stale: true

            title: isMore ? qsTr("More") : bar.titleOf(group.key)

            // `made` keeps the items reachable: items without a parent are JS-owned, and the garbage
            // collector would delete them while the menu still lists them. (A non-Item parent
            // makes QML warn that the item is not in the scene.)
            property var made: []

            function populate() {
                while (menu.count > 0)
                    menu.removeItem(menu.itemAt(0));
                const items = [];
                group.sections.forEach((section, i) => {
                    if (i > 0)
                        items.push(separatorComp.createObject(null));
                    for (const c of section)
                        items.push(itemComp.createObject(null, {
                            command: c
                        }));
                });
                for (const it of items)
                    menu.addItem(it);
                made = items;
                stale = false;
            }

            onAboutToShow: {
                if (top)
                    bar.noteFocus();
                if (stale && !isMore)
                    populate();
            }
            onClosed: {
                if (top)
                    bar.menuClosed();
            }
        }
    }
}
