; Inno Setup script for the Montage installer. Built in CI after
; scripts/package-windows.sh has filled dist\Montage:
;
;   ISCC.exe /DVersion=0.1.0 packaging\windows\montage.iss
;   -> dist\Montage-<version>-windows-x64-setup.exe

#ifndef Version
  #define Version "0.0.0"
#endif

[Setup]
AppId={{9F70E537-9316-4932-A0D8-FDF60F95F221}
AppName=Montage
AppVersion={#Version}
AppVerName=Montage {#Version}
AppPublisher=Montage
DefaultDirName={autopf}\Montage
DisableProgramGroupPage=yes
OutputDir=..\..\dist
OutputBaseFilename=Montage-{#Version}-windows-x64-setup
SetupIconFile=montage.ico
UninstallDisplayIcon={app}\montage.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Installs for everyone (admin) or just the current user, the user's choice.
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\..\dist\Montage\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Montage"; Filename: "{app}\montage.exe"
Name: "{autodesktop}\Montage"; Filename: "{app}\montage.exe"; Tasks: desktopicon

[Registry]
; Double-clicking a .montage project opens it in Montage.
Root: HKA; Subkey: "Software\Classes\.montage"; ValueType: string; ValueName: ""; ValueData: "Montage.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Montage.Project"; ValueType: string; ValueName: ""; ValueData: "Montage Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Montage.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\montage.exe,0"
Root: HKA; Subkey: "Software\Classes\Montage.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\montage.exe"" ""%1"""

[Run]
Filename: "{app}\montage.exe"; Description: "{cm:LaunchProgram,Montage}"; Flags: nowait postinstall skipifsilent
