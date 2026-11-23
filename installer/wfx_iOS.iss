; Inno Setup script for wfx_iOS (Total Commander iOS file-system plugin).
; Built by GitHub Actions: ISCC installer\wfx_iOS.iss  (expects the two plugin files in installer\bin)

; The version is passed by the build:  ISCC /DAppVer=2.1 installer\wfx_iOS.iss
#ifndef AppVer
  #define AppVer "0.0"
#endif

[Setup]
AppId={{6C0C7C5E-3B0B-4B55-9C7B-7A1F0E5D2A10}
AppName=wfx_iOS (Total Commander iOS plugin)
AppVersion={#AppVer}
AppPublisher=wfx_iOS - update of wfx_iOS 1.3 by Domo
AppPublisherURL=https://github.com/nonamegsm/wfx_iOS_2.0
DefaultDirName={code:PluginDir}
DirExistsWarning=no
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
OutputDir=out
OutputBaseFilename=wfx_iOS_{#AppVer}_setup
Compression=lzma2
SolidCompression=yes
LicenseFile=..\LICENSE
InfoBeforeFile=..\readme.txt
UninstallDisplayName=wfx_iOS {#AppVer} (Total Commander plugin)

[Tasks]
Name: "amds"; Description: "Download and install Apple Mobile Device Support from Apple (required; uses winget)"; Check: NeedsAmds

[Files]
Source: "bin\wfx_iOS.wfx";   DestDir: "{app}"; Flags: ignoreversion
Source: "bin\wfx_iOS.wfx64"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\readme.txt";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";        DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\pluginst.inf";   DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{cmd}"; Parameters: "/c winget install -e --id Apple.AppleMobileDeviceSupport --accept-package-agreements --accept-source-agreements"; \
    StatusMsg: "Installing Apple Mobile Device Support (downloading from Apple)..."; Tasks: amds; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "start ""Apple Mobile Device Service"""; Flags: runhidden waituntilterminated

[Code]
const
  AmdsKey = 'SYSTEM\CurrentControlSet\Services\Apple Mobile Device Service';

function TcDir(): String;
begin
  if not RegQueryStringValue(HKCU, 'Software\Ghisler\Total Commander', 'InstallDir', Result) then
    if not RegQueryStringValue(HKLM, 'Software\Ghisler\Total Commander', 'InstallDir', Result) then
      Result := ExpandConstant('{commonpf}\totalcmd');
  Result := RemoveBackslashUnlessRoot(Result);
end;

function ExpandTc(S: String): String;
begin
  StringChangeEx(S, '%COMMANDER_PATH%', TcDir(), True);
  StringChangeEx(S, '%APPDATA%', ExpandConstant('{userappdata}'), True);
  StringChangeEx(S, '%USERPROFILE%', GetEnv('USERPROFILE'), True);
  StringChangeEx(S, '%LOCALAPPDATA%', ExpandConstant('{localappdata}'), True);
  if (Length(S) > 1) and (S[1] = '.') and (S[2] = '\') then
    S := TcDir() + Copy(S, 2, Length(S));
  Result := S;
end;

function TcIni(): String;
begin
  if not RegQueryStringValue(HKCU, 'Software\Ghisler\Total Commander', 'IniFileName', Result) then
    if not RegQueryStringValue(HKLM, 'Software\Ghisler\Total Commander', 'IniFileName', Result) then
      Result := '%APPDATA%\GHISLER\wincmd.ini';
  Result := ExpandTc(Result);
  if ExtractFilePath(Result) = '' then
    Result := ExpandConstant('{win}\') + Result;
end;

{ Name of an existing [FileSystemPlugins] entry that points at wfx_iOS.wfx, or '' }
function ExistingKey(): String;
var
  Lines: TArrayOfString;
  I, P: Integer;
  InSection: Boolean;
  L: String;
begin
  Result := '';
  if not LoadStringsFromFile(TcIni(), Lines) then Exit;
  InSection := False;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    L := Trim(Lines[I]);
    if (Length(L) > 0) and (L[1] = '[') then
      InSection := CompareText(L, '[FileSystemPlugins]') = 0
    else if InSection then
    begin
      P := Pos('=', L);
      if (P > 1) and (Pos('WFX_IOS.WFX', Uppercase(L)) > P) then
      begin
        Result := Copy(L, 1, P - 1);
        Exit;
      end;
    end;
  end;
end;

{ Install over an existing wfx_iOS if Total Commander already has one, else into its plugins folder }
function PluginDir(Param: String): String;
var
  Key, Path: String;
begin
  Key := ExistingKey();
  if Key <> '' then
  begin
    Path := ExpandTc(GetIniString('FileSystemPlugins', Key, '', TcIni()));
    if Path <> '' then
    begin
      Result := ExtractFileDir(Path);
      Exit;
    end;
  end;
  Result := TcDir() + '\plugins\wfx\wfx_iOS';
end;

function NeedsAmds(): Boolean;
begin
  Result := not RegKeyExists(HKLM, AmdsKey);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  while FindWindowByClassName('TTOTAL_CMD') <> 0 do
    if MsgBox('Total Commander is running and keeps the plugin loaded.' + #13#10 +
              'Close Total Commander, then click OK.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Key: String;
begin
  if CurStep = ssPostInstall then
  begin
    Key := ExistingKey();
    if Key = '' then Key := 'iOS';
    if not SetIniString('FileSystemPlugins', Key, ExpandConstant('{app}\wfx_iOS.wfx'), TcIni()) then
      MsgBox('Could not register the plugin in ' + TcIni() + '.' + #13#10 +
             'In Total Commander use Configuration > Options > Plugins > File system plugins > Add, and pick' + #13#10 +
             ExpandConstant('{app}\wfx_iOS.wfx'), mbError, MB_OK);
    if not RegKeyExists(HKLM, AmdsKey) then
      MsgBox('Apple Mobile Device Support is not installed, so the plugin cannot find devices yet.' + #13#10#13#10 +
             'Install it with this command (PowerShell or cmd):' + #13#10 +
             'winget install -e --id Apple.AppleMobileDeviceSupport' + #13#10#13#10 +
             'or install iTunes from apple.com.', mbInformation, MB_OK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Key: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Key := ExistingKey();
    if Key <> '' then DeleteIniEntry('FileSystemPlugins', Key, TcIni());
  end;
end;
