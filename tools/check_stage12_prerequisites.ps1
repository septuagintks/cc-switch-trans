[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$iconPath = Join-Path $repositoryRoot "assets/icons/ccs-trans-512.png"
$failures = [System.Collections.Generic.List[string]]::new()

function Test-CommandAvailable([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        [Console]::WriteLine("[missing] $Name")
        return $false
    }
    [Console]::WriteLine("[ok] $Name -> $($command.Source)")
    return $true
}

foreach ($name in @("cmake", "ninja", "magick")) {
    if (-not (Test-CommandAvailable $name)) {
        $failures.Add("required command is not available: $name")
    }
}

$hasCompiler = (Get-Command cl -ErrorAction SilentlyContinue) `
    -or (Get-Command g++ -ErrorAction SilentlyContinue) `
    -or (Get-Command clang++ -ErrorAction SilentlyContinue)
if ($hasCompiler) {
    Write-Output "[ok] C++ compiler"
} else {
    Write-Output "[missing] C++ compiler (cl, g++, or clang++)"
    $failures.Add("an ISO C++20 compiler is required")
}

$hasResourceCompiler = (Get-Command rc -ErrorAction SilentlyContinue) `
    -or (Get-Command windres -ErrorAction SilentlyContinue)
if ($hasResourceCompiler) {
    Write-Output "[ok] Windows resource compiler"
} else {
    Write-Output "[missing] Windows resource compiler (rc or windres)"
    $failures.Add("a Windows resource compiler is required")
}

if (-not (Test-Path -LiteralPath $iconPath -PathType Leaf)) {
    Write-Output "[missing] $iconPath"
    $failures.Add("canonical icon is missing")
} else {
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Bitmap]::FromFile($iconPath)
    try {
        $format = $image.PixelFormat.ToString()
        Write-Output "[ok] icon $($image.Width)x$($image.Height) $format"
        if ($image.Width -ne 512 -or $image.Height -ne 512) {
            $failures.Add("canonical icon must be 512x512")
        }
        if ($format -notmatch "Argb") {
            $failures.Add("canonical icon must contain an alpha channel")
        }
    } finally {
        $image.Dispose()
    }
}

if ($failures.Count -ne 0) {
    Write-Error ("Stage 12 prerequisites failed:`n- " + ($failures -join "`n- "))
}

Write-Output "Stage 12 prerequisites are available."
