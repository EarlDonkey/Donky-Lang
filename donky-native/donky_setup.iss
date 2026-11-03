[Setup]
AppName=Donky
AppVersion=1.2
AppPublisher=Donky
DefaultDirName={autopf}\Donky
DefaultGroupName=Donky
OutputDir=dist
OutputBaseFilename=donky_setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
ChangesEnvironment=yes
UninstallDisplayName=Donky

[Files]
Source: "dist\bin\donky.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "dist\lib\runtime.o"; DestDir: "{app}\lib"; Flags: ignoreversion
Source: "dist\toolchain\*"; DestDir: "{app}\toolchain"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; \
    Check: NeedsAddPath(ExpandConstant('{app}\bin'))

[Code]
const EnvKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';

function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvKey, 'Path', OrigPath) then
  begin
    Result := True;
    Exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Path, Target: string;
  P: Integer;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    if RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvKey, 'Path', Path) then
    begin
      Target := ';' + ExpandConstant('{app}\bin');
      P := Pos(Uppercase(Target), Uppercase(Path));
      if P > 0 then
      begin
        Delete(Path, P, Length(Target));
        RegWriteExpandStringValue(HKEY_LOCAL_MACHINE, EnvKey, 'Path', Path);
      end;
    end;
  end;
end;