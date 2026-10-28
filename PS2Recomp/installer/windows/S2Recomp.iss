; Inno Setup script for the Windows installer (Setup compiles it into S2Recomp-<version>-windows-x64-setup.exe).
; Built by package_windows.ps1, which stages the files and passes /DStage=<folder> /DAppVersion=<version>
; /DOutputDir=<folder>.
;
; Installs S2RecompLauncher.exe per user (no admin), because the game writes its memory cards, logs and
; mods next to the program. Ships no game data and no game code. Instead it:
;   1. copies the game files from the player's own disc image into {app}\extracted (s2r_setup.exe);
;   2. makes sure Microsoft's C++ tools are there: an existing Visual Studio 2022 or newer is used, one
;      without the C++ tools gets them added, and with none Setup downloads Visual Studio Build Tools 2022.
;      Each of these only after the player says yes;
;   3. builds ps2EntryRunner.exe from those game files (build_game.ps1), now or in the background.
;
; Command line: /ISO="disc.iso" fills in the disc page; /BUILD=fast|background|none picks the build
; (default fast in the wizard, none for /SILENT installs, which never install Visual Studio).

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef Stage
  #define Stage "..\build\stage\windows"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\dist"
#endif

[Setup]
AppId={{6C1E2B7A-4F0D-4E59-9C38-2B8E6A1D5F42}
AppName=S2Recomp
AppVersion={#AppVersion}
AppVerName=S2Recomp {#AppVersion}
AppPublisher=S2Recomp
AppPublisherURL=https://github.com/OddTrie/S2Recomp
AppSupportURL=https://discord.com/invite/YwAx7DrrFY
AppUpdatesURL=https://github.com/OddTrie/S2Recomp/releases
DefaultDirName={autopf}\S2Recomp
DefaultGroupName=S2Recomp
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
; The program icon and wizard pictures, made by ps2xRuntime/tools/app_icon.py.
SetupIconFile=..\..\ps2xRuntime\assets\s2recomp.ico
WizardImageFile=wizard_1x.png,wizard_2x.png
WizardSmallImageFile=wizard_small_1x.png,wizard_small_2x.png
Compression=lzma2/ultra64
SolidCompression=yes
LZMANumBlockThreads=4
OutputDir={#OutputDir}
OutputBaseFilename=S2Recomp-{#AppVersion}-windows-x64-setup
UninstallDisplayIcon={app}\S2RecompLauncher.exe
UninstallDisplayName=S2Recomp
; About 1.4 GB of game files from the disc and about 5 GB for building the game.
ExtraDiskSpaceRequired=6979321856

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Excludes: "\mods\*,\texpacks\*,\build_game.ps1"; Flags: ignoreversion recursesubdirs
; Dated at install, so the launcher can tell a game built before this update (it needs a rebuild).
Source: "{#Stage}\build_game.ps1"; DestDir: "{app}"; Flags: ignoreversion touch
; Also available before the install, to check the disc image on the disc page.
Source: "{#Stage}\s2r_setup.exe"; Flags: dontcopy
; Mods and texture packs are the player's to change, so a reinstall or update never overwrites them.
Source: "{#Stage}\mods\*"; DestDir: "{app}\mods"; Flags: onlyifdoesntexist recursesubdirs createallsubdirs
Source: "{#Stage}\texpacks\*"; DestDir: "{app}\texpacks"; Flags: onlyifdoesntexist recursesubdirs createallsubdirs

[Dirs]
Name: "{app}\extracted"

[Icons]
Name: "{group}\S2Recomp"; Filename: "{app}\S2RecompLauncher.exe"; WorkingDir: "{app}"
Name: "{group}\Build game"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\build_game.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\S2RecompLauncher.exe"
Name: "{group}\Change game disc"; Filename: "{app}\S2Recomp Disc Setup.exe"; WorkingDir: "{app}"
Name: "{group}\Mods folder"; Filename: "{app}\mods"
Name: "{group}\S2Recomp Discord"; Filename: "https://discord.com/invite/YwAx7DrrFY"
Name: "{group}\{cm:UninstallProgram,S2Recomp}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\S2Recomp"; Filename: "{app}\S2RecompLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\S2RecompLauncher.exe"; Description: "{cm:LaunchProgram,S2Recomp}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\run_log.txt"
Type: files; Name: "{app}\ps2_log.txt"
Type: files; Name: "{app}\imgui.ini"
Type: files; Name: "{app}\prof_samples.txt"
Type: files; Name: "{app}\status_log.txt"
Type: files; Name: "{app}\launcher.ini"
Type: files; Name: "{app}\build_log.txt"
; Made on the player's PC: the game, its build and the code generated from the game files.
Type: files; Name: "{app}\ps2EntryRunner.exe"
Type: filesandordirs; Name: "{app}\build"
Type: filesandordirs; Name: "{app}\source"
Type: filesandordirs; Name: "{app}\vu1rec"
; Empty memory card folders the game makes in its working folder; the saves are in extracted\mc0.
Type: dirifempty; Name: "{app}\mc0"
Type: dirifempty; Name: "{app}\mc1"

[Code]
const
  BuildToolsUrl = 'https://aka.ms/vs/17/release/vs_BuildTools.exe';
  { The C++ tools, CMake/Ninja and clang-cl (the VU1 sources build with it, PS2X_VU_CLANG). }
  VsCppComponents = '-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project Microsoft.VisualStudio.Component.VC.Llvm.Clang';
  VsClang = 'Microsoft.VisualStudio.Component.VC.Llvm.Clang';
  { Build choices on the build page. }
  BuildFast = 0;
  BuildBackground = 1;
  BuildNone = 2;
  { Visual Studio states. }
  VsReady = 0;
  VsWithoutCpp = 1;
  VsMissing = 2;

var
  DiscPage: TInputFileWizardPage;
  BuildPage: TInputOptionWizardPage;
  DownloadPage: TDownloadWizardPage;
  ProgressPage: TOutputProgressWizardPage;
  SetupErrors: String;
  VsState: Integer;
  VsPath, VsName, VsProduct: String;
  InstallVs: Boolean;
  BuildResult: String;

function GameElf(): String;
begin
  Result := ExpandConstant('{app}\extracted\SLES_537.18');
end;

function GameReady(): Boolean;
begin
  Result := FileExists(GameElf());
end;

{ ---- Visual Studio ---- }

{ First line vswhere prints for Params, or '' (also when there is no Visual Studio Installer at all). }
function VsWhere(const Params: String): String;
var
  Exe: String;
  Code: Integer;
  Output: TExecOutput;
begin
  Result := '';
  Exe := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\vswhere.exe');
  if not FileExists(Exe) then
    exit;
  if ExecAndCaptureOutput(Exe, '-nologo -latest -products * -version [17.0, ' + Params, '', SW_HIDE, ewWaitUntilTerminated, Code, Output) and
     (GetArrayLength(Output.StdOut) > 0) then
    Result := Trim(Output.StdOut[0]);
end;

procedure DetectVs();
begin
  VsPath := VsWhere(VsCppComponents + ' -property installationPath');
  if VsPath <> '' then
  begin
    VsState := VsReady;
    VsName := VsWhere(VsCppComponents + ' -property displayName');
    exit;
  end;
  VsPath := VsWhere('-property installationPath');
  if VsPath <> '' then
  begin
    VsState := VsWithoutCpp;
    VsName := VsWhere('-property displayName');
    VsProduct := VsWhere('-property productId');
  end
  else
    VsState := VsMissing;
end;

function VsStatusText(): String;
begin
  case VsState of
    VsReady: Result := 'Microsoft''s C++ tools: found (' + VsName + ').';
    VsWithoutCpp: Result := VsName + ' is installed, but some of the C++ tools S2Recomp needs (such as the Clang compiler) are missing. Setup can add them (it asks first).';
  else
    Result := 'Microsoft''s C++ tools are not installed. Setup can download Visual Studio Build Tools 2022 with them (free; it asks first).';
  end;
end;

{ Asks before anything is installed. False = the player said no. }
function ConfirmVsInstall(): Boolean;
var
  Text: String;
begin
  if VsState = VsWithoutCpp then
    Text := VsName + ' is installed, but some of the C++ tools S2Recomp is built with (such as the Clang compiler) are missing.' + #13#10#13#10 +
      'Add them now with the Visual Studio Installer? It downloads about 3 to 6 GB, and Windows asks for administrator permission.'
  else
    Text := 'Building S2Recomp needs Microsoft''s C++ tools, and they are not installed.' + #13#10#13#10 +
      'Download and install Visual Studio Build Tools 2022 with them now? It is free, downloads about 3 to 6 GB, and Windows asks for administrator permission.';
  Result := MsgBox(Text, mbConfirmation, MB_YESNO) = IDYES;
end;

{ Installs or completes the C++ tools; the Visual Studio Installer shows its own progress. }
procedure RunVsInstall();
var
  Code: Integer;
  Workload: String;
begin
  if VsState = VsWithoutCpp then
  begin
    { The IDEs call the C++ workload NativeDesktop; Build Tools call it VCTools. }
    if VsProduct = 'Microsoft.VisualStudio.Product.BuildTools' then
      Workload := 'Microsoft.VisualStudio.Workload.VCTools'
    else
      Workload := 'Microsoft.VisualStudio.Workload.NativeDesktop';
    ShellExec('runas', ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\setup.exe'),
      'modify --installPath ' + AddQuotes(VsPath) + ' --add ' + Workload + ' --add ' + VsClang + ' --includeRecommended --passive --norestart',
      '', SW_SHOW, ewWaitUntilTerminated, Code);
  end
  else
  begin
    DownloadPage.Clear;
    DownloadPage.Add(BuildToolsUrl, 'vs_BuildTools.exe', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        MsgBox('Downloading Visual Studio Build Tools failed: ' + GetExceptionMessage, mbError, MB_OK);
        exit;
      end;
    finally
      DownloadPage.Hide;
    end;
    ShellExec('runas', ExpandConstant('{tmp}\vs_BuildTools.exe'),
      '--add Microsoft.VisualStudio.Workload.VCTools --add ' + VsClang + ' --includeRecommended --passive --wait --norestart',
      '', SW_SHOW, ewWaitUntilTerminated, Code);
  end;
  Log(Format('Visual Studio Installer exit code %d', [Code]));
  DetectVs();
end;

{ ---- Wizard ---- }

{ Setup downloads Visual Studio's C++ tools and may build the game for an hour or more, and a PC that
  sleeps meanwhile stops both. ES_CONTINUOUS | ES_SYSTEM_REQUIRED; Windows drops it when Setup exits. }
function SetThreadExecutionState(esFlags: Cardinal): Cardinal;
  external 'SetThreadExecutionState@kernel32.dll stdcall';

procedure DiscordClick(Sender: TObject);
var
  Code: Integer;
begin
  ShellExec('open', 'https://discord.com/invite/YwAx7DrrFY', '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

procedure InitializeWizard();
var
  Choice: String;
  Discord: TNewStaticText;
begin
  { A link to the community Discord, at the bottom left of every page. }
  Discord := TNewStaticText.Create(WizardForm);
  Discord.Parent := WizardForm;
  Discord.Caption := 'Join the S2Recomp Discord';
  Discord.Cursor := crHand;
  Discord.Font.Color := clHotLight;
  Discord.Font.Style := [fsUnderline];
  Discord.OnClick := @DiscordClick;
  Discord.Left := ScaleX(16);
  Discord.Top := WizardForm.CancelButton.Top + (WizardForm.CancelButton.Height - Discord.Height) div 2;
  SetThreadExecutionState($80000001);
  DetectVs();
  DiscPage := CreateInputFilePage(wpSelectDir,
    'Your game disc',
    'S2Recomp needs your own copy of The Sims 2 for PlayStation 2.',
    'Choose a disc image (.iso) of The Sims 2, PAL / Europe release (SLES-53718). ' +
    'Setup checks it and copies the game files (about 1.4 GB) from it; no game files come with S2Recomp.' + #13#10#13#10 +
    'You can also leave this empty and do it later with Change game disc in the Start menu.');
  DiscPage.Add('Disc image:', 'Disc images (*.iso)|*.iso|All files|*.*', '.iso');
  { /ISO="path" fills the page in, also for /SILENT and /VERYSILENT installs. }
  DiscPage.Values[0] := ExpandConstant('{param:ISO|}');

  BuildPage := CreateInputOptionPage(DiscPage.ID,
    'Build the game',
    'S2Recomp builds the game on your PC, from your own disc.',
    'No game code comes with S2Recomp: the recompiler turns the game on your disc into C++, and Microsoft''s ' +
    'C++ tools compile it. That takes about 30 to 90 minutes and about 5 GB of disk space.' + #13#10#13#10 +
    VsStatusText(),
    True, False);
  BuildPage.Add('Fast: use the whole PC (you can watch the progress here)');
  BuildPage.Add('In the background: the PC stays usable, Setup finishes now');
  BuildPage.Add('Not now: build later with Build game in the Start menu');
  Choice := Lowercase(ExpandConstant('{param:BUILD|}'));
  if Choice = 'background' then
    BuildPage.SelectedValueIndex := BuildBackground
  else if (Choice = 'none') or ((Choice = '') and WizardSilent()) then
    BuildPage.SelectedValueIndex := BuildNone
  else
    BuildPage.SelectedValueIndex := BuildFast;

  DownloadPage := CreateDownloadPage('Downloading Visual Studio Build Tools', 'Getting Microsoft''s installer for the C++ tools.', nil);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  { An update keeps the game files that are already there. }
  Result := (PageID = DiscPage.ID) and GameReady();
end;

procedure CollectOutput(const S: String; const Error, FirstLine: Boolean);
var
  Pct: Integer;
begin
  if Error then
  begin
    if Trim(S) <> '' then
      SetupErrors := SetupErrors + S + #13#10;
  end
  else if Copy(S, 1, 9) = 'PROGRESS ' then
  begin
    Pct := StrToIntDef(Trim(Copy(S, 10, 4)), -1);
    if (ProgressPage <> nil) and (Pct >= 0) then
      ProgressPage.SetProgress(Pct, 100);
  end
  else if (Copy(S, 1, 5) = 'STEP ') and (ProgressPage <> nil) then
    ProgressPage.SetText(Copy(S, 6, Length(S)), '');
  Log('> ' + S);
end;

{ Runs a tool with its output fed to CollectOutput and returns its exit code. }
function RunTool(const Exe, Params: String): Integer;
var
  Code: Integer;
begin
  SetupErrors := '';
  if not ExecAndLogOutput(Exe, Params, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code, @CollectOutput) then
    Code := -1;
  Result := Code;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Iso: String;
  Code: Integer;
begin
  Result := True;
  if CurPageID = DiscPage.ID then
  begin
    Iso := Trim(DiscPage.Values[0]);
    if Iso = '' then
      exit;
    if not FileExists(Iso) then
    begin
      MsgBox('This file does not exist:' + #13#10 + Iso, mbError, MB_OK);
      Result := False;
      exit;
    end;
    ExtractTemporaryFile('s2r_setup.exe');
    WizardForm.NextButton.Enabled := False;
    try
      SetupErrors := '';
      if not ExecAndLogOutput(ExpandConstant('{tmp}\s2r_setup.exe'), '--check ' + AddQuotes(Iso), '', SW_HIDE,
                              ewWaitUntilTerminated, Code, @CollectOutput) or (Code <> 0) then
      begin
        MsgBox('This disc image cannot be used:' + #13#10#13#10 + SetupErrors, mbError, MB_OK);
        Result := False;
      end;
    finally
      WizardForm.NextButton.Enabled := True;
    end;
  end
  else if (CurPageID = BuildPage.ID) and (BuildPage.SelectedValueIndex <> BuildNone) and (VsState <> VsReady) then
  begin
    InstallVs := ConfirmVsInstall();
    if not InstallVs then
      BuildPage.SelectedValueIndex := BuildNone;
  end;
end;

{ The C++ tools go in before the files, so a failed or cancelled Visual Studio install is known in time. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if InstallVs then
    RunVsInstall();
end;

procedure CopyGameFiles();
var
  Iso: String;
begin
  Iso := Trim(DiscPage.Values[0]);
  if (Iso = '') or GameReady() then
    exit;
  ProgressPage := CreateOutputProgressPage('Copying the game files', 'Copying the game files from your disc image. This takes a minute or two.');
  ProgressPage.SetText(Iso, '');
  ProgressPage.SetProgress(0, 100);
  ProgressPage.Show;
  try
    if RunTool(ExpandConstant('{app}\s2r_setup.exe'), AddQuotes(Iso) + ' ' + AddQuotes(ExpandConstant('{app}\extracted'))) <> 0 then
      SuppressibleMsgBox('The game files could not be copied:' + #13#10#13#10 + SetupErrors + #13#10 +
        'Use Change game disc in the Start menu to try again.', mbError, MB_OK, IDOK);
  finally
    ProgressPage.Hide;
    ProgressPage := nil;
  end;
end;

procedure BuildGame();
var
  PowerShell, Script: String;
  Code: Integer;
begin
  BuildResult := '';
  if BuildPage.SelectedValueIndex = BuildNone then
    exit;
  if VsState <> VsReady then
  begin
    BuildResult := 'Microsoft''s C++ tools are not ready yet, so the game was not built. Run Setup again, or install them and use Build game in the Start menu.';
    exit;
  end;
  if not GameReady() then
  begin
    BuildResult := 'The game files are not set up yet, so the game was not built. Use Change game disc, then Build game, in the Start menu.';
    exit;
  end;
  PowerShell := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  Script := '-NoProfile -ExecutionPolicy Bypass -File ' + AddQuotes(ExpandConstant('{app}\build_game.ps1'));
  if BuildPage.SelectedValueIndex = BuildBackground then
  begin
    { Its own minimized window; it stays open if the build fails, so the error can be read. }
    Exec(ExpandConstant('{cmd}'), '/c "' + AddQuotes(PowerShell) + ' ' + Script + ' -Pace background || pause"',
         ExpandConstant('{app}'), SW_SHOWMINNOACTIVE, ewNoWait, Code);
    BuildResult := 'The game is being built in the background (a minimized window in the taskbar). You can play once it has finished.';
    exit;
  end;
  ProgressPage := CreateOutputProgressPage('Building the game', 'Building the game from your disc. This takes about 30 to 90 minutes.');
  ProgressPage.SetText('Starting', '');
  ProgressPage.SetProgress(0, 100);
  ProgressPage.Show;
  try
    if RunTool(PowerShell, Script + ' -Pace fast') <> 0 then
      BuildResult := 'Building the game failed:' + #13#10 + SetupErrors + #13#10 +
        'The details are in ' + ExpandConstant('{app}\build_log.txt') + '. Build game in the Start menu tries again and continues where it stopped.';
  finally
    ProgressPage.Hide;
    ProgressPage := nil;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep <> ssPostInstall then
    exit;
  CopyGameFiles();
  BuildGame();
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (BuildResult <> '') then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 + BuildResult
  else if (CurPageID = wpFinished) and not FileExists(ExpandConstant('{app}\ps2EntryRunner.exe')) then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      'The game is not built yet: use Build game in the Start menu.';
end;

{ The disc files can be copied again at any time, so the uninstaller removes them. The memory cards
  (mc0, mc1) hold the player's saved games and are only removed when the player says so. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  GameDir, Name: String;
  Rec: TFindRec;
begin
  if CurUninstallStep <> usUninstall then
    exit;
  GameDir := ExpandConstant('{app}\extracted');
  if FindFirst(GameDir + '\*', Rec) then
  try
    repeat
      Name := Rec.Name;
      if (Name = '.') or (Name = '..') or (CompareText(Name, 'mc0') = 0) or (CompareText(Name, 'mc1') = 0) then
        continue;
      if (Rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        DelTree(GameDir + '\' + Name, True, True, True)
      else
        DeleteFile(GameDir + '\' + Name);
    until not FindNext(Rec);
  finally
    FindClose(Rec);
  end;
  if (DirExists(GameDir + '\mc0') or DirExists(GameDir + '\mc1')) and
     (UninstallSilent() or
      (MsgBox('Also delete your saved games (memory cards in ' + GameDir + ')?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES)) then
    exit;
  DelTree(GameDir, True, True, True);
end;
