import QtQuick
import QtQuick.Templates as T

T.MenuSeparator {
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    verticalPadding: Theme.spacing

    contentItem: Rectangle {
        implicitHeight: 1
        color: Theme.borderDefault
    }
}
