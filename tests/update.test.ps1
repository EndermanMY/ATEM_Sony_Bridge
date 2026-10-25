param([string]$BuildDirectory='build-clean')
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
$target=Join-Path $project "$BuildDirectory\update-test-runtime"
New-Item -ItemType Directory -Path (Join-Path $target 'config') -Force | Out-Null
Set-Content -LiteralPath (Join-Path $target 'config\config.json') -Value '{"sentinel":"keep-camera-bindings","cameras":[]}' -Encoding UTF8
Set-Content -LiteralPath (Join-Path $target 'config\mapping_rules.json') -Value '[{"id":"keep-rules"}]' -Encoding UTF8
$before=@{}
foreach($name in @('config.json','mapping_rules.json')) {$before[$name]=(Get-FileHash -LiteralPath (Join-Path $target "config\$name")).Hash}
1..2 | ForEach-Object {& (Join-Path $project 'tools\build.ps1') -SkipBuild -BuildDirectory $BuildDirectory -PortableDirectory $target}
foreach($name in @('config.json','mapping_rules.json')) {
  if((Get-FileHash -LiteralPath (Join-Path $target "config\$name")).Hash -ne $before[$name]) {throw "Update overwrote $name"}
}
foreach($name in @('AtemSonyBridge.exe','Cr_Core.dll','CrAdapter\Cr_PTP_IP.dll','ui\index.html','ui\js\bridge.js')) {
  if(-not(Test-Path -LiteralPath (Join-Path $target $name))) {throw "Missing runtime asset: $name"}
}
if(Test-Path -LiteralPath (Join-Path $target 'ui\ui')) {throw 'Nested ui directory after repeated update'}
Write-Output 'Repeated update preserves config/rules and copies complete runtime assets.'
