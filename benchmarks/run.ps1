[CmdletBinding()]
param(
    [ValidateRange(1, 100)]
    [int] $Repetitions = 7,
    [ValidateRange(0, 10)]
    [int] $Warmups = 1,
    [ValidateRange(5, 3600)]
    [int] $TimeoutSeconds = 120,
    [string] $CMakePath,
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskSettingsPath = Join-Path $env:LOCALAPPDATA 'CMakeBuild\settings.ini'
$taskInvariant = [Globalization.CultureInfo]::InvariantCulture
$taskUtf8 = New-Object Text.UTF8Encoding($false)
$taskOutputRoot = Join-Path $taskRoot 'build\engine-tests\benchmarks'
$taskRunName = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + ([guid]::NewGuid().ToString('N').Substring(0, 8))
$taskOutputDirectory = Join-Path $taskOutputRoot $taskRunName
$taskRuns = New-Object 'Collections.Generic.List[object]'
$taskWarmupRuns = New-Object 'Collections.Generic.List[object]'
$taskSteps = New-Object 'Collections.Generic.List[object]'
$taskSettingsBefore = $null
$taskSettingsAfter = $null
$taskFailure = $null
$taskReport = $null

function Write-Utf8([string] $Path, [string] $Text) {
    [IO.File]::WriteAllText($Path, $Text, $taskUtf8)
}

function Get-SettingsFingerprint {
    if (Test-Path -LiteralPath $taskSettingsPath -PathType Leaf) {
        $taskSettingsFile = Get-Item -LiteralPath $taskSettingsPath
        return [ordered]@{
            exists = $true
            bytes = $taskSettingsFile.Length
            sha256 = (Get-FileHash -LiteralPath $taskSettingsPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    return [ordered]@{ exists = $false; bytes = 0; sha256 = $null }
}

# Start-Process joins ArgumentList with spaces. Quote each argument using the
# Windows CRT rules, including backslashes before embedded/trailing quotes.
function ConvertTo-WindowsArgument([string] $Value) {
    $taskQuoted = New-Object Text.StringBuilder
    [void] $taskQuoted.Append('"')
    $taskBackslashes = 0
    foreach ($taskCharacter in $Value.ToCharArray()) {
        if ($taskCharacter -eq '\') {
            ++$taskBackslashes
        } elseif ($taskCharacter -eq '"') {
            [void] $taskQuoted.Append(('\' * (2 * $taskBackslashes + 1)))
            [void] $taskQuoted.Append('"')
            $taskBackslashes = 0
        } else {
            [void] $taskQuoted.Append(('\' * $taskBackslashes))
            [void] $taskQuoted.Append($taskCharacter)
            $taskBackslashes = 0
        }
    }
    [void] $taskQuoted.Append(('\' * (2 * $taskBackslashes)))
    [void] $taskQuoted.Append('"')
    return $taskQuoted.ToString()
}

function Find-CMake {
    if ($CMakePath) {
        return (Resolve-Path -LiteralPath $CMakePath).Path
    }
    $taskCommand = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
    if ($taskCommand) { return $taskCommand.Source }
    foreach ($taskCandidate in @(
        'C:\Program Files\CMake\bin\cmake.exe',
        'C:\Qt\Tools\CMake_64\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )) {
        if (Test-Path -LiteralPath $taskCandidate -PathType Leaf) { return $taskCandidate }
    }
    throw 'CMake not found. Install CMake 3.24+ or use -CMakePath <cmake.exe>.'
}

function Stop-OwnedProcess([Diagnostics.Process] $Process, [switch] $ProcessTree) {
    if ($null -ne $Process -and !$Process.HasExited) {
        if ($ProcessTree) {
            # CMake may own MSBuild/compiler children. Windows PowerShell 5.1
            # has no Process.Kill(bool); target only this launched PID's tree.
            $taskTaskkill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
            $taskKiller = Start-Process -FilePath $taskTaskkill -ArgumentList @('/PID', [string] $Process.Id, '/T', '/F') `
                -WindowStyle Hidden -PassThru
            try {
                if (!$taskKiller.WaitForExit(10000)) {
                    $taskKiller.Kill()
                    throw "Timed out terminating the launched CMake process tree $($Process.Id)."
                }
            } finally {
                $taskKiller.Dispose()
            }
        } else {
            # GUI benchmark processes do not create child processes.
            $Process.Kill()
        }
        if (!$Process.WaitForExit(5000)) { throw "Could not stop launched process $($Process.Id)." }
    }
}

function Invoke-CMakeStep([string] $Name, [string] $WorkingDirectory, [string[]] $Arguments) {
    $taskStdoutPath = Join-Path $taskOutputDirectory ($Name + '.stdout.log')
    $taskStderrPath = Join-Path $taskOutputDirectory ($Name + '.stderr.log')
    $taskArgumentLine = ($Arguments | ForEach-Object { ConvertTo-WindowsArgument $_ }) -join ' '
    $taskProcess = $null
    $taskStepTimer = [Diagnostics.Stopwatch]::StartNew()
    $taskLastNotice = 0.0
    Write-Host "$Name..."
    try {
        $taskProcess = Start-Process -FilePath $script:taskCMakeExecutable -ArgumentList $taskArgumentLine `
            -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $taskStdoutPath -RedirectStandardError $taskStderrPath
        # Retain the OS process handle while it is alive; this also preserves
        # ExitCode access with the Start-Process object in PowerShell 5.1.
        [void] $taskProcess.Handle
        while (!$taskProcess.WaitForExit(1000)) {
            if ($taskStepTimer.Elapsed.TotalSeconds -ge 1800) { throw "$Name exceeded 30 minutes." }
            if ($taskStepTimer.Elapsed.TotalSeconds - $taskLastNotice -ge 25) {
                Write-Host "$Name is still running ($([int] $taskStepTimer.Elapsed.TotalSeconds) s)."
                $taskLastNotice = $taskStepTimer.Elapsed.TotalSeconds
            }
        }
        $taskProcess.WaitForExit()
        $taskExitCode = $taskProcess.ExitCode
        if ($taskExitCode -ne 0) {
            $taskErrorText = [IO.File]::ReadAllText($taskStderrPath)
            throw "$Name failed with exit code $taskExitCode. See $taskStdoutPath and $taskStderrPath.`n$taskErrorText"
        }
        $taskSteps.Add([ordered]@{
            name = $Name; executable = $script:taskCMakeExecutable; arguments = $Arguments
            working_directory = $WorkingDirectory; exit_code = $taskExitCode
            duration_ms = $taskStepTimer.Elapsed.TotalMilliseconds
            stdout_file = $taskStdoutPath; stderr_file = $taskStderrPath
        })
    } finally {
        Stop-OwnedProcess $taskProcess -ProcessTree
        if ($null -ne $taskProcess) { $taskProcess.Dispose() }
    }
}

function Get-Artifact([string] $Toolkit, [string] $Kind, [string] $Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [ordered]@{ toolkit = $Toolkit; kind = $Kind; path = $Path; exists = $false }
    }
    $taskFile = Get-Item -LiteralPath $Path
    return [ordered]@{
        toolkit = $Toolkit; kind = $Kind; path = $Path; exists = $true
        bytes = $taskFile.Length
        file_version = $taskFile.VersionInfo.FileVersion
        product_version = $taskFile.VersionInfo.ProductVersion
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        last_write_utc = $taskFile.LastWriteTimeUtc.ToString('o')
    }
}

function Get-SystemMetadata {
    $taskOs = Get-CimInstance Win32_OperatingSystem
    $taskSystem = Get-CimInstance Win32_ComputerSystem
    $taskProcessors = @(Get-CimInstance Win32_Processor)
    return [ordered]@{
        captured_at = [DateTimeOffset]::Now.ToString('o')
        captured_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
        windows = $taskOs.Caption; windows_version = $taskOs.Version; windows_build = $taskOs.BuildNumber
        architecture = $taskOs.OSArchitecture
        cpu = @($taskProcessors | ForEach-Object { $_.Name.Trim() })
        cpu_cores = ($taskProcessors | Measure-Object NumberOfCores -Sum).Sum
        cpu_logical_processors = ($taskProcessors | Measure-Object NumberOfLogicalProcessors -Sum).Sum
        ram_bytes = [double] $taskSystem.TotalPhysicalMemory
        powershell_version = $PSVersionTable.PSVersion.ToString()
        configuration = 'Release'; toolchain = 'MSVC'; platform = 'x64'
    }
}

function Assert-JsonProperties([object] $Value, [string[]] $Names, [string] $Context) {
    if ($null -eq $Value) { throw "Missing JSON object in $Context." }
    foreach ($taskPropertyName in $Names) {
        if ($null -eq $Value.PSObject.Properties[$taskPropertyName]) {
            throw "Missing JSON property '$taskPropertyName' in $Context."
        }
    }
}

function Invoke-Benchmark([string] $Toolkit, [string] $Executable, [string] $Kind, [int] $Index, [int] $Position) {
    $taskStem = '{0}-{1:D2}-{2:D2}-{3}' -f $Kind, $Index, $Position, $Toolkit.ToLowerInvariant()
    $taskJsonPath = Join-Path $taskOutputDirectory ($taskStem + '.json')
    $taskReadyPath = [IO.Path]::ChangeExtension($taskJsonPath, '.ready')
    $taskStdoutPath = Join-Path $taskOutputDirectory ($taskStem + '.stdout.log')
    $taskStderrPath = Join-Path $taskOutputDirectory ($taskStem + '.stderr.log')
    $taskProcess = $null
    $taskFirstFrame = $null
    $taskTimer = [Diagnostics.Stopwatch]::StartNew()
    $taskLastNotice = 0.0
    Write-Host "$Kind $Index/$($(if ($Kind -eq 'warmup') { $Warmups } else { $Repetitions })): $Toolkit"
    try {
        $taskProcess = Start-Process -FilePath $Executable -ArgumentList (ConvertTo-WindowsArgument $taskJsonPath) `
            -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $taskStdoutPath -RedirectStandardError $taskStderrPath
        [void] $taskProcess.Handle
        while ($true) {
            if ($null -eq $taskFirstFrame -and [IO.File]::Exists($taskReadyPath)) {
                $taskFirstFrame = $taskTimer.Elapsed.TotalMilliseconds
            }
            if ($taskProcess.HasExited) { break }
            if ($taskTimer.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                throw "$Toolkit exceeded the per-process timeout of $TimeoutSeconds seconds. Logs: $taskStem.*.log"
            }
            if ($taskTimer.Elapsed.TotalSeconds - $taskLastNotice -ge 25) {
                Write-Host "$Toolkit $Kind $Index is still running ($([int] $taskTimer.Elapsed.TotalSeconds) s)."
                $taskLastNotice = $taskTimer.Elapsed.TotalSeconds
            }
            if ($null -eq $taskFirstFrame) {
                Start-Sleep -Milliseconds 2
            } else {
                [void] $taskProcess.WaitForExit(100)
            }
        }
        $taskProcess.WaitForExit()
        $taskElapsed = $taskTimer.Elapsed.TotalMilliseconds
        $taskExitCode = $taskProcess.ExitCode
        $taskStderr = [IO.File]::ReadAllText($taskStderrPath)
        if ($taskExitCode -ne 0) { throw "$Toolkit exited with code $taskExitCode. $taskStderr" }
        if ($null -eq $taskFirstFrame) { throw "$Toolkit exited without its first-frame signal: $taskReadyPath" }
        if (![IO.File]::Exists($taskJsonPath)) { throw "$Toolkit produced no result JSON: $taskJsonPath" }
        $taskDocument = [IO.File]::ReadAllText($taskJsonPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
        Assert-JsonProperties $taskDocument @('schema_version', 'toolkit', 'app_version', 'toolkit_revision', 'compiler', 'configuration', 'startup_ms', 'metrics', 'metadata') $taskJsonPath
        if ($taskDocument.schema_version -ne 1 -or $taskDocument.toolkit -ne $Toolkit -or $taskDocument.configuration -ne 'Release') {
            throw "Unexpected benchmark schema/toolkit/configuration in $taskJsonPath."
        }
        $taskMetricNames = @{}
        foreach ($taskMetric in $taskDocument.metrics) {
            Assert-JsonProperties $taskMetric @('name', 'unit', 'value') ($taskJsonPath + ' metric')
            if ($taskMetricNames.ContainsKey([string] $taskMetric.name)) { throw "Duplicate metric in $taskJsonPath`: $($taskMetric.name)" }
            $taskMetricNames[[string] $taskMetric.name] = $true
            $taskMetricValue = [double] $taskMetric.value
            if ([double]::IsNaN($taskMetricValue) -or [double]::IsInfinity($taskMetricValue) -or $taskMetricValue -lt 0) {
                throw "Invalid metric in $taskJsonPath`: $($taskMetric.name)"
            }
        }
        foreach ($taskMetricName in $script:taskRequiredMetrics) {
            if (!$taskMetricNames.ContainsKey($taskMetricName)) { throw "Missing metric in $taskJsonPath`: $taskMetricName" }
        }
        if ($taskMetricNames.ContainsKey('startup_ms') -or $taskMetricNames.ContainsKey('process_first_frame_ms')) {
            throw "Reserved runner metric duplicated in $taskJsonPath."
        }
        $taskStartup = [double] $taskDocument.startup_ms
        if ([double]::IsNaN($taskStartup) -or [double]::IsInfinity($taskStartup) -or $taskStartup -lt 0) {
            throw "Invalid startup_ms in $taskJsonPath."
        }
        $taskMetrics = @($taskDocument.metrics) + @(
            [pscustomobject]@{ name = 'startup_ms'; unit = 'ms'; value = $taskStartup },
            [pscustomobject]@{ name = 'process_first_frame_ms'; unit = 'ms'; value = $taskFirstFrame }
        )
        return [ordered]@{
            toolkit = $Toolkit; kind = $Kind; pair = $Index; position = $Position
            exit_code = $taskExitCode; process_duration_ms = $taskElapsed
            process_first_frame_ms = $taskFirstFrame; metrics = $taskMetrics
            document = $taskDocument; result_file = $taskJsonPath
            stdout_file = $taskStdoutPath; stderr_file = $taskStderrPath; stderr = $taskStderr
        }
    } finally {
        Stop-OwnedProcess $taskProcess
        if ($null -ne $taskProcess) { $taskProcess.Dispose() }
    }
}

function Get-Median([double[]] $Values) {
    $taskSorted = @($Values | Sort-Object)
    $taskMiddle = [int] [Math]::Floor($taskSorted.Count / 2)
    if ($taskSorted.Count % 2) { return $taskSorted[$taskMiddle] }
    return ($taskSorted[$taskMiddle - 1] + $taskSorted[$taskMiddle]) / 2
}

function Get-P95([double[]] $Values) {
    $taskSorted = @($Values | Sort-Object)
    return $taskSorted[[Math]::Max(0, [int] [Math]::Ceiling(0.95 * $taskSorted.Count) - 1)]
}

function Format-Number($Value) {
    if ($null -eq $Value) { return 'n/a' }
    return ([double] $Value).ToString('0.###', $taskInvariant)
}

function Assert-ComparableWorkloads([object[]] $Runs) {
    $taskReference = $Runs[0].document
    $taskMetadataFields = @(
        'dpi', 'client_width', 'client_height', 'log_input_bytes', 'log_retained_bytes',
        'log_chunks', 'lines_per_chunk', 'redraw_iterations', 'theme_iterations', 'idle_requested_ms'
    )
    foreach ($taskRun in $Runs) {
        $taskDocument = $taskRun.document
        Assert-JsonProperties $taskDocument.metadata ($taskMetadataFields + @('idle_seconds', 'log_validated')) ($taskRun.result_file + ' metadata')
        if ($taskDocument.compiler -ne $taskReference.compiler -or $taskDocument.configuration -ne 'Release') {
            throw "Compiler/configuration mismatch in $($taskRun.result_file); ratios cannot be compared."
        }
        if ($taskDocument.metadata.log_validated -ne $true) {
            throw "The log workload was not validated in $($taskRun.result_file)."
        }
        foreach ($taskField in $taskMetadataFields) {
            $taskExpected = $taskReference.metadata.PSObject.Properties[$taskField].Value
            $taskActual = $taskDocument.metadata.PSObject.Properties[$taskField].Value
            if ([double]::IsNaN([double] $taskActual) -or [double]::IsInfinity([double] $taskActual) -or [double] $taskActual -le 0 -or $taskActual -ne $taskExpected) {
                throw "Controlled workload mismatch for $taskField in $($taskRun.result_file): expected $taskExpected, got $taskActual."
            }
        }
        if ($taskDocument.metadata.idle_requested_ms -ne 1500) {
            throw "Unexpected requested idle duration in $($taskRun.result_file): expected 1500 ms."
        }
        $taskActualIdleSeconds = [double] $taskDocument.metadata.idle_seconds
        if ([double]::IsNaN($taskActualIdleSeconds) -or [double]::IsInfinity($taskActualIdleSeconds) -or $taskActualIdleSeconds -lt 1.49 -or $taskActualIdleSeconds -gt 3) {
            throw "Invalid actual idle interval in $($taskRun.result_file): $taskActualIdleSeconds seconds; expected 1.49 to 3 seconds."
        }
    }
}

$taskRequiredMetrics = @(
    'idle_cpu_ms_per_second', 'idle_private_mib', 'idle_working_set_mib', 'idle_handles',
    'idle_gdi_objects', 'idle_user_objects', 'log_total_ms', 'log_cpu_ms', 'log_chunk_p50_ms',
    'log_chunk_p95_ms', 'log_chunk_max_ms', 'log_private_mib', 'log_working_set_mib',
    'redraw_p50_ms', 'redraw_p95_ms', 'theme_p50_ms', 'theme_p95_ms'
)

try {
    if ($env:OS -ne 'Windows_NT') { throw 'These GUI benchmarks require Windows.' }
    $taskSettingsBefore = Get-SettingsFingerprint
    [void] [IO.Directory]::CreateDirectory($taskOutputDirectory)
    $taskSystemMetadata = Get-SystemMetadata
    if (!$SkipBuild) {
        $taskCMakeExecutable = Find-CMake
        # Only benchmark targets: neither command publishes the product EXE.
        Invoke-CMakeStep 'configure-fltk' (Join-Path $taskRoot 'tests') @('--preset', 'msvc')
        Invoke-CMakeStep 'build-fltk' (Join-Path $taskRoot 'tests') @('--build', '--preset', 'msvc-release', '--target', 'FltkBench', '--parallel')
        Invoke-CMakeStep 'configure-nana' (Join-Path $taskRoot 'nana') @('--preset', 'msvc-tests')
        Invoke-CMakeStep 'build-nana' (Join-Path $taskRoot 'nana') @('--build', '--preset', 'msvc-tests-release', '--target', 'NanaBench', '--parallel')
    }
    $taskExecutables = @{
        FLTK = Join-Path $taskRoot 'build\engine-tests\Release\FltkBench.exe'
        Nana = Join-Path $taskRoot 'nana\build\engine-tests\tests\Release\NanaBench.exe'
    }
    foreach ($taskToolkit in @('FLTK', 'Nana')) {
        if (!(Test-Path -LiteralPath $taskExecutables[$taskToolkit] -PathType Leaf)) {
            throw "Benchmark executable is missing: $($taskExecutables[$taskToolkit]). Run without -SkipBuild."
        }
    }
    $taskArtifacts = @(
        Get-Artifact 'FLTK' 'production_exe' (Join-Path $taskRoot 'CMakeBuild.exe')
        Get-Artifact 'Nana' 'production_exe' (Join-Path $taskRoot 'nana\CMakeBuild.exe')
        Get-Artifact 'FLTK' 'benchmark_exe' $taskExecutables['FLTK']
        Get-Artifact 'Nana' 'benchmark_exe' $taskExecutables['Nana']
    )
    for ($taskPair = 1; $taskPair -le $Warmups; ++$taskPair) {
        $taskPosition = 0
        foreach ($taskToolkit in @('FLTK', 'Nana')) {
            $taskWarmupRuns.Add((Invoke-Benchmark $taskToolkit $taskExecutables[$taskToolkit] 'warmup' $taskPair (++$taskPosition)))
        }
    }
    for ($taskPair = 1; $taskPair -le $Repetitions; ++$taskPair) {
        # Alternate AB/BA to reduce the effect of drift and cache warming.
        $taskOrder = if ($taskPair % 2) { @('FLTK', 'Nana') } else { @('Nana', 'FLTK') }
        $taskPosition = 0
        foreach ($taskToolkit in $taskOrder) {
            $taskRuns.Add((Invoke-Benchmark $taskToolkit $taskExecutables[$taskToolkit] 'run' $taskPair (++$taskPosition)))
        }
    }
    Assert-ComparableWorkloads @($taskRuns.ToArray() + $taskWarmupRuns.ToArray())
    $taskSettingsAfter = Get-SettingsFingerprint
    if (($taskSettingsBefore | ConvertTo-Json -Compress) -ne ($taskSettingsAfter | ConvertTo-Json -Compress)) {
        throw 'The real settings.ini changed during the benchmark. Results are rejected; close other panels before rerunning.'
    }
    $taskSummary = New-Object 'Collections.Generic.List[object]'
    foreach ($taskMetricName in ($taskRequiredMetrics + @('startup_ms', 'process_first_frame_ms'))) {
        $taskPerToolkit = @{}
        $taskUnits = @()
        foreach ($taskToolkit in @('FLTK', 'Nana')) {
            $taskMatches = @($taskRuns | Where-Object { $_.toolkit -eq $taskToolkit } | ForEach-Object {
                $_.metrics | Where-Object { $_.name -eq $taskMetricName }
            })
            if ($taskMatches.Count -ne $Repetitions) { throw "Metric $taskMetricName has incomplete $taskToolkit samples." }
            $taskValues = @($taskMatches | ForEach-Object { [double] $_.value })
            $taskUnits += @($taskMatches | ForEach-Object { [string] $_.unit })
            $taskPerToolkit[$taskToolkit] = [ordered]@{
                median = Get-Median $taskValues; p95 = Get-P95 $taskValues
                min = ($taskValues | Measure-Object -Minimum).Minimum
                max = ($taskValues | Measure-Object -Maximum).Maximum
                samples = $taskValues
            }
        }
        $taskUniqueUnits = @($taskUnits | Select-Object -Unique)
        if ($taskUniqueUnits.Count -ne 1) { throw "Unit mismatch for $taskMetricName." }
        $taskRatio = if ($taskPerToolkit.FLTK.median -gt 0) { $taskPerToolkit.Nana.median / $taskPerToolkit.FLTK.median } else { $null }
        $taskSummary.Add([ordered]@{
            metric = $taskMetricName; unit = $taskUniqueUnits[0]; direction = 'smaller_is_better'
            fltk = $taskPerToolkit.FLTK; nana = $taskPerToolkit.Nana
            nana_over_fltk = $taskRatio
        })
    }
    $taskReport = [ordered]@{
        schema_version = 1; accepted = $true; benchmark = 'CMakeBuild FLTK vs Nana'; system = $taskSystemMetadata
        compiler = $taskRuns[0].document.compiler; controlled_workload = $taskRuns[0].document.metadata
        repetitions_per_toolkit = $Repetitions; warmups_per_toolkit = $Warmups
        per_process_timeout_seconds = $TimeoutSeconds; first_frame_poll_interval_ms = 2
        run_order = 'serial alternating FLTK/Nana and Nana/FLTK; warmups excluded from summary'
        aggregation = 'median and nearest-rank p95 across independent process runs; raw in-process samples retained'
        ratio = 'Nana / FLTK median; below 1 means Nana uses less time/resources for this workload'
        scope = 'Actual CMakeBuild GUI implementations with a controlled workload; no universal toolkit ranking'
        settings = [ordered]@{ path = $taskSettingsPath; before = $taskSettingsBefore; after = $taskSettingsAfter; unchanged = $true }
        artifacts = $taskArtifacts; build_steps = @($taskSteps.ToArray())
        summary = @($taskSummary.ToArray()); runs = @($taskRuns.ToArray()); warmups = @($taskWarmupRuns.ToArray())
    }
    Write-Utf8 (Join-Path $taskOutputDirectory 'report.json') ($taskReport | ConvertTo-Json -Depth 32)
    $taskCsvRows = foreach ($taskRow in $taskSummary) {
        [pscustomobject][ordered]@{
            metric = $taskRow.metric; unit = $taskRow.unit; direction = $taskRow.direction
            fltk_median = Format-Number $taskRow.fltk.median; nana_median = Format-Number $taskRow.nana.median
            nana_over_fltk = Format-Number $taskRow.nana_over_fltk
            fltk_p95 = Format-Number $taskRow.fltk.p95; nana_p95 = Format-Number $taskRow.nana.p95
            independent_processes_per_toolkit = $Repetitions
        }
    }
    Write-Utf8 (Join-Path $taskOutputDirectory 'report.csv') (($taskCsvRows | ConvertTo-Csv -NoTypeInformation) -join "`r`n")
    $taskMarkdown = New-Object 'Collections.Generic.List[string]'
    $taskMarkdown.Add('# CMakeBuild FLTK vs Nana')
    $taskMarkdown.Add('')
    $taskMarkdown.Add("Date: $($taskSystemMetadata.captured_at). $($taskSystemMetadata.windows) $($taskSystemMetadata.windows_build), $($taskSystemMetadata.architecture), $($taskSystemMetadata.cpu -join '; '), RAM $(Format-Number ($taskSystemMetadata.ram_bytes / 1GB)) GiB. MSVC x64 Release.")
    $taskMarkdown.Add('')
    $taskMarkdown.Add("$Repetitions independent processes per toolkit; $Warmups warmups per toolkit excluded. Serial alternating AB/BA order. All rows use **smaller is better**. Ratio is Nana / FLTK median: below 1 favors Nana, above 1 favors FLTK in this workload.")
    $taskMarkdown.Add('')
    $taskWorkload = $taskRuns[0].document.metadata
    $taskMarkdown.Add("Controlled workload: $($taskWorkload.client_width) x $($taskWorkload.client_height) physical client pixels at $($taskWorkload.dpi) DPI; $($taskWorkload.log_chunks) chunks x $($taskWorkload.lines_per_chunk) lines, $($taskWorkload.log_input_bytes) input bytes, $($taskWorkload.log_retained_bytes) retained bytes; $($taskWorkload.redraw_iterations) redraws, $($taskWorkload.theme_iterations) theme changes, $($taskWorkload.idle_requested_ms) ms requested idle. Compiler: $($taskRuns[0].document.compiler). Log content and equal controlled workload metadata were validated across every process.")
    $taskMarkdown.Add('')
    $taskMarkdown.Add('Actual idle_seconds is measured independently in each process, checked to be finite and within 1.49 to 3 seconds, and used to normalize idle CPU time. It may vary with OS scheduling; it is recorded in raw JSON rather than required to match exactly.')
    $taskMarkdown.Add('')
    $taskMarkdown.Add('| Metric | Unit | FLTK median | Nana median | Nana / FLTK | FLTK p95 | Nana p95 |')
    $taskMarkdown.Add('| --- | --- | ---: | ---: | ---: | ---: | ---: |')
    foreach ($taskRow in $taskSummary) {
        $taskMarkdown.Add("| $($taskRow.metric) | $($taskRow.unit) | $(Format-Number $taskRow.fltk.median) | $(Format-Number $taskRow.nana.median) | $(Format-Number $taskRow.nana_over_fltk) | $(Format-Number $taskRow.fltk.p95) | $(Format-Number $taskRow.nana.p95) |")
    }
    $taskMarkdown.Add('')
    $taskMarkdown.Add('P95 columns are nearest-rank percentiles across process results. With fewer than 20 processes, p95 is the maximum. Rows named p50/p95 are medians or p95 across each process''s own latency percentile, not pooled operation samples.')
    $taskMarkdown.Add('')
    $taskMarkdown.Add('startup_ms starts inside the benchmark entry point. process_first_frame_ms starts immediately before Start-Process and ends when a first-paint .ready file is observed; it includes process launching, file signalling, and nominal 2 ms polling (Windows scheduling may exceed that interval). It is not a cold disk-cache startup test.')
    $taskMarkdown.Add('')
    $taskMarkdown.Add('## Executable artifacts')
    $taskMarkdown.Add('')
    $taskMarkdown.Add('| Toolkit | Artifact | Bytes | File version | Product version | SHA256 |')
    $taskMarkdown.Add('| --- | --- | ---: | --- | --- | --- |')
    foreach ($taskArtifact in $taskArtifacts) {
        if ($taskArtifact.exists) {
            $taskMarkdown.Add("| $($taskArtifact.toolkit) | $($taskArtifact.kind) | $($taskArtifact.bytes) | $($taskArtifact.file_version) | $($taskArtifact.product_version) | $($taskArtifact.sha256) |")
        } else {
            $taskMarkdown.Add("| $($taskArtifact.toolkit) | $($taskArtifact.kind) | missing | - | - | - |")
        }
    }
    $taskMarkdown.Add('')
    $taskMarkdown.Add('Production EXE sizes describe the existing published programs; those programs were neither launched nor rebuilt. Benchmark EXEs contain timing instrumentation and do not represent distributable program sizes.')
    $taskMarkdown.Add('')
    $taskMarkdown.Add('The actual settings.ini fingerprint was identical before and after. Workload/DPI/dimensions/compiler and raw operation samples are recorded in each process JSON and report.json. Results compare these implementations on this machine; drawing models, text controls, caches, OS scheduling, and other running software affect them. They do not establish a universal toolkit ranking.')
    Write-Utf8 (Join-Path $taskOutputDirectory 'report.md') ($taskMarkdown -join "`r`n")
    Write-Host "Reports: $taskOutputDirectory"
    Write-Output (Join-Path $taskOutputDirectory 'report.md')
} catch {
    $taskFailure = $_
    if ([IO.Directory]::Exists($taskOutputDirectory)) {
        Write-Utf8 (Join-Path $taskOutputDirectory 'failure.txt') ($_ | Out-String)
    }
} finally {
    if ($null -ne $taskSettingsBefore) {
        $taskSettingsAfter = Get-SettingsFingerprint
        if ([IO.Directory]::Exists($taskOutputDirectory)) {
            $taskUnchanged = ($taskSettingsBefore | ConvertTo-Json -Compress) -eq ($taskSettingsAfter | ConvertTo-Json -Compress)
            Write-Utf8 (Join-Path $taskOutputDirectory 'settings-check.json') ([ordered]@{
                path = $taskSettingsPath; before = $taskSettingsBefore; after = $taskSettingsAfter; unchanged = $taskUnchanged
            } | ConvertTo-Json -Depth 8)
        }
        if (($taskSettingsBefore | ConvertTo-Json -Compress) -ne ($taskSettingsAfter | ConvertTo-Json -Compress)) {
            $taskFailure = 'The real settings.ini changed during the benchmark. Results are rejected; no settings were written by this runner.'
            if ($null -ne $taskReport) {
                $taskReport.accepted = $false
                $taskReport.settings.after = $taskSettingsAfter
                $taskReport.settings.unchanged = $false
                Write-Utf8 (Join-Path $taskOutputDirectory 'report.json') ($taskReport | ConvertTo-Json -Depth 32)
                Write-Utf8 (Join-Path $taskOutputDirectory 'report.md') ('# RESULTS REJECTED' + "`r`n`r`n" + $taskFailure)
            }
            Write-Utf8 (Join-Path $taskOutputDirectory 'failure.txt') ([string] $taskFailure)
        }
    }
}
if ($null -ne $taskFailure) { throw $taskFailure }
