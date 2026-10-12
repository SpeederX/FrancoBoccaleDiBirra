[CmdletBinding()]
param(
    [string]$AssetsDirectory = (Join-Path $env:LOCALAPPDATA 'FrancoBoccaleDiBirra\external\tts'),
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$AssetsDirectory = [System.IO.Path]::GetFullPath($AssetsDirectory)
New-Item -ItemType Directory -Path $AssetsDirectory -Force | Out-Null
$ttsPython = Join-Path $AssetsDirectory 'venv\Scripts\python.exe'
Write-Host 'Preparing local Piper 1.8.0 with the Italian Paola medium voice, CPU only.'
Write-Host 'Downloads contain software and voice weights. Meeting audio and text are not uploaded.'
if (!(Test-Path -LiteralPath $ttsPython -PathType Leaf)) {
    & $Python -m venv (Join-Path $AssetsDirectory 'venv')
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not create the Piper Python environment.'
    }
}
& $ttsPython -m pip --disable-pip-version-check install 'piper-tts==1.8.0'
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install the local Piper runtime.'
}

$revision = 'c10ece1aade47bb51c153c893d14e5bf8e5b7117'
$base = "https://huggingface.co/rhasspy/piper-voices/resolve/$revision/it/it_IT/paola/medium"
$files = @('it_IT-paola-medium.onnx', 'it_IT-paola-medium.onnx.json', 'MODEL_CARD')
$receiptPath = Join-Path $AssetsDirectory 'voice-provenance.json'
$receipt = if (Test-Path -LiteralPath $receiptPath) { Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json }
$identities = @()
foreach ($name in $files) {
    $targetName = if ($name -eq 'MODEL_CARD') { 'paola-MODEL_CARD.txt' } else { $name }
    $target = Join-Path $AssetsDirectory $targetName
    if (!(Test-Path -LiteralPath $target -PathType Leaf)) {
        $temporary = "$target.download"
        Write-Host "Downloading $name"
        & curl.exe --fail --location --no-progress-meter --retry 2 --output $temporary "$base/$name"
        if ($LASTEXITCODE -ne 0) {
            throw "Could not download $name."
        }
        Move-Item -LiteralPath $temporary -Destination $target
    }
    $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($name -eq 'it_IT-paola-medium.onnx' -and $hash -ne '6fc918b5a0ea6137382833dddfa567bffbe6a5060c02043c87192ee59c04210c') {
        throw "The Piper voice differs from its pinned model hash: $target"
    }
    $previous = @($receipt.files | Where-Object { $_.name -eq $targetName })
    if ($previous.Count -gt 0 -and $previous[0].sha256 -ne $hash) {
        throw "A prepared voice file has changed: $target"
    }
    $identities += @{ name = $targetName; sha256 = $hash; bytes = (Get-Item -LiteralPath $target).Length }
}
@{ piper_version = '1.8.0'; voice = 'it_IT-paola-medium'; revision = $revision; files = $identities } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
Write-Host 'Piper is ready. Restart scripts\start-meeting.ps1; use /meeting read notes or /meeting read transcript.'
