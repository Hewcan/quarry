pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Preview SQL (H2): every statement exactly as the builder made it, with its numbered parameter
// values (NULL shown as NULL). What Save changes sends is this same list. The focus starts on the
// list so the statements can be read with the arrow keys.
QDialog {
    id: root

    property string tabId: ""
    // [{number, row, sql, parameters: [{number, text}]}]
    property var statements: []
    readonly property alias list: statementList
    readonly property alias closeButton: closeBtn
    readonly property alias saveButton: saveBtn
    readonly property string summary: statements.length === 1 ? qsTr("1 statement will run in one transaction.") : qsTr("%1 statements will run in one transaction.").arg(statements.length)

    signal saveRequested(string tabId)

    function openFor(id, list) {
        tabId = id;
        statements = list;
        open();
    }

    function describe(item) {
        let text = qsTr("Statement %1, %2: %3").arg(item.number).arg(item.row).arg(item.sql);
        for (const p of item.parameters)
            text += ". " + qsTr("Parameter %1 = %2").arg(p.number).arg(p.text);
        return text;
    }

    title: qsTr("Preview SQL")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onOpened: statementList.forceActiveFocus(Qt.TabFocusReason)

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        Text {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 24
            text: root.summary
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        ListView {
            id: statementList

            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Theme.controlHeight * 14)
            clip: true
            model: root.statements
            spacing: Theme.spacing * 3
            activeFocusOnTab: true
            boundsBehavior: Flickable.StopAtBounds

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Statements to run")
            Accessible.focusable: true

            delegate: ColumnLayout {
                id: entry

                required property var modelData

                width: statementList.width
                spacing: Theme.spacing

                Accessible.role: Accessible.ListItem
                Accessible.name: root.describe(entry.modelData)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Statement %1, %2").arg(entry.modelData.number).arg(entry.modelData.row)
                    font.pointSize: Theme.fontSize
                    font.bold: true
                    color: Theme.fgDefault
                    Accessible.ignored: true
                }

                Text {
                    Layout.fillWidth: true
                    text: entry.modelData.sql
                    textFormat: Text.PlainText
                    font.family: Theme.monoFontFamily
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WrapAnywhere
                    Accessible.ignored: true
                }

                Repeater {
                    model: entry.modelData.parameters

                    Text {
                        required property var modelData

                        Layout.fillWidth: true
                        text: qsTr("Parameter %1 = %2").arg(modelData.number).arg(modelData.text)
                        textFormat: Text.PlainText
                        font.family: Theme.monoFontFamily
                        font.pointSize: Theme.fontSize
                        color: Theme.fgMuted
                        wrapMode: Text.WrapAnywhere
                        Accessible.ignored: true
                    }
                }
            }

            FocusRing {
                shown: statementList.activeFocus
                inset: true
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: closeBtn

                text: qsTr("Close")
                onClicked: root.close()
            }

            QButton {
                id: saveBtn

                text: qsTr("Save changes")
                variant: "primary"
                iconGlyph: Icons.save
                onClicked: {
                    root.close();
                    root.saveRequested(root.tabId);
                }
            }
        }
    }
}
