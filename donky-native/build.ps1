$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) { throw "gcc not found on PATH" }

New-Item -ItemType Directory -Force -Path dist\bin, dist\lib | Out-Null

gcc -O2 -c src\runtime_template.c -o dist\lib\runtime.o
if ($LASTEXITCODE -ne 0) { throw "runtime.o compile failed" }

cargo build --release
if ($LASTEXITCODE -ne 0) { throw "cargo build failed" }

Copy-Item target\release\donky-native.exe dist\bin\donky.exe -Force

$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (Test-Path $iscc) { & $iscc donky_setup.iss } else { Write-Host "Inno Setup not found; run donky_setup.iss manually." }