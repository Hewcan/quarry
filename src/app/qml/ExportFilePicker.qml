import QtQuick
import QtQuick.Dialogs

// The platform "save as" dialog of Export. Loaded on demand so QtQuick.Dialogs stays off the
// startup path.
FileDialog {
    fileMode: FileDialog.SaveFile
    title: qsTr("Choose where to save the export")
}
