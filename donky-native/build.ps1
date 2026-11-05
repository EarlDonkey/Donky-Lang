# Usage (from repo root, on Windows):
#   .\build.ps1
# Produces: dist/DonkySetup.exe
#
# Options:
#   .\build.ps1 -Version 2.3.1      version stamped into the installer (default 2.3.0)
#   .\build.ps1 -SkipSmokeTest      skip the staged self-test (compiles + runs a tiny program)
#   .\build.ps1 -KeepRuntimeObj     keep .\runtime.o in the repo root (see note in step 8)
#
# Requires on PATH: cargo (Rust), gcc (MinGW-w64), and Inno Setup 6 (iscc.exe is also
# searched for in the standard install folders).
#
# Exit codes: 0 ok | 2 preflight | 3 cargo | 4 runtime compile | 5 staging | 6 self-test | 7 installer

[CmdletBinding()]
param(
    [string]$Version = '2.3.0',
    [switch]$SkipSmokeTest,
    [switch]$KeepRuntimeObj
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$global:LASTEXITCODE = 0

# Locate the repo root = the folder that contains Cargo.toml. $PSScriptRoot is tried first, then the
# script's own path, then the current directory; each candidate may also be one level above the
# project (a "donky-native" subfolder). The first one that really has a Cargo.toml wins.
$ScriptPath = $MyInvocation.MyCommand.Path
$RootCandidates = New-Object 'System.Collections.Generic.List[string]'
if ($PSScriptRoot) { [void]$RootCandidates.Add($PSScriptRoot) }
if ($ScriptPath)   { [void]$RootCandidates.Add((Split-Path -Parent $ScriptPath)) }
[void]$RootCandidates.Add((Get-Location).Path)
$Root = $null
foreach ($cand in $RootCandidates) {
    if (Test-Path -LiteralPath (Join-Path $cand 'Cargo.toml'))                  { $Root = $cand; break }
    if (Test-Path -LiteralPath (Join-Path $cand 'donky-native\Cargo.toml'))     { $Root = Join-Path $cand 'donky-native'; break }
}
if (-not $Root) {
    Write-Host 'ERROR: could not find Cargo.toml. Looked in:' -ForegroundColor Red
    $RootCandidates | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 2
}
Write-Host "Repo root: $Root"
$ParentDir    = Split-Path -Parent $Root
$Dist         = Join-Path $Root 'dist'
$RuntimeObj   = Join-Path $Root 'runtime.o'
$CargoExe     = Join-Path $Root 'target\release\donky-native.exe'
$IssFile      = Join-Path $Root 'installer.iss'
$SetupExe     = Join-Path $Dist 'DonkySetup.exe'
$ExamplesMaxB = 50MB

Set-Location -LiteralPath $Root

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
function Fail {
    param([string]$Message, [int]$Code = 1)
    Write-Host ""
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit $Code
}

function Step {
    param([string]$Title)
    Write-Host ""
    Write-Host "==> $Title" -ForegroundColor Cyan
}

# Runs a native command, streams its output, and returns the exit code plus the captured
# stdout/stderr lines. ErrorActionPreference is relaxed inside because Windows PowerShell 5.1
# turns every stderr line of a native command into a terminating error when it is 'Stop'
# (and cargo writes its normal progress output to stderr).
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments = @())
    $out = New-Object 'System.Collections.Generic.List[string]'
    $err = New-Object 'System.Collections.Generic.List[string]'
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = -1          # stays -1 if the process never started => counted as failure
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) {
                [void]$err.Add($_.ToString())
                Write-Host $_.ToString() -ForegroundColor DarkYellow
            } else {
                [void]$out.Add([string]$_)
                Write-Host $_
            }
        }
        $code = $global:LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    return [pscustomobject]@{ ExitCode = $code; Stdout = $out; Stderr = $err }
}

