[CmdletBinding()]
param(
    [string]$BuildDirectory = "build-release"
)

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$buildPath = (Resolve-Path -LiteralPath (Join-Path $repositoryRoot $BuildDirectory)).Path
$executable = Join-Path $buildPath "ccs-trans.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable not found: $executable"
}

$cmake = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot "CMakeLists.txt")
if ($cmake -notmatch 'project\(ccs_trans VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Unable to read the project version from CMakeLists.txt"
}
$version = $Matches[1]
$packageName = "ccs-trans-$version-windows-x64"
$distRoot = Join-Path $repositoryRoot "dist"
$packageRoot = Join-Path $distRoot $packageName
$archivePath = Join-Path $distRoot "$packageName.zip"

function Assert-RepositoryChild([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $repositoryRoot + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the repository: $fullPath"
    }
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
New-Item -ItemType Directory -Path (Join-Path $packageRoot "THIRD_PARTY_LICENSES") | Out-Null

Copy-Item -LiteralPath $executable -Destination (Join-Path $packageRoot "ccs-trans.exe")
Copy-Item -LiteralPath (Join-Path $repositoryRoot "README.md") -Destination $packageRoot

$documents = @("Design.md", "DevelopmentPlan.md", "ProjectStructure.md", "Reconstruction.md")
foreach ($document in $documents) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "docs/$document") `
        -Destination (Join-Path $packageRoot "docs/$document")
}

Copy-Item -LiteralPath (Join-Path $repositoryRoot "third_party/nlohmann/LICENSE.MIT") `
    -Destination (Join-Path $packageRoot "THIRD_PARTY_LICENSES/nlohmann-json.MIT")

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $packageRoot "ccs-trans.exe")).Hash
Set-Content -LiteralPath (Join-Path $packageRoot "SHA256SUMS.txt") `
    -Value "$hash  ccs-trans.exe" -Encoding ascii

Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal

Write-Output "Package: $packageRoot"
Write-Output "Archive: $archivePath"
Write-Output "Executable SHA-256: $hash"
