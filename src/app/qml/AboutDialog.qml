pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Quarry.Controls

// About Quarry (H10): name, version, license and the Qt notice, and a second view with the licenses
// of the third-party software. The focus starts on Close; Esc closes. The licenses text is read
// from its resource only when that view is shown.
QDialog {
    id: root

    property bool showLicenses: false
    readonly property alias closeButton: closeBtn
    readonly property alias licensesButton: licensesBtn
    readonly property alias backButton: backBtn
    readonly property alias copyButton: copyBtn
    readonly property alias versionText: versionLabel
    readonly property alias licensesLoader: licensesLoader
    readonly property string description: qsTr("A desktop database manager.")

    function showInfo() {
        showLicenses = false;
        licensesBtn.forceActiveFocus(Qt.TabFocusReason);
    }

    function showLicenseText() {
        showLicenses = true;
        // The loader is active now; the text view takes the focus so the keys scroll it at once.
        if (licensesLoader.item)
            licensesLoader.item.edit.forceActiveFocus(Qt.TabFocusReason);
    }

    title: qsTr("About Quarry")
    width: Math.min(implicitWidth, parent.width - Theme.controlHeight)
    height: Math.min(implicitHeight, parent.height - Theme.controlHeight)

    onAboutToShow: showLicenses = false
    onOpened: closeBtn.forceActiveFocus(Qt.TabFocusReason)

    contentItem: ColumnLayout {
        spacing: Theme.spacing * 3

        // The view with the name and the license.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: Theme.controlHeight * 20
            visible: !root.showLicenses
            spacing: Theme.spacing * 3

            Image {
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: Theme.controlHeight * 3
                Layout.preferredHeight: Theme.controlHeight * 3
                source: "../assets/app/quarry-256.png"
                sourceSize.width: Theme.controlHeight * 3
                sourceSize.height: Theme.controlHeight * 3
                asynchronous: true
                Accessible.ignored: true // decorative: the name is in the heading
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing * 2

                Heading {
                    Layout.fillWidth: true
                    level: 1
                    text: qsTr("Quarry")
                }

                Text {
                    id: versionLabel

                    Layout.fillWidth: true
                    text: qsTr("Version %1").arg(Qt.application.version)
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    text: root.description
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Copyright 2026 The Quarry authors")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgMuted
                    wrapMode: Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Quarry is free software under the Apache License 2.0.")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Quarry uses Qt, licensed under the GNU LGPL version 3 and linked dynamically.")
                    font.pointSize: Theme.fontSize
                    color: Theme.fgDefault
                    wrapMode: Text.WordWrap
                }
            }
        }

        // The view with the licenses: created on first use, so nothing is read before.
        Loader {
            id: licensesLoader

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.controlHeight * 28
            Layout.preferredHeight: Theme.controlHeight * 16
            Layout.minimumHeight: Theme.controlHeight * 4
            active: root.showLicenses
            visible: root.showLicenses

            sourceComponent: Item {
                id: holder

                readonly property alias edit: textEdit
                readonly property alias flick: flick

                Rectangle {
                    anchors.fill: parent
                    color: Theme.bgCanvas
                    border.width: 1
                    border.color: Theme.borderStrong
                }

                Flickable {
                    id: flick

                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    contentWidth: width
                    contentHeight: Math.max(height, textEdit.contentHeight + textEdit.topPadding * 2)
                    boundsBehavior: Flickable.StopAtBounds

                    T.ScrollBar.vertical: QScrollBar {}

                    TextEdit {
                        id: textEdit

                        // A read-only TextEdit has no caret to move, so the keys scroll the text.
                        function scrollBy(dy) {
                            flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + dy));
                        }

                        width: flick.width
                        height: Math.max(flick.height, contentHeight + topPadding * 2)
                        topPadding: Theme.spacing
                        leftPadding: Theme.spacing * 2
                        rightPadding: Theme.spacing * 2
                        text: AboutInfo.licenseText()
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        wrapMode: TextEdit.Wrap
                        textFormat: TextEdit.PlainText
                        font.family: Theme.monoFontFamily
                        font.pointSize: Theme.fontSize
                        color: Theme.fgDefault
                        selectionColor: Theme.selectionBg
                        selectedTextColor: Theme.selectionFg
                        activeFocusOnTab: true

                        Keys.onPressed: event => {
                            const line = Math.max(1, cursorRectangle.height);
                            event.accepted = true;
                            if (event.matches(StandardKey.SelectAll)) {
                                selectAll();
                            } else if (event.matches(StandardKey.Copy)) {
                                copy();
                            } else if (event.key === Qt.Key_Down) {
                                scrollBy(line);
                            } else if (event.key === Qt.Key_Up) {
                                scrollBy(-line);
                            } else if (event.key === Qt.Key_PageDown) {
                                scrollBy(flick.height - line);
                            } else if (event.key === Qt.Key_PageUp) {
                                scrollBy(line - flick.height);
                            } else if (event.key === Qt.Key_Home) {
                                scrollBy(-flick.contentHeight);
                            } else if (event.key === Qt.Key_End) {
                                scrollBy(flick.contentHeight);
                            } else {
                                // Esc, Tab and the rest keep their meaning (Esc closes the dialog).
                                event.accepted = false;
                            }
                        }

                        Accessible.role: Accessible.EditableText
                        Accessible.name: qsTr("Third-party licenses")
                        Accessible.readOnly: true
                        Accessible.multiLine: true
                        Accessible.focusable: true
                    }
                }

                FocusRing {
                    shown: textEdit.activeFocus
                    inset: true
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.spacing * 2

            QButton {
                id: backBtn

                visible: root.showLicenses
                text: qsTr("Back")
                onClicked: root.showInfo()
            }

            QButton {
                id: copyBtn

                visible: root.showLicenses
                text: qsTr("Copy text")
                iconGlyph: Icons.copy
                onClicked: {
                    const edit = licensesLoader.item?.edit;
                    if (!edit)
                        return;
                    edit.selectAll();
                    edit.copy();
                    edit.deselect();
                    copyBtn.Accessible.announce(qsTr("Licenses copied"));
                }
            }

            QButton {
                id: licensesBtn

                visible: !root.showLicenses
                text: qsTr("Third-party licenses")
                onClicked: root.showLicenseText()
            }

            QButton {
                id: closeBtn

                text: qsTr("Close")
                onClicked: root.close()
            }
        }
    }
}
