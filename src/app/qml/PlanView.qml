pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The Plan tab of the results panel (UX.md "Plan de consulta (H9)"): the plan of a statement as a
// tree of steps, each with its share of the cost (or time) as a bar and as text, the details of the
// selected step beside it, and the engine's own text as an alternative view. No box-and-arrow
// diagram: the tree is what a keyboard and a screen reader can walk.
//
// Keyboard (tree): Up/Down move, Left collapses or goes to the parent, Right expands or goes to
// the first child, Home/End first/last step, Enter or Space expands and collapses.
FocusScope {
    id: root

    required property var plan // PlanModel
    readonly property string state: root.plan.state
    // 0 = tree and details, 1 = engine text, 2 = a status (running, cancelled, nothing yet).
    readonly property int page: state === "ready" ? (viewBar.currentIndex === 1 ? 1 : 0) : state === "failed" && root.plan.rawText !== "" ? 1 : 2
    readonly property alias tree: treeLoader.item
    readonly property alias viewTabs: viewBar
    readonly property alias statusPage: statusView
    readonly property alias copyButton: copyBtn
    readonly property alias detailList: detailView
    readonly property alias errorPanel: errorView
    // The text view, null until it is shown (a big plan is not laid out for nothing).
    readonly property Item textEdit: textLoader.item ? (textLoader.item as PlanTextView).edit : null
    // Where the focus belongs in the current page.
    readonly property Item focusTarget: page === 0 && treeLoader.item ? treeLoader.item : page === 1 && root.textEdit ? root.textEdit : statusView
    readonly property var current: treeLoader.item ? treeLoader.item.selectionModel.currentIndex : null
    readonly property string headerText: [root.plan.engine, root.plan.analyzed ? qsTr("analyzed: the query was run") : qsTr("estimated: the query was not run"), root.plan.nodeCount === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(Qt.locale().toString(root.plan.nodeCount)), root.plan.planningTimeText !== "" ? qsTr("planning %1").arg(root.plan.planningTimeText) : "", root.plan.executionTimeText !== "" ? qsTr("execution %1").arg(root.plan.executionTimeText) : ""].filter(s => s !== "").join(" · ")
    readonly property string basisText: root.plan.barBasis === "time" ? qsTr("Bars show each step's share of the total execution time.") : root.plan.barBasis === "cost" ? qsTr("Bars show each step's share of the total estimated cost.") : qsTr("%1 gives no cost or time, so there are no bars.").arg(root.plan.engine !== "" ? root.plan.engine : qsTr("This engine"))
    // The tree takes the focus when the plan arrives, if it was asked to.
    property bool focusWhenReady: false

    // Moves the focus into the view: the tree when there is one, else the status.
    function takeFocus() {
        focusWhenReady = state === "running";
        focusTarget.forceActiveFocus(Qt.TabFocusReason);
    }

    // Everything the selected step says, in reading order: its numbers, its warnings, then the
    // engine's own details.
    function entriesOf(index) {
        const out = [];
        if (!index || !index.valid)
            return out;
        const p = root.plan;
        const add = (kind, name, value) => {
            if (value !== "")
                out.push({
                    "kind": kind,
                    "name": name,
                    "value": value
                });
        };
        add("metric", qsTr("Share"), p.data(index, PlanModel.ShareTextRole));
        add("metric", qsTr("Cost"), p.data(index, PlanModel.CostTextRole));
        add("metric", qsTr("Rows"), p.data(index, PlanModel.RowsTextRole));
        add("metric", qsTr("Time"), p.data(index, PlanModel.TimeTextRole));
        for (const w of p.data(index, PlanModel.WarningsRole))
            add("warning", qsTr("Warning"), w);
        for (const d of p.data(index, PlanModel.DetailsRole))
            add("detail", d.name, d.value);
        return out;
    }

    onPageChanged: {
        if (focusWhenReady && state !== "running") {
            focusWhenReady = false;
            focusTimer.restart();
        }
    }

    // After the page switch settled: the tree (or the text) exists by then.
    Timer {
        id: focusTimer

        interval: 0
        onTriggered: root.focusTarget.forceActiveFocus(Qt.TabFocusReason)
    }

    // The engine's text. Read-only, but with a caret so the keyboard can move and select in it.
    component PlanTextView: Item {
        id: holder

        required property var plan // PlanModel
        readonly property alias edit: textEdit

        Flickable {
            id: flick

            anchors.fill: parent
            clip: true
            contentWidth: Math.max(width, textEdit.contentWidth + textEdit.leftPadding * 2)
            contentHeight: Math.max(height, textEdit.contentHeight + textEdit.topPadding * 2)
            boundsBehavior: Flickable.StopAtBounds

            TextEdit {
                id: textEdit

                // A read-only TextEdit has no caret to move, so the keys scroll the text.
                function scrollBy(dx, dy) {
                    flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, flick.contentX + dx));
                    flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + dy));
                }

                width: Math.max(flick.width, contentWidth + leftPadding * 2)
                height: Math.max(flick.height, contentHeight + topPadding * 2)
                topPadding: Theme.spacing
                leftPadding: Theme.spacing * 2
                text: holder.plan.rawText
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                wrapMode: TextEdit.NoWrap
                textFormat: TextEdit.PlainText
                font.family: Theme.monoFontFamily
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                selectionColor: Theme.selectionBg
                selectedTextColor: Theme.selectionFg
                activeFocusOnTab: true

                Keys.onPressed: event => {
                    const line = Math.max(1, cursorRectangle.height);
                    event.accepted = true;
                    if (event.matches(StandardKey.SelectAll)) {
                        selectAll();
                    } else if (event.matches(StandardKey.Copy)) {
                        copy();
                    } else if (event.key === Qt.Key_Down) {
                        scrollBy(0, line);
                    } else if (event.key === Qt.Key_Up) {
                        scrollBy(0, -line);
                    } else if (event.key === Qt.Key_Right) {
                        scrollBy(line, 0);
                    } else if (event.key === Qt.Key_Left) {
                        scrollBy(-line, 0);
                    } else if (event.key === Qt.Key_PageDown) {
                        scrollBy(0, flick.height - line);
                    } else if (event.key === Qt.Key_PageUp) {
                        scrollBy(0, line - flick.height);
                    } else if (event.key === Qt.Key_Home) {
                        scrollBy(-flick.contentWidth, -flick.contentHeight);
                    } else if (event.key === Qt.Key_End) {
                        scrollBy(0, flick.contentHeight);
                    } else {
                        event.accepted = false;
                    }
                }

                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Plan text")
                Accessible.readOnly: true
                Accessible.multiLine: true
                Accessible.focusable: true
            }
        }

        FocusRing {
            shown: textEdit.activeFocus
            inset: true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        // What this plan is.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacing
            Layout.bottomMargin: 0
            visible: root.state === "ready" || root.state === "failed" || root.plan.notice !== ""
            spacing: Theme.spacing

            Text {
                Layout.fillWidth: true
                visible: root.state === "ready"
                text: root.headerText
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
                wrapMode: Text.WordWrap

                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Plan: %1").arg(text)
            }

            Text {
                Layout.fillWidth: true
                visible: root.state === "ready"
                text: root.basisText
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }

            // Cut at the parser's limit: said in words, with an icon.
            RowLayout {
                Layout.fillWidth: true
                visible: root.state === "ready" && root.plan.truncated
                spacing: Theme.spacing

                Icon {
                    Layout.alignment: Qt.AlignTop
                    text: Icons.triangleAlert
                    color: Theme.warning
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Showing the first %1 steps").arg(Qt.locale().toString(root.plan.nodeCount))
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.warning
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            // A request that was refused: why, in plain words.
            RowLayout {
                Layout.fillWidth: true
                visible: root.plan.notice !== ""
                spacing: Theme.spacing

                Icon {
                    Layout.alignment: Qt.AlignTop
                    text: Icons.info
                    color: Theme.fgMuted
                }

                Text {
                    Layout.fillWidth: true
                    text: root.plan.notice
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            ErrorPanel {
                id: errorView

                Layout.fillWidth: true
                visible: root.state === "failed"
                title: qsTr("The query plan could not be shown")
                message: root.plan.errorMessage
                hint: root.plan.rawText !== "" ? qsTr("The engine's own output is shown in the Text view.") : ""
                detail: root.plan.errorDetail
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.state === "ready"
            spacing: Theme.spacing

            QTabBar {
                id: viewBar

                Layout.fillWidth: true
                accessibleName: qsTr("Plan views")

                QTabButton {
                    text: qsTr("Tree")
                }

                QTabButton {
                    text: qsTr("Text")
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.page

            // 0: the tree and the details of its current step.
            RowLayout {
                spacing: 0

                Loader {
                    id: treeLoader

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 3
                    active: root.state === "ready"

                    sourceComponent: TreeView {
                        id: treeView

                        // The tree of steps is read in full at once: it is not a place to explore.
                        function expandAll() {
                            if (rows === 0 && root.plan.nodeCount > 0) {
                                expandTimer.restart(); // the view has not read the model yet
                                return;
                            }
                            expandRecursively();
                            if (!selectionModel.currentIndex.valid)
                                goTo(0);
                        }

                        function cursorRow() {
                            return selectionModel.currentIndex.valid ? rowAtIndex(selectionModel.currentIndex) : -1;
                        }

                        function goTo(row) {
                            if (row < 0 || row >= rows)
                                return;
                            selectionModel.setCurrentIndex(index(row, 0), ItemSelectionModel.ClearAndSelect);
                            positionViewAtRow(row, TableView.Contain);
                        }

                        function parentRow(row) {
                            const d = depth(row);
                            for (let r = row - 1; r >= 0; --r)
                                if (depth(r) < d)
                                    return r;
                            return -1;
                        }

                        function hasChildNodes(row) {
                            return root.plan.hasChildren(index(row, 0));
                        }

                        function toggleRow(row) {
                            if (isExpanded(row)) {
                                collapse(row);
                                // The branch takes the current step with it: it stays visible.
                                if (cursorRow() < 0)
                                    goTo(row);
                            } else {
                                expand(row);
                            }
                        }

                        clip: true
                        model: root.plan
                        selectionModel: ItemSelectionModel {
                            model: treeView.model
                        }
                        boundsBehavior: Flickable.StopAtBounds
                        activeFocusOnTab: true
                        keyNavigationEnabled: false
                        pointerNavigationEnabled: false
                        editTriggers: TableView.NoEditTriggers
                        columnWidthProvider: () => treeView.width
                        onWidthChanged: forceLayout()
                        Component.onCompleted: expandTimer.restart()

                        // Not Qt.callLater: the view can be gone (another editor tab) when it fires.
                        Timer {
                            id: expandTimer

                            interval: 0
                            onTriggered: treeView.expandAll()
                        }

                        Accessible.role: Accessible.Tree
                        Accessible.name: qsTr("Query plan")
                        Accessible.description: root.headerText
                        Accessible.focusable: true

                        Connections {
                            target: root.plan

                            function onModelReset() {
                                expandTimer.restart();
                            }
                        }

                        onActiveFocusChanged: {
                            if (activeFocus && !selectionModel.currentIndex.valid && rows > 0)
                                goTo(0);
                        }

                        Keys.onReturnPressed: if (cursorRow() >= 0 && hasChildNodes(cursorRow()))
                            toggleRow(cursorRow())
                        Keys.onEnterPressed: if (cursorRow() >= 0 && hasChildNodes(cursorRow()))
                            toggleRow(cursorRow())
                        Keys.onPressed: event => {
                            const row = cursorRow();
                            const page = Math.max(1, Math.floor(height / Theme.controlHeight) - 1);
                            event.accepted = true;
                            switch (event.key) {
                            case Qt.Key_Down:
                                goTo(Math.min(row + 1, rows - 1));
                                return;
                            case Qt.Key_Up:
                                goTo(Math.max(row - 1, 0));
                                return;
                            case Qt.Key_PageDown:
                                goTo(Math.min(Math.max(row, 0) + page, rows - 1));
                                return;
                            case Qt.Key_PageUp:
                                goTo(Math.max(row - page, 0));
                                return;
                            case Qt.Key_Home:
                                goTo(0);
                                return;
                            case Qt.Key_End:
                                goTo(rows - 1);
                                return;
                            case Qt.Key_Right:
                                if (row < 0)
                                    goTo(0);
                                else if (hasChildNodes(row) && !isExpanded(row))
                                    expand(row);
                                else if (isExpanded(row) && row + 1 < rows && depth(row + 1) > depth(row))
                                    goTo(row + 1);
                                return;
                            case Qt.Key_Left:
                                if (row >= 0 && isExpanded(row))
                                    toggleRow(row);
                                else if (row >= 0)
                                    goTo(parentRow(row));
                                return;
                            case Qt.Key_Space:
                                if (row >= 0 && hasChildNodes(row)) {
                                    toggleRow(row);
                                    return;
                                }
                                break;
                            }
                            event.accepted = false;
                        }

                        delegate: Item {
                            id: node

                            required property TreeView treeView
                            required property bool isTreeNode
                            required property bool expanded
                            required property bool hasChildren
                            required property int depth
                            required property int row
                            required property int column
                            required property bool current
                            required property string label
                            required property string costText
                            required property string rowsText
                            required property string timeText
                            required property real costFraction
                            required property string costPercentText
                            required property var warnings
                            required property string accessibleDescription
                            required property bool hasBar

                            readonly property bool expandable: isTreeNode && hasChildren
                            readonly property color textColor: current ? Theme.selectionFg : Theme.fgDefault
                            readonly property color mutedColor: current ? Theme.selectionFg : Theme.fgMuted
                            // Time when the plan was analyzed, else the cost.
                            readonly property string measure: timeText !== "" ? timeText : costText

                            implicitHeight: Math.max(Theme.controlHeight, content.implicitHeight + Theme.spacing * 2)

                            Accessible.role: Accessible.TreeItem
                            Accessible.name: label
                            Accessible.description: accessibleDescription
                            Accessible.expandable: expandable
                            Accessible.expanded: expanded
                            Accessible.selected: current
                            Accessible.focusable: true
                            Accessible.focused: current && node.treeView.activeFocus

                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.radius
                                color: node.current ? Theme.selectionBg : hover.hovered ? Theme.bgHover : Qt.alpha(Theme.bgSurface, 0)

                                // The current step is marked by a bar as well as by its fill.
                                Rectangle {
                                    visible: node.current
                                    width: Theme.focusRingWidth
                                    height: parent.height
                                    color: Theme.accent
                                }
                            }

                            ColumnLayout {
                                id: content

                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacing + node.depth * Theme.spacing * 4
                                anchors.rightMargin: Theme.spacing * 2
                                anchors.topMargin: Theme.spacing
                                spacing: 0

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.spacing

                                    // The expander. Decorative: the state is on the item itself.
                                    Item {
                                        id: expander

                                        Layout.alignment: Qt.AlignTop
                                        Layout.preferredWidth: Theme.spacing * 6
                                        Layout.preferredHeight: Math.max(Theme.controlHeight - Theme.spacing * 2, Theme.spacing * 6)

                                        Icon {
                                            anchors.centerIn: parent
                                            visible: node.expandable
                                            text: node.expanded ? Icons.chevronDown : Icons.chevronRight
                                            color: node.mutedColor
                                        }
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignTop
                                        Layout.topMargin: Math.max(0, (expander.Layout.preferredHeight - implicitHeight) / 2)
                                        text: node.label
                                        font.pointSize: Theme.fontSize
                                        font.bold: true
                                        color: node.textColor
                                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                        Accessible.ignored: true // the item carries name and description
                                    }
                                }

                                // The numbers of the step: its share as a bar and as text, rows, time or cost.
                                Flow {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: expander.Layout.preferredWidth + Theme.spacing
                                    Layout.bottomMargin: Theme.spacing
                                    spacing: Theme.spacing * 2

                                    Row {
                                        visible: node.hasBar
                                        spacing: Theme.spacing

                                        Rectangle {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: Theme.controlHeight * 3
                                            height: Theme.spacing * 2
                                            radius: Theme.radius
                                            color: node.current ? Theme.selectionBg : Theme.bgHover
                                            border.width: 1
                                            border.color: node.current ? Theme.selectionFg : Theme.borderStrong

                                            Rectangle {
                                                width: parent.width * node.costFraction
                                                height: parent.height
                                                radius: parent.radius
                                                color: node.current ? Theme.selectionFg : Theme.accent
                                            }
                                        }

                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: node.costPercentText
                                            font.pointSize: Theme.fontSize
                                            font.bold: true
                                            color: node.textColor
                                            Accessible.ignored: true
                                        }
                                    }

                                    Text {
                                        visible: text !== ""
                                        text: node.rowsText
                                        font.pointSize: Theme.fontSize
                                        color: node.mutedColor
                                        Accessible.ignored: true
                                    }

                                    Text {
                                        visible: text !== ""
                                        text: node.measure
                                        font.pointSize: Theme.fontSize
                                        color: node.mutedColor
                                        Accessible.ignored: true
                                    }

                                    // A warning is an icon and words, never only a color.
                                    Row {
                                        visible: node.warnings.length > 0
                                        spacing: Theme.spacing

                                        Icon {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: Icons.triangleAlert
                                            color: node.current ? Theme.selectionFg : Theme.warning
                                        }

                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: node.warnings.length === 1 ? qsTr("1 warning") : qsTr("%1 warnings").arg(node.warnings.length)
                                            font.pointSize: Theme.fontSize
                                            color: node.current ? Theme.selectionFg : Theme.warning
                                            Accessible.ignored: true
                                        }
                                    }
                                }
                            }

                            HoverHandler {
                                id: hover
                            }

                            TapHandler {
                                onTapped: point => {
                                    node.treeView.selectionModel.setCurrentIndex(node.treeView.index(node.row, 0), ItemSelectionModel.ClearAndSelect);
                                    node.treeView.forceActiveFocus(Qt.MouseFocusReason);
                                    if (node.expandable && point.position.x < expander.x + expander.width + content.anchors.leftMargin)
                                        // qmllint disable missing-property
                                        node.treeView.toggleRow(node.row);
                                        // qmllint enable missing-property
                                }
                                onDoubleTapped: point => {
                                    if (node.expandable && point.position.x >= expander.x + expander.width + content.anchors.leftMargin)
                                        // qmllint disable missing-property
                                        node.treeView.toggleRow(node.row);
                                        // qmllint enable missing-property
                                }
                            }
                        }

                        FocusRing {
                            parent: treeView // not the flickable's content item
                            shown: treeView.activeFocus
                            inset: true
                        }
                    }
                }

                Rectangle {
                    Layout.fillHeight: true
                    implicitWidth: 1
                    color: Theme.borderDefault
                }

                // The selected step: its numbers, its warnings and the engine's details.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 2
                    Layout.margins: Theme.spacing
                    spacing: Theme.spacing

                    Text {
                        Layout.fillWidth: true
                        text: root.current && root.current.valid ? root.plan.data(root.current, PlanModel.LabelRole) : ""
                        font.pointSize: Theme.fontSize
                        font.bold: true
                        color: Theme.fgDefault
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        Accessible.ignored: true // the list below carries it in its name
                    }

                    ListView {
                        id: detailView

                        readonly property string stepName: root.current && root.current.valid ? root.plan.data(root.current, PlanModel.LabelRole) : ""

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: root.entriesOf(root.current)
                        spacing: Theme.spacing
                        activeFocusOnTab: true
                        boundsBehavior: Flickable.StopAtBounds

                        Accessible.role: Accessible.List
                        Accessible.name: stepName !== "" ? qsTr("Details of %1").arg(stepName) : qsTr("Step details")
                        Accessible.focusable: true

                        delegate: ColumnLayout {
                            id: entry

                            required property var modelData
                            readonly property bool warning: modelData.kind === "warning"

                            width: detailView.width
                            spacing: 0

                            Accessible.role: Accessible.ListItem
                            Accessible.name: modelData.name + ": " + modelData.value

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing

                                Icon {
                                    Layout.alignment: Qt.AlignTop
                                    visible: entry.warning
                                    text: Icons.triangleAlert
                                    color: Theme.warning
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: entry.warning ? entry.modelData.name + ": " + entry.modelData.value : entry.modelData.name
                                    font.pointSize: Theme.fontSize
                                    font.bold: true
                                    color: entry.warning ? Theme.warning : Theme.fgMuted
                                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                    Accessible.ignored: true
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: !entry.warning
                                text: entry.modelData.value
                                font.family: entry.modelData.kind === "detail" ? Theme.monoFontFamily : Qt.application.font.family
                                font.pointSize: Theme.fontSize
                                color: Theme.fgDefault
                                wrapMode: Text.WrapAnywhere
                                Accessible.ignored: true
                            }
                        }

                        FocusRing {
                            shown: detailView.activeFocus
                            inset: true
                        }
                    }
                }
            }

            // 1: the engine's text, to select and copy.
            ColumnLayout {
                spacing: Theme.spacing

                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: Theme.spacing
                    Layout.bottomMargin: 0

                    Item {
                        Layout.fillWidth: true
                    }

                    QButton {
                        id: copyBtn

                        text: qsTr("Copy text")
                        iconGlyph: Icons.copy
                        variant: "flat"
                        enabled: root.plan.rawText !== ""
                        onClicked: {
                            if (!root.textEdit)
                                return;
                            root.textEdit.selectAll();
                            root.textEdit.copy();
                            root.textEdit.deselect();
                        }
                    }
                }

                Loader {
                    id: textLoader

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: root.page === 1

                    sourceComponent: PlanTextView {
                        plan: root.plan
                    }
                }
            }

            // 2: nothing to show yet, and why.
            EmptyState {
                id: statusView

                focusable: true
                iconGlyph: root.state === "running" ? Icons.loaderCircle : root.state === "failed" ? Icons.circleAlert : Icons.listTree
                title: root.state === "running" ? (root.plan.analyzed ? qsTr("Running the query to analyze it…") : qsTr("Getting the query plan…")) : root.state === "cancelled" ? qsTr("Plan cancelled") : root.state === "failed" ? qsTr("No plan to show") : qsTr("No plan yet")
                message: root.state === "running" ? qsTr("The plan appears here when it is ready. Esc cancels.") : root.state === "cancelled" ? qsTr("Nothing was shown. Explain the statement again to get the plan.") : root.state === "failed" ? qsTr("The error is above.") : qsTr("Press %1 in the editor to see the plan of the current statement.").arg(CommandRegistry.displayShortcut("Ctrl+E"))
            }
        }
    }
}
