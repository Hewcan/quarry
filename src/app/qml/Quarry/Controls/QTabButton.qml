pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T

// Tab. With closable: true a close button is shown (accessible name "Close <tab>"), and Delete
// on the focused tab also emits closeRequested(). The owner removes the tab.
T.TabButton {
    id: control

    property bool closable: false
    // Optional decoration before the title (a color dot) and after it (a badge). Decorative: the
    // owner puts what they say into Accessible.name.
    property alias leading: leadingRow.data
    property alias trailing: trailingRow.data
    // With fitContent the tab is as wide as its content (up to maximumWidth, the title elides
    // beyond that and the full title is in a tooltip) instead of sharing the bar's width.
    property bool fitContent: false
    property real maximumWidth: Theme.controlHeight * 12
    // A colored tab: the owner passes the fill and a color that reads on it (>= 4.5:1). The
    // selected tab is told apart by a thick bar, not by color.
    property color tintColor: Qt.alpha(Theme.bgSurface, 0)
    property color tintInk: Qt.alpha(Theme.bgSurface, 0)
    readonly property bool tinted: tintColor.a > 0
    // Width of the content on one line: decoration, title, badge, close.
    readonly property real rowWidth: {
        let w = 0;
        let n = 0;
        for (const item of [leadingRow, titleText, trailingRow, closeButton]) {
            if (item.visible) {
                w += item.implicitWidth;
                ++n;
            }
        }
        return w + Math.max(0, n - 1) * Theme.spacing;
    }

    signal closeRequested

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding, Theme.controlHeight)
    verticalPadding: 0
    leftPadding: Theme.spacing * 2
    rightPadding: closable ? Theme.spacing : Theme.spacing * 2
    font.pointSize: Theme.fontSize
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    Accessible.role: Accessible.PageTab
    Accessible.name: text
    Accessible.focusable: true
    Accessible.selectable: true
    Accessible.selected: checked
    Accessible.onPressAction: control.click()

    Binding {
        target: control
        property: "width"
        value: Math.min(control.leftPadding + control.rightPadding + control.rowWidth, control.maximumWidth)
        when: control.fitContent
    }

    Keys.onDeletePressed: event => {
        event.accepted = control.closable;
        if (control.closable)
            control.closeRequested();
    }

    // One line: decoration, title, badge, close. When the tab is too narrow for that (200% zoom,
    // long names) the badge drops to a second line instead of being pushed out of the tab.
    contentItem: GridLayout {
        readonly property bool fits: control.availableWidth >= control.rowWidth

        columns: 4
        columnSpacing: Theme.spacing
        rowSpacing: 0

        RowLayout {
            id: leadingRow

            Layout.row: 0
            Layout.column: 0
            Layout.alignment: Qt.AlignVCenter
            visible: implicitWidth > 0
            spacing: Theme.spacing
        }

        Text {
            id: titleText

            Layout.row: 0
            Layout.column: 1
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignVCenter
            text: control.text
            font: control.font
            color: control.tinted ? control.tintInk : control.checked ? Theme.fgDefault : Theme.fgMuted
            elide: Text.ElideRight
        }

        RowLayout {
            id: trailingRow

            Layout.row: parent.fits ? 0 : 1
            Layout.column: parent.fits ? 2 : 0
            Layout.columnSpan: parent.fits ? 1 : 4
            Layout.alignment: Qt.AlignVCenter
            visible: implicitWidth > 0
            spacing: Theme.spacing
        }

        IconButton {
            id: closeButton

            Layout.row: 0
            Layout.column: 3
            Layout.alignment: Qt.AlignVCenter
            visible: control.closable
            accessibleName: qsTr("Close %1").arg(control.text)
            iconGlyph: Icons.x
            tintInk: control.tinted ? control.tintInk : Qt.alpha(Theme.bgSurface, 0)
            focusPolicy: Qt.NoFocus
            onClicked: control.closeRequested()
        }
    }

    background: Rectangle {
        color: control.tinted ? control.tintColor : control.checked ? Theme.bgCanvas : control.hovered ? Theme.bgHover : Theme.bgSurface

        // Hover on a colored tab: a veil of the readable color.
        Rectangle {
            anchors.fill: parent
            visible: control.tinted && control.hovered && !control.checked
            color: Qt.alpha(control.tintInk, 0.16)
        }

        // The selected tab is marked with an accent bar, not only a fill; on a colored tab the
        // bar is thicker and in the readable color.
        Rectangle {
            visible: control.checked
            anchors.bottom: parent.bottom
            width: parent.width
            height: control.tinted ? Theme.focusRingWidth * 2 : Theme.focusRingWidth
            color: control.tinted ? control.tintInk : Theme.accent
        }
    }

    FocusRing {
        shown: control.visualFocus
        inset: true
        ringColor: control.tinted ? control.tintInk : Theme.focusRing
    }

    QToolTip {
        visible: control.hovered && titleText.truncated && !control.down
        delay: 500
        text: control.text
    }
}
