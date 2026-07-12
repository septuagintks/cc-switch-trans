[CmdletBinding()]
param(
    [string]$ArchivePath = ""
)

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$cmake = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot "CMakeLists.txt")
if ($cmake -notmatch 'project\(ccs_trans VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Unable to read the project version from CMakeLists.txt"
}
$version = $Matches[1]
$packageName = "ccs-trans-$version-windows-x64"
if (-not $ArchivePath) {
    $ArchivePath = Join-Path $repositoryRoot "dist/$packageName.zip"
}
$archive = (Resolve-Path -LiteralPath $ArchivePath).Path
$verificationRoot = Join-Path $repositoryRoot "tmp/package-verification-$([Guid]::NewGuid().ToString('N'))"
$packageRoot = Join-Path $verificationRoot $packageName

function Assert-RepositoryChild([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $repositoryRoot + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the repository: $fullPath"
    }
}

function Get-RelativePackagePath([string]$Root, [string]$Path) {
    $rootPrefix = [IO.Path]::GetFullPath($Root).TrimEnd("\", "/") + [IO.Path]::DirectorySeparatorChar
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the package root: $fullPath"
    }
    return $fullPath.Substring($rootPrefix.Length).Replace("\", "/")
}

Assert-RepositoryChild $verificationRoot

$documents = @(
    "Design.md",
    "DevelopmentPlan.md",
    "ProjectStructure.md",
    "Reconstruction.md",
    "WindowsValidationChecklist.md"
)
$expectedFiles = @(
    "README.md",
    "SHA256SUMS.txt",
    "THIRD_PARTY_LICENSES/nlohmann-json.MIT",
    "ccs-trans-tray.exe",
    "ccs-trans.exe"
) + ($documents | ForEach-Object { "docs/$_" })
$expectedDirectories = @(
    "THIRD_PARTY_LICENSES",
    "docs"
)

try {
    New-Item -ItemType Directory -Force -Path $verificationRoot | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $verificationRoot
    if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
        throw "Archive does not contain the expected root directory: $packageName"
    }

    $actualFiles = @(
        Get-ChildItem -LiteralPath $packageRoot -Recurse -File | ForEach-Object {
            Get-RelativePackagePath $packageRoot $_.FullName
        }
    )
    $difference = Compare-Object ($expectedFiles | Sort-Object) ($actualFiles | Sort-Object)
    if ($difference) {
        throw "Archive whitelist mismatch:`n$($difference | Out-String)"
    }
    $actualDirectories = @(
        Get-ChildItem -LiteralPath $packageRoot -Recurse -Directory | ForEach-Object {
            Get-RelativePackagePath $packageRoot $_.FullName
        }
    )
    $difference = Compare-Object ($expectedDirectories | Sort-Object) ($actualDirectories | Sort-Object)
    if ($difference) {
        throw "Archive directory whitelist mismatch:`n$($difference | Out-String)"
    }

    $checksumPath = Join-Path $packageRoot "SHA256SUMS.txt"
    $checksumLines = @(Get-Content -LiteralPath $checksumPath)
    if ($checksumLines.Count -ne 2) {
        throw "SHA256SUMS.txt must contain exactly two executable hashes"
    }
    foreach ($name in @("ccs-trans.exe", "ccs-trans-tray.exe")) {
        $line = $checksumLines | Where-Object { $_ -match "^[A-F0-9]{64}  $([regex]::Escape($name))$" }
        if (@($line).Count -ne 1) {
            throw "SHA256SUMS.txt is missing a canonical entry for $name"
        }
        $expectedHash = $line.Substring(0, 64)
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $packageRoot $name)).Hash
        if ($actualHash -ne $expectedHash) {
            throw "Checksum mismatch for $name"
        }
    }

    $cli = Join-Path $packageRoot "ccs-trans.exe"
    $tray = Join-Path $packageRoot "ccs-trans-tray.exe"
    $reportedVersion = & $cli --version
    if ($LASTEXITCODE -ne 0 -or "$reportedVersion".Trim() -ne "ccs-trans $version") {
        throw "Extracted CLI reported an unexpected version: $reportedVersion"
    }
    $trayVersion = (Get-Item -LiteralPath $tray).VersionInfo.FileVersion
    if ($trayVersion -ne $version) {
        throw "Extracted tray resource reported version $trayVersion instead of $version"
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        throw "Python is required for the extracted tray smoke test"
    }
    & $python.Source (Join-Path $repositoryRoot "tests/integration/run_tray_integration.py") `
        --tray $tray --cli $cli
    if ($LASTEXITCODE -ne 0) {
        throw "Extracted tray integration failed"
    }

    Write-Output "Windows package verification passed: $archive"
} finally {
    if (Test-Path -LiteralPath $verificationRoot) {
        Remove-Item -LiteralPath $verificationRoot -Recurse -Force
    }
}
