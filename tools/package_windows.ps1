[CmdletBinding()]
param(
    [string]$BuildDirectory = "build-release",
    [string]$OutputDirectory = "dist"
)

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$buildPath = (Resolve-Path -LiteralPath (Join-Path $repositoryRoot $BuildDirectory)).Path
$executables = @("ccs-trans.exe", "ccs-trans-tray.exe")
foreach ($name in $executables) {
    $executable = Join-Path $buildPath $name
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Release executable not found: $executable"
    }
}

$cmake = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot "CMakeLists.txt")
if ($cmake -notmatch 'project\(ccs_trans VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Unable to read the project version from CMakeLists.txt"
}
$version = $Matches[1]
$packageName = "ccs-trans-$version-Windows-x64"
$distRoot = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputDirectory))
}
$packageRoot = Join-Path $distRoot $packageName
$archivePath = Join-Path $distRoot "$packageName.zip"

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

Assert-RepositoryChild $distRoot
Assert-RepositoryChild $packageRoot
Assert-RepositoryChild $archivePath

New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}

New-Item -ItemType Directory -Path $packageRoot | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot "docs") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot "docs/Archived") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot "THIRD_PARTY_LICENSES") | Out-Null

foreach ($name in $executables) {
    Copy-Item -LiteralPath (Join-Path $buildPath $name) -Destination (Join-Path $packageRoot $name)
}
Copy-Item -LiteralPath (Join-Path $repositoryRoot "README.md") -Destination $packageRoot

$documents = @(
    "Design.md",
    "DevelopmentPlan.md",
    "ProjectStructure.md",
    "Archived/Reconstruction.md",
    "Archived/WindowsValidationCheckResult.md"
)
foreach ($document in $documents) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "docs/$document") `
        -Destination (Join-Path $packageRoot "docs/$document")
}

Copy-Item -LiteralPath (Join-Path $repositoryRoot "third_party/nlohmann/LICENSE.MIT") `
    -Destination (Join-Path $packageRoot "THIRD_PARTY_LICENSES/nlohmann-json.MIT")

$hashLines = foreach ($name in $executables) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $packageRoot $name)).Hash
    "$hash  $name"
}
Set-Content -LiteralPath (Join-Path $packageRoot "SHA256SUMS.txt") `
    -Value $hashLines -Encoding ascii

$expectedFiles = @(
    "README.md",
    "SHA256SUMS.txt",
    "THIRD_PARTY_LICENSES/nlohmann-json.MIT",
    "ccs-trans-tray.exe",
    "ccs-trans.exe"
) + ($documents | ForEach-Object { "docs/$_" })
$expectedDirectories = @(
    "THIRD_PARTY_LICENSES",
    "docs",
    "docs/Archived"
)
$actualFiles = @(
    Get-ChildItem -LiteralPath $packageRoot -Recurse -File | ForEach-Object {
        Get-RelativePackagePath $packageRoot $_.FullName
    }
)
$difference = Compare-Object ($expectedFiles | Sort-Object) ($actualFiles | Sort-Object)
if ($difference) {
    throw "Package whitelist mismatch:`n$($difference | Out-String)"
}
$actualDirectories = @(
    Get-ChildItem -LiteralPath $packageRoot -Recurse -Directory | ForEach-Object {
        Get-RelativePackagePath $packageRoot $_.FullName
    }
)
$difference = Compare-Object ($expectedDirectories | Sort-Object) ($actualDirectories | Sort-Object)
if ($difference) {
    throw "Package directory whitelist mismatch:`n$($difference | Out-String)"
}

Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal
$archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash

Write-Output "Package: $packageRoot"
Write-Output "Archive: $archivePath"
Write-Output "Executable SHA-256:"
$hashLines | ForEach-Object { Write-Output "  $_" }
Write-Output "Archive SHA-256: $archiveHash"
