pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Asks before Explain analyze: the engine runs the whole query to measure it. The connection and
// its PRODUCTION mark are shown, and the focus starts on Cancel. Esc or Cancel runs nothing.
QDialog {
    id: root

    property string tabId: ""
    property string connectionName: ""
    property bool production: false
    property string sql: ""
    // Set when the user chose to run, so closing does not count as declining.
    property bool answered: false
    readonly property alias cancelButton: cancelBtn
    readonly property alias runButton: runBtn
    readonly property string summary: qsTr("Explain analyze runs this query to the end on “%1” to measure it. It may take as long as the query itself.").arg(connectionName) + (production ? " " + qsTr("This is a production connection.") : "")

    signal confirmed(string tabId)
    signal declined(string tabId)

    function openFor(request) {
        tabId = request.tabId;
        connectionName = request.connectionName;
        production = request.production;
        sql = request.sql;
        open();
    }

    title: qsTr("Run the query to analyze it?")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onAboutToShow: answered = false
    onOpened: cancelBtn.forceActiveFocus(Qt.TabFocusReason)
    onClosed: {
        if (!answered)
            declined(tabId);
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 18
            text: root.summary
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing * 2

            Text {
                text: qsTr("Connection: %1").arg(root.connectionName)
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
            }

            ProductionBadge {
                visible: root.production
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.sql
            font.family: Theme.monoFontFamily
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WrapAnywhere

            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Query to analyze")
            Accessible.description: text
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: cancelBtn

                text: qsTr("Cancel")
                Accessible.description: root.summary
                onClicked: root.close()
            }

            QButton {
                id: runBtn

                text: qsTr("Run and analyze")
                variant: root.production ? "danger" : "primary"
                onClicked: {
                    root.answered = true;
                    root.confirmed(root.tabId);
                    root.close();
                }
            }
        }
    }
}
