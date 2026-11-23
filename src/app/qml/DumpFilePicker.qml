import QtQuick
import QtQuick.Dialogs

// The platform file dialog of Dump (save as) and Restore (open). Loaded on demand so
// QtQuick.Dialogs stays off the startup path.
FileDialog {
    property bool restoring: false

    fileMode: restoring ? FileDialog.OpenFile : FileDialog.SaveFile
    title: restoring ? qsTr("Choose the file to restore from") : qsTr("Choose where to save the dump")
}
