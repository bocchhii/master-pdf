; Inno Setup script - builds MasterPDF-Windows-Setup.exe from dist\ (packaging/build_windows.sh makes it)
#define MyAppName "Master PDF"
#ifndef MyAppVersion
  #define MyAppVersion "2.0.0"
#endif
#define MyAppExe "MasterPDF.exe"

[Setup]
; the app's own id - the Python version's too: every version has the same one, so installing a new version upgrades
; the one that's there instead of adding a second copy
AppId={{20336EE8-615D-4A02-8FBC-82E3144648B5}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; installs just for the current user by default, so no admin password is needed
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=installer_output
OutputBaseFilename=MasterPDF-Windows-Setup
SetupIconFile=resources\icons\app.ico
UninstallDisplayIcon={app}\{#MyAppExe}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; in-app updates run this silently: close the running app first, but don't let Windows
; restart it - the [Run] entry below starts the new version (just once)
CloseApplications=yes
RestartApplications=no
; a log of every install in %TEMP% ("Setup Log <date> #<n>.txt"), for when something goes wrong
SetupLogging=yes

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; an update: what the last version had beside the program (Qt's plugins...) cleared first, so nothing old is left
Type: filesandordirs; Name: "{app}\fonts"
Type: filesandordirs; Name: "{app}\tessdata"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\iconengines"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
; after a silent install (the in-app update), start the new version straight away
Filename: "{app}\{#MyAppExe}"; Flags: nowait; Check: WizardSilent

[Code]
{ The in-app update runs this installer silently once the app has closed (a small helper waits for it). If the app
  were still running, its files couldn't be replaced and a silent install would quietly fail. So before any file is
  touched: wait for Master PDF to close by itself, and end whatever is still running after that - it's a leftover of
  the app that asked for this update. }

{ True while the installed .exe is still open (the app running): the very thing that stops it from being replaced.
  Tested directly - can it be opened exclusively for writing? - rather than by looking for the process by name. }
function AppFileInUse(): Boolean;
var
  FileName: String;
  Stream: TFileStream;
begin
  Result := False;
  FileName := ExpandConstant('{app}\{#MyAppExe}');
  if not FileExists(FileName) then
    Exit;
  try
    Stream := TFileStream.Create(FileName, fmOpenReadWrite or fmShareExclusive);
    Stream.Free;
  except
    Result := True;
  end;
end;

function SetEnvironmentVariable(Name, Value: String): Boolean;
  external 'SetEnvironmentVariableW@kernel32.dll stdcall';

{ An update from the Python version (a PyInstaller one-file app): it passed its "your files are unpacked in my temp
  folder" variables down to this installer - harmless to this version, cleared all the same. }
function InitializeSetup(): Boolean;
begin
  SetEnvironmentVariable('PYINSTALLER_RESET_ENVIRONMENT', '1');
  Result := True;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  I, ResultCode: Integer;
begin
  Result := '';
  if not WizardSilent then
    Exit;  { someone running the installer themselves: the usual "close the app" question }
  for I := 1 to 40 do  { up to 10 s for the app to close by itself }
  begin
    if not AppFileInUse() then
    begin
      Log('Master PDF has closed; installing.');
      Exit;
    end;
    Sleep(250);
  end;
  Log('Master PDF is still running after 10 s: ending it, so it can be replaced.');
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExe}', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  for I := 1 to 20 do  { and up to 5 s more for Windows to let go of the file }
  begin
    if not AppFileInUse() then
      Exit;
    Sleep(250);
  end;
end;
