$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
$archive = 'tools/SDL3-devel-3.4.20-VC.zip'
$expected = '1EC3009655EE37B1F73E52B3B120ACC5AFD8ABAF9532F3303B1BF1DFE91B5969'
if (!(Test-Path $archive)) {
    Invoke-WebRequest 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-devel-3.4.20-VC.zip' -OutFile $archive
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $expected) { throw 'SDL3 archive checksum mismatch' }
Expand-Archive $archive -DestinationPath tools -Force
Write-Output 'SDL 3.4.20 VC SDK ready (x86 runtime used by the game).'
