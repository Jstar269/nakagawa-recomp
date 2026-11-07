; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 the Nakagawa Recomp authors
;
; Per-user Windows installer for Nakagawa Recomp (#2208). Built by tools/build_installer.py with the
; Inno Setup version pinned in assets/upstream/innosetup.lock.json, from the folder a release ZIP unpacks to.
; The compiler is passed: PackageDir, AppVersion, IconFile, TouchDate, TouchTime.
;
; What this installer does and does not do (the rules are pinned by tools/test_installer.py):
;   - Per user, no administrator: nothing is written under Program Files, HKLM or any all-users folder.
;   - Writes the program files, a Start-menu shortcut (and an optional desktop shortcut) with the player's
;     taskbar identity, and Inno's own per-user uninstall entry. No other registry value, no file
;     association, no PATH entry, no service, no scheduled task.
;   - Uses no network: no download, no plugin, no web request.
;   - The uninstaller removes what Setup installed. It never touches %LOCALAPPDATA%\Nakagawa (settings,
;     games, saves, per-title folders) unless the person ticks the box on the uninstall page; a silent
;     uninstall never deletes it.

#ifndef PackageDir
  #error PackageDir is required: the folder a release ZIP unpacks to
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef IconFile
  #error IconFile is required
#endif
#ifndef TouchDate
  #error TouchDate is required (YYYY-MM-DD, from SOURCE_DATE_EPOCH)
#endif
#ifndef TouchTime
  #error TouchTime is required (HH:MM:SS, from SOURCE_DATE_EPOCH)
#endif

#define AppName "Nakagawa Recomp"
; The shortcut's name. Only tests/test_installer runs override it (/DShortcutName=...), so a trial install on a
; PC that has the real shortcut never replaces or removes it.
#ifndef ShortcutName
  #define ShortcutName AppName
#endif
#define AppExe "nakagawa_player.exe"
; The player's AppUserModelID (APP_USER_MODEL_ID in tools/stage_install.py, #2006).
#define AppUserModelID "Nakagawa.Recomp"

[Setup]
; Fixed for the life of the product: a changed AppId makes an upgrade a second, separate install.
AppId={{A37F65B8-FB5B-41BB-AC7B-3700FE43CB02}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Nakagawa Recomp project
AppPublisherURL=https://github.com/Jstar269/nakagawa-recomp
AppSupportURL=https://github.com/Jstar269/nakagawa-recomp/issues
AppUpdatesURL=https://github.com/Jstar269/nakagawa-recomp/releases
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile={#PackageDir}\LICENSE
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\bin\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
ChangesAssociations=no
ChangesEnvironment=no
; Deterministic file times inside the installer: the release commit's time, never the clock.
TimeStampsInUTC=yes
TouchDate={#TouchDate}
TouchTime={#TouchTime}
OutputDir=.
OutputBaseFilename=nakagawa-recomp-{#AppVersion}-windows-x64-setup
SetupLogging=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion touch

[Icons]
Name: "{userprograms}\{#ShortcutName}"; Filename: "{app}\bin\{#AppExe}"; WorkingDir: "{app}\bin"; AppUserModelID: "{#AppUserModelID}"
Name: "{userdesktop}\{#ShortcutName}"; Filename: "{app}\bin\{#AppExe}"; WorkingDir: "{app}\bin"; AppUserModelID: "{#AppUserModelID}"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\{#AppExe}"; Description: "Start {#AppName}"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
const
  USER_DATA_FOLDER_COUNT = 5;

var
  DeleteUserData: Boolean;

{ The five folders of the per-user data layout (docs/SETUP.md, "What stays after an uninstall"): the only
  folders the uninstall opt-in may delete. }
function UserDataFolder(Index: Integer): String;
begin
  case Index of
    0: Result := 'config';
    1: Result := 'data';
    2: Result := 'cache';
    3: Result := 'logs';
    4: Result := 'saves';
  else
    Result := '';
  end;
end;

function UserDataRoot: String;
begin
  Result := ExpandConstant('{localappdata}') + '\Nakagawa';
end;

function IsReparsePoint(const Path: String): Boolean;
var
  Found: TFindRec;
begin
  Result := False;
  if FindFirst(Path, Found) then
  begin
    try
      Result := (Found.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0;
    finally
      FindClose(Found);
    end;
  end;
end;

{ A folder this installer must not install into: the developer install tool's own (it has its own
  uninstall, and two uninstallers must never share a folder). }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}') + '\.nakagawa-install.json') then
    Result := 'This folder was set up by the developer install tool (tools/stage_install.py), which has its own uninstall. Choose another folder.';
end;

{ Deletes exactly the five data folders under %LOCALAPPDATA%\Nakagawa, and that folder if it is then empty.
  Nothing is deleted through a junction or symbolic link. }
procedure RemoveUserData;
var
  Index: Integer;
  Root, Folder: String;
begin
  Root := UserDataRoot;
  if (not DirExists(Root)) or IsReparsePoint(Root) then
    Exit;
  for Index := 0 to USER_DATA_FOLDER_COUNT - 1 do
  begin
    Folder := Root + '\' + UserDataFolder(Index);
    if DirExists(Folder) and (not IsReparsePoint(Folder)) then
      DelTree(Folder, True, True, True);
  end;
  RemoveDir(Root);
end;

{ The uninstall page: one box, unchecked. A silent uninstall skips it and keeps the data. }
function InitializeUninstall(): Boolean;
var
  Form: TSetupForm;
  Heading, Detail: TNewStaticText;
  Box: TNewCheckBox;
  Ok, Cancel: TNewButton;
begin
  Result := True;
  DeleteUserData := False;
  if UninstallSilent then
    Exit;
  Form := CreateCustomForm(ScaleX(440), ScaleY(210), False, True);
  try
    Form.Caption := 'Uninstall Nakagawa Recomp';
    Heading := TNewStaticText.Create(Form);
    Heading.Parent := Form;
    Heading.Left := ScaleX(16);
    Heading.Top := ScaleY(16);
    Heading.Width := Form.ClientWidth - ScaleX(32);
    Heading.AutoSize := False;
    Heading.WordWrap := True;
    Heading.Height := ScaleY(48);
    Heading.Caption := 'This removes the program. Your imported games, saves and settings stay in ' + UserDataRoot + ' unless you tick the box below.';
    Box := TNewCheckBox.Create(Form);
    Box.Parent := Form;
    Box.Left := ScaleX(16);
    Box.Top := ScaleY(72);
    Box.Width := Form.ClientWidth - ScaleX(32);
    Box.Height := ScaleY(20);
    Box.Caption := 'Also delete my games, saves and settings';
    Box.Checked := False;
    Detail := TNewStaticText.Create(Form);
    Detail.Parent := Form;
    Detail.Left := ScaleX(36);
    Detail.Top := ScaleY(98);
    Detail.Width := Form.ClientWidth - ScaleX(52);
    Detail.AutoSize := False;
    Detail.WordWrap := True;
    Detail.Height := ScaleY(48);
    Detail.Caption := 'Deletes the config, data, cache, logs and saves folders. Saves cannot be recovered. Your disc images elsewhere are never touched.';
    Ok := TNewButton.Create(Form);
    Ok.Parent := Form;
    Ok.Caption := 'Uninstall';
    Ok.ModalResult := mrOk;
    Ok.Left := Form.ClientWidth - ScaleX(184);
    Ok.Top := Form.ClientHeight - ScaleY(40);
    Ok.Width := ScaleX(80);
    Ok.Height := ScaleY(25);
    Ok.Default := True;
    Cancel := TNewButton.Create(Form);
    Cancel.Parent := Form;
    Cancel.Caption := 'Cancel';
    Cancel.ModalResult := mrCancel;
    Cancel.Left := Form.ClientWidth - ScaleX(96);
    Cancel.Top := Form.ClientHeight - ScaleY(40);
    Cancel.Width := ScaleX(80);
    Cancel.Height := ScaleY(25);
    Cancel.Cancel := True;
    Form.ActiveControl := Ok;
    Result := Form.ShowModal = mrOk;
    if Result and Box.Checked then
    begin
      { Saves are not recoverable: ask once more, with "No" the default. }
      if MsgBox('Delete your games, saves and settings in ' + UserDataRoot + '? This cannot be undone.',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DeleteUserData := True;
    end;
  finally
    Form.Free;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usPostUninstall) and DeleteUserData and (not UninstallSilent) then
    RemoveUserData;
end;
