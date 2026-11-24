param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [string]$Python='python',
    [switch]$SkipSetup
)
$ErrorActionPreference='Stop'
$game=(Resolve-Path -LiteralPath $GameDirectory).Path
Set-Location $PSScriptRoot
$expected='3242DA6EE1C453D124DEA26769BD802B0346AC383D9F5CA96784A9F1FC27380F'
$exe=Join-Path $game 'LT2.exe'
if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $expected){throw 'Unsupported LT2.exe version; see README.'}

# Build inputs are copied to a temporary folder that is removed when the build ends.
$inputDir=Join-Path ([IO.Path]::GetTempPath()) ('LT2re-build-'+[guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force (Join-Path $inputDir 'misc'),'build/native','dist' | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $inputDir 'LT2.exe') -Force
    foreach($shader in 'dcWorld1.vso','dcWorld2.vso'){
        Copy-Item -LiteralPath (Join-Path $game "misc/$shader") -Destination (Join-Path $inputDir 'misc') -Force
    }
    $env:LT2_INPUT_DIR=$inputDir

    if(!$SkipSetup){
        if(!(Test-Path '.venv/Scripts/python.exe')){
            & $Python -m venv .venv
            if($LASTEXITCODE){throw 'Python venv failed'}
        }
        & .venv/Scripts/python.exe -m pip install -r scripts/requirements.txt
        if($LASTEXITCODE){throw 'Python dependency installation failed'}
        & scripts/setup-sdl.ps1
        & scripts/setup-vulkan.ps1
        & scripts/setup-llvm.ps1
    }
    & scripts/lift.ps1
    & .venv/Scripts/python.exe scripts/extract-icon.py
    if($LASTEXITCODE){throw 'Icon extraction failed'}
    & $env:ComSpec /d /c scripts\build-native.cmd
    if($LASTEXITCODE){throw 'Native compilation failed'}
    foreach($file in 'LT2re.exe','lt2-renderer.dll','SDL3.dll'){
        Copy-Item -LiteralPath "build/native/$file" -Destination dist -Force
    }
    Copy-Item -LiteralPath scripts/Play.cmd -Destination dist -Force
    Copy-Item -LiteralPath licenses -Destination dist -Recurse -Force
    Write-Output 'Build complete: dist/LT2re.exe. Copy dist contents beside the original LT2.exe and run Play.cmd.'
}
finally {
    Set-Location $PSScriptRoot
    Remove-Item Env:LT2_INPUT_DIR -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $inputDir -Recurse -Force -ErrorAction SilentlyContinue
}
