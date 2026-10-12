[CmdletBinding()]
param(
    [string]$Executable,
    [string]$AssetsDirectory = (Join-Path $env:LOCALAPPDATA 'FrancoBoccaleDiBirra\external\meeting'),
    [string]$FFmpeg = 'ffmpeg',
    [ValidateRange(0, 65535)][int]$LlmPort = 0,
    [string]$LlmModel,
    [string]$MeetingFolder,
    [string]$Microphone,
    [string]$OutputDevice,
    [string]$TtsAssetsDirectory = (Join-Path $env:LOCALAPPDATA 'FrancoBoccaleDiBirra\external\tts'),
    [string]$TtsModel,
    [switch]$PrintCommand
)

$ErrorActionPreference = 'Stop'
if (!$Executable) {
    $Executable = Join-Path $PSScriptRoot '..\build\native\Release\fbdb.exe'
}
$Executable = [System.IO.Path]::GetFullPath($Executable)
$AssetsDirectory = [System.IO.Path]::GetFullPath($AssetsDirectory)
$parakeet = Join-Path $AssetsDirectory 'parakeet-v0.6.1-bin-win-cpu-x64\parakeet-cli.exe'
$library = Join-Path $AssetsDirectory 'parakeet-v0.6.1-lib-win-cpu-x64\parakeet.dll'
$asr = Join-Path $AssetsDirectory 'tdt-0.6b-v3-q8_0.gguf'
$diar = Join-Path $AssetsDirectory 'parakeet-bundle-small.gguf'
foreach ($path in @($Executable, $parakeet, $library, $asr, $diar)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing $path. Build Release and run scripts\prepare-meeting.ps1 first."
    }
}
if (($LlmPort -gt 0) -ne [bool]$LlmModel) {
    throw 'Provide both -LlmPort and -LlmModel, or select a loaded model from inside Meeting.'
}
$launchArguments = @('--meeting', '--parakeet', $parakeet, '--library', $library, '--asr', $asr, '--diar', $diar, '--ffmpeg', $FFmpeg)
$ttsPython = Join-Path $TtsAssetsDirectory 'venv\Scripts\pythonw.exe'
if (!$TtsModel) {
    $TtsModel = Join-Path $TtsAssetsDirectory 'it_IT-paola-medium.onnx'
}
if ((Test-Path -LiteralPath $ttsPython -PathType Leaf) -and (Test-Path -LiteralPath $TtsModel -PathType Leaf)) {
    $ttsScript = Join-Path $PSScriptRoot 'meeting_read.py'
    $launchArguments += @('--tts-python', [System.IO.Path]::GetFullPath($ttsPython),
        '--tts-script', [System.IO.Path]::GetFullPath($ttsScript), '--tts-model', [System.IO.Path]::GetFullPath($TtsModel))
}
if ($MeetingFolder) {
    $launchArguments += @('--meeting-folder', $MeetingFolder)
}
if ($Microphone) {
    $launchArguments += @('--microphone', $Microphone)
}
if ($OutputDevice) {
    $launchArguments += @('--output-device', $OutputDevice)
}
if ($LlmPort) {
    $launchArguments += @('--llm-port', $LlmPort.ToString(), '--llm-model', $LlmModel)
}
if ($PrintCommand) {
    @($Executable) + $launchArguments | ConvertTo-Json
    return
}
& $Executable @launchArguments
exit $LASTEXITCODE
