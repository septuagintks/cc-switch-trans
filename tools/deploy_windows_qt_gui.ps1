[CmdletBinding()]
param(
    [string]$BuildDirectory = "build-windows-qt-gui-release",
    [string]$StagingDirectory = "tmp/windows-qt-gui-stage",
    [switch]$SkipSmoke
)

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$lock = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot "dependencies/windows-qt.lock.json") |
    ConvertFrom-Json
$qtVersion = $lock.qt.version
$qtRoot = if ($env:CCS_TRANS_QT_ROOT) {
    [IO.Path]::GetFullPath($env:CCS_TRANS_QT_ROOT)
} else {
    [IO.Path]::GetFullPath((Join-Path $env:USERPROFILE "Qt/$qtVersion/mingw_64"))
}
$mingwRoot = if ($env:CCS_TRANS_QT_MINGW_ROOT) {
    [IO.Path]::GetFullPath($env:CCS_TRANS_QT_MINGW_ROOT)
} else {
    [IO.Path]::GetFullPath((Join-Path $env:USERPROFILE "Qt/Tools/mingw1310_64"))
}

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

function Get-RelativeFileList([string]$Root) {
    $prefix = $Root.TrimEnd("\", "/") + [IO.Path]::DirectorySeparatorChar
    return @(
        Get-ChildItem -LiteralPath $Root -Recurse -File | ForEach-Object {
            $_.FullName.Substring($prefix.Length).Replace("\", "/")
        } | Sort-Object
    )
}

$buildPath = Resolve-RepositoryPath $BuildDirectory
$stagePath = Resolve-RepositoryPath $StagingDirectory
$guiExecutable = Join-Path $buildPath "src/gui/windows/ccs-trans-gui.exe"
$windeployqt = Join-Path $qtRoot "bin/windeployqt.exe"
$qtpaths = Join-Path $qtRoot "bin/qtpaths.exe"

foreach ($required in @($guiExecutable, $windeployqt, $qtpaths)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required Qt GUI deployment input is missing: $required"
    }
}
$actualQtVersion = (& $qtpaths --qt-version).Trim()
if ($LASTEXITCODE -ne 0 -or $actualQtVersion -ne $qtVersion) {
    throw "Qt version mismatch: expected $qtVersion, found $actualQtVersion"
}

if (Test-Path -LiteralPath $stagePath) {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
New-Item -ItemType Directory -Path $stagePath | Out-Null
Copy-Item -LiteralPath $guiExecutable -Destination (Join-Path $stagePath "ccs-trans-gui.exe")

& $windeployqt `
    --release `
    --dir $stagePath `
    --qmldir (Join-Path $repositoryRoot "src/gui/windows/ui") `
    --no-translations `
    --no-system-d3d-compiler `
    --no-system-dxc-compiler `
    --compiler-runtime `
    --no-opengl-sw `
    --skip-plugin-types generic,qmltooling,sqldrivers,tls,imageformats,iconengines,networkinformation,styles `
    (Join-Path $stagePath "ccs-trans-gui.exe")
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

Get-ChildItem -LiteralPath $stagePath -Recurse -Directory |
    Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { @(Get-ChildItem -LiteralPath $_.FullName -Force).Count -eq 0 } |
    Remove-Item -Force

$licenses = Join-Path $stagePath "THIRD_PARTY_LICENSES"
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot "third_party/qt/LGPL-3.0-only.txt") `
    -Destination (Join-Path $licenses "Qt-LGPL-3.0-only.txt")
Copy-Item -LiteralPath (Join-Path $repositoryRoot "third_party/qt/NOTICE.md") `
    -Destination (Join-Path $licenses "Qt-NOTICE.md")
Copy-Item -LiteralPath (Join-Path $qtRoot "sbom/qtbase-$qtVersion.spdx.json") `
    -Destination (Join-Path $licenses "QtBase-$qtVersion.spdx.json")
Copy-Item -LiteralPath (Join-Path $qtRoot "sbom/qtdeclarative-$qtVersion.spdx.json") `
    -Destination (Join-Path $licenses "QtDeclarative-$qtVersion.spdx.json")
Copy-Item -LiteralPath (Join-Path $mingwRoot "licenses/gcc/COPYING3") `
    -Destination (Join-Path $licenses "GCC-GPL-3.0.txt")
Copy-Item -LiteralPath (Join-Path $mingwRoot "licenses/gcc/COPYING.RUNTIME") `
    -Destination (Join-Path $licenses "GCC-RUNTIME-LIBRARY-EXCEPTION.txt")
Copy-Item -LiteralPath (Join-Path $mingwRoot "licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt") `
    -Destination (Join-Path $licenses "MinGW-w64-runtime.txt")
Copy-Item -LiteralPath (Join-Path $mingwRoot "licenses/winpthreads/COPYING") `
    -Destination (Join-Path $licenses "Winpthreads-COPYING.txt")

$manifestPath = Join-Path $repositoryRoot "packaging/windows/qt-runtime-manifest.txt"
$expectedFiles = @(
    Get-Content -LiteralPath $manifestPath |
        Where-Object { $_ -and -not $_.StartsWith("#") } |
        Sort-Object
)
$actualFiles = Get-RelativeFileList $stagePath
$difference = Compare-Object $expectedFiles $actualFiles
if ($difference) {
    throw "Qt deployment manifest mismatch:`n$($difference | Out-String)"
}

if (-not $SkipSmoke) {
    & (Join-Path $stagePath "ccs-trans-gui.exe") --self-test --software-renderer
    if ($LASTEXITCODE -ne 0) {
        throw "Deployed Qt GUI self-test failed with exit code $LASTEXITCODE"
    }
}

$bytes = (Get-ChildItem -LiteralPath $stagePath -Recurse -File |
    Measure-Object -Property Length -Sum).Sum
Write-Output "Qt GUI staging: $stagePath"
Write-Output "Files: $($actualFiles.Count)"
Write-Output ("Size: {0:N2} MiB" -f ($bytes / 1MB))
