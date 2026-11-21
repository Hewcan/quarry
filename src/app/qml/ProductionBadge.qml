pragma ComponentBehavior: Bound
import QtQuick
import Quarry.Controls

// Production marker: icon and text, never color alone. Decorative: the item that shows it already
// names "production" in its accessible name. `framed` puts it on a plain surface, for use on a
// colored background where the danger color alone would not be readable.
Rectangle {
    id: root

    property bool framed: false

    implicitWidth: row.implicitWidth + (framed ? Theme.spacing * 2 : 0)
    implicitHeight: row.implicitHeight + (framed ? Theme.spacing : 0)
    radius: Theme.radius
    color: framed ? Theme.bgSurface : Qt.alpha(Theme.bgSurface, 0)

    Accessible.ignored: true

    Row {
        id: row

        anchors.centerIn: parent
        spacing: Theme.spacing

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            text: Icons.shieldAlert
            color: Theme.danger
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("PRODUCTION")
            font.pointSize: Theme.fontSize
            font.bold: true
            color: Theme.danger
        }
    }
}
