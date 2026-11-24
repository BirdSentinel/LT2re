$ErrorActionPreference='Stop'
Set-Location (Join-Path $PSScriptRoot '..')
New-Item -ItemType Directory -Force tools/vulkan | Out-Null
$headers='d277a0a3cda9c682708c93279f0c72cd6b0ae3f3'
if (!(Test-Path tools/vulkan/headers/.git)) {
    git clone https://github.com/KhronosGroup/Vulkan-Headers.git tools/vulkan/headers
    if ($LASTEXITCODE) { throw 'Vulkan-Headers clone failed' }
    git -C tools/vulkan/headers checkout --detach $headers
    if ($LASTEXITCODE) { throw 'Vulkan-Headers checkout failed' }
}
if ((git -C tools/vulkan/headers rev-parse HEAD) -ne $headers) { throw 'Unexpected Vulkan-Headers revision' }
$volk='653b2bfd1142df0f00f921eb2c1ae9943092340a'
$files=@{
    'volk.h'='FCD191F3F79DF7DDA8995608BAC6A51409C978D100E8AAA39B8C5D0304ACF575'
    'volk.c'='AEDB9C976F89954A7945BBBC50A8270ECD6864F7D7073F2B70533FF45E1BC879'
}
foreach ($name in $files.Keys) {
    $dest=Join-Path 'tools/vulkan' $name
    if (!(Test-Path $dest)) { Invoke-WebRequest "https://raw.githubusercontent.com/zeux/volk/$volk/$name" -OutFile $dest }
    if ((Get-FileHash $dest -Algorithm SHA256).Hash -ne $files[$name]) { throw "Checksum mismatch: $name" }
}
$archive='tools/vulkan/glslang.zip'
if (!(Test-Path $archive)) { Invoke-WebRequest 'https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip' -OutFile $archive }
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne '82BF434E69B9BB4829DE7E2B4BC2C5E7A7861E53D66CF75E5CC70F5F694A8D9B') { throw 'glslang checksum mismatch' }
Expand-Archive $archive -DestinationPath tools/vulkan/glslang -Force
Write-Output 'Pinned Vulkan headers, volk loader, and glslang compiler ready.'

