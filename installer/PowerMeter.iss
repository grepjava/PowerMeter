#define AppName "PowerMeter"
#ifndef AppVersion
  #define AppVersion "1.2.0"
#endif
#ifndef BundleRoot
  #define BundleRoot "..\dist\PowerMeter_v" + AppVersion
#endif

[Setup]
AppId={{B4A0D847-2F4E-47D2-B18E-A2FB0D96E4B1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=grepjava
DefaultDirName={autopf}\PowerMeter
DefaultGroupName=PowerMeter
OutputDir=..\dist
OutputBaseFilename=PowerMeterInstaller_v{#AppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation (UI + ACSIL studies)"
Name: "uionly"; Description: "UI only (no ACSIL DLL install)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "ui"; Description: "PowerMeter UI"; Types: full uionly custom; Flags: fixed
Name: "acsil"; Description: "Sierra Chart ACSIL studies (DLLs)"; Types: full custom

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "{#BundleRoot}\PowerMeter.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: ui
Source: "{#BundleRoot}\README.md"; DestDir: "{app}"; Flags: ignoreversion; Components: ui
Source: "{#BundleRoot}\docs\ALGORITHM_NOTES.md"; DestDir: "{app}\docs"; Flags: ignoreversion; Components: ui
Source: "{#BundleRoot}\ACSIL\PowerMeterFeed_64.dll"; DestDir: "{code:GetSCDataDir}"; Flags: ignoreversion; Components: acsil
Source: "{#BundleRoot}\ACSIL\PowerMeterFeedJS_64.dll"; DestDir: "{code:GetSCDataDir}"; Flags: ignoreversion; Components: acsil
Source: "{#BundleRoot}\ACSIL\build_acsil.ps1"; DestDir: "{app}\ACSIL"; Flags: ignoreversion; Components: acsil
Source: "{#BundleRoot}\ACSIL\src\PowerMeterFeed.cpp"; DestDir: "{app}\ACSIL\src"; Flags: ignoreversion; Components: acsil
Source: "{#BundleRoot}\ACSIL\src\PowerMeterFeedJS.cpp"; DestDir: "{app}\ACSIL\src"; Flags: ignoreversion; Components: acsil
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: ui
Source: "..\COMMERCIAL_LICENSE.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: ui

[Icons]
Name: "{group}\PowerMeter"; Filename: "{app}\PowerMeter.exe"
Name: "{group}\README"; Filename: "{app}\README.md"
Name: "{group}\Uninstall PowerMeter"; Filename: "{uninstallexe}"
Name: "{autodesktop}\PowerMeter"; Filename: "{app}\PowerMeter.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PowerMeter.exe"; Description: "Launch PowerMeter"; Flags: nowait postinstall skipifsilent

[Code]
var
  SCPathPage: TInputDirWizardPage;

function GetSCDataDir(Param: string): string;
begin
  Result := AddBackslash(SCPathPage.Values[0]) + 'Data';
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (PageID = SCPathPage.ID) and (not WizardIsComponentSelected('acsil')) then
    Result := True;
end;

procedure InitializeWizard;
begin
  SCPathPage := CreateInputDirPage(
    wpSelectDir,
    'Sierra Chart Folder',
    'Select your Sierra Chart installation folder',
    'PowerMeter will copy ACSIL DLLs into the Data folder under this location.',
    False,
    ''
  );
  SCPathPage.Add('Sierra Chart root folder:');
  if DirExists('C:\SierraChart') then
    SCPathPage.Values[0] := 'C:\SierraChart'
  else
    SCPathPage.Values[0] := ExpandConstant('{autopf}\SierraChart');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  DataPath: string;
begin
  Result := True;
  if CurPageID = SCPathPage.ID then
  begin
    DataPath := AddBackslash(SCPathPage.Values[0]) + 'Data';
    if not DirExists(SCPathPage.Values[0]) then
    begin
      MsgBox('Selected Sierra Chart folder does not exist.', mbError, MB_OK);
      Result := False;
      exit;
    end;

    if not DirExists(DataPath) then
    begin
      if MsgBox('"' + DataPath + '" was not found. Create it?', mbConfirmation, MB_YESNO) = IDYES then
      begin
        if not ForceDirectories(DataPath) then
        begin
          MsgBox('Could not create "' + DataPath + '".', mbError, MB_OK);
          Result := False;
        end;
      end
      else
      begin
        Result := False;
      end;
    end;
  end;
end;
