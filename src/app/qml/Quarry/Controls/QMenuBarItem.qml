pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T

// One title of a QMenuBar. It is not a Tab stop (a menu bar is entered with F10 and walked with
// the arrow keys, like the platform ones); the open or hovered state is drawn with a border too,
// so it does not rely on the fill color.
T.MenuBarItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding, Theme.controlHeight)
    verticalPadding: Theme.spacing
    horizontalPadding: Theme.spacing * 2
    font.pointSize: Theme.fontSize
    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    opacity: enabled ? 1 : 0.6

    Accessible.role: Accessible.MenuItem
    Accessible.name: text
    Accessible.focusable: true
    Accessible.onPressAction: control.clicked()

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? Theme.fgDefault : Theme.fgDisabled
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: control.enabled && (control.highlighted || control.down || control.hovered) ? Theme.bgHover : Qt.alpha(Theme.bgSurface, 0)
        border.width: control.highlighted && control.enabled ? 2 : 0
        border.color: Theme.focusRing
    }
}
