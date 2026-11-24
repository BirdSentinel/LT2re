$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
$destination = Join-Path (Get-Location) 'tools/llvm'
New-Item -ItemType Directory -Force $destination | Out-Null
$archive = Join-Path $destination 'llvm-21.1.8.tar.xz'
$expected = '749d22f565fcd5718dbed06512572d0e5353b502c03fe1f7f17ee8b8aca21a47'
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest 'https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/clang%2Bllvm-21.1.8-x86_64-pc-windows-msvc.tar.xz' -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'LLVM archive SHA256 mismatch'
}
& .venv/Scripts/python.exe scripts/extract-llvm.py
if ($LASTEXITCODE -ne 0) { throw 'LLVM extraction failed' }
