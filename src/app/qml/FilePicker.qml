import QtQuick
import QtQuick.Dialogs

// The platform file dialog (native where the platform has one, Qt's own otherwise). Loaded on
// demand so QtQuick.Dialogs stays off the startup path. `purpose` "key" picks an SSH private key
// (only its path is kept; Quarry never reads the file).
FileDialog {
    property string purpose: "database"

    title: purpose === "key" ? qsTr("Choose the SSH private key") : qsTr("Choose a database file")
    nameFilters: purpose === "key" ? [qsTr("All files (*)")] : [qsTr("SQLite databases (*.db *.sqlite *.sqlite3 *.db3)"), qsTr("All files (*)")]
}
