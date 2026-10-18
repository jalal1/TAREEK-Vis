; Inno Setup script for TAREEK-Vis.
; Builds a per-user installer (no admin rights required) from the folder
; produced by deploy.bat (..\dist\TAREEK-Vis).
;
; Requires Inno Setup 6: https://jrsoftware.org/isinfo.php
; Build with: iscc installer\TAREEK-Vis.iss
; Output: installer\Output\TAREEK-Vis-Setup-<version>.exe

#define MyAppName "TAREEK-Vis"
; Keep in step with project(... VERSION ...) in CMakeLists.txt.
#define MyAppVersion "1.1.0"
#define MyAppPublisher "TAREEK-Vis Contributors"
#define MyAppURL "https://github.com/jalal1/TAREEK-Vis"
#define MyAppExeName "TAREEK-Vis.exe"

[Setup]
AppId={{B6E2A6C1-6E7B-4B6B-9B7E-6F6C6B7A9C10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=Output
OutputBaseFilename=TAREEK-Vis-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
SetupIconFile=
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "..\dist\TAREEK-Vis\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

; The app writes outside {app}: the map tile cache (up to 300 MB) under
; LocalAppData, and its settings (incl. the recent-folders list) under HKCU.
; Remove both so an uninstall leaves nothing behind.
[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\{#MyAppName}"

[Registry]
Root: HKCU; Subkey: "Software\{#MyAppName}"; Flags: uninsdeletekey dontcreatekey

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
