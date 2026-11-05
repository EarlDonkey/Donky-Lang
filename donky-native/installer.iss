; ============================================================================
;  Donky Language installer  (Inno Setup 6.3 or newer)
;
;  Build:   .\build.ps1                      (stages dist\ and calls ISCC for you)
;  Manual:  iscc /DMySourceDir=dist installer.iss
;
;  Layout assumption: this file lives at the REPO ROOT, next to build.ps1.
;  All [Files] sources come from the staging tree built by build.ps1:
;
;      dist\bin\donky.exe        <- target\release\donky-native.exe (renamed)
;      dist\lib\runtime.o        <- gcc -O2 -c runtime_template.c
;      dist\lib\*.donk           <- lib\*.donk (recursive)
;      dist\examples\...         <- examples\* (build.ps1 skips it if > 50 MB)
;
;  If you move this file into a subfolder (e.g. installer\), add
;  `SourceDir=..` under [Setup]; relative paths below are resolved from the
;  folder that contains the .iss unless SourceDir says otherwise.
;
;  NOTE ON THE BINARY NAME: Cargo.toml names the package "donky-native", so
;  cargo produces target\release\donky-native.exe. build.ps1 renames it to
;  donky.exe while staging. If you rename the Cargo package, only build.ps1
;  needs to change - not this file.
; ============================================================================

#ifndef MyAppVersion
  #define MyAppVersion "2.3.0"
#endif
#ifndef MyAppShortVersion
  #define MyAppShortVersion "2.3"
#endif
; Staging directory (relative to this file). build.ps1 passes /DMySourceDir=dist.
#ifndef MySourceDir
  #define MySourceDir "dist"
#endif

#define MyAppName      "Donky Language"
#define MyAppPublisher "Donky Language Project"

[Setup]
; AppId identifies this product across versions (upgrade-in-place + uninstall entry).
; NEVER change it once a release has shipped. The doubled "{{" is Inno's escape for a literal "{".
AppId={{B7E2C4A1-5D3F-4E8A-9C61-2F0D8A7B3E54}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}

DefaultDirName={autopf}\Donky
DefaultGroupName=Donky
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\bin\donky.exe

; --- Privileges -------------------------------------------------------------
; admin: Donky is a system-wide developer tool, {autopf} (Program Files) needs
; elevation anyway, and an admin install lets us put {app}\bin on the SYSTEM
; PATH so every account (and services / CI agents) can run `donky`.
; "commandline" additionally lets scripted installs opt into a per-user install
; with `DonkySetup.exe /CURRENTUSER` (then {autopf} becomes %LOCALAPPDATA%\Programs
; and PATH goes to HKCU). The [Code] section picks the right registry hive
; automatically via IsAdminInstallMode.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline

; 64-bit only (the Rust binary is built for x64). "x64compatible" needs Inno 6.3+;
; on 6.0-6.2 use "x64" instead.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Broadcasts WM_SETTINGCHANGE after install/uninstall so newly opened terminals
; see the PATH change without a reboot.
ChangesEnvironment=yes

