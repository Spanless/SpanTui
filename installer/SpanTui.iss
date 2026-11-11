#define MyAppName "SpanTui"
#ifndef MyAppVersion
  #define MyAppVersion "0.2.4"
#endif
#define MyAppPublisher "Span"
; Public portable and installed executable name (overridable via /D).
#ifndef MyAppExeName
  #define MyAppExeName "SpanTui.exe"
#endif
#define MyInstalledExeName "SpanTui.exe"
#ifndef MyOutputDir
  #define MyOutputDir "..\target\release\" + MyAppVersion + "\windows-x64"
#endif
#ifndef MyAppSourceDir
  #define MyAppSourceDir MyOutputDir + "\SpanTui-portable"
#endif
#ifndef MyShellCertificateSelfSigned
  #define MyShellCertificateSelfSigned 1
#endif

[Setup]
AppId={{F3D0F6EC-7151-4E8B-8A74-70DDEBE17B41}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; A desktop workspace should be installable without administrator rights.  The
; user can still select elevated installation from Inno Setup when desired.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={localappdata}\Programs\Span\SpanTui
DefaultGroupName={#MyAppName}
UninstallDisplayName={#MyAppName}
OutputDir={#MyOutputDir}
OutputBaseFilename=SpanTui-{#MyAppVersion}-Setup
SetupIconFile=..\assets\brand\spantui.ico
UninstallDisplayIcon={app}\{#MyInstalledExeName}
ChangesAssociations=yes
; lzma2/max+solid was ~50s for a ~250MB portable tree; lzma2/fast+solid is ~8s
; with only ~15MB larger Setup — worth it for every local package iteration.
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
; Windows 10 1809: the first build with ConPTY (CreatePseudoConsole), and the
; oldest Windows the bundled Qt 6 supports. Older systems would install and
; then fail to load the executable with a loader error.
MinVersion=10.0.17763
; Close in-use product images during upgrade (Restart Manager). In-app updates
; run Setup silently, where this closes them without asking: Explorer, when it
; has loaded the context-menu DLL, is closed and restarted by Windows.
CloseApplications=yes
RestartApplications=no
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "{#MyAppSourceDir}\{#MyAppExeName}"; DestDir: "{app}"; DestName: "{#MyInstalledExeName}"; Flags: ignoreversion
; portable.txt switches an image to portable mode; an installed copy keeps
; its data in %LOCALAPPDATA%\SpanTui.
Source: "{#MyAppSourceDir}\*"; DestDir: "{app}"; Excludes: "{#MyAppExeName},\portable.txt"; Flags: recursesubdirs ignoreversion

[InstallDelete]
Type: files; Name: "{app}\SpanTui-pre.exe"
Type: files; Name: "{app}\SpanTui-termhost.exe"
; The former in-app updater replaced the tree itself: its helper, the stage and
; backup images it kept beside the install directory, and the package it
; extracted and logged into the default data root.
Type: files; Name: "{app}\SpanTui-updater.exe"
Type: filesandordirs; Name: "{app}\..\.spantui-stage-*"
Type: filesandordirs; Name: "{app}\..\.spantui-backup-*"
Type: filesandordirs; Name: "{userappdata}\Span\SpanTui\cache\updates\package"
Type: files; Name: "{userappdata}\Span\SpanTui\logs\updater.log"
Type: filesandordirs; Name: "{app}\tools\clangd"
; In-place upgrades must drop QML that left the product. A leftover
; components/CodeEditor.qml plus import "components" shadows the current editor.
Type: files; Name: "{app}\qml\SpanTui\qml\components\CodeEditor.qml"
Type: files; Name: "{app}\qml\SpanTui\qml\components\SessionCreatePicker.qml"
Type: files; Name: "{app}\qml\SpanTui\qml\components\TerminalSwitch.qml"
Type: filesandordirs; Name: "{app}\qml\SpanTui\qml\components\acp"
Type: filesandordirs; Name: "{app}\qml\SpanTui\qml\components\lsp"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyInstalledExeName}"; IconFilename: "{app}\{#MyInstalledExeName}"; IconIndex: 0
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyInstalledExeName}"; IconFilename: "{app}\{#MyInstalledExeName}"; IconIndex: 0; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"
Name: "contextmenu"; Description: "Add SpanTui to the Explorer &context menu"; GroupDescription: "Explorer integration:"

[Run]
Filename: "{app}\{#MyInstalledExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
; In-app update (docs/services/update/updater.md): start the new version.
Filename: "{app}\{#MyInstalledExeName}"; Flags: nowait runasoriginaluser; Check: RelaunchRequested

[Code]
const
  ShellMenuStateKey = 'Software\Span\SpanTui\Setup';
  ShellMenuStateValue = 'ShellMenu';
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{F3D0F6EC-7151-4E8B-8A74-70DDEBE17B41}_is1';

  SynchronizeAccess = $00100000;
  HostExitTimeoutMs = 90000;

var
  WasInstalled: Boolean;

function OpenProcess(DesiredAccess: Cardinal; InheritHandle: Integer;
  ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(ObjectHandle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(ObjectHandle: THandle): Integer;
  external 'CloseHandle@kernel32.dll stdcall';

{ The in-app update starts Setup and then quits: /WAITPID=<pid> lets the host
  release its files before Setup looks for applications that use them. A host
  still running after the timeout is left to CloseApplications. }
procedure WaitForUpdatingHost;
var
  ProcessId: Cardinal;
  HostProcess: THandle;
begin
  ProcessId := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if ProcessId = 0 then
    Exit;
  HostProcess := OpenProcess(SynchronizeAccess, 0, ProcessId);
  if HostProcess = 0 then
    Exit;
  if WaitForSingleObject(HostProcess, HostExitTimeoutMs) <> 0 then
    Log(Format('SpanTui (pid %d) did not exit within %d ms.', [ProcessId, HostExitTimeoutMs]));
  CloseHandle(HostProcess);
end;

function InitializeSetup: Boolean;
begin
  WasInstalled := RegKeyExists(HKCU, UninstallKey) or RegKeyExists(HKLM, UninstallKey);
  WaitForUpdatingHost;
  Result := True;
end;

{ /RELAUNCH=1 comes only from the in-app update, which runs Setup silently. }
function RelaunchRequested: Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:RELAUNCH|0}') = '1');
end;

function IsWindows11: Boolean;
var
  Version: TWindowsVersion;
begin
  GetWindowsVersionEx(Version);
  Result := (Version.Major > 10) or
    ((Version.Major = 10) and (Version.Build >= 22000));
end;

function UsesSelfSignedShellCertificate: Boolean;
begin
  Result := {#MyShellCertificateSelfSigned} = 1;
end;

function PowerShellPath: String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function ShellScriptParameters(Script: String): String;
begin
  Result := '-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden ' +
    '-ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\shell\') + Script + '"';
end;

function ShellCertificatePath: String;
begin
  Result := ExpandConstant('{app}\shell\SpanTui-shell-identity.cer');
end;

{ Reading the machine store needs no elevation: an upgrade whose publisher is
  already trusted skips the administrator prompt. }
function MachineCertificateTrusted: Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(PowerShellPath, ShellScriptParameters('trust-machine.ps1') +
    ' -Mode Check -CertificatePath "' + ShellCertificatePath + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function RunMachineCertificateTrust(Mode: String; var ResultCode: Integer): Boolean;
begin
  ResultCode := -1;
  Result := ShellExec('runas', PowerShellPath,
    ShellScriptParameters('trust-machine.ps1') + ' -Mode ' + Mode +
    ' -CertificatePath "' + ShellCertificatePath + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

{ Classic verbs are per-user registry keys and need no elevation. Windows 10
  lists them in the context menu; Windows 11 under "Show more options". }
procedure WriteClassicVerb(Root, Verb, Caption, Arguments: String);
var
  Key: String;
  Exe: String;
begin
  Key := 'Software\Classes\' + Root + '\shell\' + Verb;
  Exe := ExpandConstant('{app}\{#MyInstalledExeName}');
  RegWriteStringValue(HKCU, Key, '', Caption);
  RegWriteStringValue(HKCU, Key, 'Icon', Exe);
  RegWriteStringValue(HKCU, Key + '\command', '', '"' + Exe + '" ' + Arguments);
end;

procedure InstallClassicMenu;
begin
  WriteClassicVerb('Directory', 'SpanTui.Open', '在 SpanTui 中打开', '--project "%1"');
  WriteClassicVerb('Directory', 'SpanTui.Terminal', '在 SpanTui 终端中打开', '--terminal --cwd "%1"');
  WriteClassicVerb('Directory\Background', 'SpanTui.Open', '在 SpanTui 中打开', '--project "%V"');
  WriteClassicVerb('Directory\Background', 'SpanTui.Terminal', '在 SpanTui 终端中打开', '--terminal --cwd "%V"');
end;

procedure RemoveClassicMenu;
begin
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Directory\shell\SpanTui.Open');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Directory\shell\SpanTui.Terminal');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Directory\Background\shell\SpanTui.Open');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Directory\Background\shell\SpanTui.Terminal');
end;

{ Windows 11 top-level menu: sparse package identity for the COM
  IExplorerCommand handler. A self-signed publisher needs one elevated trust
  step; a failed step leaves no trust or package behind. }
function InstallModernMenu: Boolean;
var
  ResultCode: Integer;
  CleanupCode: Integer;
begin
  Result := False;
  if UsesSelfSignedShellCertificate and not MachineCertificateTrusted then
  begin
    if (not RunMachineCertificateTrust('Install', ResultCode)) or (ResultCode <> 0) then
    begin
      Log(Format('SpanTui publisher trust was not installed (exit code %d).', [ResultCode]));
      Exit;
    end;
  end;
  ResultCode := -1;
  if Exec(PowerShellPath, ShellScriptParameters('register.ps1') +
      ' -PackagePath "' + ExpandConstant('{app}\shell\SpanTui-shell-identity.msix') +
      '" -ExternalLocation "' + ExpandConstant('{app}') +
      '" -CertificatePath "' + ShellCertificatePath + '"',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
  begin
    Result := True;
    Exit;
  end;
  Log(Format('SpanTui context-menu package registration failed (exit code %d).', [ResultCode]));
  if UsesSelfSignedShellCertificate then
    RunMachineCertificateTrust('Remove', CleanupCode);
end;

procedure RemoveModernMenu;
var
  ResultCode: Integer;
begin
  Exec(PowerShellPath, ShellScriptParameters('unregister.ps1'), '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  if UsesSelfSignedShellCertificate then
    RunMachineCertificateTrust('Remove', ResultCode);
end;

{ 'modern', 'classic' or 'none'. Installers before the optional task left no
  record: they registered the modern menu on Windows 11 and the classic verbs
  elsewhere. }
function RecordedShellMenu(Installed: Boolean): String;
begin
  if RegQueryStringValue(HKCU, ShellMenuStateKey, ShellMenuStateValue, Result) then
    Exit;
  if not Installed then
    Result := 'none'
  else if IsWindows11 then
    Result := 'modern'
  else
    Result := 'classic';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Previous: String;
  Current: String;
begin
  if CurStep <> ssPostInstall then
    Exit;

  Previous := RecordedShellMenu(WasInstalled);
  RemoveClassicMenu;
  Current := 'none';
  if WizardIsTaskSelected('contextmenu') then
  begin
    if IsWindows11 then
    begin
      if InstallModernMenu then
        Current := 'modern';
    end;
    if Current <> 'modern' then
    begin
      { The context menu is optional: falling back must never fail setup. }
      if IsWindows11 then
        SuppressibleMsgBox('The Windows 11 context menu could not be registered ' +
          '(administrator approval was declined, or app package installation is blocked). ' +
          'SpanTui was added under "Show more options" instead.',
          mbInformation, MB_OK, IDOK);
      InstallClassicMenu;
      Current := 'classic';
    end;
  end;
  if (Previous = 'modern') and (Current <> 'modern') then
    RemoveModernMenu;
  RegWriteStringValue(HKCU, ShellMenuStateKey, ShellMenuStateValue, Current);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep <> usUninstall then
    Exit;

  RemoveClassicMenu;
  if RecordedShellMenu(True) = 'modern' then
    RemoveModernMenu;
  RegDeleteKeyIncludingSubkeys(HKCU, ShellMenuStateKey);
  RegDeleteKeyIfEmpty(HKCU, 'Software\Span\SpanTui');
  RegDeleteKeyIfEmpty(HKCU, 'Software\Span');
end;
