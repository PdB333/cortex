; Cortex installer (Inno Setup 6).
;
; A per-user install: no administrator rights, files in
; %LOCALAPPDATA%\Programs\Cortex, a Start menu entry and an uninstaller in
; Settings > Apps. Settings and records stay in %LOCALAPPDATA%\Cortex and
; survive updates and uninstalls.
;
; Built by .github/workflows/publish-release.yml from the validated portable
; bundle:
;   iscc /DAppVersion=2.1.0 /DSourceDir=<bundle> /DOutputDir=dist installer\cortex.iss

#ifndef AppVersion
  #define AppVersion "2.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\stage\Cortex"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{8E4C2B1A-5F7D-4C3E-9A61-2D0B7C4E9F13}
AppName=Cortex
AppVersion={#AppVersion}
AppVerName=Cortex {#AppVersion}
AppPublisher=Cortex contributors
AppPublisherURL=https://github.com/PdB333/cortex
AppSupportURL=https://github.com/PdB333/cortex/issues
AppUpdatesURL=https://github.com/PdB333/cortex/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\Cortex
DefaultGroupName=Cortex
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=cortex-v{#AppVersion}-windows-setup
SetupIconFile=..\resources\windows\cortex.ico
UninstallDisplayIcon={app}\cortex.exe
UninstallDisplayName=Cortex {#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
; The desktop holds this mutex while it runs: the installer asks to close it.
AppMutex=CortexDesktopRunning
CloseApplications=yes
RestartApplications=no
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[CustomMessages]
english.OpenWith=Add "Analyze with Cortex" to the right-click menu of .exe, .dll and .asar files
french.OpenWith=Ajouter « Analyser avec Cortex » au clic droit des fichiers .exe, .dll et .asar
english.SetupClients=Register Cortex in my AI clients (Claude, Cursor, VS Code, Codex...), read-only
french.SetupClients=Ajouter Cortex à mes clients IA (Claude, Cursor, VS Code, Codex...), en lecture seule
english.AnalyzeWith=Analyze with Cortex
french.AnalyzeWith=Analyser avec Cortex
english.Integration=Integration:
french.Integration=Intégration :

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "openwith"; Description: "{cm:OpenWith}"; GroupDescription: "{cm:Integration}"; Flags: unchecked
Name: "setupclients"; Description: "{cm:SetupClients}"; GroupDescription: "{cm:Integration}"; Flags: unchecked

[Files]
; The portable bundle minus its portable marker; test programs are never in it.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "cortex.portable"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Cortex"; Filename: "{app}\cortex.exe"
Name: "{autodesktop}\Cortex"; Filename: "{app}\cortex.exe"; Tasks: desktopicon

[Registry]
; Right-click entry for the files Cortex reads (per user, removed on uninstall).
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.exe\shell\Cortex"; ValueType: string; ValueName: ""; ValueData: "{cm:AnalyzeWith}"; Flags: uninsdeletekey; Tasks: openwith
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.exe\shell\Cortex"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\cortex.exe,0"; Tasks: openwith
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.exe\shell\Cortex\command"; ValueType: string; ValueName: ""; ValueData: """{app}\cortex.exe"" ""%1"""; Tasks: openwith
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.dll\shell\Cortex"; ValueType: string; ValueName: ""; ValueData: "{cm:AnalyzeWith}"; Flags: uninsdeletekey; Tasks: openwith
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.dll\shell\Cortex"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\cortex.exe,0"; Tasks: openwith
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.dll\shell\Cortex\command"; ValueType: string; ValueName: ""; ValueData: """{app}\cortex.exe"" ""%1"""; Tasks: openwith
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.asar\shell\Cortex"; ValueType: string; ValueName: ""; ValueData: "{cm:AnalyzeWith}"; Flags: uninsdeletekey; Tasks: openwith
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.asar\shell\Cortex"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\cortex.exe,0"; Tasks: openwith
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.asar\shell\Cortex\command"; ValueType: string; ValueName: ""; ValueData: """{app}\cortex.exe"" ""%1"""; Tasks: openwith

[Run]
; Registers this cortex.exe in the AI clients installed for the user
; (read-only; the person allows writes later in Help > Set up AI clients).
Filename: "{app}\cortex.exe"; Parameters: "setup --yes"; Flags: runhidden waituntilterminated; Tasks: setupclients
; After an update started from Cortex (/RELAUNCH=1), reopen it.
Filename: "{app}\cortex.exe"; Flags: nowait; Check: ShouldRelaunch
Filename: "{app}\cortex.exe"; Description: "{cm:LaunchProgram,Cortex}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Only the entries that start this copy; a portable copy elsewhere keeps its own.
Filename: "{app}\cortex.exe"; Parameters: "setup --remove --only-this-copy --yes"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveAiClientEntries"

[Code]
function ShouldRelaunch(): Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;
