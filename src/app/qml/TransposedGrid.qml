pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// Transposed view (toggled with Tab in a table grid): fields (columns) on the left with their type
// badges, and one value column per selected row on the right. Overlays the table grid viewport and
// fills any remaining space to the right and below with striped filler cells, matching QuarryGrid.
FocusScope {
    id: root

    required property ResultTableModel results
    property var edits: null
    property int startRow: 0
    property int endRow: 0
    property int rowHeight: Theme.controlHeight

    readonly property int minColumnWidth: Theme.controlHeight * 2
    readonly property int maxSampleWidth: Theme.controlHeight * 10
    readonly property int fieldCount: transposedModel.rows
    readonly property int valueColumnCount: transposedModel.columns
    readonly property alias model: transposedModel
    readonly property alias body: tBody
    readonly property alias focusTarget: tBody
    readonly property alias tableView: tTable
    readonly property alias verticalBar: tvbar
    readonly property alias horizontalBar: thbar
    readonly property alias filler: filler
    readonly property alias belowRows: belowRepeater
    readonly property alias rightRows: rightRepeater
    readonly property alias lineMarks: lineRepeater
    readonly property alias fieldBelowRows: fieldBelowRepeater
    readonly property alias headerFiller: headerFiller
    readonly property alias fieldList: fieldColumn
    readonly property alias headerRow: valueHeaderRow

    // Current cell and rectangular selection inside the value grid.
    property int currentRow: 0
    property int currentColumn: 0
    property int anchorRow: 0
    property int anchorColumn: 0
    property var selection: ({
            "top": 0,
            "left": 0,
            "bottom": 0,
            "right": 0
        })
    property real userFieldWidth: -1
    property var userWidths: ({})

    readonly property real autoFieldWidth: {
        let maxChars = qsTr("Field").length;
        for (let c = 0; c < root.results.columns; ++c)
            maxChars = Math.max(maxChars, root.results.columnName(c).length);
        const badgeExtra = Math.ceil(badgeMetrics.advanceWidth) + Theme.spacing;
        return clamp(Math.ceil(maxChars * boldMetrics.advanceWidth) + badgeExtra + Theme.spacing * 4, Theme.controlHeight * 3, Theme.controlHeight * 12);
    }
    readonly property real fieldColWidth: userFieldWidth > 0 ? userFieldWidth : autoFieldWidth

    signal closeRequested
    signal announced(string text, bool assertive)

    function clamp(value, low, high) {
        return Math.max(low, Math.min(high, value));
    }

    function rowHeaderText(colIndex) {
        const srcRow = root.startRow + colIndex;
        return root.edits ? root.edits.rowLabel(srcRow) : qsTr("Row %1").arg(srcRow + 1);
    }

    function widthOf(column) {
        const set = userWidths[column];
        if (set !== undefined)
            return set;
        const headerChars = rowHeaderText(column).length + 2;
        const chars = Math.max(headerChars, transposedModel.sampleChars(column));
        return clamp(Math.ceil(chars * metrics.advanceWidth) + Theme.spacing * 4, minColumnWidth, maxSampleWidth);
    }

    function setWidth(column, width) {
        const widths = Object.assign({}, userWidths);
        widths[column] = clamp(Math.round(width), minColumnWidth, Theme.controlHeight * 40);
        userWidths = widths;
        tTable.forceLayout();
    }

    function setSelection(top, left, bottom, right) {
        selection = {
            "top": top,
            "left": left,
            "bottom": bottom,
            "right": right
        };
    }

    function moveTo(row, column, extend) {
        if (fieldCount === 0 || valueColumnCount === 0)
            return;
        currentRow = clamp(row, 0, fieldCount - 1);
        currentColumn = clamp(column, 0, valueColumnCount - 1);
        if (!extend) {
            anchorRow = currentRow;
            anchorColumn = currentColumn;
        }
        setSelection(Math.min(anchorRow, currentRow), Math.min(anchorColumn, currentColumn), Math.max(anchorRow, currentRow), Math.max(anchorColumn, currentColumn));
        tTable.positionViewAtCell(Qt.point(currentColumn, currentRow), TableView.Contain);
    }

    function selectAll() {
        if (fieldCount === 0 || valueColumnCount === 0)
            return;
        anchorRow = 0;
        anchorColumn = 0;
        setSelection(0, 0, fieldCount - 1, valueColumnCount - 1);
    }

    function pageRows() {
        return Math.max(1, Math.floor(tTable.height / rowHeight) - 1);
    }

    function handleKey(event) {
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0;
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0;
        const alt = (event.modifiers & Qt.AltModifier) !== 0;
        const meta = (event.modifiers & Qt.MetaModifier) !== 0;
        event.accepted = true;
        if (event.key === Qt.Key_Tab && !shift && !ctrl && !alt && !meta) {
            root.closeRequested();
            return;
        }
        if (fieldCount === 0 || valueColumnCount === 0) {
            event.accepted = false;
            return;
        }
        const r = currentRow;
        const c = currentColumn;
        switch (event.key) {
        case Qt.Key_Left:
            moveTo(r, c - 1, shift);
            break;
        case Qt.Key_Right:
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
            moveTo(ctrl ? fieldCount - 1 : r, valueColumnCount - 1, shift);
            break;
        case Qt.Key_A:
            if (ctrl && !shift)
                selectAll();
            else
                event.accepted = false;
            break;
        case Qt.Key_C:
            if (ctrl) {
                const count = (selection.bottom - selection.top + 1) * (selection.right - selection.left + 1);
                transposedModel.copyToClipboard(selection.top, selection.left, selection.bottom, selection.right);
                root.announced(qsTr("Copied %1 cells").arg(count.toLocaleString(Qt.locale(), "f", 0)), false);
            } else {
                event.accepted = false;
            }
            break;
        default:
            event.accepted = false;
        }
    }

    onStartRowChanged: transposedModel.setRange(startRow, endRow)
    onEndRowChanged: transposedModel.setRange(startRow, endRow)

    Component.onCompleted: transposedModel.setRange(startRow, endRow)

    TransposedTableModel {
        id: transposedModel

        source: root.edits ? root.edits : root.results
    }

    TextMetrics {
        id: metrics

        font.pointSize: Theme.fontSize
        text: "0"
    }

    TextMetrics {
        id: boldMetrics

        font.pointSize: Theme.fontSize
        font.bold: true
        text: "0"
    }

    TextMetrics {
        id: badgeMetrics

        font.family: Theme.monoFontFamily
        font.pointSize: Math.max(6, Theme.fontSize * 0.72)
        font.bold: true
        text: "A-Z"
    }

    Connections {
        target: Theme

        function onMetricsChanged() {
            tTable.forceLayout();
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Sticky header: "Field" corner cell on the left, selected row labels scrolling on the right.
        Rectangle {
            id: headerBar

            Layout.fillWidth: true
            implicitHeight: root.rowHeight
            color: Theme.borderDefault
            clip: true

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                z: 5
                color: Theme.borderStrong
            }

            // Corner header cell ("Field") aligned with the sticky field column.
            Rectangle {
                id: cornerCell

                x: 0
                width: root.fieldColWidth
                height: root.rowHeight
                z: 4
                color: Theme.bgRaised

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing
                    anchors.rightMargin: Theme.spacing * 2
                    Accessible.ignored: true
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: qsTr("Field")
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                }

                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: Theme.borderStrong
                }

                MouseArea {
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
                        startWidth = root.fieldColWidth;
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            root.userFieldWidth = root.clamp(Math.round(startWidth + mapToItem(null, mouse.x, 0).x - startX), root.minColumnWidth, Theme.controlHeight * 24);
                    }
                }
            }

            // Clipped row headers to the right of the corner cell.
            Item {
                id: valueHeaderClip

                x: root.fieldColWidth
                width: Math.max(0, headerBar.width - x)
                height: root.rowHeight
                clip: true

                Rectangle {
                    id: headerFiller

                    x: filler.endX
                    width: Math.max(0, valueHeaderClip.width - x)
                    height: root.rowHeight
                    visible: width > 0
                    color: Theme.bgRaised
                    Accessible.ignored: true
                }

                Row {
                    id: valueHeaderRow

                    spacing: 1
                    x: -tTable.contentX

                    Repeater {
                        model: root.valueColumnCount

                        delegate: Rectangle {
                            id: rHead

                            required property int index
                            readonly property string headerTitle: root.rowHeaderText(index)

                            width: root.widthOf(index)
                            height: root.rowHeight
                            color: Theme.bgRaised

                            Text {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacing
                                anchors.rightMargin: Theme.spacing * 2
                                Accessible.ignored: true
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: rHead.headerTitle
                                font.pointSize: Theme.fontSize
                                font.bold: true
                                color: Theme.fgDefault
                            }

                            MouseArea {
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
                                    startWidth = rHead.width;
                                }
                                onPositionChanged: mouse => {
                                    if (pressed)
                                        root.setWidth(rHead.index, startWidth + mapToItem(null, mouse.x, 0).x - startX);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Body: sticky field column on the left + virtualized value TableView and filler cells on the right.
        Item {
            id: tBody

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: root.rowHeight
            focus: true
            activeFocusOnTab: true
            clip: true

            Accessible.role: Accessible.Table
            Accessible.name: qsTr("Record table")
            Accessible.description: qsTr("%1 fields, %2 rows. Arrow keys move, Ctrl+C copies, Tab returns to the table view.").arg(root.fieldCount).arg(root.valueColumnCount)
            Accessible.focusable: true

            Keys.onPressed: event => root.handleKey(event)

            Rectangle {
                anchors.fill: parent
                color: Theme.borderDefault
            }

            // Left sticky column: field names with their type badges, synced with vertical scroll,
            // plus empty filler rows below if there is spare vertical space.
            Item {
                id: fieldPane

                x: 0
                y: 0
                width: root.fieldColWidth
                height: filler.areaHeight
                clip: true
                z: 2

                Repeater {
                    id: fieldBelowRepeater

                    model: filler.belowCount

                    delegate: Rectangle {
                        required property int index

                        y: filler.endY + index * filler.stride
                        width: fieldPane.width - 1
                        height: root.rowHeight
                        Accessible.ignored: true
                        color: (root.fieldCount + index) % 2 === 0 ? Theme.bgCanvas : Theme.bgSurface
                    }
                }

                Column {
                    id: fieldColumn

                    y: -tTable.contentY
                    spacing: 1
                    width: fieldPane.width - 1

                    Repeater {
                        model: root.fieldCount

                        delegate: Rectangle {
                            id: fieldCell

                            required property int index
                            readonly property string columnTitle: root.results.columnName(index)
                            readonly property string typeTitle: root.results.nativeType(index)
                            readonly property string typeKind: root.results.columnTypeKind(index)

                            width: fieldColumn.width
                            height: root.rowHeight
                            color: Theme.bgRaised

                            ColumnTypeBadge {
                                id: fieldBadge

                                anchors.left: parent.left
                                anchors.leftMargin: Theme.spacing
                                anchors.verticalCenter: parent.verticalCenter
                                kind: fieldCell.typeKind
                                badgeColor: Theme.fgMuted
                            }

                            Text {
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.left: fieldBadge.visible ? fieldBadge.right : parent.left
                                anchors.leftMargin: Theme.spacing
                                anchors.right: parent.right
                                anchors.rightMargin: Theme.spacing
                                Accessible.ignored: true
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: fieldCell.columnTitle
                                font.pointSize: Theme.fontSize
                                font.bold: true
                                color: Theme.fgDefault
                            }

                            HoverHandler {
                                id: fieldHover
                            }

                            QToolTip {
                                visible: fieldHover.hovered && fieldCell.typeTitle !== ""
                                delay: 700
                                text: fieldCell.columnTitle + " (" + fieldCell.typeTitle + ")"
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: Theme.borderStrong
                    Accessible.ignored: true
                }
            }

            // Right values viewport: filler cells behind + virtualized TableView on top.
            Item {
                id: valuePane

                x: root.fieldColWidth
                y: 0
                width: Math.max(0, tBody.width - root.fieldColWidth)
                height: tBody.height
                clip: true

                Item {
                    id: filler

                    readonly property var edges: {
                        void root.userWidths;
                        void root.valueColumnCount;
                        const out = [];
                        let sum = 0;
                        for (let c = 0; c < root.valueColumnCount; ++c) {
                            sum += root.widthOf(c) + tTable.columnSpacing;
                            out.push(sum);
                        }
                        return out;
                    }
                    readonly property real stride: root.rowHeight + tTable.rowSpacing
                    readonly property real areaWidth: Math.max(0, valuePane.width - tvbar.width)
                    readonly property real areaHeight: Math.max(0, valuePane.height - thbar.height)
                    readonly property real endX: (edges.length > 0 ? edges[edges.length - 1] : 0) - tTable.contentX
                    readonly property real endY: root.fieldCount * stride - tTable.contentY
                    readonly property int screenRows: Math.ceil(areaHeight / stride) + 1
                    readonly property int belowCount: endY >= areaHeight ? 0 : Math.min(screenRows + 1, Math.ceil((areaHeight - endY) / stride))
                    readonly property int firstRow: Math.floor(Math.max(0, tTable.contentY) / stride)
                    readonly property int rightCount: endX >= areaWidth || firstRow >= root.fieldCount ? 0 : Math.min(screenRows, root.fieldCount - firstRow)
                    readonly property int firstLine: {
                        let low = 0;
                        let high = edges.length;
                        while (low < high) {
                            const mid = (low + high) >> 1;
                            if (edges[mid] - tTable.columnSpacing - tTable.contentX < 0)
                                low = mid + 1;
                            else
                                high = mid;
                        }
                        return low;
                    }
                    readonly property int lineCount: {
                        if (belowCount === 0)
                            return 0;
                        let n = 0;
                        while (firstLine + n < edges.length && edges[firstLine + n] - tTable.columnSpacing - tTable.contentX < areaWidth)
                            ++n;
                        return n;
                    }

                    width: areaWidth
                    height: areaHeight
                    clip: true
                    Accessible.ignored: true

                    Repeater {
                        id: belowRepeater

                        model: filler.belowCount

                        delegate: Rectangle {
                            required property int index

                            y: filler.endY + index * filler.stride
                            width: filler.areaWidth
                            height: root.rowHeight
                            Accessible.ignored: true
                            color: (root.fieldCount + index) % 2 === 0 ? Theme.bgCanvas : Theme.bgSurface
                        }
                    }

                    Repeater {
                        id: rightRepeater

                        model: filler.rightCount

                        delegate: Rectangle {
                            required property int index

                            x: Math.max(0, filler.endX)
                            y: (filler.firstRow + index) * filler.stride - tTable.contentY
                            width: filler.areaWidth - x
                            height: root.rowHeight
                            Accessible.ignored: true
                            color: (filler.firstRow + index) % 2 === 0 ? Theme.bgCanvas : Theme.bgSurface
                        }
                    }

                    Repeater {
                        id: lineRepeater

                        model: filler.lineCount

                        delegate: Rectangle {
                            required property int index

                            x: filler.edges[filler.firstLine + index] - tTable.columnSpacing - tTable.contentX
                            y: Math.max(0, filler.endY)
                            width: tTable.columnSpacing
                            height: filler.areaHeight - y
                            color: Theme.borderDefault
                            Accessible.ignored: true
                        }
                    }
                }

                TableView {
                    id: tTable

                    anchors.fill: parent
                    model: transposedModel
                    clip: true
                    reuseItems: true
                    rowSpacing: 1
                    columnSpacing: 1
                    keyNavigationEnabled: false
                    activeFocusOnTab: false
                    pointerNavigationEnabled: false
                    boundsBehavior: Flickable.StopAtBounds
                    rightMargin: tvbar.width
                    bottomMargin: thbar.height
                    rowHeightProvider: () => root.rowHeight
                    columnWidthProvider: column => root.widthOf(column)

                    T.ScrollBar.vertical: QScrollBar {
                        id: tvbar
                    }

                    T.ScrollBar.horizontal: QScrollBar {
                        id: thbar
                    }

                    delegate: Rectangle {
                        id: cell

                        required property int row
                        required property int column
                        required property string display
                        required property bool isNull
                        required property int cellState
                        required property int rowState
                        readonly property bool deleted: rowState === 3
                        readonly property bool inSelection: row >= root.selection.top && row <= root.selection.bottom && column >= root.selection.left && column <= root.selection.right

                        color: inSelection ? Theme.selectionBg : cellState === 1 ? Qt.alpha(Theme.warning, 0.22) : row % 2 === 0 ? Theme.bgCanvas : Theme.bgSurface
                        border.width: row === root.currentRow && column === root.currentColumn && tBody.activeFocus ? Theme.focusRingWidth : 0
                        border.color: Theme.focusRing

                        Icon {
                            id: changedMark

                            visible: cell.cellState === 1
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.spacing
                            anchors.verticalCenter: parent.verticalCenter
                            text: Icons.pencil
                            color: cell.inSelection ? Theme.selectionFg : Theme.fgDefault
                        }

                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacing
                            anchors.rightMargin: Theme.spacing + (changedMark.visible ? changedMark.width + Theme.spacing : 0)
                            Accessible.ignored: true
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: cell.display
                            font.pointSize: Theme.fontSize
                            font.italic: cell.isNull
                            font.strikeout: cell.deleted
                            color: cell.inSelection ? Theme.selectionFg : cell.isNull || cell.deleted ? Theme.gridNull : Theme.fgDefault
                        }
                    }
                }

                MouseArea {
                    function cellAt(x, y) {
                        return tTable.cellAtPosition(root.clamp(x, 0, tTable.width - tvbar.width - 1) + tTable.contentX, root.clamp(y, 0, tTable.height - thbar.height - 1) + tTable.contentY, true);
                    }

                    anchors.fill: tTable
                    anchors.rightMargin: tvbar.width
                    anchors.bottomMargin: thbar.height
                    acceptedButtons: Qt.LeftButton
                    preventStealing: true
                    onPressed: mouse => {
                        tBody.forceActiveFocus(Qt.MouseFocusReason);
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
            }

            FocusRing {
                shown: tBody.activeFocus
                inset: true
            }
        }
    }
}
