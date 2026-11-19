; Inno Setup script of the Quarry installer (docs/ARCHITECTURE.md section 15).
;
; Built by packaging/windows/build_installers.ps1, which passes:
;   /DAppVersion=<x.y.z>      the version of project(quarry VERSION ...)
;   /DSourceDir=<path>        the tree laid out by `cmake --install` (bin\, LICENSE, ...)
;   /DOutputDir=<path>        where Quarry-<version>-x64-setup.exe goes
;
; Per-user by default (no administrator rights); the wizard offers an install for all users, which
; asks for them. Uninstalling removes what was installed and leaves the user's data alone
; (%APPDATA%\Quarry and the credentials in the Windows keychain).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #error SourceDir is required: the tree laid out by cmake --install
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Fixed forever: it identifies the application for upgrades and the uninstaller.
AppId={{2F2ACA01-3B8D-42A8-BCE9-6E377100815E}
AppName=Quarry
AppVersion={#AppVersion}
AppVerName=Quarry {#AppVersion}
AppPublisher=The Quarry authors
AppPublisherURL=https://github.com/Hewcan/quarry
AppSupportURL=https://github.com/Hewcan/quarry/issues
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Quarry
DefaultGroupName=Quarry
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809, the minimum of the project.
MinVersion=10.0.17763
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=Quarry-{#AppVersion}-x64-setup
LicenseFile={#SourceDir}\LICENSE
SetupIconFile=..\..\src\app\assets\app\quarry.ico
UninstallDisplayIcon={app}\bin\quarry.exe
UninstallDisplayName=Quarry
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Quarry"; Filename: "{app}\bin\quarry.exe"
Name: "{autodesktop}\Quarry"; Filename: "{app}\bin\quarry.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\quarry.exe"; Description: "{cm:LaunchProgram,Quarry}"; Flags: nowait postinstall skipifsilent
