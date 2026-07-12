[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$MagickPath = ""
)

$ErrorActionPreference = "Stop"
$expectedSizes = @(256, 128, 64, 48, 32, 24, 20, 16)

function Resolve-Magick([string]$ExplicitPath) {
    if ($ExplicitPath) {
        $resolved = Resolve-Path -LiteralPath $ExplicitPath -ErrorAction SilentlyContinue
        if ($resolved) {
            return $resolved.Path
        }
        throw "ImageMagick executable not found: $ExplicitPath"
    }

    $command = Get-Command magick -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $pathEntries = @(
        [Environment]::GetEnvironmentVariable("Path", "Machine"),
        [Environment]::GetEnvironmentVariable("Path", "User")
    ) -join ";"
    foreach ($entry in $pathEntries.Split(";", [StringSplitOptions]::RemoveEmptyEntries)) {
        $candidate = Join-Path $entry.Trim() "magick.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "ImageMagick 'magick' was not found. Install it and restart the terminal or pass -MagickPath."
}

$source = (Resolve-Path -LiteralPath $SourcePath).Path
$magick = Resolve-Magick $MagickPath
$output = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = [IO.Path]::GetDirectoryName($output)
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

$metadata = & $magick identify -format "%w %h %[channels]" $source
if ($LASTEXITCODE -ne 0) {
    throw "ImageMagick failed to inspect the canonical icon"
}
$parts = "$metadata".Trim().Split(" ", [StringSplitOptions]::RemoveEmptyEntries)
if ($parts.Count -lt 3 -or $parts[0] -ne "512" -or $parts[1] -ne "512") {
    throw "Canonical icon must be 512x512; found: $metadata"
}
if ($parts[2] -notmatch "a") {
    throw "Canonical icon must contain an alpha channel; found: $($parts[2])"
}

$alphaMeanText = & $magick $source -alpha extract -format "%[fx:mean]" info:
if ($LASTEXITCODE -ne 0 -or [double]::Parse(
        "$alphaMeanText",
        [Globalization.CultureInfo]::InvariantCulture) -le 0) {
    throw "Canonical icon contains no visible pixels"
}

$temporary = "$output.tmp.ico"
$outlineMask = "$output.tmp-mask.png"
$windowsSource = "$output.tmp-windows.png"
Remove-Item -LiteralPath $temporary, $outlineMask, $windowsSource -Force -ErrorAction SilentlyContinue
try {
    & $magick $source -alpha extract -morphology Dilate Disk:24 $outlineMask
    if ($LASTEXITCODE -ne 0) {
        throw "ImageMagick failed to generate the Windows icon outline mask"
    }
    & $magick -size 512x512 xc:white $outlineMask -alpha off `
        -compose CopyOpacity -composite $source -compose over -composite $windowsSource
    if ($LASTEXITCODE -ne 0) {
        throw "ImageMagick failed to generate the outlined Windows icon source"
    }

    $sizes = $expectedSizes -join ","
    & $magick $windowsSource -background none -define "icon:auto-resize=$sizes" $temporary
    if ($LASTEXITCODE -ne 0) {
        throw "ImageMagick failed to generate the Windows icon"
    }

    $frames = & $magick identify -format "%wx%h`n" $temporary
    if ($LASTEXITCODE -ne 0) {
        throw "ImageMagick failed to inspect the generated Windows icon"
    }
    $actualSizes = @($frames -split "\s+" | Where-Object { $_ } | ForEach-Object {
        if ($_ -notmatch "^(\d+)x(\d+)$" -or $Matches[1] -ne $Matches[2]) {
            throw "Generated ICO contains an invalid frame: $_"
        }
        [int]$Matches[1]
    })
    if (($actualSizes -join ",") -ne ($expectedSizes -join ",")) {
        throw "Generated ICO frame mismatch. Expected $sizes; found $($actualSizes -join ',')"
    }

    Move-Item -LiteralPath $temporary -Destination $output -Force
} finally {
    Remove-Item -LiteralPath $temporary, $outlineMask, $windowsSource -Force -ErrorAction SilentlyContinue
}

Write-Output "Generated Windows icon: $output"
