pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The side panel under the schema tree: two tabs, History and Saved. Left/Right on the tab bar
// switch tabs. It is created the first time it is shown, and each tab loads its data then (the
// history search and the saved-queries load start when the tab is first shown), so closed it
// costs nothing.
Item {
    id: root

    required property var history // HistoryModel
    required property var saved // SavedQueriesModel
    required property var connections // ConnectionController
    property string activeConnectionId: ""
    // 0 = History, 1 = Saved
    property int tabIndex: 0

    readonly property alias tabBar: bar
    readonly property alias historyView: historyTab
    readonly property alias savedView: savedTab
    readonly property Item defaultFocus: tabIndex === 0 ? historyTab.defaultFocus : savedTab.defaultFocus

    signal copyRequested(string sql, string connectionId)
    signal runRequested(string sql, string connectionId)
    signal openSavedRequested(string sql, string connectionId)
    signal saveCurrentRequested
    signal useConnectionRequested(string connectionId)
    signal closeRequested

    // The data is read the first time a tab is shown.
    function ensureLoaded() {
        if (tabIndex === 0)
            history.start();
        else
            saved.start();
    }

    // Popups belong to the overlay, not to the panel: they must not outlive it.
    function closeDialogs() {
        for (const d of [historyTab.clearConfirm, historyTab.actionsMenu, savedTab.nameDialog, savedTab.confirmDialog, savedTab.moveDialog, savedTab.queryMenu, savedTab.folderMenu])
            d.close();
    }

    function showTab(index) {
        tabIndex = index;
        bar.currentIndex = index;
        ensureLoaded();
    }

    onTabIndexChanged: {
        ensureLoaded();
        if (tabIndex === 0)
            historyTab.announce(qsTr("History"), false);
        else
            savedTab.announce(qsTr("Saved queries"), false);
    }

    Component.onCompleted: ensureLoaded()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            spacing: 0

            QTabBar {
                id: bar

                Layout.fillWidth: true
                accessibleName: qsTr("History and saved queries")
                currentIndex: root.tabIndex
                onCurrentIndexChanged: root.tabIndex = currentIndex

                QTabButton {
                    text: qsTr("History")
                    Accessible.description: qsTr("Statements you ran")
                }

                QTabButton {
                    text: qsTr("Saved")
                    Accessible.description: qsTr("Queries you saved")
                }
            }

            IconButton {
                accessibleName: qsTr("Close history and saved queries")
                iconGlyph: Icons.x
                onClicked: root.closeRequested()
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.tabIndex

            HistoryView {
                id: historyTab

                history: root.history
                connections: root.connections
                activeConnectionId: root.activeConnectionId
                onCopyRequested: (sql, connectionId) => root.copyRequested(sql, connectionId)
                onRunRequested: (sql, connectionId) => root.runRequested(sql, connectionId)
            }

            SavedView {
                id: savedTab

                saved: root.saved
                connections: root.connections
                onOpenRequested: (sql, connectionId) => root.openSavedRequested(sql, connectionId)
                onSaveCurrentRequested: root.saveCurrentRequested()
                onUseConnectionRequested: id => root.useConnectionRequested(id)
            }
        }
    }
}
