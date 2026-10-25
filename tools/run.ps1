$ErrorActionPreference='Stop'
$releases=Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'releases'
$version=(Get-Content -LiteralPath (Join-Path $releases 'current.txt') -Raw).Trim()
if($version -notmatch '^\d+\.\d+\.\d+(-[A-Za-z0-9.-]+)?$') {throw 'Invalid current runtime version'}
$portable=Join-Path $releases $version
Start-Process -FilePath (Join-Path $portable 'AtemSonyBridge.exe') -WorkingDirectory $portable
