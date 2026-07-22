[CmdletBinding()]
param(
    [string]$StagingDirectory = "tmp/windows-qt-gui-stage",
    [string]$OutputDirectory = "tmp/windows-installer-prototype"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path

function Resolve-RepositoryPath([string]$Path) {
    $fullPath = if ([IO.Path]::IsPathRooted($Path)) {
        [IO.Path]::GetFullPath($Path)
    } else {
        [IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
    }
    $prefix = $repositoryRoot.TrimEnd("\", "/") + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must stay inside the repository: $fullPath"
    }
    return $fullPath
}

$stagePath = Resolve-RepositoryPath $StagingDirectory
$outputPath = Resolve-RepositoryPath $OutputDirectory
$manifestPath = Join-Path $repositoryRoot "packaging/windows/qt-runtime-manifest.txt"
$installerScript = Join-Path $repositoryRoot "packaging/windows/installer/ccs-trans-prototype.iss"
$iscc = Join-Path $env:LOCALAPPDATA "Programs/Inno Setup 7/ISCC.exe"

foreach ($required in @($stagePath, $manifestPath, $installerScript, $iscc)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Installer prototype input is missing: $required"
    }
}

$expected = @(Get-Content -LiteralPath $manifestPath |
    Where-Object { $_ -and -not $_.StartsWith("#") } | Sort-Object)
$prefix = $stagePath.TrimEnd("\", "/") + [IO.Path]::DirectorySeparatorChar
$actual = @(Get-ChildItem -LiteralPath $stagePath -Recurse -File | ForEach-Object {
    $_.FullName.Substring($prefix.Length).Replace("\", "/")
} | Sort-Object)
$difference = Compare-Object $expected $actual
if ($difference) {
    throw "Refusing to package an unverified Qt staging tree:`n$($difference | Out-String)"
}

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
& $iscc `
    "/DStagingRoot=$stagePath" `
    "/DOutputRoot=$outputPath" `
    "/DAppVersion=0.8.0" `
    $installerScript
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compiler failed with exit code $LASTEXITCODE"
}

$installer = Join-Path $outputPath "ccs-trans-0.8.0-Windows-x64-setup-prototype.exe"
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Installer prototype was not produced: $installer"
}
Write-Output "Installer prototype: $installer"
Get-FileHash -Algorithm SHA256 -LiteralPath $installer
