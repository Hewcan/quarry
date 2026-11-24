import QtQuick
import QtQuick.Dialogs

// The platform file dialog (native where the platform has one, Qt's own otherwise). Loaded on
// demand so QtQuick.Dialogs stays off the startup path. `purpose` "key" picks an SSH private key
// (only its path is kept; Quarry never reads the file); `purpose` "php" picks the php executable of
// a project's migrations (only its path is kept; it runs only when a migration is run). `purpose`
// "claude" picks the Claude Code executable of the AI assistant (run only by the Test button).
// "pgtool" picks pg_dump, pg_restore or psql (only its path is kept; Dump and Restore run it).
FileDialog {
    property string purpose: "database"

    title: purpose === "key" ? qsTr("Choose the SSH private key") : purpose === "php" ? qsTr("Choose the PHP executable") : purpose === "claude" ? qsTr("Choose the Claude Code executable") : purpose === "pgtool" ? qsTr("Choose the PostgreSQL tool") : qsTr("Choose a database file")
    nameFilters: purpose === "key" || purpose === "php" || purpose === "claude" || purpose === "pgtool" ? [qsTr("All files (*)")] : [qsTr("SQLite databases (*.db *.sqlite *.sqlite3 *.db3)"), qsTr("All files (*)")]
}
