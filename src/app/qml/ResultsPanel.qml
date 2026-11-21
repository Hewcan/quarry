pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The content of the Results region (UX.md "Ejecucion"): the tabs Results (the QuarryGrid with the
// result of the last statement that returned columns) and Messages (what each statement did,
// notices and errors), and Plan (H9) once a plan was asked for in the tab. It shows the state of
// the current editor tab. An error switches to Messages without moving the focus; a normal run
// brings Results back to the front and leaves the plan where it is.
ColumnLayout {
    id: root

    required property var queries // QueryController
    // ExportController (null in tests that do not export).
    property var exporter: null
    readonly property var query: root.queries.current // QueryState
    readonly property alias tabBar: resultsTabs
    readonly property alias exportButton: exportBtn
    readonly property alias messageList: messageList
    // The Plan tab is the third one; its view is created with the first plan.
    readonly property int planIndex: 2
    readonly property bool planShown: root.query.plan.requested
    readonly property var planView: planLoader.item
    // The grid is created with the first result: nothing of it loads before the first frame.
    // A data tab shows its rows in the editor region; here only its messages.
    readonly property bool needsGrid: root.query.results.columns > 0 && !root.query.edits.active
    property bool gridWanted: false
    // null until the first result.
    readonly property QuarryGrid grid: gridLoader.item as QuarryGrid
    // What Mod+3 focuses: the grid when it has a result, else the tab.
    readonly property Item focusTarget: gridLoader.visible && root.grid && resultsTabs.currentIndex === 0 ? root.grid.body : resultsTabs.currentIndex === root.planIndex && root.planView ? root.planView.focusTarget : resultsTabs.currentItem
    readonly property alias resultsEmpty: resultsEmptyState
    readonly property alias messagesEmpty: messagesEmptyState

    spacing: 0

    // Brings the Plan tab to the front and puts the focus in it (the tree, once there is one).
    function showPlan() {
        if (!planShown)
            return;
        resultsTabs.currentIndex = planIndex;
        if (root.planView)
            root.planView.takeFocus();
    }

    Component.onCompleted: gridWanted = needsGrid
    // The tab of a plan goes with the plan (another editor tab may have none).
    onPlanShownChanged: {
        if (!planShown && resultsTabs.currentIndex === planIndex)
            resultsTabs.currentIndex = 0;
    }
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

        // A normal run: the results come back to the front; the plan stays for the next look.
        function onRunStarted(tabId) {
            if (root.queries.stateOf(tabId) === root.query && resultsTabs.currentIndex === root.planIndex)
                resultsTabs.currentIndex = 0;
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing

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

            // Only when a plan was asked for in this editor tab.
            Repeater {
                model: root.planShown ? 1 : 0

                QTabButton {
                    text: qsTr("Plan")
                }
            }
        }

        // Export of the current tab (UX.md "Exportacion"). When it cannot, the reason is in a tooltip
        // (the hover goes to the holder: a disabled button gets none) and in its description.
        Item {
            id: exportHolder

            Layout.rightMargin: Theme.spacing
            implicitWidth: exportBtn.implicitWidth
            implicitHeight: exportBtn.implicitHeight

            IconButton {
                id: exportBtn

                readonly property string reason: root.exporter?.info.reason ?? ""

                anchors.fill: parent
                accessibleName: qsTr("Export…")
                iconGlyph: Icons.download
                shortcutText: enabled ? CommandRegistry.find("export.open")?.shortcutText ?? "" : ""
                enabled: (root.exporter?.info.available ?? false) && !(root.exporter?.busy ?? false)
                Accessible.description: enabled ? shortcutText : reason
                onClicked: CommandRegistry.trigger("export.open")
            }

            QToolTip {
                visible: !exportBtn.enabled && exportHover.hovered && exportBtn.reason !== ""
                delay: 500
                text: exportBtn.reason
            }

            HoverHandler {
                id: exportHover
            }
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
                title: root.query.edits.active ? qsTr("Rows are shown in the data tab") : root.query.running ? root.query.statusText : root.query.state === "failed" ? qsTr("The query failed") : root.query.state === "cancelled" ? qsTr("Query cancelled") : root.query.state === "done" ? root.query.statusText : qsTr("No results yet")
                message: root.query.edits.active ? qsTr("Errors and what was saved appear in the Messages tab.") : root.query.running ? qsTr("Results appear here when the query ends.") : root.query.state === "failed" ? qsTr("See the Messages tab for the details.") : root.query.state === "done" ? qsTr("This statement returns no rows.") : qsTr("Run a query to see results here.")
            }

            // The result of the last statement that returned columns, as it streams in.
            Loader {
                id: gridLoader

                anchors.fill: parent
                active: root.gridWanted
                visible: root.needsGrid

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

                        // The output of a migration (H12 phase C): a line of stdout has no icon, a
                        // line of stderr says "stderr" and has one, and both are monospaced.
                        Icon {
                            Layout.alignment: Qt.AlignTop
                            visible: entry.kind !== "output"
                            text: entry.kind === "statement" ? Icons.circleCheck : entry.kind === "stderr" ? Icons.triangleAlert : Icons.info
                            color: entry.kind === "statement" ? Theme.success : entry.kind === "stderr" ? Theme.warning : Theme.fgMuted
                        }

                        Text {
                            Layout.fillWidth: true
                            text: entry.kind === "statement" || entry.kind === "cancelled" || entry.kind === "output" ? entry.text : entry.type + ": " + entry.text
                            font.family: entry.kind === "output" || entry.kind === "stderr" ? Theme.monoFontFamily : font.family
                            font.pointSize: Theme.fontSize
                            color: Theme.fgDefault
                            wrapMode: entry.kind === "output" || entry.kind === "stderr" ? Text.WrapAnywhere : Text.WordWrap
                        }
                    }
                }

                FocusRing {
                    shown: messageList.activeFocus
                    inset: true
                }
            }
        }

        // The plan of the last Explain of this editor tab. The view is created with the first one.
        Item {
            Loader {
                id: planLoader

                anchors.fill: parent
                active: root.planShown

                sourceComponent: PlanView {
                    plan: root.query.plan
                }
            }
        }
    }
}
