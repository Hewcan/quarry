pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Quarry.Controls

// Updates: the answer of "Check for updates" (Help menu) and the alert shown at startup when a
// newer release exists. Nothing is downloaded or installed until the user presses "Update now";
// "Later" just closes it (the next start asks again). The state lives in the UpdateChecker.
QDialog {
    id: root

    readonly property int state: UpdateChecker.state
    readonly property alias primaryButton: primaryBtn
    readonly property alias laterButton: laterBtn
    readonly property alias notesButton: notesBtn
    readonly property alias messageText: message
    readonly property string headline: {
        switch (state) {
        case UpdateChecker.Checking:
            return qsTr("Checking for updates…");
        case UpdateChecker.UpToDate:
            return qsTr("Quarry is up to date");
        case UpdateChecker.Available:
            return qsTr("Quarry %1 is available").arg(UpdateChecker.latestVersion);
        case UpdateChecker.Downloading:
            return qsTr("Downloading Quarry %1…").arg(UpdateChecker.latestVersion);
        case UpdateChecker.Ready:
            return qsTr("Quarry %1 is ready to install").arg(UpdateChecker.latestVersion);
        case UpdateChecker.Failed:
            return qsTr("The update did not work");
        default:
            return qsTr("Updates");
        }
    }
    readonly property string detail: {
        switch (state) {
        case UpdateChecker.Checking:
            return qsTr("Looking for a newer version.");
        case UpdateChecker.UpToDate:
            return qsTr("You have the latest version (%1).").arg(UpdateChecker.currentVersion);
        case UpdateChecker.Available:
            return UpdateChecker.canInstall ? qsTr("You have version %1. Update now downloads it and then asks to restart Quarry.").arg(UpdateChecker.currentVersion) : qsTr("You have version %1. There is no package for this system yet; the release page has the details.").arg(UpdateChecker.currentVersion);
        case UpdateChecker.Downloading:
            return qsTr("%1% downloaded.").arg(Math.round(UpdateChecker.progress * 100));
        case UpdateChecker.Ready:
            return qsTr("Quarry will close, run the installer and open again. Save your work first: unsaved changes in data tabs are not kept.");
        case UpdateChecker.Failed:
            return UpdateChecker.errorText;
        default:
            return "";
        }
    }

    title: qsTr("Updates")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)

    // The state changes are asynchronous: they are announced.
    onHeadlineChanged: if (visible)
        message.Accessible.announce(headline)
    onOpened: focusAction()

    function focusAction() {
        (primaryBtn.visible ? primaryBtn : laterBtn).forceActiveFocus(Qt.TabFocusReason);
    }

    onStateChanged: Qt.callLater(() => {
        if (visible)
            focusAction();
    })

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 18
            spacing: Theme.spacing * 2

            Text {
                id: message

                Layout.fillWidth: true
                text: root.headline
                font.pointSize: Theme.fontSize
                font.bold: true
                color: Theme.fgDefault
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.StaticText
                Accessible.name: root.headline + ". " + root.detail
            }

            Text {
                Layout.fillWidth: true
                text: root.detail
                visible: text !== ""
                font.pointSize: Theme.fontSize
                color: Theme.fgDefault
                wrapMode: Text.WordWrap
                Accessible.ignored: true // read together with the headline
            }

            // The percentage is also in words above: the bar is not the only carrier.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.spacing * 2
                visible: root.state === UpdateChecker.Downloading
                color: Theme.bgCanvas
                border.width: 1
                border.color: Theme.borderStrong
                Accessible.ignored: true

                Rectangle {
                    width: parent.width * UpdateChecker.progress
                    height: parent.height
                    color: Theme.accent
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: notesBtn

                visible: (root.state === UpdateChecker.Available || root.state === UpdateChecker.Ready) && UpdateChecker.releaseUrl.toString() !== ""
                text: qsTr("Release notes")
                onClicked: Qt.openUrlExternally(UpdateChecker.releaseUrl)
            }

            QButton {
                id: laterBtn

                text: {
                    switch (root.state) {
                    case UpdateChecker.Available:
                    case UpdateChecker.Ready:
                        return qsTr("Later");
                    case UpdateChecker.Downloading:
                        return qsTr("Cancel");
                    default:
                        return qsTr("Close");
                    }
                }
                onClicked: {
                    if (root.state === UpdateChecker.Downloading)
                        UpdateChecker.cancelDownload();
                    else
                        root.close();
                }
            }

            QButton {
                id: primaryBtn

                variant: "primary"
                visible: root.state === UpdateChecker.Available || root.state === UpdateChecker.Ready || root.state === UpdateChecker.Failed
                text: {
                    switch (root.state) {
                    case UpdateChecker.Available:
                        return UpdateChecker.canInstall ? qsTr("Update now") : qsTr("Open release page");
                    case UpdateChecker.Ready:
                        return qsTr("Install and restart");
                    default:
                        return qsTr("Try again");
                    }
                }
                onClicked: {
                    switch (root.state) {
                    case UpdateChecker.Available:
                        if (UpdateChecker.canInstall)
                            UpdateChecker.download();
                        else
                            Qt.openUrlExternally(UpdateChecker.releaseUrl);
                        break;
                    case UpdateChecker.Ready:
                        UpdateChecker.installAndQuit();
                        break;
                    default:
                        UpdateChecker.check(false);
                    }
                }
            }
        }
    }
}
