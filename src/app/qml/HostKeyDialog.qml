pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// The SSH server's host key, before a connection goes through the tunnel. Two shapes:
// - unknown server: shows the key type and the SHA256 fingerprint and asks. The focus starts on
//   Cancel; "Trust once" lasts until Quarry closes, "Trust and remember" is written to Quarry's own
//   known_hosts file.
// - `mismatch`: the key is not the one on file. Nothing can be accepted from here, so there is only
//   a Close button; the connection was already refused.
QDialog {
    id: root

    property string host: ""
    property int port: 22
    property string keyType: ""
    property string fingerprint: ""
    // mismatch only: the fingerprint on file ("" when the connection attempt could not tell)
    property string knownFingerprint: ""
    property bool mismatch: false
    // Set when the user chose one of the trust buttons, so closing does not count as declining.
    property bool answered: false
    readonly property string hostLabel: port !== 22 ? "%1:%2".arg(host).arg(port) : host
    readonly property string message: mismatch ? qsTr("The host key of the SSH server %1 is not the one Quarry saved. Someone may be impersonating the server, or it was reinstalled. Quarry did not connect and cannot accept the new key from here.").arg(hostLabel) : qsTr("The authenticity of the server %1 can't be verified.").arg(hostLabel)
    readonly property alias cancelButton: cancelBtn
    readonly property alias onceButton: onceBtn
    readonly property alias rememberButton: rememberBtn
    readonly property alias closeButton: closeBtn
    readonly property alias fingerprintText: unknownFp.edit
    readonly property alias knownFingerprintText: knownFp.edit
    readonly property alias receivedFingerprintText: receivedFp.edit

    // remember: write it to known_hosts (true) or trust it only until Quarry closes (false)
    signal trusted(bool remember)
    signal declined

    function openFor(serverHost, serverPort, type, print) {
        mismatch = false;
        host = serverHost;
        port = serverPort;
        keyType = type;
        fingerprint = print;
        knownFingerprint = "";
        open();
    }

    function openMismatch(serverHost, serverPort, type, known, received) {
        mismatch = true;
        host = serverHost;
        port = serverPort;
        keyType = type;
        knownFingerprint = known;
        fingerprint = received;
        open();
    }

    function trust(remember) {
        answered = true;
        close();
        trusted(remember);
    }

    title: mismatch ? qsTr("The server's host key has changed") : qsTr("Unknown SSH server")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    onAboutToShow: answered = false
    onOpened: (mismatch ? closeBtn : cancelBtn).forceActiveFocus(Qt.TabFocusReason)
    onClosed: {
        if (!answered && !mismatch)
            declined();
    }

    // A fingerprint the user can select and copy, read out in full by a screen reader.
    component FingerprintField: ColumnLayout {
        id: fp

        property string label: ""
        property string value: ""
        readonly property alias edit: edit

        Layout.fillWidth: true
        visible: value !== ""
        spacing: Theme.spacing

        Text {
            Layout.fillWidth: true
            text: fp.label
            font.pointSize: Theme.fontSize
            font.bold: true
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
            Accessible.ignored: true
        }

        TextEdit {
            id: edit

            Layout.fillWidth: true
            activeFocusOnTab: fp.visible
            readOnly: true
            selectByMouse: true
            text: fp.value
            wrapMode: TextEdit.WrapAnywhere
            font.family: Theme.monoFontFamily
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            selectionColor: Theme.selectionBg
            selectedTextColor: Theme.selectionFg

            Accessible.role: Accessible.EditableText
            Accessible.name: fp.label
            Accessible.description: fp.value
            Accessible.readOnly: true
            Accessible.focusable: true

            FocusRing {
                shown: edit.activeFocus
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 20
            spacing: Theme.spacing

            Icon {
                Layout.alignment: Qt.AlignTop
                text: Icons.shieldAlert
                color: root.mismatch ? Theme.danger : Theme.fgDefault
            }

            Text {
                Layout.fillWidth: true
                text: root.message
                font.pointSize: Theme.fontSize
                font.bold: root.mismatch
                color: root.mismatch ? Theme.danger : Theme.fgDefault
                wrapMode: Text.WordWrap
            }
        }

        Text {
            Layout.fillWidth: true
            visible: !root.mismatch
            text: qsTr("Quarry has not connected to this server before. Compare the fingerprint with the one your administrator gave you, and continue only if they are identical.")
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: root.mismatch
            text: qsTr("Ask the administrator of the server whether its key was changed on purpose before you try again.")
            font.pointSize: Theme.fontSize
            color: Theme.fgDefault
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: root.keyType !== ""
            text: qsTr("Key type: %1").arg(root.keyType)
            font.pointSize: Theme.fontSize
            color: Theme.fgMuted
            wrapMode: Text.WordWrap
        }

        FingerprintField {
            id: unknownFp

            visible: !root.mismatch && root.fingerprint !== ""
            label: qsTr("SHA256 fingerprint")
            value: root.mismatch ? "" : root.fingerprint
        }

        FingerprintField {
            id: knownFp

            visible: root.mismatch && root.knownFingerprint !== ""
            label: qsTr("Known fingerprint")
            value: root.mismatch ? root.knownFingerprint : ""
        }

        FingerprintField {
            id: receivedFp

            visible: root.mismatch && root.fingerprint !== ""
            label: qsTr("Received fingerprint")
            value: root.mismatch ? root.fingerprint : ""
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: cancelBtn

                visible: !root.mismatch
                text: qsTr("Cancel")
                Accessible.description: root.message
                onClicked: root.close()
            }

            QButton {
                id: onceBtn

                visible: !root.mismatch
                text: qsTr("Trust once")
                onClicked: root.trust(false)
            }

            QButton {
                id: rememberBtn

                visible: !root.mismatch
                text: qsTr("Trust and remember")
                onClicked: root.trust(true)
            }

            QButton {
                id: closeBtn

                visible: root.mismatch
                text: qsTr("Close")
                Accessible.description: root.message
                onClicked: root.close()
            }
        }
    }
}
