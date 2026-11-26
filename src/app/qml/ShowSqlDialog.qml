pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Shows the complete SQL statement currently executing (or last executed) for a data tab,
// together with any bound parameter values.
QDialog {
    id: root

    property string sqlText: ""
    // [{number, text}]
    property var parameters: []
    property var copyAction: null
    readonly property alias sqlField: sqlEdit
    readonly property alias copyButton: copyBtn
    readonly property alias closeButton: closeBtn

    function openWith(sql, params) {
        sqlText = sql ?? "";
        parameters = params ?? [];
        open();
    }

    title: qsTr("Current SQL")
    width: Math.min(Math.max(implicitWidth, Theme.controlHeight * 22), parent ? parent.width - Theme.controlHeight : Theme.controlHeight * 24)

    onOpened: sqlEdit.forceActiveFocus(Qt.TabFocusReason)

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 22
            Layout.preferredHeight: Math.min(Math.max(sqlEdit.contentHeight + Theme.spacing * 4, Theme.controlHeight * 4), Theme.controlHeight * 12)
            color: Theme.bgSurface
            border.width: 1
            border.color: Theme.borderDefault
            radius: Theme.radius
            clip: true

            Flickable {
                id: sqlFlick

                anchors.fill: parent
                anchors.margins: Theme.spacing * 2
                contentWidth: width
                contentHeight: sqlEdit.contentHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                TextEdit {
                    id: sqlEdit

                    width: sqlFlick.width
                    text: root.sqlText !== "" ? root.sqlText : qsTr("No SQL statement available yet.")
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    textFormat: TextEdit.PlainText
                    font.family: Theme.monoFontFamily
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    selectedTextColor: Theme.selectionFg
                    selectionColor: Theme.selectionBg
                    activeFocusOnTab: true

                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Current SQL statement")
                    Accessible.readOnly: true
                    Accessible.focusable: true
                }
            }

            FocusRing {
                shown: sqlEdit.activeFocus
                inset: true
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: root.parameters.length > 0
            spacing: Theme.spacing

            Text {
                Layout.fillWidth: true
                text: qsTr("Parameters")
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault

                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            Repeater {
                model: root.parameters

                Text {
                    required property var modelData

                    Layout.fillWidth: true
                    text: qsTr("Parameter %1 = %2").arg(modelData.number).arg(modelData.text)
                    textFormat: Text.PlainText
                    font.family: Theme.monoFontFamily
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WrapAnywhere

                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: copyBtn

                text: qsTr("Copy SQL")
                iconGlyph: Icons.copy
                enabled: root.sqlText !== "" && root.copyAction !== null
                onClicked: {
                    if (root.copyAction)
                        root.copyAction(root.sqlText);
                }
            }

            QButton {
                id: closeBtn

                text: qsTr("Close")
                variant: "primary"
                onClicked: root.close()
            }
        }
    }
}
