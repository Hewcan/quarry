pragma ComponentBehavior: Bound
import QtQuick
import Quarry.Controls

// One SQL editor: a line-number gutter, the text, and two bands behind the text: the statement
// under the cursor (always) and what Mod+Enter / Mod+Shift+Enter would run (until the next edit
// or cursor move). The text and cursor live in EditorTabsModel (`tabs`, row `row`), which saves
// them.
//
// Keyboard: Tab indents and Shift+Tab outdents, so Ctrl+Tab (next tab), F6 and Mod+2..3 are the
// ways out of the editor (WCAG 2.1.2: no keyboard trap).
//
// Read-only mode (H12 phase B, the code of a migration): `readOnly` shows `sourceText` instead of the
// text of the tab, writes nothing back to `tabs`, finds no statements and offers no suggestions;
// the numbers, the highlighting, the selection and Copy stay. Tab then leaves the editor.
//
// Suggestions (ARCHITECTURE 6.5): the popup opens by itself after "." and two identifier
// characters (150 ms after the last key, when the setting allows it) and always with suggest()
// (Mod+Space). While it is open the editor keeps the focus and takes its keys: Up, Down, PageUp,
// PageDown, Home and End move in the list; Enter and Tab accept; Esc closes it (and only that).
FocusScope {
    id: root

    required property var tabs // EditorTabsModel
    required property int row
    readonly property bool isEditor: true
    property string definition: "SQL"
    property string tabTitle: ""
    property string connectionId: ""
    property string connectionName: ""
    property bool production: false
    property string placeholder: ""
    // Read-only mode: shows sourceText; accessibleName / accessibleDescription replace the SQL ones.
    property bool readOnly: false
    property string sourceText: ""
    property string accessibleName: ""
    property string accessibleDescription: ""
    // What the last run of this tab says (QueryState): its error is underlined below.
    property var queryState: null
    // Ranges marked by a run command: [{startLine, endLine}, ...] (1-based lines).
    property var marked: []
    // CompletionController (null: no suggestions).
    property var completion: null
    // The last thing said to the screen reader about the suggestions (tests read it).
    property string lastAnnouncement: ""

    readonly property alias edit: textEdit
    readonly property alias document: doc
    readonly property alias flick: scroller
    readonly property int line: doc.lineOf(textEdit.cursorPosition)
    readonly property int column: doc.columnOf(textEdit.cursorPosition)
    // Statement under the cursor: {index, count, startLine, endLine, ...}; index -1 = none.
    property var statement: ({
            "index": -1
        })
    // Every line has this height (no wrapping, one font): the gutter relies on it instead of
    // creating an item per line.
    readonly property real lineHeight: textEdit.contentHeight / Math.max(1, textEdit.lineCount)
    readonly property int firstVisibleLine: Math.max(0, Math.floor((scroller.contentY - textEdit.topPadding) / lineHeight))
    readonly property int lastVisibleLine: Math.min(textEdit.lineCount - 1, Math.ceil((scroller.contentY + scroller.height - textEdit.topPadding) / lineHeight))
    property bool restoring: true
    readonly property int errorOffset: queryState ? queryState.errorOffset : -1
    readonly property int errorLine: queryState ? queryState.errorLine : 0

    // ---- suggestions -------------------------------------------------------------------------

    readonly property CompletionPopup suggestionPopup: popupLoader.item as CompletionPopup
    property bool suggesting: false
    property var suggestResult: ({
            "count": 0,
            "notice": "",
            "replaceStart": 0,
            "replaceEnd": 0
        })
    property int suggestAnchor: -1
    // "auto" | "explicit": nothing to show yet because the tables are still being read; the popup
    // opens when they arrive.
    property string awaiting: ""
    // Edits seen, the edit count the last accepted suggestion left behind, and whether the last
    // edit added exactly one character (a typed key; deleting or pasting never opens the popup).
    property int editCount: 0
    property int acceptedAt: -1
    property int lastLength: 0
    property bool typedOne: false
    property int announcedCount: -1
    property string announcedNotice: ""
    readonly property rect caretRect: {
        scroller.contentX;
        scroller.contentY;
        const r = textEdit.cursorRectangle;
        const p = textEdit.mapToItem(root, r.x, r.y);
        return Qt.rect(p.x, p.y, r.width, r.height);
    }

    function say(text) {
        lastAnnouncement = text;
        textEdit.Accessible.announce(text);
    }

    // Two identifier characters, or a dot, right before the cursor.
    function wantsAutoOpen() {
        const pos = textEdit.cursorPosition;
        const before = textEdit.getText(Math.max(0, pos - 64), pos);
        if (before.endsWith("."))
            return true;
        let n = 0;
        for (let i = before.length - 1; i >= 0 && /[\w\u00C0-\uFFFF]/.test(before[i]); --i)
            ++n;
        return n >= 2;
    }

    // Asks for the suggestions at the cursor. `opening`: the popup may open (Mod+Space, or the
    // auto-open rule); otherwise it only follows what is open, and closes when the cursor
    // leaves the word it was opened on. `explicit`: the user asked, so "no suggestions" is said.
    function refreshSuggestions(opening, explicit) {
        suggestTimer.stop();
        if (!completion || restoring)
            return;
        const r = completion.query(row, textEdit.cursorPosition);
        const any = r.count > 0 || r.notice !== "";
        if (!any || (!opening && r.replaceStart !== suggestAnchor)) {
            const waiting = !any && opening && r.loading;
            closeSuggestions();
            if (waiting)
                awaiting = explicit ? "explicit" : "auto";
            else if (explicit)
                say(qsTr("No suggestions"));
            return;
        }
        awaiting = "";
        suggestResult = r;
        if (opening || !suggesting) {
            suggestAnchor = r.replaceStart;
            popupLoader.active = true;
            suggesting = true;
        }
        suggestionPopup.select(0);
        announceTimer.restart();
    }

    function closeSuggestions() {
        suggestTimer.stop();
        announceTimer.stop();
        suggesting = false;
        suggestAnchor = -1;
        awaiting = "";
        announcedCount = -1;
        announcedNotice = "";
    }

    // Mod+Space: always opens, whatever the setting.
    function suggest() {
        textEdit.forceActiveFocus(); // the popup is driven by this editor's keys
        refreshSuggestions(true, true);
    }

    function announceSuggestions() {
        if (!suggesting)
            return;
        const n = suggestionPopup.count;
        const notice = suggestResult.notice;
        let text = "";
        if (n !== announcedCount)
            text = n === 0 ? "" : n === 1 ? qsTr("1 suggestion") : qsTr("%1 suggestions").arg(n);
        if (notice !== announcedNotice)
            text = text !== "" ? text + ". " + notice : notice;
        announcedCount = n;
        announcedNotice = notice;
        if (n > 0) {
            const i = suggestionPopup.currentIndex;
            const current = completion.model.labelAt(i) + ", " + completion.model.kindTextAt(i);
            text = text !== "" ? text + ". " + current : current;
        }
        if (text !== "")
            say(text);
    }

    // Replaces the word being completed (and a closing quote already after the cursor).
    function acceptSuggestion() {
        if (suggestTimer.running)
            refreshSuggestions(false, false); // what was typed a moment ago
        if (!suggesting || suggestionPopup.count === 0)
            return false;
        const text = completion.model.textAt(suggestionPopup.currentIndex);
        const start = suggestResult.replaceStart;
        doc.replace(start, suggestResult.replaceEnd, text);
        textEdit.cursorPosition = start + text.length;
        acceptedAt = editCount;
        closeSuggestions();
        return true;
    }

    // The keys of the open popup. True when the key was used.
    function suggestionKey(event) {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier | Qt.ShiftModifier))
            return false;
        const list = suggestionPopup;
        switch (event.key) {
        case Qt.Key_Up:
            list.move(-1);
            break;
        case Qt.Key_Down:
            list.move(1);
            break;
        case Qt.Key_PageUp:
            list.movePage(false);
            break;
        case Qt.Key_PageDown:
            list.movePage(true);
            break;
        case Qt.Key_Home:
            list.select(0);
            break;
        case Qt.Key_End:
            list.select(list.count - 1);
            break;
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Tab:
            // Nothing to accept (the list changed under the key): the key does what it always does.
            return acceptSuggestion();
        case Qt.Key_Escape:
            closeSuggestions();
            break;
        default:
            return false;
        }
        if (suggesting)
            announceTimer.restart();
        return true;
    }

    function updateStatement() {
        if (readOnly)
            return;
        statement = tabs.statementAt(row, textEdit.cursorPosition);
    }

    function mark(ranges) {
        marked = ranges;
    }

    // Replaces the whole script with `text` as ONE edit of the document, so a single Undo brings the
    // old script back (the review of the AI's proposal, H13). It only writes text: running is the
    // Run button's business, with its confirmations.
    function applyText(text) {
        if (readOnly)
            return;
        const cursor = textEdit.cursorPosition;
        doc.replace(0, textEdit.length, text);
        textEdit.cursorPosition = Math.min(cursor, textEdit.length);
    }

    function ensureCursorVisible() {
        const r = textEdit.cursorRectangle;
        if (r.x < scroller.contentX)
            scroller.contentX = Math.max(0, r.x - Theme.spacing * 4);
        else if (r.x + r.width > scroller.contentX + scroller.width)
            scroller.contentX = r.x + r.width - scroller.width + Theme.spacing * 4;
        if (r.y < scroller.contentY)
            scroller.contentY = r.y;
        else if (r.y + r.height > scroller.contentY + scroller.height)
            scroller.contentY = r.y + r.height - scroller.height;
    }

    // PageUp/PageDown: TextEdit would move by its own height, which inside a Flickable is the
    // whole document.
    function movePage(down, select) {
        const r = textEdit.cursorRectangle;
        const page = Math.max(1, Math.floor(scroller.height / root.lineHeight) - 1) * root.lineHeight;
        const pos = textEdit.positionAt(r.x, r.y + r.height / 2 + (down ? page : -page));
        if (select)
            textEdit.moveCursorSelection(pos, TextEdit.SelectCharacters);
        else
            textEdit.cursorPosition = pos;
    }

    Component.onCompleted: {
        textEdit.text = readOnly ? sourceText : tabs.textOf(row);
        textEdit.cursorPosition = readOnly ? 0 : Math.min(tabs.cursorOf(row), textEdit.length);
        restoring = false;
        updateStatement();
    }

    onSourceTextChanged: {
        if (readOnly && !restoring) {
            textEdit.text = sourceText;
            textEdit.cursorPosition = 0;
        }
    }

    onVisibleChanged: {
        if (!visible)
            closeSuggestions();
    }
    onConnectionIdChanged: closeSuggestions()

    // The tables arrived, or the cache was dropped: ask again.
    Connections {
        target: root.completion

        function onChanged() {
            if (root.suggesting || root.awaiting !== "")
                suggestTimer.restart();
        }
    }

    // Debounce: the keys of a typist are one request, 150 ms after the last.
    Timer {
        id: suggestTimer

        interval: 150
        onTriggered: {
            if (root.suggesting)
                root.refreshSuggestions(false, false);
            else if (root.awaiting !== "")
                root.refreshSuggestions(true, root.awaiting === "explicit");
            else if (root.completion && root.completion.autoOpen && textEdit.activeFocus && root.typedOne && root.editCount !== root.acceptedAt && root.wantsAutoOpen())
                root.refreshSuggestions(true, false);
        }
    }

    Timer {
        id: announceTimer

        interval: 250
        onTriggered: root.announceSuggestions()
    }

    Connections {
        target: root.tabs

        function onStatementsChanged(changedRow) {
            if (changedRow === root.row)
                root.updateStatement();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bgCanvas
    }

    SqlDocument {
        id: doc

        textDocument: textEdit.textDocument
        definition: root.definition
        theme: Theme
    }

    FontMetrics {
        id: metrics

        font: textEdit.font
    }

    // Line numbers: only the lines in view get an item, so a 10 000 line file costs the same as a
    // short one.
    Item {
        id: gutter

        readonly property real digitWidth: metrics.advanceWidth("0")

        // Room for the error marker beside the numbers.
        width: Math.max(3, String(textEdit.lineCount).length) * digitWidth + Theme.spacing * 4 + marker.implicitWidth
        height: parent.height
        clip: true

        Rectangle {
            anchors.fill: parent
            color: Theme.bgSurface
        }

        Rectangle {
            anchors.right: parent.right
            width: 1
            height: parent.height
            color: Theme.borderDefault
        }

        // The line of the run error: an icon, not only a color (the line is also announced).
        Icon {
            id: marker

            x: Theme.spacing / 2
            y: textEdit.topPadding + (root.errorLine - 1) * root.lineHeight - scroller.contentY + (root.lineHeight - height) / 2
            opacity: root.errorOffset >= 0 && root.errorLine > 0 ? 1 : 0
            text: Icons.circleAlert
            color: Theme.danger
            objectName: "errorMarker"
        }

        Repeater {
            model: Math.max(0, root.lastVisibleLine - root.firstVisibleLine + 1)

            Text {
                id: number

                required property int index
                readonly property int lineNumber: root.firstVisibleLine + index + 1

                x: gutter.width - width - Theme.spacing * 2
                y: textEdit.topPadding + (lineNumber - 1) * root.lineHeight - scroller.contentY
                height: root.lineHeight
                text: lineNumber
                font.family: textEdit.font.family
                font.pointSize: textEdit.font.pointSize
                verticalAlignment: Text.AlignVCenter
                // The line of the cursor is the strongest, the others are muted (both pass 4.5:1).
                color: lineNumber === root.line ? Theme.fgDefault : Theme.fgMuted
                font.bold: lineNumber === root.line

                Accessible.ignored: true // the screen reader gets the line from the status bar
            }
        }
    }

    Flickable {
        id: scroller

        anchors.left: gutter.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        clip: true
        contentWidth: Math.max(width, textEdit.contentWidth + textEdit.leftPadding * 2)
        contentHeight: Math.max(height, textEdit.contentHeight + textEdit.topPadding * 2)
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.AutoFlickDirection

        // The statement under the cursor.
        Rectangle {
            visible: root.statement.index >= 0
            x: 0
            y: textEdit.topPadding + (root.statement.startLine - 1) * root.lineHeight
            width: scroller.contentWidth
            height: (root.statement.endLine - root.statement.startLine + 1) * root.lineHeight
            color: Theme.bgHover
        }

        // What a run command would execute: the same band with an outline, since a fill alone is
        // too faint to tell apart (and is not the only cue: the command announces it).
        Repeater {
            model: root.marked

            Rectangle {
                required property var modelData

                x: 0
                y: textEdit.topPadding + (modelData.startLine - 1) * root.lineHeight
                width: scroller.contentWidth
                height: (modelData.endLine - modelData.startLine + 1) * root.lineHeight
                color: Theme.bgHover
                border.width: Theme.focusRingWidth
                border.color: Theme.accent
            }
        }

        TextEdit {
            id: textEdit

            width: Math.max(scroller.width, contentWidth + leftPadding * 2)
            height: Math.max(scroller.height, contentHeight + topPadding * 2)
            topPadding: Theme.spacing
            leftPadding: Theme.spacing * 2
            font.family: Theme.monoFontFamily
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            selectionColor: Theme.selectionBg
            selectedTextColor: Theme.selectionFg
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            textFormat: TextEdit.PlainText
            tabStopDistance: metrics.advanceWidth("0") * 4
            focus: true
            activeFocusOnTab: true
            persistentSelection: true
            readOnly: root.readOnly

            Accessible.role: Accessible.EditableText
            Accessible.name: root.readOnly ? root.accessibleName : qsTr("SQL editor, %1").arg(root.tabTitle)
            Accessible.description: root.readOnly ? root.accessibleDescription : (root.connectionName !== "" ? qsTr("Connection: %1").arg(root.connectionName) + (root.production ? ", " + qsTr("production") : "") : qsTr("No connection. Choose a connection in the tab header.")) + ". " + root.placeholder
            Accessible.focusable: true
            Accessible.multiLine: true
            Accessible.readOnly: root.readOnly

            onTextChanged: {
                if (root.restoring || root.readOnly)
                    return;
                root.editCount++;
                root.awaiting = "";
                root.typedOne = length === root.lastLength + 1;
                root.lastLength = length;
                if (root.suggesting || (root.typedOne && root.completion && root.completion.autoOpen && activeFocus))
                    suggestTimer.restart();
                if (root.marked.length > 0)
                    root.marked = [];
                if (root.queryState)
                    root.queryState.clearError(); // its position refers to the old text
                root.tabs.setText(root.row, text);
            }
            onCursorPositionChanged: {
                if (root.restoring || root.readOnly)
                    return;
                root.awaiting = "";
                if (root.suggesting)
                    suggestTimer.restart();
                if (root.marked.length > 0)
                    root.marked = [];
                root.tabs.setCursor(root.row, cursorPosition);
                root.updateStatement();
            }
            onCursorRectangleChanged: root.ensureCursorVisible()

            onActiveFocusChanged: {
                if (!activeFocus)
                    root.closeSuggestions();
            }

            Keys.onPressed: event => {
                if (root.suggesting && root.suggestionKey(event)) {
                    event.accepted = true;
                    return;
                }
                if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                    return;
                const shift = (event.modifiers & Qt.ShiftModifier) !== 0;
                if (event.key === Qt.Key_Tab && !shift && !root.readOnly) {
                    textEdit.remove(textEdit.selectionStart, textEdit.selectionEnd);
                    textEdit.insert(textEdit.cursorPosition, "\t");
                    event.accepted = true;
                } else if (!root.readOnly && (event.key === Qt.Key_Backtab || (event.key === Qt.Key_Tab && shift))) {
                    doc.outdent(textEdit.cursorPosition);
                    event.accepted = true;
                } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                    root.movePage(event.key === Qt.Key_PageDown, shift);
                    event.accepted = true;
                }
            }

            // The error of the last run, underlined at its token.
            Rectangle {
                readonly property rect start: textEdit.positionToRectangle(Math.max(0, root.errorOffset))
                readonly property rect end: textEdit.positionToRectangle(Math.max(0, root.errorOffset) + (root.queryState?.errorLength ?? 0))

                objectName: "errorUnderline"
                visible: root.errorOffset >= 0
                x: start.x
                y: start.y + start.height - height
                width: Math.max(end.x - start.x, metrics.advanceWidth("0"))
                height: Theme.focusRingWidth
                color: Theme.danger
            }

            Text {
                x: textEdit.leftPadding
                y: textEdit.topPadding
                visible: textEdit.length === 0 && textEdit.preeditText === ""
                text: root.placeholder
                font: textEdit.font
                color: Theme.fgMuted
                Accessible.ignored: true
            }
        }
    }

    FocusRing {
        shown: textEdit.activeFocus
        inset: true
    }

    // Created the first time suggestions open, so nothing of it is on the startup path.
    Loader {
        id: popupLoader

        anchors.fill: parent
        active: false
        sourceComponent: CompletionPopup {
            suggestions: root.completion.model
            caret: root.caretRect
            notice: root.suggestResult.notice
            shown: root.suggesting
            onAccepted: root.acceptSuggestion()
        }
    }
}
