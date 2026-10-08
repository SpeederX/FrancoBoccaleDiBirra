[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\build\native\Release\fbdb.exe'),
    [string]$AssetsDirectory = (Join-Path $env:LOCALAPPDATA 'FrancoBoccaleDiBirra\external\meeting'),
    [string]$FFmpeg = 'ffmpeg',
    [ValidateRange(0, 65535)][int]$LlmPort = 0,
    [string]$LlmModel,
    [switch]$PrintCommand
)

$ErrorActionPreference = 'Stop'
$Executable = [System.IO.Path]::GetFullPath($Executable)
$AssetsDirectory = [System.IO.Path]::GetFullPath($AssetsDirectory)
$parakeet = Join-Path $AssetsDirectory 'parakeet-v0.6.1-bin-win-cpu-x64\parakeet-cli.exe'
$asr = Join-Path $AssetsDirectory 'tdt-0.6b-v3-q8_0.gguf'
$diar = Join-Path $AssetsDirectory 'parakeet-bundle-small.gguf'
foreach ($path in @($Executable, $parakeet, $asr, $diar)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing $path. Build Release and run scripts\prepare-meeting.ps1 first."
    }
}
if (($LlmPort -gt 0) -ne [bool]$LlmModel) {
    throw 'Provide both -LlmPort and -LlmModel, or select a loaded model from inside Meeting.'
}
$launchArguments = @('--meeting', '--parakeet', $parakeet, '--asr', $asr, '--diar', $diar, '--ffmpeg', $FFmpeg)
if ($LlmPort) {
    $launchArguments += @('--llm-port', $LlmPort.ToString(), '--llm-model', $LlmModel)
}
if ($PrintCommand) {
    @($Executable) + $launchArguments | ConvertTo-Json
    return
}
& $Executable @launchArguments
exit $LASTEXITCODE
