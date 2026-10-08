[CmdletBinding()]
param(
    [string]$AssetsDirectory = (Join-Path $env:LOCALAPPDATA 'FrancoBoccaleDiBirra\external\meeting'),
    [string]$CacheDirectory
)

$ErrorActionPreference = 'Stop'
$AssetsDirectory = [System.IO.Path]::GetFullPath($AssetsDirectory)
New-Item -ItemType Directory -Path $AssetsDirectory -Force | Out-Null
$revision = '741158ae71e64ef5c89385862c18f777d07a97a1'
$modelBase = "https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/$revision"
$files = @(
    @{
        Name = 'parakeet-win-cpu.zip'
        Url = 'https://github.com/mudler/parakeet.cpp/releases/download/v0.6.1/parakeet-v0.6.1-bin-win-cpu-x64.zip'
        Hash = 'D74B4E857F75F91AE215C5E91EC42BF15EC84ED3A369423C134DBD6422243ED8'
    },
    @{
        Name = 'tdt-0.6b-v3-q8_0.gguf'
        Url = "$modelBase/tdt-0.6b-v3-q8_0.gguf"
        Hash = '4D69A4A6683F4F2D952BAD794C1357CA6EB628027695B4699C5A9AD4CD07D757'
    },
    @{
        Name = 'parakeet-bundle-small.gguf'
        Url = "$modelBase/parakeet-bundle-small.gguf"
        Hash = '5F2C6697EB08D4D67E858692D4223A9D911AACC51AE16CF2AEE9E760F8A2269A'
    }
)

Write-Host 'Preparing local Parakeet v0.6.1: Italian/multilingual ASR plus speaker diarization.'
Write-Host 'Downloads total about 1.28 GB if the files are not already cached. No meeting data is uploaded.'
foreach ($file in $files) {
    $destination = Join-Path $AssetsDirectory $file.Name
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $file.Hash) {
            throw "Hash mismatch: $destination. Move that file aside before running preparation again."
        }
        Write-Host "Using verified $($file.Name)"
        continue
    }
    $temporary = "$destination.download"
    $cached = if ($CacheDirectory) { Join-Path $CacheDirectory $file.Name }
    if ($cached -and (Test-Path -LiteralPath $cached -PathType Leaf)) {
        Copy-Item -LiteralPath $cached -Destination $temporary
    } else {
        & curl.exe --fail --location --retry 2 --output $temporary $file.Url
        if ($LASTEXITCODE -ne 0) {
            throw "Download failed: $($file.Name). Run preparation again to retry."
        }
    }
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $file.Hash) {
        throw "Downloaded or cached file failed verification: $temporary"
    }
    Move-Item -LiteralPath $temporary -Destination $destination
}

Expand-Archive -LiteralPath (Join-Path $AssetsDirectory 'parakeet-win-cpu.zip') -DestinationPath $AssetsDirectory -Force
$credits = @('NOTICE-parakeet-bundle-small.txt', 'licenses/CC-BY-4.0.txt',
             'licenses/OpenMDW-1.1.txt', 'licenses/MIT.txt', 'licenses/Apache-2.0.txt')
foreach ($name in $credits) {
    $destination = Join-Path $AssetsDirectory $name
    if (!(Test-Path -LiteralPath $destination -PathType Leaf)) {
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        & curl.exe --fail --location --retry 2 --output "$destination.download" "$modelBase/$name"
        if ($LASTEXITCODE -ne 0) {
            throw "Could not download model credits: $name"
        }
        Move-Item -LiteralPath "$destination.download" -Destination $destination
    }
}
$manifest = @{
    parakeet_version = 'v0.6.1'
    model_revision = $revision
    files = $files
    asr_credit = 'NVIDIA Parakeet TDT 0.6B v3, CC-BY-4.0; converted and Q8_0 quantized by mudler/parakeet.cpp'
    asr_source = 'https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3'
    diarization_credit = 'NVIDIA Nemotron-3-Diarization, OpenMDW 1.1; see bundle NOTICE for all components'
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $AssetsDirectory 'manifest.json') -Encoding UTF8
Write-Host "Parakeet is ready in $AssetsDirectory"
