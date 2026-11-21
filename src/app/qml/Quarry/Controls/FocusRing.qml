pragma ComponentBehavior: Bound
import QtQuick

// 2 px keyboard-focus ring, drawn outside its parent unless `inset` is set (use inset when an
// ancestor clips). Bind `shown` to the control's `visualFocus`, never to `activeFocus`.
Rectangle {
    objectName: "focusRing"
    property bool shown: false
    property bool inset: false
    // Another color for a ring on a colored surface, where the focus token may not stand out.
    property color ringColor: Theme.focusRing

    anchors.fill: parent
    anchors.margins: inset ? 0 : -Theme.focusRingWidth
    visible: shown
    z: 10
    radius: Theme.radius + (inset ? 0 : Theme.focusRingWidth)
    color: Qt.alpha(Theme.bgSurface, 0)
    border.width: Theme.focusRingWidth
    border.color: ringColor

    Accessible.ignored: true
}
