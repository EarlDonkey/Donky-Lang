param([string]$Dir)
$ErrorActionPreference = 'SilentlyContinue'
Set-Location $Dir
$d = (Get-Location).Path
function Put($name, $text) { [IO.File]::WriteAllText((Join-Path $d $name), $text) }
function Cap($name) {
  $p = Join-Path $d $name
  if (-not (Test-Path $p)) { return '' }
  $s = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite')
  $buf = New-Object byte[] 16384
  $n = $s.Read($buf, 0, 16384)
  $s.Close()
  return [Text.Encoding]::UTF8.GetString($buf, 0, $n)
}
$b = Start-Process -FilePath 'donky' -ArgumentList 'build-exe','temp.donk' -WorkingDirectory $d -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $d 'build.out') -RedirectStandardError (Join-Path $d 'build.err')
$null = $b.Handle
if (-not $b.WaitForExit(30000)) { taskkill /PID $b.Id /T /F | Out-Null; Put 'status.txt' 'STATUS_BUILD_TIMEOUT'; exit }
if (-not (Test-Path (Join-Path $d 'donky_app.exe'))) { Put 'result.txt' ((Cap 'build.err') + (Cap 'build.out')); Put 'status.txt' 'STATUS_BUILD_FAILED'; exit }
$r = Start-Process -FilePath (Join-Path $d 'donky_app.exe') -WorkingDirectory $d -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $d 'run.out') -RedirectStandardError (Join-Path $d 'run.err')
$null = $r.Handle
if (-not $r.WaitForExit(2000)) { taskkill /PID $r.Id /T /F | Out-Null; $r.WaitForExit(2000) | Out-Null; Put 'status.txt' 'STATUS_RUN_TIMEOUT'; exit }
Put 'result.txt' ((Cap 'run.out') + (Cap 'run.err'))
Put 'status.txt' 'STATUS_OK'