function Show-Failure {
    param([string]$What, $Result)
    Write-Host ""
    Write-Host "ERROR: $What failed (exit code $($Result.ExitCode))." -ForegroundColor Red
    $lines = $Result.Stderr
    $label = 'stderr'
    if ($lines.Count -eq 0) { $lines = $Result.Stdout; $label = 'output' }   # e.g. iscc reports on stdout
    Write-Host "First 30 lines of ${label}:" -ForegroundColor Red
    $lines | Select-Object -First 30 | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
}

function Invoke-Required {
    param([string]$What, [int]$ExitOnFail, [string]$Exe, [string[]]$Arguments = @())
    Step $What
    $r = Invoke-Native -Exe $Exe -Arguments $Arguments
    if ($r.ExitCode -ne 0) { Show-Failure -What $What -Result $r; exit $ExitOnFail }
    return $r
}

function Find-Tool {
    param([string]$Name)
    $c = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    return $null
}

# ---------------------------------------------------------------------------
# 1. Preflight
# ---------------------------------------------------------------------------
Step 'Preflight checks'

$cargo = Find-Tool 'cargo'
if (-not $cargo) { Fail 'cargo not found on PATH. Install the Rust toolchain from https://rustup.rs and reopen this terminal.' 2 }

$gcc = Find-Tool 'gcc'
if (-not $gcc) { Fail 'gcc not found on PATH. Install MinGW-w64 (e.g. MSYS2 or WinLibs) and make sure gcc.exe is on PATH.' 2 }

$iscc = Find-Tool 'iscc'
if (-not $iscc) {
    $candidates = @(
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        'C:\Program Files\Inno Setup 6\ISCC.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    )
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { $iscc = $c; break } }
}
if (-not $iscc) {
    Fail ("Inno Setup compiler (ISCC.exe) not found on PATH or in:`n  C:\Program Files (x86)\Inno Setup 6\`n  C:\Program Files\Inno Setup 6\`n" +
          "Install Inno Setup 6.3+ from https://jrsoftware.org/isinfo.php") 2
}

# Runtime source: main.rs embeds src\runtime_template.c via include_str!, so that is the primary location.
$RuntimeSrc = $null
foreach ($c in @((Join-Path $Root 'src\runtime_template.c'), (Join-Path $Root 'runtime_template.c'))) {
    if (Test-Path -LiteralPath $c) { $RuntimeSrc = $c; break }
}
if (-not $RuntimeSrc)             { Fail 'runtime_template.c not found (looked in src\ and the repo root).' 2 }
if (-not (Test-Path $IssFile))    { Fail "installer.iss not found at $IssFile" 2 }
if (-not (Test-Path (Join-Path $Root 'Cargo.toml'))) { Fail 'Cargo.toml not found - run this script from the repo root.' 2 }
if (-not (Test-Path (Join-Path $ParentDir 'lib')))   { Fail 'lib\ directory not found (expected lib\*.donk).' 2 }

