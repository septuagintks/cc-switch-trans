[CmdletBinding()]
param(
    [string]$RuntimeBuildDirectory = "build-windows-runtime-release",
    [string]$QtStagingDirectory = "tmp/windows-qt-gui-stage",
    [string]$InstallerPath = "tmp/windows-installer-prototype/ccs-trans-0.8.0-Windows-x64-setup-prototype.exe",
    [string]$OutputPath = "benchmark-results/0.8-a-windows-resource-baseline.json",
    [int]$LifecycleIterations = 100,
    [switch]$WithTrayIcon
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
        throw "Required baseline input is missing: $fullPath"
    }
    return $fullPath
}

function Get-FreePort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Wait-ForPort([int]$Port,
                      [Diagnostics.Stopwatch]$Stopwatch,
                      [Diagnostics.Process]$Process) {
    while ($Stopwatch.ElapsedMilliseconds -lt 10000) {
        if ($Process.HasExited) {
            throw "Runtime exited before opening listener port $Port`: $($Process.ExitCode)"
        }
        $client = [Net.Sockets.TcpClient]::new()
        try {
            $task = $client.ConnectAsync("127.0.0.1", $Port)
            if ($task.Wait(100) -and $client.Connected) {
                return
            }
        } catch {
        } finally {
            $client.Dispose()
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Timed out waiting for listener port $Port"
}

function Invoke-Cli([string[]]$Arguments) {
    $output = & $cli @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "CLI command failed ($($Arguments -join ' ')):`n$($output -join [Environment]::NewLine)"
    }
}

function Write-BaselineDiagnostics([string]$HomeDirectory) {
    $applicationRoot = Join-Path $HomeDirectory ".ccs-trans"
    foreach ($name in @("logs/ccs-trans-host.log", "logs/ccs-trans.log", "config.json")) {
        $path = Join-Path $applicationRoot $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Write-Warning "Baseline diagnostic: $path"
            Get-Content -LiteralPath $path -ErrorAction Continue | Write-Warning
        }
    }
}

function Wait-ForMainWindow([Diagnostics.Process]$Process,
                            [Diagnostics.Stopwatch]$Stopwatch) {
    while ($Stopwatch.ElapsedMilliseconds -lt 10000) {
        if ($Process.HasExited) {
            throw "Process exited before creating its main window: $($Process.ExitCode)"
        }
        $Process.Refresh()
        if ($Process.MainWindowHandle -ne 0) {
            return
        }
        Start-Sleep -Milliseconds 10
    }
    throw "Timed out waiting for GUI main window"
}

function Measure-Process([Diagnostics.Process]$Process, [long]$ColdStartMilliseconds) {
    Start-Sleep -Seconds 2
    $Process.Refresh()
    $cpuStart = $Process.TotalProcessorTime
    Start-Sleep -Seconds 2
    $Process.Refresh()
    $cpuDeltaMilliseconds = ($Process.TotalProcessorTime - $cpuStart).TotalMilliseconds
    $modules = @(
        try {
            $Process.Modules | ForEach-Object { $_.ModuleName }
        } catch {
            @()
        }
    )
    return [ordered]@{
        cold_start_ms = $ColdStartMilliseconds
        working_set_bytes = $Process.WorkingSet64
        private_bytes = $Process.PrivateMemorySize64
        threads = $Process.Threads.Count
        handles = $Process.HandleCount
        idle_cpu_percent = [math]::Round(
            ($cpuDeltaMilliseconds / 2000.0 / [Environment]::ProcessorCount) * 100.0, 4)
        loaded_qt_dlls = @($modules | Where-Object { $_ -match '^Qt6.*[.]dll$' }).Count
    }
}

$runtimeBuild = Resolve-RepositoryPath $RuntimeBuildDirectory
$qtStage = Resolve-RepositoryPath $QtStagingDirectory
$installer = Resolve-RepositoryPath $InstallerPath
$output = Resolve-RepositoryPath $OutputPath $false
$cli = Join-Path $runtimeBuild "ccs-trans.exe"
$tray = Join-Path $runtimeBuild "ccs-trans-tray.exe"
$gui = Join-Path $qtStage "ccs-trans-gui.exe"
foreach ($required in @($cli, $tray, $gui, $installer)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Baseline executable is missing: $required"
    }
}

$testHome = Join-Path $repositoryRoot ("tmp/baseline-home-" + [Guid]::NewGuid().ToString("N"))
$savedUserProfile = $env:USERPROFILE
$savedInstanceSuffix = $env:CCS_TRANS_TRAY_TEST_INSTANCE_SUFFIX
$savedNoTrayIcon = $env:CCS_TRANS_TRAY_TEST_NO_ICON
$runtimeProcess = $null
$guiProcess = $null
$completed = $false

