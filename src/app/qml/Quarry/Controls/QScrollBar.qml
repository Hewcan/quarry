pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T

// A scroll bar that is always there, so a grid of a million rows says where it is. Its thumb is
// hidden when everything fits. Not a tab stop: the view it belongs to is scrolled with the keyboard.
T.ScrollBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    padding: 1
    minimumSize: 0.05
    policy: T.ScrollBar.AlwaysOn

    Accessible.ignored: true

    contentItem: Rectangle {
        implicitWidth: Theme.spacing * 2
        implicitHeight: Theme.spacing * 2
        radius: Theme.radius
        visible: control.size < 1
        color: control.pressed ? Theme.fgMuted : Theme.borderStrong
    }

    background: Rectangle {
        color: Theme.bgSurface
    }
}