; --- Output -----------------------------------------------------------------
OutputDir={#MySourceDir}
OutputBaseFilename=DonkySetup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

; --- Deliberately omitted for v1.0 ------------------------------------------
; SetupIconFile=...   no icon asset exists yet (the installer uses Inno's default icon).
; LicenseFile=...     no LICENSE file exists yet. Add `LicenseFile=LICENSE` once one does.

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Checked by default. Declining it still installs everything, just without PATH changes.
Name: "addtopath"; Description: "Add Donky to the &PATH environment variable (recommended)"; GroupDescription: "Environment:"

[Files]
; Compiler binary: renamed on the way in (see NOTE ON THE BINARY NAME above).
Source: "{#MySourceDir}\bin\donky.exe";      DestDir: "{app}\bin";      Flags: ignoreversion
; Pre-compiled C runtime. donky.exe finds it at {exe_dir}\..\lib\runtime.o.
Source: "{#MySourceDir}\lib\runtime.o";      DestDir: "{app}\lib";      Flags: ignoreversion
; Standard library (os.donk, math.donk, micrograd.donk, ...). recursesubdirs keeps any lib\ subfolders.
Source: "{#MySourceDir}\lib\*.donk";         DestDir: "{app}\lib";      Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
; Examples are optional: build.ps1 only stages them when the folder is under 50 MB.
Source: "{#MySourceDir}\examples\*";         DestDir: "{app}\examples"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
; %USERPROFILE% is expanded by cmd.exe, not by Inno. The leading `set PATH=...` guarantees
; `donky` works in this shell even before Explorer has picked up the new PATH
; (a small addition to the requested command line; delete it if you prefer the literal version).
; cmd /k "A && B && C": the outer quotes are stripped by cmd because the string contains `&&`.
Name: "{group}\Donky Command Prompt"; Filename: "{cmd}"; Parameters: "/k ""set PATH=%PATH%;{app}\bin&& cd /d %USERPROFILE% && echo Donky v{#MyAppShortVersion} ready. Type 'donky --help' to start."""; Comment: "Command prompt with Donky on the PATH"
Name: "{group}\Uninstall Donky"; Filename: "{uninstallexe}"

[Messages]
; Text of the final wizard page (requested post-install message).
FinishedLabel=Donky is installed.%n%nOpen a new terminal and type donky --help.
FinishedLabelNoIcons=Donky is installed.%n%nOpen a new terminal and type donky --help.

; Uninstall: Inno removes only the files it installed. Nothing under the user's
; project folders is touched, and files a user adds under {app} are left in place.

[Code]
// ---------------------------------------------------------------------------
//  PATH handling
//
//  Why this is in [Code] and not in [Registry]:
//  A [Registry] line such as  ValueData: "{olddata};{app}\bin"  appends fine on
//  install, but on uninstall Inno can only delete the WHOLE value
//  (uninsdeletevalue) - that would wipe the user's entire PATH - or leave the
//  entry behind, so PATH would grow with every install/uninstall cycle.
//  So we edit the semicolon-separated list ourselves:
//    install   -> append {app}\bin if (and only if) it is not already present
//    uninstall -> remove exactly that one entry, leave every other entry intact
//  The value is written back as REG_EXPAND_SZ so existing %VAR% entries keep working.
// ---------------------------------------------------------------------------

function EnvRoot(): Integer;
begin
  if IsAdminInstallMode then
    Result := HKEY_LOCAL_MACHINE
  else
    Result := HKEY_CURRENT_USER;
end;

function EnvSubKey(): String;
begin
  if IsAdminInstallMode then
    Result := 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
  else
    Result := 'Environment';
end;

// Case-insensitive, trailing-backslash-insensitive form of a directory.
function NormalizeDir(const S: String): String;
begin
  Result := Lowercase(RemoveBackslashUnlessRoot(S));
end;

function PathContainsDir(const PathValue, Dir: String): Boolean;
var
  Rest, Part, Target: String;
  P: Integer;
begin
  Result := False;
  Target := NormalizeDir(Dir);
  Rest := PathValue;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P = 0 then
    begin
      Part := Rest;
      Rest := '';
    end
    else
    begin
      Part := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end;
    if (Part <> '') and (NormalizeDir(Part) = Target) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function RemoveDirFromPath(const PathValue, Dir: String): String;
var
  Rest, Part, Target: String;
  P: Integer;
begin
  Result := '';
  Target := NormalizeDir(Dir);
  Rest := PathValue;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P = 0 then
    begin
      Part := Rest;
      Rest := '';
    end
    else
    begin
      Part := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end;
    if (Part <> '') and (NormalizeDir(Part) <> Target) then
    begin
      if Result <> '' then
        Result := Result + ';';
      Result := Result + Part;
    end;
  end;
end;

procedure AddToPath(const Dir: String);
var
  OldPath: String;
begin
  if not RegQueryStringValue(EnvRoot(), EnvSubKey(), 'Path', OldPath) then
    OldPath := '';
  if PathContainsDir(OldPath, Dir) then
    Exit;
  if (OldPath <> '') and (OldPath[Length(OldPath)] <> ';') then
    OldPath := OldPath + ';';
  RegWriteExpandStringValue(EnvRoot(), EnvSubKey(), 'Path', OldPath + Dir);
end;

procedure RemoveFromPath(const Dir: String);
var
  OldPath, NewPath: String;
begin
  if not RegQueryStringValue(EnvRoot(), EnvSubKey(), 'Path', OldPath) then
    Exit;
  if not PathContainsDir(OldPath, Dir) then
    Exit;
  NewPath := RemoveDirFromPath(OldPath, Dir);
  RegWriteExpandStringValue(EnvRoot(), EnvSubKey(), 'Path', NewPath);
end;

// ---------------------------------------------------------------------------
//  GCC is required at link time (see build.ps1 / main.rs notes). Warn, don't block.
// ---------------------------------------------------------------------------
function GccOnPath(): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C where gcc >nul 2>&1', '', SW_HIDE,
                 ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('addtopath') then
      AddToPath(ExpandConstant('{app}\bin'));

    if not GccOnPath() then
      SuppressibleMsgBox(
        'Donky was installed, but GCC (MinGW-w64) was not found on your PATH.' + #13#10 + #13#10 +
        'donky build-exe needs gcc to link executables. Install a MinGW-w64 toolchain ' +
        '(for example MSYS2 or WinLibs), make sure gcc.exe is on PATH, then open a new terminal.',
        mbInformation, MB_OK, IDOK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath(ExpandConstant('{app}\bin'));
end;
