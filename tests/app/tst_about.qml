import QtQuick
import QtTest
import Quarry.Controls

// About Quarry (H10): the dialog opened from the command palette, with the focus on Close, Esc,
// the version, the licenses view that loads on demand and can be selected and copied, the
// accessibility walker in every theme, 200 % zoom and Spanish. The licenses come from a temporary
// file (the test executable does not embed the real resource).
TestCase {
    id: testCase

    property var shell: null
    property string savedPath: ""
    readonly property string licenseFile: testTempDir + "/about-licenses.txt"
    property string licenses: ""

    name: "About"
    when: windowShown
    width: 900
    height: 650
    visible: true

    A11y {
        id: walker
    }

    // ---- helpers -----------------------------------------------------------------------------

    function dlg() {
        return shell.aboutDialog;
    }

    function paletteField() {
        return walker.findAll(shell.overlayItem, i => i.accessibleName === "Search commands")[0];
    }

    function typeText(text) {
        for (const ch of text)
            keyClick(ch === " " ? Qt.Key_Space : ch);
    }

    function openAbout() {
        if (Language.effective === Language.Choice.Spanish) {
            // The palette's field is found by its English name; Spanish is about the strings.
            CommandRegistry.find("about.open").trigger();
        } else {
            keyClick(Qt.Key_K, Qt.ControlModifier);
            tryVerify(() => shell.paletteDialog?.opened);
            tryVerify(() => paletteField().activeFocus);
            typeText("about quarry");
            keyClick(Qt.Key_Return);
        }
        tryVerify(() => dlg() !== null && dlg().opened);
        tryVerify(() => dlg().closeButton.activeFocus);
    }

    function closeAbout() {
        const d = dlg();
        if (d && d.visible) {
            tryVerify(() => d.opened || !d.visible);
            d.close();
            tryVerify(() => !d.visible);
        }
        const p = shell.paletteDialog;
        if (p && p.visible) {
            tryVerify(() => p.opened || !p.visible);
            p.close();
            tryVerify(() => !p.visible);
        }
    }

    function showLicenses() {
        const d = dlg();
        d.licensesButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryVerify(() => d.licensesLoader.item !== null);
        tryVerify(() => d.licensesLoader.item.edit.activeFocus);
    }

    // The user's clipboard text is put back.
    function clipboardAround(fn) {
        const saved = a11yProbe.clipboardText();
        a11yProbe.setClipboardText("");
        try {
            fn();
        } finally {
            a11yProbe.setClipboardText(saved);
        }
    }

    // A text is clipped when it is elided or wider/taller than the box the layout gave it.
    function textProblems(root) {
        const bad = [];
        const texts = walker.findAll(root, i => i.visible && i.opacity > 0 && i.truncated !== undefined && i.contentWidth !== undefined && i.text !== undefined && i.text !== "");
        for (const t of texts) {
            if (t.truncated)
                bad.push(t.text + ": truncated");
            if (t.contentWidth > t.width + 1)
                bad.push(t.text + ": contentWidth " + t.contentWidth + " > width " + t.width);
            if (t.contentHeight > t.height + 1)
                bad.push(t.text + ": contentHeight " + t.contentHeight + " > height " + t.height);
        }
        return {
            count: texts.length,
            problems: bad
        };
    }

    function initTestCase() {
        let text = "Third-party software in Quarry\n==============================\n\nQt 6\n---\nLGPLv3, dynamic.\n";
        for (let i = 0; i < 300; ++i)
            text += "Line " + i + " of the license text, long enough to need some room on the screen.\n";
        licenses = text;
        a11yProbe.writeFile(licenseFile, text);
        savedPath = AboutInfo.licensesPath;
        AboutInfo.licensesPath = licenseFile;
        const comp = Qt.createComponent(Qt.resolvedUrl("../../src/app/qml/MainWindow.qml"));
        compare(comp.status, Component.Ready, comp.errorString());
        shell = comp.createObject(testCase, {
            width: testCase.width,
            height: testCase.height,
            databasePath: testTempDir + "/about.db",
            providerDirectory: testPluginDir
        });
        verify(shell !== null);
        // Nothing of the dialog exists before it is used.
        compare(shell.aboutDialog, null);
    }

    function cleanupTestCase() {
        Language.choice = Language.Choice.English;
        AboutInfo.licensesPath = savedPath;
    }

    function init() {
        closeAbout();
        Language.choice = Language.Choice.English;
        Theme.mode = Theme.Mode.System;
        Theme.resetZoom();
        shell.resetLayout();
        shell.focusRegion(1);
    }

    // ---- the command and the dialog ----------------------------------------------------------

    function test_commandIsInThePaletteWithoutAShortcut() {
        const c = CommandRegistry.find("about.open");
        verify(c !== null && c !== undefined);
        compare(c.title, "About Quarry");
        compare(c.category, "General");
        compare(c.shortcut, "");
        verify(c.enabled);
    }

    function test_opensFromThePaletteWithTheFocusOnClose() {
        openAbout();
        const d = dlg();
        compare(d.title, "About Quarry");
        compare(d.closeButton.Accessible.role, Accessible.Button);
        compare(d.closeButton.Accessible.name, "Close");
        verify(d.closeButton.activeFocus);
        compare(walker.problems(d.contentItem), []);
    }

    function test_escCloses() {
        openAbout();
        keyClick(Qt.Key_Escape);
        tryVerify(() => !dlg().visible);
    }

    function test_theVersionIsTheApplicationVersion() {
        const before = Qt.application.version;
        Qt.application.version = "9.8.7"; // the test executable has none of its own
        compare(Qt.application.version, "9.8.7");
        openAbout();
        compare(dlg().versionText.text, "Version 9.8.7");
        Qt.application.version = before;
    }

    function test_showsLicenseAndQtNotice() {
        openAbout();
        const texts = walker.findAll(dlg().contentItem, i => i.visible && typeof i.text === "string").map(i => i.text);
        verify(texts.includes("Copyright 2026 The Quarry authors"), texts);
        verify(texts.some(t => t.includes("Apache License 2.0")));
        verify(texts.some(t => t.includes("LGPL") && t.includes("dynamically")));
        verify(texts.includes("A desktop database manager."));
    }

    // ---- the licenses view -------------------------------------------------------------------

    function test_licensesLoadOnlyWhenAsked() {
        openAbout();
        const d = dlg();
        compare(d.licensesLoader.item, null);
        compare(d.licensesLoader.active, false);
        compare(d.licensesButton.Accessible.name, "Third-party licenses");
        showLicenses();
        const edit = d.licensesLoader.item.edit;
        compare(edit.text, licenses);
        compare(edit.readOnly, true);
        compare(edit.Accessible.role, Accessible.EditableText);
        compare(edit.Accessible.name, "Third-party licenses");
        verify(edit.Accessible.readOnly);
        verify(!d.licensesButton.visible);
        verify(d.backButton.visible);
        compare(walker.problems(d.contentItem), []);
    }

    function test_licensesCanBeSelectedAndCopied() {
        openAbout();
        showLicenses();
        const d = dlg();
        clipboardAround(() => {
            keyClick(Qt.Key_A, Qt.ControlModifier);
            compare(d.licensesLoader.item.edit.selectedText.length, licenses.length);
            keyClick(Qt.Key_C, Qt.ControlModifier);
            compare(a11yProbe.clipboardText(), licenses);
        });
        clipboardAround(() => {
            d.copyButton.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            compare(a11yProbe.clipboardText(), licenses);
        });
    }

    function test_keysScrollTheLicenses() {
        openAbout();
        showLicenses();
        const flick = dlg().licensesLoader.item.flick;
        verify(flick.contentHeight > flick.height, "the text needs scrolling");
        compare(flick.contentY, 0);
        keyClick(Qt.Key_PageDown);
        verify(flick.contentY > 0);
        const y = flick.contentY;
        keyClick(Qt.Key_Down);
        verify(flick.contentY > y);
        keyClick(Qt.Key_End);
        compare(flick.contentY, flick.contentHeight - flick.height);
        keyClick(Qt.Key_Home);
        compare(flick.contentY, 0);
    }

    function test_tabOrderInTheLicensesView() {
        openAbout();
        showLicenses();
        const d = dlg();
        verify(d.licensesLoader.item.edit.activeFocus);
        keyClick(Qt.Key_Tab);
        verify(d.backButton.activeFocus, "Back follows the text");
        keyClick(Qt.Key_Tab);
        verify(d.copyButton.activeFocus);
        keyClick(Qt.Key_Tab);
        verify(d.closeButton.activeFocus);
    }

    function test_backReturnsToTheInfoAndEscCloses() {
        openAbout();
        showLicenses();
        const d = dlg();
        d.backButton.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryVerify(() => !d.showLicenses);
        tryVerify(() => d.licensesButton.activeFocus);
        verify(d.versionText.visible);
        showLicenses();
        keyClick(Qt.Key_Escape); // Esc in the text view closes the dialog, it is not swallowed
        tryVerify(() => !d.visible);
        // Reopened, it starts on the info view again.
        openAbout();
        verify(!d.showLicenses);
        verify(d.versionText.visible);
    }

    function test_missingLicensesFileShowsAMessage() {
        AboutInfo.licensesPath = testTempDir + "/does-not-exist.txt";
        openAbout();
        showLicenses();
        const text = dlg().licensesLoader.item.edit.text;
        verify(text.includes("not available"), text);
        AboutInfo.licensesPath = licenseFile;
    }

    // ---- themes, zoom and language -----------------------------------------------------------

    function test_accessibleInEveryTheme() {
        for (const mode of [Theme.Mode.Light, Theme.Mode.Dark, Theme.Mode.HighContrast]) {
            Theme.mode = mode;
            openAbout();
            const d = dlg();
            compare(walker.problems(d.contentItem), []);
            for (const b of walker.findAll(d.contentItem, i => i.Accessible.role === Accessible.Button && i.visible))
                verify(b.height >= Theme.controlHeight && b.Accessible.name !== "", b.text);
            showLicenses();
            compare(walker.problems(d.contentItem), []);
            closeAbout();
        }
        verify(Theme.isHighContrast);
    }

    function test_dialogFitsAt200PercentZoom() {
        Theme.zoom = 2.0;
        openAbout();
        const d = dlg();
        tryVerify(() => d.height <= shell.height, 5000, "the dialog is not taller than the window");
        tryVerify(() => d.width <= shell.width);
        const t = textProblems(d.contentItem);
        verify(t.count >= 5);
        compare(t.problems, []);
        let bottom = d.closeButton.mapToItem(shell, 0, d.closeButton.height).y;
        verify(bottom <= shell.height, "Close is inside the window");
        showLicenses();
        tryVerify(() => d.height <= shell.height);
        tryVerify(() => d.width <= shell.width);
        verify(d.licensesLoader.height > 0);
        bottom = d.copyButton.mapToItem(shell, 0, d.copyButton.height).y;
        verify(bottom <= shell.height, "Copy text is inside the window");
        compare(walker.problems(d.contentItem), []);
        Theme.resetZoom();
    }

    function test_spanishStrings() {
        Language.choice = Language.Choice.Spanish;
        compare(CommandRegistry.find("about.open").title, "Acerca de Quarry");
        openAbout();
        const d = dlg();
        compare(d.title, "Acerca de Quarry");
        compare(d.versionText.text, "Versión " + Qt.application.version);
        compare(d.closeButton.text, "Cerrar");
        compare(d.licensesButton.text, "Licencias de terceros");
        showLicenses();
        compare(d.licensesLoader.item.edit.Accessible.name, "Licencias de terceros");
        compare(d.backButton.text, "Volver");
        compare(d.copyButton.text, "Copiar el texto");
    }
}
