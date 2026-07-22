[CmdletBinding()]
param(
    [string]$InstallerPath =
        "tmp/windows-installer-prototype/ccs-trans-0.8.0-Windows-x64-setup-prototype.exe",
    [string]$InstallDirectory = "tmp/windows-installer-smoke/install"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path

function Resolve-RepositoryPath([string]$Path, [bool]$MustExist = $true) {
    $fullPath = if ([IO.Path]::IsPathRooted($Path)) {
        [IO.Path]::GetFullPath($Path)
    } else {
        [IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
    }
    $prefix = $repositoryRoot.TrimEnd("\", "/") + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must stay inside the repository: $fullPath"
    }
    if ($MustExist -and -not (Test-Path -LiteralPath $fullPath)) {
        throw "Required installer smoke input is missing: $fullPath"
    }
    return $fullPath
}

function Invoke-And-Wait([string]$Executable, [string]$Arguments, [string]$Description) {
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -PassThru -Wait
    if ($process.ExitCode -ne 0) {
        throw "$Description failed with exit code $($process.ExitCode)"
    }
}

$installer = Resolve-RepositoryPath $InstallerPath
$installPath = Resolve-RepositoryPath $InstallDirectory $false
$smokeRoot = Split-Path -Parent $installPath
$manifest = Resolve-RepositoryPath "packaging/windows/qt-runtime-manifest.txt"
$installLog = Join-Path $smokeRoot "install.log"
$uninstallLog = Join-Path $smokeRoot "uninstall.log"

if (Test-Path -LiteralPath $smokeRoot) {
    Remove-Item -LiteralPath $smokeRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $smokeRoot | Out-Null

$installArguments =
    "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS " +
    "/DIR=`"$installPath`" /LOG=`"$installLog`""
Invoke-And-Wait $installer $installArguments "Prototype installation"

$missingFiles = @(
    Get-Content -LiteralPath $manifest |
        Where-Object { $_ -and -not $_.StartsWith("#") } |
        Where-Object { -not (Test-Path -LiteralPath (Join-Path $installPath $_) -PathType Leaf) }
)
if ($missingFiles.Count -ne 0) {
    throw "Installed tree is missing manifest files:`n$($missingFiles -join [Environment]::NewLine)"
}

$gui = Join-Path $installPath "ccs-trans-gui.exe"
Invoke-And-Wait $gui "--self-test --software-renderer" "Installed Qt GUI self-test"

$uninstaller = Join-Path $installPath "unins000.exe"
if (-not (Test-Path -LiteralPath $uninstaller -PathType Leaf)) {
    throw "Prototype uninstaller is missing: $uninstaller"
}
$uninstallArguments =
    "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=`"$uninstallLog`""
Invoke-And-Wait $uninstaller $uninstallArguments "Prototype uninstall"

$deadline = [DateTime]::UtcNow.AddSeconds(5)
while ((Test-Path -LiteralPath $installPath) -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
}
if (Test-Path -LiteralPath $installPath) {
    $remaining = @(Get-ChildItem -LiteralPath $installPath -Recurse -Force |
        Select-Object -ExpandProperty FullName)
    throw "Prototype uninstall left files behind:`n$($remaining -join [Environment]::NewLine)"
}

Write-Output "Installer prototype smoke passed"
Write-Output "Install log: $installLog"
Write-Output "Uninstall log: $uninstallLog"
