pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The AI assistant settings of one connection (H13): off by default, with what is never sent and
// what may be sent. The values live here until save() hands them to AiController (they are stored
// in quarry.db, never inside the connection). Turning it on for a production connection needs the
// name of the connection typed first; AiController refuses it otherwise.
ColumnLayout {
    id: form

    property var ai: null // AiController
    property string connectionName: ""
    property bool production: false

    property bool enabled: false
    property string excludedSchemas: ""
    property string excludedTables: ""
    property bool sendComments: false
    property bool sendDefaults: false
    property bool sendChecks: false
    property bool sendEnumValues: false
    property bool sendDefinitions: false
    property bool maskLiterals: true
    property bool maskNumbers: false
    property string maxTables: "40"
    property string confirmation: ""
    // The AI was already on when the dialog opened (no new confirmation to keep it on).
    property bool wasEnabled: false
    // Something was changed since load(): only then is anything stored.
    property bool dirty: false

    readonly property bool needsConfirmation: enabled && production && !wasEnabled
    readonly property bool confirmed: connectionName.trim() !== "" && confirmation.trim() === connectionName.trim()
    readonly property int maxTablesValue: /^\d+$/.test(maxTables.trim()) ? parseInt(maxTables.trim()) : -1
    readonly property string maxTablesProblem: enabled && (maxTablesValue < 1 || maxTablesValue > 200) ? qsTr("Enter a number from 1 to 200.") : ""
    // Why Save has to wait, or "".
    readonly property string blockingReason: {
        if (ai === null || !enabled)
            return "";
        if (maxTablesProblem !== "")
            return qsTr("Fix the maximum number of tables in the AI assistant settings to save.");
        if (needsConfirmation && !confirmed)
            return connectionName.trim() === "" ? qsTr("Enter a name, then type it to turn on the AI assistant for this production connection.") : qsTr("Type the connection name to turn on the AI assistant for this production connection.");
        return "";
    }
    readonly property bool blocked: blockingReason !== ""
    readonly property alias enableBox: enableBox
    readonly property alias schemasField: schemasInput
    readonly property alias tablesField: tablesInput
    readonly property alias commentsBox: commentsBox
    readonly property alias defaultsBox: defaultsBox
    readonly property alias checksBox: checksBox
    readonly property alias enumBox: enumBox
    readonly property alias definitionsBox: definitionsBox
    readonly property alias literalsBox: literalsBox
    readonly property alias numbersBox: numbersBox
    readonly property alias maxField: maxInput
    readonly property alias confirmField: confirmInput

    // The values of AiController.settingsOf() (the defaults for {}). A new connection passes {}.
    function load(settings) {
        enabled = settings.enabled ?? false;
        wasEnabled = enabled;
        excludedSchemas = (settings.excludedSchemas ?? []).join(", ");
        excludedTables = (settings.excludedTables ?? []).join(", ");
        sendComments = settings.sendComments ?? false;
        sendDefaults = settings.sendDefaults ?? false;
        sendChecks = settings.sendChecks ?? false;
        sendEnumValues = settings.sendEnumValues ?? false;
        sendDefinitions = settings.sendDefinitions ?? false;
        maskLiterals = settings.maskLiterals ?? true;
        maskNumbers = settings.maskNumbers ?? false;
        maxTables = String(settings.maxTables ?? 40);
        confirmation = "";
        dirty = false;
    }

    function settings() {
        return {
            "enabled": enabled,
            "excludedSchemas": excludedSchemas,
            "excludedTables": excludedTables,
            "sendComments": sendComments,
            "sendDefaults": sendDefaults,
            "sendChecks": sendChecks,
            "sendEnumValues": sendEnumValues,
            "sendDefinitions": sendDefinitions,
            "maskLiterals": maskLiterals,
            "maskNumbers": maskNumbers,
            "maxTables": maxTablesValue < 1 ? 40 : Math.min(maxTablesValue, 200),
            "confirmedProduction": needsConfirmation && confirmed
        };
    }

    // Stores the settings of a saved connection; nothing happens when nothing was touched.
    function save(connectionId) {
        if (ai === null || !dirty)
            return;
        ai.saveSettings(connectionId, settings());
        dirty = false;
    }

    // One option: a check box and, under it, why it matters. The help is also its description.
    component OptionBox: ColumnLayout {
        id: option

        property alias text: box.text
        property alias checked: box.checked
        property string help: ""
        readonly property alias check: box

        signal toggled

        Layout.fillWidth: true
        spacing: 0

        QCheckBox {
            id: box

            Layout.fillWidth: true
            onToggled: option.toggled()

            Accessible.description: option.help
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Math.round(Theme.controlHeight * 0.7) + Theme.spacing * 3
            text: option.help
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WordWrap
        }
    }

    spacing: Theme.spacing

    // The settings arrive the first time they are asked for: fill the form unless it was edited.
    Connections {
        target: form.ai
        enabled: form.ai !== null

        function onSettingsChanged(connectionId) {
            if (connectionId === "" && !form.dirty && form.reloadId !== "")
                form.load(form.ai.settingsOf(form.reloadId));
        }
    }

    // The saved connection whose settings are shown ("" = a new one).
    property string reloadId: ""

    OptionBox {
        id: enableBox

        text: qsTr("Enable the AI assistant for this connection")
        help: qsTr("Off by default. When on, your Claude Code receives the structure of this database (names, types, keys and indexes) with your request and script, never the rows.")
        checked: form.enabled
        onToggled: {
            form.enabled = checked;
            form.dirty = true;
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: form.enabled
        spacing: Theme.spacing * 2

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Text {
                id: schemasLabel

                Layout.fillWidth: true
                text: qsTr("Excluded schemas")
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
            }

            QTextField {
                id: schemasInput

                Layout.fillWidth: true
                text: form.excludedSchemas
                accessibleName: schemasLabel.text
                placeholderText: qsTr("For example: audit, internal")
                onTextEdited: {
                    form.excludedSchemas = text;
                    form.dirty = true;
                }

                Accessible.description: qsTr("Separated by commas or line breaks. Never sent, not even as the target of a foreign key.")
            }

            Text {
                id: tablesLabel

                Layout.fillWidth: true
                text: qsTr("Excluded tables")
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
            }

            QTextField {
                id: tablesInput

                Layout.fillWidth: true
                text: form.excludedTables
                accessibleName: tablesLabel.text
                placeholderText: qsTr("For example: users, billing.cards")
                onTextEdited: {
                    form.excludedTables = text;
                    form.dirty = true;
                }

                Accessible.description: qsTr("Separated by commas or line breaks. Never sent, not even as the target of a foreign key.")
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Separated by commas or line breaks. Use table or schema.table. Never sent, not even as the target of a foreign key.")
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Text {
                Layout.fillWidth: true
                text: qsTr("Structure details (off by default: they can contain sensitive data)")
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                wrapMode: Text.WordWrap
            }

            OptionBox {
                id: commentsBox

                text: qsTr("Send column and table comments")
                help: qsTr("People write comments, and they may include examples or notes about real data.")
                checked: form.sendComments
                onToggled: {
                    form.sendComments = checked;
                    form.dirty = true;
                }
            }

            OptionBox {
                id: defaultsBox

                text: qsTr("Send default values")
                help: qsTr("Defaults can hold real values such as addresses, codes or keys.")
                checked: form.sendDefaults
                onToggled: {
                    form.sendDefaults = checked;
                    form.dirty = true;
                }
            }

            OptionBox {
                id: checksBox

                text: qsTr("Send CHECK constraints")
                help: qsTr("A constraint can list the real values a column accepts.")
                checked: form.sendChecks
                onToggled: {
                    form.sendChecks = checked;
                    form.dirty = true;
                }
            }

            OptionBox {
                id: enumBox

                text: qsTr("Send the values of enum columns")
                help: qsTr("The allowed values can be names, codes or other business data.")
                checked: form.sendEnumValues
                onToggled: {
                    form.sendEnumValues = checked;
                    form.dirty = true;
                }
            }

            OptionBox {
                id: definitionsBox

                text: qsTr("Send view and routine definitions")
                help: qsTr("The code can contain literals and internal business rules.")
                checked: form.sendDefinitions
                onToggled: {
                    form.sendDefinitions = checked;
                    form.dirty = true;
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            OptionBox {
                id: literalsBox

                text: qsTr("Mask text literals of the script before sending")
                help: qsTr("Text between quotes in your script is replaced by a placeholder and put back in the answer.")
                checked: form.maskLiterals
                onToggled: {
                    form.maskLiterals = checked;
                    form.dirty = true;
                }
            }

            OptionBox {
                id: numbersBox

                text: qsTr("Also mask numbers")
                help: qsTr("Numbers in your script are replaced too.")
                checked: form.maskNumbers
                onToggled: {
                    form.maskNumbers = checked;
                    form.dirty = true;
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Text {
                id: maxLabel

                Layout.fillWidth: true
                text: qsTr("Maximum tables sent")
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
            }

            QTextField {
                id: maxInput

                Layout.fillWidth: true
                text: form.maxTables
                accessibleName: maxLabel.text
                inputMethodHints: Qt.ImhDigitsOnly
                onTextEdited: {
                    form.maxTables = text;
                    form.dirty = true;
                }

                Accessible.description: form.maxTablesProblem !== "" ? form.maxTablesProblem : qsTr("From 1 to 200. The tables of your script and the ones they point to come first.")
            }

            RowLayout {
                Layout.fillWidth: true
                visible: form.maxTablesProblem !== ""
                spacing: Theme.spacing

                Icon {
                    Layout.alignment: Qt.AlignTop
                    text: Icons.circleAlert
                    color: Theme.danger
                }

                Text {
                    Layout.fillWidth: true
                    text: form.maxTablesProblem
                    font.pointSize: Theme.fontSize
                    color: Theme.danger
                    wrapMode: Text.WordWrap
                }
            }

            Text {
                Layout.fillWidth: true
                visible: form.maxTablesProblem === ""
                text: qsTr("From 1 to 200. The tables of your script and the ones they point to come first.")
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }
        }

        // Production: the AI is turned on only after typing the name of the connection.
        ColumnLayout {
            id: confirmSection

            Layout.fillWidth: true
            visible: form.needsConfirmation
            spacing: Theme.spacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing

                Icon {
                    Layout.alignment: Qt.AlignTop
                    text: Icons.triangleAlert
                    color: Theme.warning
                }

                Text {
                    id: confirmLabel

                    Layout.fillWidth: true
                    text: form.connectionName.trim() === "" ? qsTr("This is a production connection. Give it a name, then type the name here to turn the AI assistant on.") : qsTr("This is a production connection. Type its name (%1) to turn the AI assistant on.").arg(form.connectionName.trim())
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap

                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Warning: %1").arg(text)
                }
            }

            QTextField {
                id: confirmInput

                Layout.fillWidth: true
                text: form.confirmation
                accessibleName: qsTr("Connection name to confirm")
                onTextEdited: form.confirmation = text

                Accessible.description: form.confirmed ? qsTr("The name matches.") : confirmLabel.text
            }

            Text {
                Layout.fillWidth: true
                text: form.confirmed ? qsTr("The name matches.") : qsTr("Save stays disabled until the name matches.")
                font.pointSize: Theme.fontSize
                color: Theme.fgMuted
                wrapMode: Text.WordWrap
            }
        }
    }
}
