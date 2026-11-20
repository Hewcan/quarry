pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Read-only view of a connection's project migrations (H12): pending, applied (with batch) and
// registered without a file. The state is always written as text next to the name, never shown
// only with a color. Up/Down move through the list; F5 reads again; Esc stops or closes.
QDialog {
    id: root

    required property var migrations // MigrationController
    property string connectionName: ""
    readonly property alias list: list
    readonly property alias refreshButton: refreshBtn

    function openFor(connectionId, name) {
        connectionName = name;
        title = qsTr("Migrations of %1").arg(name);
        migrations.show(connectionId);
        open();
    }

    width: Math.min(Theme.controlHeight * 24, parent.width - Theme.controlHeight)

    onOpened: list.forceActiveFocus(Qt.TabFocusReason)

    Connections {
        target: root.migrations

        function onAnnouncement(text) {
            list.Accessible.announce(text, Accessible.Polite);
        }
    }

    Shortcut {
        sequences: [StandardKey.Refresh]
        enabled: root.opened
        onActivated: root.migrations.refresh()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 2

        Text {
            Layout.fillWidth: true
            visible: root.migrations.state === "ready"
            text: root.migrations.summary
            font.pointSize: Theme.fontSize
            font.bold: true
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: root.migrations.message
            font.pointSize: Theme.fontSize
            color: root.migrations.state === "failed" ? Theme.danger : Theme.fgMuted
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: root.migrations.state === "failed" ? qsTr("Error: %1").arg(text) : text
        }

        Text {
            Layout.fillWidth: true
            visible: root.migrations.state === "failed" && text !== ""
            text: root.migrations.detail
            font.family: Theme.monoFontFamily
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WrapAnywhere
            maximumLineCount: 4
            elide: Text.ElideRight
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Details: %1").arg(text)
        }

        ListView {
            id: list

            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight * 12
            clip: true
            model: root.migrations.model
            activeFocusOnTab: true
            keyNavigationEnabled: true
            reuseItems: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Migrations")
            Accessible.description: root.migrations.summary
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Escape && root.migrations.state === "loading") {
                    root.migrations.cancel();
                    event.accepted = true;
                }
            }

            delegate: Rectangle {
                id: row

                required property int index
                required property string name
                required property string state
                required property string stateText

                readonly property bool current: ListView.isCurrentItem

                width: ListView.view.width
                height: Theme.controlHeight
                color: current && list.activeFocus ? Theme.selectionBg : "transparent"
                border.width: current && list.activeFocus ? Theme.focusRingWidth : 0
                border.color: Theme.focusRing
                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, %2").arg(row.name, row.stateText)
                Accessible.selected: current

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing
                    anchors.rightMargin: Theme.spacing
                    spacing: Theme.spacing

                    Text {
                        Layout.fillWidth: true
                        text: row.name
                        font.family: Theme.monoFontFamily
                        font.pointSize: Theme.fontSize
                        color: row.current && list.activeFocus ? Theme.selectionFg : Theme.fgDefault
                        elide: Text.ElideMiddle
                    }

                    // Text first; the color only reinforces it.
                    Text {
                        text: row.stateText
                        font.pointSize: Theme.fontSize
                        font.bold: row.state !== "applied"
                        color: row.current && list.activeFocus ? Theme.selectionFg : row.state === "orphan" ? Theme.danger : row.state === "pending" ? Theme.fgDefault : Theme.fgMuted
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        list.currentIndex = row.index;
                        list.forceActiveFocus(Qt.MouseFocusReason);
                    }
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                visible: root.migrations.state === "loading"
                text: qsTr("Stop")
                onClicked: root.migrations.cancel()
            }

            QButton {
                id: refreshBtn

                text: qsTr("Refresh")
                enabled: root.migrations.state !== "loading" && root.migrations.state !== "unconfigured"
                onClicked: root.migrations.refresh()
            }

            QButton {
                text: qsTr("Close")
                variant: "primary"
                onClicked: root.close()
            }
        }
    }
}
