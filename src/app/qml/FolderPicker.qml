import QtQuick
import QtQuick.Dialogs

// The platform folder dialog, loaded on demand like FilePicker so QtQuick.Dialogs stays off the
// startup path. It picks the project root of a connection's migrations (only its path is kept).
FolderDialog {
    title: qsTr("Choose the project folder")
}
