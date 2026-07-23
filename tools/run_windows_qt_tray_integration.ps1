[CmdletBinding()]
param(
    [string]$RuntimeBuildDirectory = "build-windows-runtime-warning",
    [string]$QtBuildDirectory = "build-windows-qt-gui-warning",
    [string]$StagingDirectory = "tmp/windows-qt-tray-integration"
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

function Get-BuildIdentity([string]$BuildDirectory) {
    $header = Join-Path $BuildDirectory "generated/include/core/version.hpp"
    if (-not (Test-Path -LiteralPath $header -PathType Leaf)) {
        throw "Build identity is missing; configure the build tree first: $header"
    }
    $content = Get-Content -Raw -LiteralPath $header
    $version = [regex]::Match($content, 'kVersion\[\]\s*=\s*"([^"]+)"').Groups[1].Value
    $commit = [regex]::Match($content, 'kSourceCommit\[\]\s*=\s*"([^"]+)"').Groups[1].Value
    if (-not $version -or -not $commit) {
        throw "Build identity is malformed: $header"
    }
    return [pscustomobject]@{ Version = $version; Commit = $commit; Header = $header }
}

$runtimeBuild = Resolve-RepositoryPath $RuntimeBuildDirectory
$qtBuild = Resolve-RepositoryPath $QtBuildDirectory
$stage = Resolve-RepositoryPath $StagingDirectory
$runtimeIdentity = Get-BuildIdentity $runtimeBuild
$qtIdentity = Get-BuildIdentity $qtBuild
$sourceCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not $sourceCommit) {
    throw "Failed to resolve the current source commit"
}
if ($runtimeIdentity.Version -ne $qtIdentity.Version -or
    $runtimeIdentity.Commit -ne $qtIdentity.Commit -or
    $runtimeIdentity.Commit -ne $sourceCommit) {
    throw "Runtime/Qt build identity mismatch. Reconfigure and rebuild both trees. " +
        "runtime=$($runtimeIdentity.Version)@$($runtimeIdentity.Commit), " +
        "qt=$($qtIdentity.Version)@$($qtIdentity.Commit), source=$sourceCommit"
}
& (Join-Path $PSScriptRoot "deploy_windows_qt_gui.ps1") `
    -BuildDirectory $QtBuildDirectory `
    -StagingDirectory $StagingDirectory `
    -SkipSmoke
if ($LASTEXITCODE -ne 0) {
    throw "Qt GUI deployment failed with exit code $LASTEXITCODE"
}

foreach ($name in @("ccs-trans.exe", "ccs-trans-tray.exe")) {
    $source = Join-Path $runtimeBuild $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Runtime integration input is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $name)
}

$python = Get-Command python -ErrorAction Stop
& $python.Source `
    (Join-Path $repositoryRoot "tests/integration/run_qt_tray_lifecycle.py") `
    --stage $stage
if ($LASTEXITCODE -ne 0) {
    throw "Qt tray lifecycle integration failed with exit code $LASTEXITCODE"
}
