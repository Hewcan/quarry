pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// The results grid (H1.7): a virtualized TableView over a ResultTableModel with a sticky header,
// one tab stop for the grid and another for "Fetch all", a rectangular selection (cell, range, row,
// column or everything), keyboard navigation, copy as TSV and resizable columns.
//
// Keys (the grid has the focus):
//   arrows move; Home / End first / last column of the row; Ctrl+Home / Ctrl+End first / last cell;
//   PageUp / PageDown a page of rows; Shift with any of them extends the selection from the anchor;
//   Ctrl+A everything; Shift+Space the row; Ctrl+Space the column;
//   Mod+C copies the selection as TSV, Mod+Shift+C with a row of column names;
//   Alt+Shift+Left / Right narrows / widens the column of the current cell; Esc stops "Fetch all".
//
// Screen readers (ARCHITECTURE section 13, plan B): Qt Quick has no table interface, so the grid
// is one item with the Table role, and every cell move says "Row X of N, <column>: <value>" with
// Accessible.announce, debounced so a held key does not flood the reader. Cells and headers are
// ignored by the accessibility tree.
FocusScope {
    id: root

    required property ResultTableModel results
    // The statement is still running: no "no rows" message yet.
    property bool running: false

    // The current cell and the anchor the selection grows from.
    property int currentRow: 0
    property int currentColumn: 0
    property int anchorRow: 0
    property int anchorColumn: 0
    // Inclusive rectangle of selected cells.
    property var selection: ({
            "top": 0,
            "left": 0,
            "bottom": 0,
            "right": 0
        })
    readonly property double selectedCells: results.rows > 0 && results.columns > 0 ? (selection.bottom - selection.top + 1) * (selection.right - selection.left + 1) : 0
    // Widths the user set, by column (object); the rest comes from the sample of the first rows.
    property var userWidths: ({})
    property int sampleRevision: 0
    // The last thing sent to the screen reader (tests read it).
    property string lastAnnouncement: ""
    // A short message in place of the counter (copy refused); cleared when the selection changes.
    property string notice: ""
    readonly property int rowHeight: Theme.controlHeight
    readonly property int minColumnWidth: Theme.controlHeight * 2
    readonly property int maxSampleWidth: Theme.controlHeight * 10
    readonly property int loadAheadRows: 100
    readonly property alias body: body
    readonly property alias tableView: table
    readonly property alias fetchButton: fetchButton
    readonly property alias header: headerRow
    readonly property string counterText: {
        const n = results.rows;
        const rows = n === 1 ? qsTr("1 row") : qsTr("%1 rows").arg(n.toLocaleString(Qt.locale(), "f", 0));
        if (results.fetchingAll)
            return qsTr("%1, loading all rows…").arg(rows);
        if (results.moreAvailable)
            return qsTr("%1, more available").arg(rows);
        if (results.truncated)
            return qsTr("%1, not all loaded").arg(rows);
        return rows;
    }

    function clamp(value, low, high) {
        return Math.max(low, Math.min(high, value));
    }

    // Everything sent to the screen reader (tests listen to it).
    signal announced(string text, bool assertive)

    function announce(text, assertive) {
        lastAnnouncement = text;
        root.Accessible.announce(text, assertive ? Accessible.Assertive : Accessible.Polite);
        announced(text, assertive);
    }

    // Width of a column: what the user set, or what its header and the first rows need.
    function widthOf(column) {
        const set = userWidths[column];
        if (set !== undefined)
            return set;
        void sampleRevision;
        return clamp(Math.ceil(results.sampleChars(column) * metrics.advanceWidth) + Theme.spacing * 4, minColumnWidth, maxSampleWidth);
    }

    function setWidth(column, width) {
        const widths = Object.assign({}, userWidths);
        widths[column] = clamp(Math.round(width), minColumnWidth, Theme.controlHeight * 40);
        userWidths = widths;
        table.forceLayout();
    }

    function setSelection(top, left, bottom, right) {
        selection = {
            "top": top,
            "left": left,
            "bottom": bottom,
            "right": right
        };
    }

    function cellDescription() {
        let text = qsTr("Row %1 of %2, %3: %4").arg(currentRow + 1).arg(results.rows).arg(results.columnName(currentColumn)).arg(results.cellText(currentRow, currentColumn));
        if (selectedCells > 1)
            text += ", " + qsTr("%1 cells selected").arg(selectedCells.toLocaleString(Qt.locale(), "f", 0));
        return text;
    }

    function resetSelection() {
        currentRow = 0;
        currentColumn = 0;
        anchorRow = 0;
        anchorColumn = 0;
        setSelection(0, 0, 0, 0);
        userWidths = ({});
        table.contentX = 0;
        table.contentY = 0;
        table.forceLayout();
    }

    // Moves the current cell; `extend` keeps the anchor and selects the rectangle to it.
    function moveTo(row, column, extend) {
        if (results.rows === 0 || results.columns === 0)
            return;
        currentRow = clamp(row, 0, results.rows - 1);
        currentColumn = clamp(column, 0, results.columns - 1);
        if (!extend) {
            anchorRow = currentRow;
            anchorColumn = currentColumn;
        }
        setSelection(Math.min(anchorRow, currentRow), Math.min(anchorColumn, currentColumn), Math.max(anchorRow, currentRow), Math.max(anchorColumn, currentColumn));
        table.positionViewAtCell(Qt.point(currentColumn, currentRow), TableView.Contain);
        announceTimer.restart();
    }

    function selectRow() {
        anchorRow = currentRow;
        anchorColumn = 0;
        setSelection(currentRow, 0, currentRow, results.columns - 1);
        announceTimer.restart();
    }

    function selectColumn(column) {
        if (results.rows === 0)
            return;
        currentColumn = clamp(column, 0, results.columns - 1);
        anchorRow = 0;
        anchorColumn = currentColumn;
        setSelection(0, currentColumn, results.rows - 1, currentColumn);
        announceTimer.restart();
    }

    function selectAll() {
        if (results.rows === 0 || results.columns === 0)
            return;
        anchorRow = 0;
        anchorColumn = 0;
        setSelection(0, 0, results.rows - 1, results.columns - 1);
        announceTimer.restart();
    }

    function copySelection(withHeader) {
        if (results.rows === 0 || results.columns === 0)
            return;
        const count = selectedCells.toLocaleString(Qt.locale(), "f", 0);
        if (selectedCells > results.maxCopyCells) {
            // Nothing is built or copied; the notice stays until the selection changes.
            notice = qsTr("Too many cells to copy (%1). Select fewer than %2 or use export.").arg(count).arg(results.maxCopyCells.toLocaleString(Qt.locale(), "f", 0));
            announce(notice, true);
            return;
        }
        results.copyToClipboard(selection.top, selection.left, selection.bottom, selection.right, withHeader);
        announce(qsTr("Copied %1 cells").arg(count), false);
    }

    function resizeColumn(column, delta) {
        setWidth(column, widthOf(column) + delta);
        announce(qsTr("Column %1, width %2 characters").arg(results.columnName(column)).arg(Math.round(widthOf(column) / metrics.advanceWidth)), false);
    }

    function pageRows() {
        return Math.max(1, Math.floor(table.height / rowHeight) - 1);
    }

    // Loads the next page when the view gets close to the end (a TableView does not call
    // fetchMore itself).
    function loadAhead() {
        if (results.moreAvailable && !results.fetching && table.bottomRow >= results.rows - loadAheadRows)
            results.loadMore();
    }

    function handleKey(event) {
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0;
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0;
        const alt = (event.modifiers & Qt.AltModifier) !== 0;
        event.accepted = true;
        if (event.key === Qt.Key_Escape) {
            event.accepted = results.fetchingAll;
            if (event.accepted)
                results.cancelFetchAll();
            return;
        }
        if (results.rows === 0 || results.columns === 0) {
            event.accepted = false;
            return;
        }
        const r = currentRow;
        const c = currentColumn;
        switch (event.key) {
        case Qt.Key_Left:
            if (alt && shift)
                resizeColumn(c, -Theme.spacing * 4);
            else
                moveTo(r, c - 1, shift);
            break;
        case Qt.Key_Right:
            if (alt && shift)
                resizeColumn(c, Theme.spacing * 4);
            else
                moveTo(r, c + 1, shift);
            break;
        case Qt.Key_Up:
            moveTo(r - 1, c, shift);
            break;
        case Qt.Key_Down:
            moveTo(r + 1, c, shift);
            break;
        case Qt.Key_PageUp:
            moveTo(r - pageRows(), c, shift);
            break;
        case Qt.Key_PageDown:
            moveTo(r + pageRows(), c, shift);
            break;
        case Qt.Key_Home:
            moveTo(ctrl ? 0 : r, 0, shift);
            break;
        case Qt.Key_End:
            moveTo(ctrl ? results.rows - 1 : r, results.columns - 1, shift);
            break;
        case Qt.Key_Space:
            if (shift && !ctrl)
                selectRow();
            else if (ctrl && !shift)
                selectColumn(c);
            else
                event.accepted = false;
            break;
        case Qt.Key_A:
            if (ctrl && !shift)
                selectAll();
            else
                event.accepted = false;
            break;
        case Qt.Key_C:
            if (ctrl)
                copySelection(shift);
            else
                event.accepted = false;
            break;
        default:
            event.accepted = false;
        }
    }

    onResultsChanged: resetSelection()
    onSelectionChanged: notice = ""

    Connections {
        target: root.results

        // A new result (also an empty one): back to the first cell.
        function onColumnsChanged() {
            root.resetSelection();
        }

        function onSampleChanged() {
            root.sampleRevision++;
            table.forceLayout();
        }

        function onRowsChanged() {
            root.loadAhead();
        }

        function onStateChanged() {
            root.loadAhead();
            if (root.results.fetchingAll && !fetchState.active) {
                fetchState.active = true;
                root.announce(qsTr("Loading all rows"), false);
            } else if (!root.results.fetchingAll && fetchState.active) {
                fetchState.active = false;
                if (root.results.moreAvailable)
                    root.announce(qsTr("Stopped. %1 rows loaded").arg(root.results.rows.toLocaleString(Qt.locale(), "f", 0)), false);
                else
                    root.announce(qsTr("All %1 rows loaded").arg(root.results.rows.toLocaleString(Qt.locale(), "f", 0)), false);
            }
            // The button goes away with the last page: the focus stays in the grid.
            if (!root.results.moreAvailable && !root.results.fetchingAll && fetchButton.activeFocus)
                body.forceActiveFocus(Qt.TabFocusReason);
        }
    }

    Connections {
        target: Theme

        function onMetricsChanged() {
            table.forceLayout();
        }
    }

    QtObject {
        id: fetchState

        property bool active: false
    }

    TextMetrics {
        id: metrics

        font.pointSize: Theme.fontSize
        text: "0"
    }

    Timer {
        id: announceTimer

        interval: 120
        onTriggered: root.announce(root.cellDescription(), false)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The sticky header: it follows the horizontal scroll of the table and nothing else.
        // The lines between cells are the 1 px gaps (spacing) with this colour behind: a cell is
        // only a rectangle and a text.
        Rectangle {
            id: headerClip

            color: Theme.borderDefault

            Layout.fillWidth: true
            implicitHeight: root.rowHeight
            clip: true
            visible: root.results.columns > 0

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                z: 1
                color: Theme.borderStrong
            }

            Row {
                id: headerRow

                spacing: 1

                x: -table.contentX

                Repeater {
                    model: root.results.columns

                    delegate: Rectangle {
                        id: head

                        required property int index
                        readonly property string columnTitle: root.results.columnName(index)
                        readonly property string typeTitle: root.results.nativeType(index)

                        width: root.widthOf(index)
                        height: root.rowHeight
                        color: root.selection.left <= index && index <= root.selection.right && root.selection.top === 0 && root.selection.bottom === root.results.rows - 1 && root.results.rows > 0 ? Theme.selectionBg : Theme.bgRaised

                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacing
                            anchors.rightMargin: Theme.spacing * 2
                            Accessible.ignored: true
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: head.columnTitle
                            font.pointSize: Theme.fontSize
                            font.bold: true
                            color: head.color === Theme.selectionBg ? Theme.selectionFg : Theme.fgDefault
                        }



                        HoverHandler {
                            id: hover
                        }

                        TapHandler {
                            onTapped: {
                                body.forceActiveFocus(Qt.MouseFocusReason);
                                root.selectColumn(head.index);
                            }
                        }

                        QToolTip {
                            visible: hover.hovered && head.typeTitle !== ""
                            delay: 700
                            text: head.columnTitle + " (" + head.typeTitle + ")"
                        }

                        // The edge: drag to resize (Alt+Shift+Left / Right does it from the keyboard).
                        MouseArea {
                            id: edge

                            property real startX: 0
                            property real startWidth: 0

                            anchors.right: parent.right
                            width: Theme.spacing * 2
                            height: parent.height
                            acceptedButtons: Qt.LeftButton
                            cursorShape: Qt.SplitHCursor
                            preventStealing: true
                            onPressed: mouse => {
                                startX = mapToItem(null, mouse.x, 0).x;
                                startWidth = head.width;
                            }
                            onPositionChanged: mouse => {
                                if (pressed)
                                    root.setWidth(head.index, startWidth + mapToItem(null, mouse.x, 0).x - startX);
                            }
                        }
                    }
                }
            }
        }

        Item {
            id: body

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: root.rowHeight
            focus: true
            activeFocusOnTab: true
            clip: true

            Accessible.role: Accessible.Table
            Accessible.name: qsTr("Results grid")
            Accessible.description: qsTr("%1 rows, %2 columns. Arrow keys move, Shift extends the selection, Ctrl+C copies, Ctrl+Shift+C copies with the column names, Alt+Shift+Left or Right resizes the column.").arg(root.results.rows).arg(root.results.columns)
            Accessible.focusable: true

            Keys.onPressed: event => root.handleKey(event)
            onActiveFocusChanged: {
                if (activeFocus && root.results.rows > 0)
                    announceTimer.restart();
            }

            Rectangle {
                anchors.fill: parent
                color: Theme.borderDefault
            }

            TableView {
                id: table

                anchors.fill: parent
                model: root.results
                clip: true
                reuseItems: true
                rowSpacing: 1
                columnSpacing: 1
                keyNavigationEnabled: false
                activeFocusOnTab: false
                pointerNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                rightMargin: vbar.width
                bottomMargin: hbar.height
                rowHeightProvider: () => root.rowHeight
                columnWidthProvider: column => root.widthOf(column)
                onBottomRowChanged: root.loadAhead()

                T.ScrollBar.vertical: QScrollBar {
                    id: vbar
                }

                T.ScrollBar.horizontal: QScrollBar {
                    id: hbar
                }

                delegate: Rectangle {
                    id: cell

                    required property int row
                    required property int column
                    required property string display
                    required property bool isNull
                    readonly property bool inSelection: row >= root.selection.top && row <= root.selection.bottom && column >= root.selection.left && column <= root.selection.right

                    color: inSelection ? Theme.selectionBg : row % 2 === 0 ? Theme.bgCanvas : Theme.bgSurface
                    border.width: row === root.currentRow && column === root.currentColumn && body.activeFocus ? Theme.focusRingWidth : 0
                    border.color: Theme.focusRing

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacing
                        anchors.rightMargin: Theme.spacing
                        Accessible.ignored: true
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: cell.display
                        font.pointSize: Theme.fontSize
                        // NULL is the word in italics, and muted: never the colour alone.
                        font.italic: cell.isNull
                        color: cell.inSelection ? Theme.selectionFg : cell.isNull ? Theme.gridNull : Theme.fgDefault
                    }
                }
            }

            MouseArea {
                id: pointer

                function cellAt(x, y) {
                    return table.cellAtPosition(root.clamp(x, 0, table.width - vbar.width - 1), root.clamp(y, 0, table.height - hbar.height - 1), true);
                }

                anchors.fill: table
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                onPressed: mouse => {
                    body.forceActiveFocus(Qt.MouseFocusReason);
                    const cell = cellAt(mouse.x, mouse.y);
                    if (cell.x >= 0 && cell.y >= 0)
                        root.moveTo(cell.y, cell.x, (mouse.modifiers & Qt.ShiftModifier) !== 0);
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return;
                    const cell = cellAt(mouse.x, mouse.y);
                    if (cell.x >= 0 && cell.y >= 0)
                        root.moveTo(cell.y, cell.x, true);
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - Theme.spacing * 4
                visible: root.results.columns > 0 && root.results.rows === 0 && !root.running
                text: qsTr("This query returned no rows.")
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            FocusRing {
                shown: body.activeFocus
                inset: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: footer.implicitHeight + Theme.spacing * 2
            color: Theme.bgSurface

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.borderDefault
            }

            RowLayout {
                id: footer

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.spacing * 2
                anchors.rightMargin: Theme.spacing * 2
                spacing: Theme.spacing * 2

                Text {
                    id: counter

                    Layout.fillWidth: true
                    text: root.notice !== "" ? root.notice : root.counterText
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    elide: Text.ElideRight

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                QButton {
                    id: fetchButton

                    visible: root.results.moreAvailable || root.results.fetchingAll
                    text: root.results.fetchingAll ? qsTr("Stop fetching") : qsTr("Fetch all")
                    onClicked: {
                        if (root.results.fetchingAll)
                            root.results.cancelFetchAll();
                        else
                            root.results.fetchAll();
                    }
                }
            }
        }
    }
}
