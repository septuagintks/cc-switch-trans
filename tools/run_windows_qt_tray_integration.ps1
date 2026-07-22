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

$runtimeBuild = Resolve-RepositoryPath $RuntimeBuildDirectory
$stage = Resolve-RepositoryPath $StagingDirectory
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
