$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
if (!$env:LT2_INPUT_DIR) { throw 'LT2_INPUT_DIR is not set; run Build.ps1.' }
$exe = Join-Path $env:LT2_INPUT_DIR 'LT2.exe'
$expected = '3242DA6EE1C453D124DEA26769BD802B0346AC383D9F5CA96784A9F1FC27380F'
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $expected) {
    throw 'Different LT2.exe version: entry point and recovery seeds must be reviewed first.'
}
$env:PYTHONPATH = (Resolve-Path 'tools/pcrecomp').Path
New-Item -ItemType Directory -Force analysis,src/generated | Out-Null
& .venv/Scripts/python.exe -u -m tools $exe --all --output src/generated --split 200 --functions-json analysis/functions.json --pe-json analysis/pe.json
if ($LASTEXITCODE -ne 0) { throw 'pcrecomp lift failed' }
if (Select-String -Path src/generated/*.c -Pattern 'ERROR lifting|FAILED') { throw 'Generated code contains failed lifts' }
& .venv/Scripts/python.exe scripts/recover.py
if ($LASTEXITCODE -ne 0) { throw 'Callback recovery failed' }
