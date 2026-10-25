param([switch]$SkipBuild,[string]$BuildDirectory="build-maintenance",[string]$PortableDirectory="")
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
$workspace=Split-Path $project -Parent
$cmake=(Get-Command cmake -ErrorAction SilentlyContinue).Source
if(-not $cmake) {
  $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if(Test-Path -LiteralPath $vswhere) {
    $cmake=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1)
  }
}
if(-not $cmake) {
  $candidates=foreach($drive in Get-PSDrive -PSProvider FileSystem) {
    Get-ChildItem -Path ($drive.Root+'Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe') -ErrorAction SilentlyContinue
  }
  $cmake=($candidates | Sort-Object FullName -Descending | Select-Object -First 1).FullName
}
if(-not $cmake) {throw 'Install Visual Studio C++ Build Tools with CMake, or add CMake to PATH.'}
$build=if([IO.Path]::IsPathRooted($BuildDirectory)) {$BuildDirectory} else {Join-Path $project $BuildDirectory}
if(-not $SkipBuild) {
  & $cmake -S $project -B $build -A x64
  if($LASTEXITCODE) {throw 'CMake configure failed'}
  & $cmake --build $build --config Release -- /m:1 /nr:false /v:minimal
  if($LASTEXITCODE) {throw 'Build failed'}
}
$source=Join-Path $build 'bin\Release'
$version=(Get-Content -LiteralPath (Join-Path $build 'bridge-version.txt') -Raw).Trim()
if($version -notmatch '^\d+\.\d+\.\d+(-[A-Za-z0-9.-]+)?$') {throw 'Invalid generated version'}
$portable=if($PortableDirectory) {[IO.Path]::GetFullPath((Join-Path $project $PortableDirectory))} else {Join-Path $workspace "releases\$version"}
New-Item -ItemType Directory -Force -Path $portable | Out-Null
foreach($name in @('AtemSonyBridge.exe','Cr_Core.dll','monitor_protocol.dll','monitor_protocol_pf.dll')) {
  Copy-Item -LiteralPath (Join-Path $source $name) -Destination $portable -Force
}
foreach($name in @('ui','CrAdapter')) {
  $asset=Join-Path $portable $name
  New-Item -ItemType Directory -Force -Path $asset | Out-Null
  Get-ChildItem -LiteralPath (Join-Path $source $name) | Copy-Item -Destination $asset -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $portable 'config') | Out-Null
foreach($name in @('config.default.json','mapping_rules.default.json')) {
  Copy-Item -LiteralPath (Join-Path $source "config\$name") -Destination (Join-Path $portable 'config') -Force
}
# Runtime migration is first-install only. Repeated builds never overwrite user files.
foreach($name in @('config.json','mapping_rules.json')) {
  $target=Join-Path $portable "config\$name"
  if(-not(Test-Path -LiteralPath $target)) {
    $legacy=Join-Path $workspace "releases\0.2.0-dev-original\config\$name"
    if(-not(Test-Path -LiteralPath $legacy)) {$legacy=Join-Path $project "build\bin\Release\config\$name"}
    if(Test-Path -LiteralPath $legacy) { Copy-Item -LiteralPath $legacy -Destination $target }
  }
}
if((Split-Path $portable -Leaf) -eq $version) {Set-Content -LiteralPath (Join-Path $workspace 'releases\current.txt') -Value $version -Encoding ASCII}
Write-Output "Portable runtime: $portable"