try {
    $env:USERPROFILE = $testHome
    $env:CCS_TRANS_TRAY_TEST_INSTANCE_SUFFIX = "baseline-" + [Guid]::NewGuid().ToString("N")
    if ($WithTrayIcon) {
        Remove-Item Env:CCS_TRANS_TRAY_TEST_NO_ICON -ErrorAction SilentlyContinue
    } else {
        $env:CCS_TRANS_TRAY_TEST_NO_ICON = "1"
    }
    $port = Get-FreePort
    $upstreamPort = Get-FreePort
    Invoke-Cli @("config", "set", "listener.port", $port)
    Invoke-Cli @("profile", "create", "baseline")
    Invoke-Cli @("profile", "set", "baseline", "protocol", "responses")
    Invoke-Cli @("profile", "set", "baseline", "local.request-path", "/v1/responses")
    Invoke-Cli @(
        "profile", "set", "baseline", "upstream.base-url", "http://127.0.0.1:$upstreamPort")
    Invoke-Cli @("profile", "set", "baseline", "upstream.request-path", "/v1/responses")
    Invoke-Cli @("profile", "enable", "baseline")

    $runtimeTimer = [Diagnostics.Stopwatch]::StartNew()
    $runtimeProcess = Start-Process -FilePath $tray -PassThru
    Wait-ForPort $port $runtimeTimer $runtimeProcess
    $runtimeColdStart = $runtimeTimer.ElapsedMilliseconds
    $runtimeSample = Measure-Process $runtimeProcess $runtimeColdStart
    if ($runtimeSample.loaded_qt_dlls -ne 0) {
        throw "The lightweight tray loaded Qt DLLs while the GUI was closed"
    }
    Stop-Process -Id $runtimeProcess.Id -Force
    $runtimeProcess.WaitForExit()
    $runtimeProcess = $null

    $guiTimer = [Diagnostics.Stopwatch]::StartNew()
    $guiProcess = Start-Process -FilePath $gui -WorkingDirectory $qtStage -PassThru
    Wait-ForMainWindow $guiProcess $guiTimer
    $guiColdStart = $guiTimer.ElapsedMilliseconds
    $guiSample = Measure-Process $guiProcess $guiColdStart
    if ($guiSample.loaded_qt_dlls -eq 0) {
        throw "Qt GUI baseline did not observe any loaded Qt DLLs"
    }
    Stop-Process -Id $guiProcess.Id -Force
    $guiProcess.WaitForExit()
    $guiProcess = $null

    $lifecycleFailures = 0
    $lifecycleTimer = [Diagnostics.Stopwatch]::StartNew()
    for ($iteration = 0; $iteration -lt $LifecycleIterations; ++$iteration) {
        $probe = Start-Process -FilePath $gui -WorkingDirectory $qtStage `
            -ArgumentList "--lifecycle-probe" -PassThru -Wait
        if ($probe.ExitCode -ne 0) {
            ++$lifecycleFailures
        }
    }
    $lifecycleTimer.Stop()

    $stageFiles = @(Get-ChildItem -LiteralPath $qtStage -Recurse -File)
    $result = [ordered]@{
        schema = "ccs-trans.0.8-a-resource-baseline/v1"
        captured_at = [DateTime]::UtcNow.ToString("o")
        os = [Environment]::OSVersion.VersionString
        logical_processors = [Environment]::ProcessorCount
        tray_icon_enabled = [bool]$WithTrayIcon
        runtime_tray = $runtimeSample
        qt_gui = $guiSample
        qt_gui_lifecycle = [ordered]@{
            iterations = $LifecycleIterations
            failures = $lifecycleFailures
            elapsed_ms = $lifecycleTimer.ElapsedMilliseconds
            remaining_processes = @(Get-Process -Name "ccs-trans-gui" -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -and $_.Path.StartsWith($qtStage) }).Count
        }
        qt_staging = [ordered]@{
            files = $stageFiles.Count
            bytes = ($stageFiles | Measure-Object -Property Length -Sum).Sum
        }
        installer = [ordered]@{
            bytes = (Get-Item -LiteralPath $installer).Length
        }
    }

    $outputDirectory = Split-Path -Parent $output
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $output -Encoding utf8
    $result | ConvertTo-Json -Depth 8
    if ($lifecycleFailures -ne 0 -or $result.qt_gui_lifecycle.remaining_processes -ne 0) {
        throw "Qt GUI lifecycle baseline did not return to zero processes"
    }
    $completed = $true
} catch {
    Write-BaselineDiagnostics $testHome
    throw
} finally {
    if ($runtimeProcess -and -not $runtimeProcess.HasExited) {
        Stop-Process -Id $runtimeProcess.Id -Force
    }
    if ($guiProcess -and -not $guiProcess.HasExited) {
        Stop-Process -Id $guiProcess.Id -Force
    }
    $env:USERPROFILE = $savedUserProfile
    $env:CCS_TRANS_TRAY_TEST_INSTANCE_SUFFIX = $savedInstanceSuffix
    $env:CCS_TRANS_TRAY_TEST_NO_ICON = $savedNoTrayIcon
    if ($completed -and (Test-Path -LiteralPath $testHome)) {
        Remove-Item -LiteralPath $testHome -Recurse -Force
    } elseif (Test-Path -LiteralPath $testHome) {
        Write-Warning "Preserved failed baseline home: $testHome"
    }
}
