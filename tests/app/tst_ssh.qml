import QtQuick
import QtTest
import Quarry.Controls

// SSH tunnel (H8) in the UI: the connection dialog's tunnel section, the fingerprint dialog and
// the secret prompt, driven by keyboard. No server and no keychain: the preflight's signals are
// emitted by hand (a signal can be called from JS), and the dialogs' answers reach a preflight with
// nothing waiting, which ignores them. Nothing here clicks at coordinates.
TestCase {
    id: testCase

    property var shell: null

    name: "Ssh"
    when: windowShown
    width: 900
    height: 800
    visible: true

    A11y {
        id: walker
    }

    Component {
        id: spyComp

        SignalSpy {}
    }

    // ---- helpers -----------------------------------------------------------------------------

    function overlay() {
        return shell.overlayItem;
    }

    function focused() {
        return testCase.Window.activeFocusItem;
    }

    function find(root, pred) {
        return walker.findAll(root, i => i.visible && pred(i))[0];
    }

    function field(name) {
        return find(overlay(), i => i.accessibleName === name && i.Accessible.role === Accessible.EditableText);
    }

    function text(str) {
        return find(overlay(), i => i.text === str);
    }

    function typeText(str) {
        for (const ch of str)
            keyClick(ch === " " ? Qt.Key_Space : ch);
    }

    function pre() {
        return shell.connections.preflight;
    }

    function openPostgres() {
        CommandRegistry.trigger("connection.new");
        tryVerify(() => shell.connectionDialog?.opened);
        const dlg = shell.connectionDialog;
        tryVerify(() => dlg.nameField.activeFocus);
        // What picking the engine in the combo does
        dlg.providerId = "postgres";
        verify(shell.connections.selectEngine("postgres"), shell.connections.errorMessage);
        return dlg;
    }

    function closeOverlays() {
        for (const d of [shell.connectionDialog, shell.hostKeyDialog, shell.sshSecretDialog])
            if (d && d.visible) {
                tryVerify(() => d.opened || !d.visible);
                d.close();
            }
        tryVerify(() => ![shell.connectionDialog, shell.hostKeyDialog, shell.sshSecretDialog].some(d => d && d.visible), 5000);
    }

    function initTestCase() {
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/ssh-connections.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        tryVerify(() => !shell.connections.model.loading, 10000);
    }

    function init() {
        closeOverlays();
        Language.choice = Language.Choice.English;
        shell.focusRegion(1);
    }

    // ---- connection dialog: the tunnel section ------------------------------------------------

    function test_sectionIsOnlyForEnginesThatCanTunnel() {
        CommandRegistry.trigger("connection.new");
        tryVerify(() => shell.connectionDialog?.opened);
        const dlg = shell.connectionDialog;
        tryVerify(() => dlg.nameField.activeFocus);
        // SQLite: no tunnel
        compare(shell.connections.form.sshAvailable, false);
        verify(text("SSH tunnel") === undefined);
        verify(text("Connect through an SSH tunnel") === undefined);
        dlg.close();
        tryVerify(() => !dlg.visible);

        const pg = openPostgres();
        compare(shell.connections.form.sshAvailable, true);
        verify(text("SSH tunnel") !== undefined);
        verify(pg.sshCheckBox.visible);
        compare(pg.sshCheckBox.checked, false);
        // Off: nothing else of the section is there
        verify(field("SSH host") === undefined);
        compare(walker.problems(overlay()), []);
        pg.close();
    }

    function test_keyboardFlowThroughTheSection() {
        const dlg = openPostgres();
        dlg.nameField.forceActiveFocus(Qt.TabFocusReason);
        typeText("Prod");
        const box = dlg.sshCheckBox;
        box.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        compare(shell.connections.form.sshEnabled, true);
        compare(box.checked, true);

        // The save button says why it is still off, and the section is first to blame
        compare(dlg.saveButton.enabled, false);
        compare(dlg.invalidReason, "Fill in the SSH tunnel settings to save.");
        // Nothing is shouted before the user acts
        verify(text("Enter the SSH server host.") === undefined);

        // Tab order: switch, host, port, user, method
        keyClick(Qt.Key_Tab);
        compare(focused().accessibleName, "SSH host");
        keyClick(Qt.Key_Tab);
        compare(focused().accessibleName, "SSH port");
        compare(focused().text, "22");
        keyClick(Qt.Key_Tab);
        compare(focused().accessibleName, "SSH user");
        keyClick(Qt.Key_Tab);
        compare(focused().accessibleName, "SSH authentication");

        // Trying to save marks every missing field, with text and icon, and announces it
        dlg.trySave();
        verify(text("Enter the SSH server host.") !== undefined);
        verify(text("Enter the SSH user name.") !== undefined);
        compare(field("SSH host").Accessible.description, "Required. Enter the SSH server host.");
        tryVerify(() => dlg.lastAnnouncement.indexOf("SSH") >= 0);

        field("SSH host").forceActiveFocus(Qt.TabFocusReason);
        typeText("bastion.example.com");
        verify(text("Enter the SSH server host.") === undefined);
        field("SSH user").forceActiveFocus(Qt.TabFocusReason);
        typeText("deploy");
        compare(shell.connections.form.sshHost, "bastion.example.com");
        compare(shell.connections.form.sshUser, "deploy");

        // A wrong port is said at the field
        field("SSH port").forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_A, Qt.ControlModifier);
        typeText("99999");
        verify(text("The SSH port must be between 1 and 65535.") !== undefined);
        keyClick(Qt.Key_A, Qt.ControlModifier);
        typeText("2222");
        verify(text("The SSH port must be between 1 and 65535.") === undefined);
        compare(shell.connections.form.sshPort, "2222");
        compare(walker.problems(overlay()), []);
        dlg.close();
    }

    function test_methodsChangeWhatIsAskedFor() {
        const dlg = openPostgres();
        shell.connections.form.sshEnabled = true;
        // Password: the secret is the SSH password
        verify(field("SSH password") !== undefined);
        verify(field("Private key file") === undefined);
        verify(find(overlay(), i => i.accessibleName === "SSH secret storage") !== undefined);

        // Key: key path with Browse… (named for its field) and the passphrase
        shell.connections.form.sshAuth = "key";
        verify(field("Private key file") !== undefined);
        verify(find(overlay(), i => i.text === "Browse…" && i.Accessible.name === "Browse… (Private key file)") !== undefined);
        verify(field("Key passphrase") !== undefined);
        verify(field("SSH password") === undefined);
        verify(text("Quarry only remembers where the key file is. It never reads or copies the key.") !== undefined);
        dlg.trySave();
        verify(text("Choose the SSH private key file.") !== undefined);
        field("Private key file").forceActiveFocus(Qt.TabFocusReason);
        typeText("C:/keys/id_ed25519");
        compare(shell.connections.form.sshKeyPath, "C:/keys/id_ed25519");
        verify(text("Choose the SSH private key file.") === undefined);

        // Agent: nothing to store
        shell.connections.form.sshAuth = "agent";
        verify(field("Key passphrase") === undefined);
        verify(field("SSH password") === undefined);
        verify(field("Private key file") === undefined);
        verify(find(overlay(), i => i.accessibleName === "SSH secret storage") === undefined);
        compare(walker.problems(overlay()), []);
        dlg.close();
    }

    function test_theSecretIsTouchedOnlyWhenTyped() {
        const dlg = openPostgres();
        shell.connections.form.sshEnabled = true;
        compare(dlg.draft().sshSecretTouched, false);
        const pw = dlg.sshSecretField;
        verify(pw.visible);
        compare(pw.echoMode, TextInput.Password);
        pw.forceActiveFocus(Qt.TabFocusReason);
        typeText("s3cret");
        const d = dlg.draft();
        compare(d.sshSecretTouched, true);
        compare(d.sshSecret, "s3cret");

        // "Ask every time" says it is never saved
        verify(text("It is asked for every time you connect. It is never saved.") === undefined);
        shell.connections.form.sshSecretMode = 2;
        verify(text("It is asked for every time you connect. It is never saved.") !== undefined);
        compare(pw.placeholderText, "Used only to test; never saved");

        // Closing forgets what was typed
        dlg.close();
        tryVerify(() => !dlg.visible);
        compare(dlg.sshSecret, "");
    }

    // ---- fingerprint dialog -------------------------------------------------------------------

    function test_unknownServerDialogStartsOnCancelAndShowsTheFingerprint() {
        pre().hostKeyRequested("bastion.example.com", 2222, "ssh-ed25519", "SHA256:nThbg6kXUpJWGl7E1IGOCspRomTxdCARLviKw6E5SY8");
        tryVerify(() => shell.hostKeyDialog?.opened);
        const dlg = shell.hostKeyDialog;
        verify(dlg.cancelButton.activeFocus, "the focus starts on Cancel");
        verify(text("The authenticity of the server bastion.example.com:2222 can't be verified.") !== undefined);
        verify(text("Key type: ssh-ed25519") !== undefined);
        const fp = dlg.fingerprintText;
        verify(fp.visible);
        compare(fp.text, "SHA256:nThbg6kXUpJWGl7E1IGOCspRomTxdCARLviKw6E5SY8");
        compare(fp.font.family, Theme.monoFontFamily);
        compare(fp.readOnly, true);
        compare(fp.Accessible.description, fp.text); // the screen reader gets it whole
        compare(fp.Accessible.name, "SHA256 fingerprint");
        verify(fp.activeFocusOnTab);
        // Nothing to accept on a changed key is shown here
        verify(!dlg.closeButton.visible);
        verify(dlg.cancelButton.visible && dlg.onceButton.visible && dlg.rememberButton.visible);
        compare(dlg.cancelButton.Accessible.description, dlg.message);
        compare(walker.problems(overlay()), []);
        dlg.close();
    }

    function test_theThreeAnswersByKeyboard() {
        const dlg = (() => {
                pre().hostKeyRequested("bastion", 22, "ssh-rsa", "SHA256:abc");
                tryVerify(() => shell.hostKeyDialog?.opened);
                return shell.hostKeyDialog;
            })();
        // Port 22 is not written out
        verify(text("The authenticity of the server bastion can't be verified.") !== undefined);
        const trusted = createTemporaryObject(spyComp, testCase, {
            target: dlg,
            signalName: "trusted"
        });
        const declined = createTemporaryObject(spyComp, testCase, {
            target: dlg,
            signalName: "declined"
        });

        // Tab from Cancel: Trust once, then Trust and remember
        verify(dlg.cancelButton.activeFocus);
        keyClick(Qt.Key_Tab);
        verify(dlg.onceButton.activeFocus);
        keyClick(Qt.Key_Space);
        compare(trusted.count, 1);
        compare(trusted.signalArguments[0][0], false); // for this run only
        compare(declined.count, 0);
        tryVerify(() => !dlg.visible);

        pre().hostKeyRequested("bastion", 22, "ssh-rsa", "SHA256:abc");
        tryVerify(() => dlg.opened);
        keyClick(Qt.Key_Tab);
        keyClick(Qt.Key_Tab);
        verify(dlg.rememberButton.activeFocus);
        keyClick(Qt.Key_Space);
        compare(trusted.count, 2);
        compare(trusted.signalArguments[1][0], true); // written to known_hosts
        tryVerify(() => !dlg.visible);

        // Cancel, and Esc, decline: the connection is dropped, no error is made up
        pre().hostKeyRequested("bastion", 22, "ssh-rsa", "SHA256:abc");
        tryVerify(() => dlg.opened);
        keyClick(Qt.Key_Space); // on Cancel
        tryCompare(declined, "count", 1);
        tryVerify(() => !dlg.visible);
        pre().hostKeyRequested("bastion", 22, "ssh-rsa", "SHA256:abc");
        tryVerify(() => dlg.opened);
        keyClick(Qt.Key_Escape);
        tryCompare(declined, "count", 2);
        compare(trusted.count, 2);
        tryVerify(() => !dlg.visible);
    }

    function test_theFingerprintCanBeCopiedFromTheKeyboard() {
        pre().hostKeyRequested("h", 22, "ssh-ed25519", "SHA256:copyme");
        tryVerify(() => shell.hostKeyDialog?.opened);
        const dlg = shell.hostKeyDialog;
        const fp = dlg.fingerprintText;
        fp.forceActiveFocus(Qt.TabFocusReason);
        verify(fp.activeFocus);
        keyClick(Qt.Key_A, Qt.ControlModifier);
        compare(fp.selectedText, "SHA256:copyme");
        dlg.close();
    }

    function test_aChangedKeyHasNoWayToAcceptIt() {
        pre().hostKeyMismatch("bastion", 22, "ssh-ed25519", "SHA256:known", "SHA256:received");
        tryVerify(() => shell.hostKeyDialog?.opened);
        const dlg = shell.hostKeyDialog;
        compare(dlg.mismatch, true);
        compare(dlg.title, "The server's host key has changed");
        verify(dlg.closeButton.visible);
        verify(dlg.closeButton.activeFocus);
        verify(!dlg.cancelButton.visible);
        verify(!dlg.onceButton.visible);
        verify(!dlg.rememberButton.visible);
        verify(text("Trust once") === undefined);
        verify(text("Trust and remember") === undefined);
        compare(dlg.knownFingerprintText.text, "SHA256:known");
        compare(dlg.receivedFingerprintText.text, "SHA256:received");
        verify(!dlg.fingerprintText.visible);
        // Said in words with an icon, not by color alone
        verify(text("The host key of the SSH server bastion is not the one Quarry saved. Someone may be impersonating the server, or it was reinstalled. Quarry did not connect and cannot accept the new key from here.") !== undefined);
        compare(walker.problems(overlay()), []);

        // Keyboard: no trust signal can come out of it
        const trusted = createTemporaryObject(spyComp, testCase, {
            target: dlg,
            signalName: "trusted"
        });
        const declined = createTemporaryObject(spyComp, testCase, {
            target: dlg,
            signalName: "declined"
        });
        keyClick(Qt.Key_Space);
        tryVerify(() => !dlg.visible);
        compare(trusted.count, 0);
        compare(declined.count, 0);

        // Only the fingerprint that came back (a race at open time)
        pre().hostKeyMismatch("bastion", 22, "", "", "SHA256:late");
        tryVerify(() => dlg.opened);
        verify(!dlg.knownFingerprintText.visible);
        compare(dlg.receivedFingerprintText.text, "SHA256:late");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dlg.visible);
        compare(declined.count, 0);
    }

    // ---- the secret prompt --------------------------------------------------------------------

    function test_theTunnelsSecretIsAskedWithItsOwnWords() {
        pre().secretRequested("c1", "Prod", false, false);
        tryVerify(() => shell.sshSecretDialog?.opened);
        const dlg = shell.sshSecretDialog;
        compare(dlg.title, "SSH password required");
        verify(dlg.passwordField.activeFocus);
        compare(dlg.passwordField.accessibleName, "SSH password");
        compare(dlg.passwordField.echoMode, TextInput.Password);
        verify(text("Enter the SSH password to open the tunnel of “Prod”. Quarry keeps it in memory until you close the application.") !== undefined);
        verify(text("The SSH password was not accepted. Try again.") === undefined);
        const submitted = createTemporaryObject(spyComp, testCase, {
            target: dlg,
            signalName: "submitted"
        });
        const dismissed = createTemporaryObject(spyComp, testCase, {
            target: dlg,
            signalName: "dismissed"
        });
        typeText("pw");
        keyClick(Qt.Key_Return);
        compare(submitted.count, 1);
        compare(submitted.signalArguments[0][0], "c1");
        compare(submitted.signalArguments[0][1], "pw");
        compare(dismissed.count, 0);
        tryVerify(() => !dlg.visible);

        // A key's passphrase, after a refused one
        pre().secretRequested("c1", "Prod", true, true);
        tryVerify(() => dlg.opened);
        compare(dlg.title, "Key passphrase required");
        compare(dlg.passwordField.accessibleName, "Key passphrase");
        verify(text("The key passphrase was not accepted. Try again.") !== undefined);
        verify(text("Enter the passphrase of the SSH key for “Prod”. Leave it empty if the key has none. Quarry keeps it in memory until you close the application.") !== undefined);
        compare(walker.problems(overlay()), []);
        keyClick(Qt.Key_Escape);
        tryCompare(dismissed, "count", 1);
        compare(dismissed.signalArguments[0][0], "c1");
        compare(submitted.count, 1);
    }

    function test_theDatabasePasswordPromptIsUnchanged() {
        pre().secretRequested("c2", "Db", false, false);
        tryVerify(() => shell.sshSecretDialog?.opened);
        shell.sshSecretDialog.close();
        tryVerify(() => !shell.sshSecretDialog.visible);
        // The dialog is shared by the database password: its default kind is still "database"
        shell.sshSecretDialog.openFor("c3", "Plain", false);
        tryVerify(() => shell.sshSecretDialog.opened);
        compare(shell.sshSecretDialog.title, "Password required");
        compare(shell.sshSecretDialog.passwordField.accessibleName, "Password");
        shell.sshSecretDialog.close();
    }
}