$gccMachine = (& $gcc -dumpmachine 2>$null | Select-Object -First 1)
Write-Host "  cargo : $cargo"
Write-Host "  gcc   : $gcc  ($gccMachine)"
Write-Host "  iscc  : $iscc"
Write-Host "  C src : $RuntimeSrc"
if ($gccMachine -and ($gccMachine -notmatch 'x86_64|amd64')) {
    Write-Host "  WARNING: gcc targets '$gccMachine', but the installer is x64-only. Linking may fail." -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# 2. Clean previous artifacts
# ---------------------------------------------------------------------------
Step 'Cleaning previous artifacts'
foreach ($p in @($Dist, $RuntimeObj, $CargoExe)) {
    if (Test-Path -LiteralPath $p) {
        Remove-Item -LiteralPath $p -Recurse -Force
        Write-Host "  removed $p"
    }
}

# ---------------------------------------------------------------------------
# 3. Build the Rust compiler
# ---------------------------------------------------------------------------
[void](Invoke-Required -What 'cargo build --release' -ExitOnFail 3 -Exe $cargo -Arguments @('build', '--release'))
if (-not (Test-Path -LiteralPath $CargoExe)) { Fail "cargo succeeded but $CargoExe was not produced (did the package name in Cargo.toml change?)." 3 }

# ---------------------------------------------------------------------------
# 4. Compile the C runtime to runtime.o
#    -mstackrealign: the same flag build-exe passes today. Cranelift-generated code does not
#                    guarantee 16-byte stack alignment at calls into C, so every runtime
#                    function must realign its own frame. It is a COMPILE flag, so it has to be
#                    applied here: it does nothing at link time once runtime.o is pre-built.
# ---------------------------------------------------------------------------
Step 'Compiling C runtime (runtime.o)'
$gccArgs = @('-O2', '-mstackrealign', '-c', $RuntimeSrc, '-o', $RuntimeObj)
$r = Invoke-Native -Exe $gcc -Arguments $gccArgs
if ($r.ExitCode -ne 0) {
    # Fallback: older MinGW-w64 headers default to a pre-Vista _WIN32_WINNT and hide
    # CONDITION_VARIABLE / SleepConditionVariableCS, which the thread pool uses.
    Write-Host "  First attempt failed; retrying with -D_WIN32_WINNT=0x0601 ..." -ForegroundColor Yellow
    $r = Invoke-Native -Exe $gcc -Arguments ($gccArgs + '-D_WIN32_WINNT=0x0601')
    if ($r.ExitCode -ne 0) { Show-Failure -What 'gcc -c runtime_template.c' -Result $r; exit 4 }
    Write-Host "  NOTE: runtime built only with -D_WIN32_WINNT=0x0601. Consider adding it to the first attempt." -ForegroundColor Yellow
}
if (-not (Test-Path -LiteralPath $RuntimeObj)) { Fail 'gcc reported success but runtime.o was not created.' 4 }

# ---------------------------------------------------------------------------
# 5. Stage the install tree (mirrors the installed layout, so donky.exe resolves
#    ..\lib\runtime.o from dist\bin exactly as it will under Program Files)
# ---------------------------------------------------------------------------
Step 'Staging dist\'
$DistBin      = Join-Path $Dist 'bin'
$DistLib      = Join-Path $Dist 'lib'
$DistExamples = Join-Path $Dist 'examples'
New-Item -ItemType Directory -Force -Path @($DistBin, $DistLib, $DistExamples) | Out-Null

Copy-Item -LiteralPath $CargoExe   -Destination (Join-Path $DistBin 'donky.exe') -Force
Copy-Item -LiteralPath $RuntimeObj -Destination (Join-Path $DistLib 'runtime.o') -Force

$LibSrc   = (Resolve-Path -LiteralPath (Join-Path $ParentDir 'lib')).Path
$libFiles = @(Get-ChildItem -LiteralPath $LibSrc -Recurse -File -Filter '*.donk')
if ($libFiles.Count -eq 0) { Write-Host '  WARNING: no lib\*.donk files found; the installer will ship no standard library.' -ForegroundColor Yellow }
foreach ($f in $libFiles) {
    $rel  = $f.FullName.Substring($LibSrc.Length).TrimStart('\', '/')
    $dest = Join-Path $DistLib $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
}
Write-Host "  lib: $($libFiles.Count) .donk file(s)"

$ExSrc = Join-Path $ParentDir 'examples'
if (Test-Path -LiteralPath $ExSrc) {
    $exBytes = (Get-ChildItem -LiteralPath $ExSrc -Recurse -File | Measure-Object -Property Length -Sum).Sum
    if ($null -eq $exBytes) { $exBytes = 0 }
    if ($exBytes -lt $ExamplesMaxB) {
        Copy-Item -Path (Join-Path $ExSrc '*') -Destination $DistExamples -Recurse -Force
        Write-Host ("  examples: {0:N1} MB staged" -f ($exBytes / 1MB))
    } else {
        Write-Host ("  examples: {0:N1} MB exceeds the 50 MB limit - NOT included in the installer" -f ($exBytes / 1MB)) -ForegroundColor Yellow
    }
} else {
    Write-Host '  examples: no examples\ folder, skipping'
}

# ---------------------------------------------------------------------------
# 6. Staged self-test: run the staged compiler against the staged runtime.
#    Proves {exe_dir}\..\lib\runtime.o resolution and the gcc link step work
#    before we spend time on the installer.
# ---------------------------------------------------------------------------
if (-not $SkipSmokeTest) {
    Step 'Staged self-test (donky --help, then compile + run a tiny program)'
    $staged = Join-Path $DistBin 'donky.exe'

    $r = Invoke-Native -Exe $staged -Arguments @('--help')
    if ($r.ExitCode -ne 0) { Show-Failure -What 'staged donky.exe --help' -Result $r; exit 6 }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('donky-selftest-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    Push-Location $tmp
    try {
        Set-Content -LiteralPath (Join-Path $tmp 'hello.donk') -Value 'print("donky-selftest-ok");' -Encoding ASCII
        $r = Invoke-Native -Exe $staged -Arguments @('build-exe', 'hello.donk')
        if ($r.ExitCode -ne 0) { Show-Failure -What 'staged donky.exe build-exe hello.donk' -Result $r; exit 6 }
        if (-not (Test-Path -LiteralPath (Join-Path $tmp 'donky_app.exe'))) { Fail 'build-exe reported success but donky_app.exe is missing.' 6 }
        if (-not (($r.Stdout -join "`n") -match 'prebuilt runtime')) {
            Write-Host '  NOTE: build-exe did not report using the prebuilt runtime.o (it compiled the embedded C source instead).' -ForegroundColor Yellow
        }
        $ra  = Invoke-Native -Exe (Join-Path $tmp 'donky_app.exe')
        $run = ($ra.Stdout -join "`n")
        if ($ra.ExitCode -ne 0 -or $run -notmatch 'donky-selftest-ok') { Fail "self-test program failed (exit $($ra.ExitCode)); output: $run" 6 }
        Write-Host '  self-test OK' -ForegroundColor Green
    } finally {
        Pop-Location
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------
# 7. Compile the installer. installer.iss reads the staging dir through the
#    MySourceDir preprocessor define and writes dist\DonkySetup.exe (OutputDir).
# ---------------------------------------------------------------------------
[void](Invoke-Required -What 'Compiling installer (ISCC)' -ExitOnFail 7 -Exe $iscc `
        -Arguments @("/DMyAppVersion=$Version", '/DMySourceDir=dist', $IssFile))

if (-not (Test-Path -LiteralPath $SetupExe)) {
    # Defensive: if someone changes OutputDir in the .iss, find the file and copy it into dist\.
    $found = Get-ChildItem -LiteralPath $Root -Recurse -File -Filter 'DonkySetup.exe' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $found) { Fail 'ISCC succeeded but DonkySetup.exe was not found anywhere under the repo.' 7 }
    Copy-Item -LiteralPath $found.FullName -Destination $SetupExe -Force
}

# ---------------------------------------------------------------------------
# 8. Tidy up + report
#    runtime.o is removed from the repo root by default: main.rs also searches .\runtime.o, so a
#    stale copy left here would silently be linked by `cargo run -- build-exe` after you edit
#    runtime_template.c. dist\lib\runtime.o is the shipped copy. (Set DONKY_RUNTIME=source to
#    force the embedded C source at any time.)
# ---------------------------------------------------------------------------
if (-not $KeepRuntimeObj -and (Test-Path -LiteralPath $RuntimeObj)) { Remove-Item -LiteralPath $RuntimeObj -Force }

Step 'Build complete'
$item = Get-Item -LiteralPath $SetupExe
$hash = (Get-FileHash -LiteralPath $SetupExe -Algorithm SHA256).Hash
Set-Content -LiteralPath ($SetupExe + '.sha256') -Value "$hash  DonkySetup.exe" -Encoding ASCII
Write-Host ("  File   : {0}" -f $item.FullName)
Write-Host ("  Size   : {0:N2} MB ({1:N0} bytes)" -f ($item.Length / 1MB), $item.Length)
Write-Host ("  SHA256 : {0}" -f $hash)
Write-Host ""
Write-Host 'dist\DonkySetup.exe is ready.' -ForegroundColor Green
exit 0
