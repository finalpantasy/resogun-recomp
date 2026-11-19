; Inno Setup 6 script: per-user installer for the release folder.
; Built by tools/package.py --installer (defines AppVersion and SourceDir); or run ISCC with both.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist"
#endif

[Setup]
AppId={{6D1B5C0E-4F7A-4C58-9E21-8A3F0B7D52C4}
AppName=Everybody's Golf PC
AppVersion={#AppVersion}
AppPublisher=Everybody's Golf PC project
DefaultDirName={sd}\Games\Everybody's Golf PC
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
DisableDirPage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra
SolidCompression=yes
OutputDir={#SourceDir}
OutputBaseFilename=EverybodysGolfPC-Setup-{#AppVersion}
UninstallDisplayName=Everybody's Golf PC
UninstallDisplayIcon={app}\EverybodysGolf.exe
#ifexist "..\..\res\golf.ico"
SetupIconFile=..\..\res\golf.ico
#endif
WizardStyle=modern

[Messages]
SelectDirDesc=Where should Everybody's Golf PC be installed?
SelectDirBrowseLabel=Pick any folder. The game needs about 15 GB more here for its prepared files, and everything it writes (saves, settings, shader cache, texture packs) stays in this folder. Click Next to continue.

[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
; Everything in the release folder except the installer itself and any game-derived folder.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "EverybodysGolfPC-Setup-*.exe,game,data,save,shaders,log,gpu,paths.ini"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{userprograms}\Everybody's Golf"; Filename: "{app}\EverybodysGolf.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\Everybody's Golf"; Filename: "{app}\EverybodysGolf.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\EverybodysGolf.exe"; Description: "Launch Everybody's Golf"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
// Saves, settings, the prepared image, shader cache and texture packs live in {app}\data; keep them unless asked.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    if DirExists(ExpandConstant('{app}\data')) and not UninstallSilent then
      if MsgBox('Also delete saves, settings, the prepared game files and texture packs?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ExpandConstant('{app}\data'), True, True, True);
end;
