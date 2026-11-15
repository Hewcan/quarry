pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The content of the Results region (UX.md "Ejecucion"): the tabs Results (the QuarryGrid with the
// result of the last statement that returned columns) and Messages (what each statement did,
// notices and errors). It shows the state of the current editor tab. An error switches to Messages
// without moving the focus.
ColumnLayout {
    id: root

    required property var queries // QueryController
    readonly property var query: root.queries.current // QueryState
    readonly property alias tabBar: resultsTabs
    readonly property alias messageList: messageList
    // The grid is created with the first result: nothing of it loads before the first frame.
    readonly property bool needsGrid: root.query.results.columns > 0
    property bool gridWanted: false
    // null until the first result.
    readonly property QuarryGrid grid: gridLoader.item as QuarryGrid
    // What Mod+3 focuses: the grid when it has a result, else the tab.
    readonly property Item focusTarget: gridLoader.visible && root.grid && resultsTabs.currentIndex === 0 ? root.grid.body : resultsTabs.currentItem
    readonly property alias resultsEmpty: resultsEmptyState
    readonly property alias messagesEmpty: messagesEmptyState

    spacing: 0

    Component.onCompleted: gridWanted = needsGrid
    onNeedsGridChanged: {
        if (needsGrid)
            gridWanted = true;
    }

    Connections {
        target: root.queries

        function onPlanFailed(tabId) {
            if (root.queries.stateOf(tabId) === root.query)
                resultsTabs.currentIndex = 1;
        }
    }

    QTabBar {
        id: resultsTabs

        Layout.fillWidth: true
        accessibleName: qsTr("Results panel tabs")

        QTabButton {
            text: qsTr("Results")
        }

        QTabButton {
            text: qsTr("Messages")
        }
    }

    StackLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        currentIndex: resultsTabs.currentIndex

        Item {
            EmptyState {
                id: resultsEmptyState

                anchors.fill: parent
                visible: !gridLoader.visible
                iconGlyph: root.query.running ? Icons.loaderCircle : root.query.state === "done" ? Icons.circleCheck : Icons.table
                title: root.query.running ? root.query.statusText : root.query.state === "failed" ? qsTr("The query failed") : root.query.state === "cancelled" ? qsTr("Query cancelled") : root.query.state === "done" ? root.query.statusText : qsTr("No results yet")
                message: root.query.running ? qsTr("Results appear here when the query ends.") : root.query.state === "failed" ? qsTr("See the Messages tab for the details.") : root.query.state === "done" ? qsTr("This statement returns no rows.") : qsTr("Run a query to see results here.")
            }

            // The result of the last statement that returned columns, as it streams in.
            Loader {
                id: gridLoader

                anchors.fill: parent
                active: root.gridWanted
                visible: root.query.results.columns > 0

                sourceComponent: QuarryGrid {
                    results: root.query.results
                    running: root.query.running
                }
            }
        }

        Item {
            EmptyState {
                id: messagesEmptyState

                anchors.fill: parent
                visible: root.query.messages.count === 0
                iconGlyph: Icons.info
                title: qsTr("No messages.")
                message: qsTr("Errors, notices and affected row counts appear here.")
            }

            ListView {
                id: messageList

                anchors.fill: parent
                anchors.margins: Theme.spacing
                visible: root.query.messages.count > 0
                model: root.query.messages
                clip: true
                spacing: Theme.spacing * 2
                activeFocusOnTab: visible
                boundsBehavior: Flickable.StopAtBounds

                Accessible.role: Accessible.List
                Accessible.name: qsTr("Messages")
                Accessible.focusable: true

                delegate: Item {
                    id: entry

                    required property int index
                    required property string kind
                    required property string type
                    required property string text
                    required property string detail
                    required property int line
                    required property int column

                    width: messageList.width
                    implicitHeight: kind === "error" ? errorPanel.implicitHeight : plain.implicitHeight
                    height: implicitHeight

                    Accessible.role: Accessible.ListItem
                    Accessible.name: type + ": " + text + (line > 0 ? ", " + qsTr("line %1, column %2").arg(line).arg(column) : "")

                    // Errors: icon, the type in words, the message, and the driver text apart.
                    ErrorPanel {
                        id: errorPanel

                        width: parent.width
                        visible: entry.kind === "error"
                        title: entry.type
                        message: entry.text
                        hint: entry.line > 0 ? qsTr("Line %1, column %2").arg(entry.line).arg(entry.column) : ""
                        detail: entry.detail
                    }

                    RowLayout {
                        id: plain

                        width: parent.width
                        visible: entry.kind !== "error"
                        spacing: Theme.spacing

                        Icon {
                            Layout.alignment: Qt.AlignTop
                            text: entry.kind === "statement" ? Icons.circleCheck : Icons.info
                            color: entry.kind === "statement" ? Theme.success : Theme.fgMuted
                        }

                        Text {
                            Layout.fillWidth: true
                            text: entry.kind === "statement" || entry.kind === "cancelled" ? entry.text : entry.type + ": " + entry.text
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                FocusRing {
                    shown: messageList.activeFocus
                    inset: true
                }
            }
        }
    }
}
