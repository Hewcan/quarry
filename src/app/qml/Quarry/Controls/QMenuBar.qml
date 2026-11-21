pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T

// The strip of menu titles. Clicking a title opens its QMenu; with one open, hovering or
// Left/Right moves to the next (the template does this).
// shortcut: no native/global menu bar on macOS (it would need Qt.labs.platform); the bar stays in
// the window on every OS. Revisit if a macOS user asks for the system menu.
T.MenuBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, contentWidth + leftPadding + rightPadding)
    implicitHeight: Theme.controlHeight
    font.pointSize: Theme.fontSize

    Accessible.role: Accessible.MenuBar
    Accessible.name: qsTr("Menu bar")

    delegate: QMenuBarItem {}

    contentItem: Row {
        spacing: control.spacing

        Repeater {
            model: control.contentModel
        }
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        color: Theme.bgSurface
    }
}
