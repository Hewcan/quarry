pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T

T.Menu {
    id: control

    // Sized to its items, not to a fixed width: the widest title, a fixed gap, the widest shortcut
    // and the paddings. Read from the items (not from what is on screen), so it holds for menus
    // filled by code. Capped to the window; then the titles elide, never the shortcuts.
    readonly property real naturalWidth: {
        let title = 0;
        let accessory = 0;
        let gap = 0;
        let padding = 0;
        for (let i = 0; i < control.count; ++i) {
            const item = control.itemAt(i);
            if (!item || item.titleWidth === undefined)
                continue;
            title = Math.max(title, item.titleWidth);
            accessory = Math.max(accessory, item.accessoryWidth);
            gap = Math.max(gap, item.spacing);
            padding = Math.max(padding, item.leftPadding + item.rightPadding);
        }
        return title + (accessory > 0 ? gap + accessory : 0) + padding;
    }
    readonly property real maximumWidth: control.contentItem.Window.window ? control.contentItem.Window.window.width - Theme.spacing * 2 : Number.POSITIVE_INFINITY

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, Math.min(naturalWidth + leftPadding + rightPadding, maximumWidth))
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, contentHeight + topPadding + bottomPadding)
    margins: 0
    padding: 2
    overlap: 1
    focus: true
    font.pointSize: Theme.fontSize

    onOpened: forceActiveFocus(Qt.PopupFocusReason)

    delegate: QMenuItem {}

    contentItem: ListView {
        implicitHeight: contentHeight
        model: control.contentModel
        interactive: Window.window ? contentHeight + control.topPadding + control.bottomPadding > Window.window.height : false
        clip: true
        currentIndex: control.currentIndex

        Accessible.role: Accessible.List
        Accessible.name: control.title
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight * 6
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: Theme.bgRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"
            from: 0
            to: 1
            duration: Theme.animationDuration
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"
            from: 1
            to: 0
            duration: Theme.animationDuration
        }
    }
}
