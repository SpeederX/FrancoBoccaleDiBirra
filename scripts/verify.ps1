[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string[]]$Configuration = @('Debug', 'Release'),
    [string]$Generator
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)

    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program exited with code $LASTEXITCODE"
    }
}

Push-Location $repoRoot
try {
    Invoke-Checked 'git' @('diff', '--check')

    if (-not $Generator) {
        if ($env:OS -eq 'Windows_NT') {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
            if (-not (Test-Path -LiteralPath $vswhere)) {
                throw 'Install Visual Studio with Desktop development with C++, or pass -Generator Ninja from a matching developer shell.'
            }

            $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
            if (-not $installation) {
                throw 'Visual Studio C++ tools were not found.'
            }

            $majorVersion = ([version]$installation.installationVersion).Major
            $capabilities = & cmake -E capabilities | ConvertFrom-Json
            $Generator = ($capabilities.generators | Where-Object { $_.name -like "Visual Studio $majorVersion *" } | Select-Object -First 1).name
            if (-not $Generator) {
                throw "CMake does not support Visual Studio $majorVersion. Update CMake, or pass -Generator Ninja from a matching developer shell."
            }
        }
        else {
            $Generator = 'Ninja'
        }
    }

    if ($Generator -like 'Visual Studio *') {
        $buildDir = Join-Path $repoRoot 'build/verify-msvc'
        Invoke-Checked 'cmake' @('-S', '.', '-B', $buildDir, '-G', $Generator, '-A', 'x64')
        foreach ($buildConfiguration in $Configuration) {
            Invoke-Checked 'cmake' @('--build', $buildDir, '--config', $buildConfiguration, '--parallel')
            Invoke-Checked 'ctest' @('--test-dir', $buildDir, '--build-config', $buildConfiguration, '--output-on-failure', '--no-tests=error')
        }
    }
    else {
        foreach ($buildConfiguration in $Configuration) {
            $buildDir = Join-Path $repoRoot "build/verify-$($buildConfiguration.ToLowerInvariant())"
            Invoke-Checked 'cmake' @('-S', '.', '-B', $buildDir, '-G', $Generator, "-DCMAKE_BUILD_TYPE=$buildConfiguration")
            Invoke-Checked 'cmake' @('--build', $buildDir, '--parallel')
            Invoke-Checked 'ctest' @('--test-dir', $buildDir, '--output-on-failure', '--no-tests=error')
        }
    }
}
finally {
    Pop-Location
}
