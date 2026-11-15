pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import Quarry.Controls

ApplicationWindow {
    id: root

    readonly property alias shell: shell

    width: 1100
    height: 720
    minimumWidth: 640
    minimumHeight: 400
    visible: true
    title: qsTr("Quarry")
    color: Theme.bgCanvas
    font.pointSize: Theme.fontSize

    // Unsaved changes of a data tab are asked about first.
    onClosing: close => {
        close.accepted = root.shell.canQuit();
    }

    MainWindow {
        id: shell

        anchors.fill: parent
    }
}
