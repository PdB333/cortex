param(
    [Parameter(Mandatory = $true)]
    [string]$BuildRoot,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x86", "x64")]
    [string]$Architecture,

    [string]$ResultsRoot = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$BuildRoot = (Resolve-Path $BuildRoot).Path
if (-not $ResultsRoot) {
    $ResultsRoot = Join-Path $BuildRoot "e2e-results-$Architecture"
}
New-Item -ItemType Directory -Force -Path $ResultsRoot | Out-Null
$CrashRoot = Join-Path $ResultsRoot "crashes"
New-Item -ItemType Directory -Force -Path $CrashRoot | Out-Null

$HostExe = Join-Path $BuildRoot "cortex_host.exe"
$CoreDll = Join-Path $BuildRoot "cortex_core.dll"
$FakeModDll = Join-Path $BuildRoot "cortex_e2e_mod.dll"
$TargetExe = Join-Path $BuildRoot "cortex_test_target_$Architecture.exe"
$D3D9Exe = Join-Path $BuildRoot "cortex_test_target_d3d9_$Architecture.exe"
$D3D11Exe = Join-Path $BuildRoot "cortex_test_target_d3d11_$Architecture.exe"
$TokenPath = Join-Path $BuildRoot "cortex.token"
$ConfigPath = Join-Path $BuildRoot "cortex.ini"
$BaseUri = "http://127.0.0.1:6969"

foreach ($required in @($HostExe, $CoreDll, $FakeModDll, $TargetExe, $D3D9Exe, $D3D11Exe)) {
    if (-not (Test-Path $required)) {
        throw "Missing E2E artifact: $required"
    }
}

@"
diagnostics_enabled=true
diagnostics_write_minidump=true
diagnostics_crash_directory=$CrashRoot
diagnostics_symbolize=true
diagnostics_max_stack_frames=64
"@ | Set-Content -Encoding ascii $ConfigPath

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class CortexE2EWin32 {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern IntPtr OpenEvent(uint desiredAccess, bool inheritHandle, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetEvent(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);
}
"@

$script:Results = [System.Collections.Generic.List[object]]::new()
$script:Token = ""

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Wait-Until {
    param(
        [scriptblock]$Condition,
        [int]$TimeoutMs = 15000,
        [int]$PollMs = 100,
        [string]$Description = "condition"
    )
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        try {
            if (& $Condition) { return }
        } catch {}
        Start-Sleep -Milliseconds $PollMs
    }
    throw "Timed out waiting for $Description"
}

function Invoke-CortexRequest {
    param(
        [string]$Method,
        [string]$Path,
        $Body = $null,
        [string]$Token = $script:Token,
        [int[]]$ExpectedStatus = @(200),
        [string]$ContentType = "application/json"
    )

    $headers = @{}
    if ($Token) { $headers["X-Cortex-Token"] = $Token }
    $arguments = @{
        Uri = "$BaseUri$Path"
        Method = $Method
        Headers = $headers
        TimeoutSec = 10
        SkipHttpErrorCheck = $true
    }
    if ($null -ne $Body) {
        $arguments["ContentType"] = $ContentType
        $arguments["Body"] = if ($Body -is [string]) { $Body } else { $Body | ConvertTo-Json -Depth 12 -Compress }
    }

    $response = Invoke-WebRequest @arguments
    Assert-True ($ExpectedStatus -contains [int]$response.StatusCode) \
        "$Method $Path returned HTTP $($response.StatusCode), expected $($ExpectedStatus -join ',') body=$($response.Content)"

    $content = [string]$response.Content
    if (-not $content) { return $null }
    if ($response.Headers["Content-Type"] -match "application/json" -or $content.TrimStart().StartsWith("{") -or $content.TrimStart().StartsWith("[")) {
        return $content | ConvertFrom-Json
    }
    return $content
}

function Wait-CortexApi {
    Wait-Until -TimeoutMs 20000 -PollMs 200 -Description "Cortex /health" -Condition {
        $response = Invoke-WebRequest -Uri "$BaseUri/health" -TimeoutSec 1 -SkipHttpErrorCheck
        return $response.StatusCode -eq 200
    }

    Wait-Until -TimeoutMs 10000 -Description "cortex.token" -Condition { Test-Path $TokenPath }
    $script:Token = (Get-Content $TokenPath -Raw).Trim()
    Assert-True ($script:Token.Length -ge 32) "Generated Cortex token is too short"
}

function Wait-PortClosed {
    Wait-Until -TimeoutMs 10000 -PollMs 200 -Description "Cortex API shutdown" -Condition {
        try {
            $response = Invoke-WebRequest -Uri "$BaseUri/health" -TimeoutSec 1 -SkipHttpErrorCheck
            return $false
        } catch {
            return $true
        }
    }
}

function Inject-Dll {
    param([int]$Pid, [string]$Dll)
    $output = & $HostExe inject $Pid $Dll 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "Injection failed for $Dll into $Pid (exit $LASTEXITCODE): $output"
    }
}

function Signal-NamedEvent {
    param([string]$Name)
    $handle = [CortexE2EWin32]::OpenEvent(0x0002, $false, $Name)
    if ($handle -eq [IntPtr]::Zero) {
        throw "OpenEvent failed for $Name, win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    try {
        if (-not [CortexE2EWin32]::SetEvent($handle)) {
            throw "SetEvent failed for $Name, win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
    } finally {
        [void][CortexE2EWin32]::CloseHandle($handle)
    }
}

function Start-ControlledTarget {
    param([string]$Scenario)
    $scenarioRoot = Join-Path $ResultsRoot $Scenario
    New-Item -ItemType Directory -Force -Path $scenarioRoot | Out-Null
    $manifestPath = Join-Path $scenarioRoot "target-manifest.json"
    Remove-Item $manifestPath -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $TargetExe -ArgumentList "--e2e-manifest=$manifestPath" -PassThru
    Wait-Until -TimeoutMs 10000 -Description "$Scenario target manifest" -Condition { Test-Path $manifestPath }
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    Assert-True ([int]$manifest.pid -eq $process.Id) "Manifest PID does not match target PID"
    $expectedPointerSize = if ($Architecture -eq "x64") { 8 } else { 4 }
    Assert-True ([int]$manifest.pointer_size -eq $expectedPointerSize) "Target bitness mismatch"
    return [pscustomobject]@{ Process = $process; Manifest = $manifest; Root = $scenarioRoot }
}

function Stop-ControlledTarget {
    param($Target)
    if (-not $Target) { return }
    try {
        if (-not $Target.Process.HasExited) {
            try { Signal-NamedEvent $Target.Manifest.stop_event } catch {}
            if (-not $Target.Process.WaitForExit(3000)) {
                Stop-Process -Id $Target.Process.Id -Force -ErrorAction SilentlyContinue
            }
        }
    } finally {
        Wait-PortClosed
    }
}

function Stop-ProcessSafe {
    param($Process)
    if ($Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        [void]$Process.WaitForExit(3000)
    }
}

function Read-AddressNumber {
    param([string]$Address)
    return [Convert]::ToUInt64($Address.Substring(2), 16)
}

function Find-NewestCapture {
    param([string]$Prefix, [int]$Pid)
    $match = Get-ChildItem -Path $CrashRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "$Prefix*_$Pid*" -or $_.Name -like "$Prefix*" } |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if (-not $match) { throw "No $Prefix capture directory found for PID $Pid" }
    return $match.FullName
}

function Assert-NonEmptyFile {
    param([string]$Directory, [string]$Name)
    $path = Join-Path $Directory $Name
    Assert-True (Test-Path $path) "Missing report artifact: $path"
    Assert-True ((Get-Item $path).Length -gt 0) "Empty report artifact: $path"
}

function Run-Scenario {
    param([string]$Name, [scriptblock]$Body)
    Write-Host "`n=== E2E scenario: $Name ==="
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        & $Body
        $watch.Stop()
        $script:Results.Add([pscustomobject]@{
            name = $Name
            status = "passed"
            duration_ms = $watch.ElapsedMilliseconds
            error = ""
        })
        Write-Host "PASS $Name ($($watch.ElapsedMilliseconds) ms)"
    } catch {
        $watch.Stop()
        $message = $_.Exception.Message
        $script:Results.Add([pscustomobject]@{
            name = $Name
            status = "failed"
            duration_ms = $watch.ElapsedMilliseconds
            error = $message
        })
        Write-Warning "FAIL $Name: $message"
    }
}

Run-Scenario "api-memory-security" {
    $target = $null
    try {
        $target = Start-ControlledTarget "api-memory-security"
        Inject-Dll $target.Process.Id $CoreDll
        Wait-CortexApi

        $health = Invoke-CortexRequest GET "/health"
        Assert-True $health.ok "Health endpoint did not report ok"
        $status = Invoke-CortexRequest GET "/status"
        Assert-True ([int]$status.pid -eq $target.Process.Id) "Status endpoint PID mismatch"
        $tools = Invoke-CortexRequest GET "/tools"
        Assert-True (($tools | ConvertTo-Json -Depth 8) -match 'memory_read') "Tools manifest lacks memory_read"
        $openApi = Invoke-CortexRequest GET "/openapi.json"
        Assert-True (($openApi | ConvertTo-Json -Depth 12) -match '/memory/read') "OpenAPI lacks /memory/read"

        [void](Invoke-CortexRequest GET "/modules" -Token "" -ExpectedStatus @(401))
        [void](Invoke-CortexRequest GET "/modules" -Token "not-the-token" -ExpectedStatus @(401))
        [void](Invoke-CortexRequest POST "/memory/read" -Body '{"address":"0x1","type":"u8"}' -ContentType "text/plain" -ExpectedStatus @(415))

        $modules = Invoke-CortexRequest GET "/modules"
        $moduleText = $modules | ConvertTo-Json -Depth 8
        Assert-True ($moduleText -match 'cortex_core.dll') "Loaded modules omit cortex_core.dll"
        Assert-True ($moduleText -match "cortex_test_target_$Architecture.exe") "Loaded modules omit test target"

        $u32 = Invoke-CortexRequest POST "/memory/read" @{ address = $target.Manifest.u32; type = "u32" }
        Assert-True ($u32.ok -and [uint64]$u32.value -eq 0xDEADBEEF) "u32 memory read returned the wrong value"
        $string = Invoke-CortexRequest POST "/memory/read" @{ address = $target.Manifest.string; type = "string"; count = 32 }
        Assert-True ($string.ok -and $string.value -eq "cortex-canary") "String memory read returned the wrong value"

        $batch = Invoke-CortexRequest POST "/memory/read_batch" @{
            reads = @(
                @{ address = $target.Manifest.u32; type = "u32" },
                @{ address = $target.Manifest.u64; type = "u64" },
                @{ address = $target.Manifest.float; type = "float" }
            )
        }
        Assert-True ($batch.results.Count -eq 3) "Batch read did not return three entries"
        Assert-True (($batch.results | Where-Object { -not $_.ok }).Count -eq 0) "A batch read entry failed"

        $write = Invoke-CortexRequest POST "/memory/write" @{ address = $target.Manifest.health; type = "u32"; value = 777 }
        Assert-True $write.ok "Memory write failed"
        $written = Invoke-CortexRequest POST "/memory/read" @{ address = $target.Manifest.health; type = "u32" }
        Assert-True ([int]$written.value -eq 777) "Memory write was not observable"

        $freeze = Invoke-CortexRequest POST "/freeze" @{
            address = $target.Manifest.health
            type = "u32"
            value = 4242
            label = "e2e-health"
        }
        Assert-True ($freeze.ok -and [int]$freeze.id -gt 0) "Freeze creation failed"
        Start-Sleep -Milliseconds 800
        for ($sample = 0; $sample -lt 3; ++$sample) {
            $frozen = Invoke-CortexRequest POST "/memory/read" @{ address = $target.Manifest.health; type = "u32" }
            Assert-True ([int]$frozen.value -eq 4242) "Frozen value changed"
            Start-Sleep -Milliseconds 150
        }
        [void](Invoke-CortexRequest DELETE "/freeze/$($freeze.id)")
        Start-Sleep -Milliseconds 700
        $unfrozen = Invoke-CortexRequest POST "/memory/read" @{ address = $target.Manifest.health; type = "u32" }
        Assert-True ([int]$unfrozen.value -ne 4242) "Value did not resume changing after freeze removal"

        $start = Read-AddressNumber $target.Manifest.u32
        $end = $start + 4
        $scan = Invoke-CortexRequest POST "/scan/new" @{
            type = "u32"
            value = "3735928559"
            start = "0x$($start.ToString('x'))"
            end = "0x$($end.ToString('x'))"
            writable_only = $false
            alignment = 1
        }
        Assert-True ($scan.ok -and [int]$scan.count -ge 1) "Exact range scan did not find the canary"
        $scanResults = Invoke-CortexRequest GET "/scan/results/$($scan.scan_id)?limit=10"
        Assert-True ([int]$scanResults.total -ge 1) "Scan results are empty"
        [void](Invoke-CortexRequest DELETE "/scan/$($scan.scan_id)")

        $symbolModule = Invoke-CortexRequest GET "/symbols/module?address=$($target.Manifest.anchor)"
        Assert-True ($symbolModule.ok -and $symbolModule.module -match "cortex_test_target") "Symbol module resolution failed"

        $lua = Invoke-CortexRequest POST "/lua/exec" @{ code = "return 6 * 7"; timeout_ms = 2000 }
        Assert-True ($lua.ok -and [int]$lua.result -eq 42) "Lua execution failed"

        $mcpInit = Invoke-CortexRequest POST "/mcp" @{
            jsonrpc = "2.0"
            id = 1
            method = "initialize"
            params = @{
                protocolVersion = "2024-11-05"
                capabilities = @{}
                clientInfo = @{ name = "cortex-e2e"; version = "1.0" }
            }
        }
        Assert-True ($mcpInit.jsonrpc -eq "2.0" -and $null -ne $mcpInit.result) "MCP initialize failed"
        $mcpTools = Invoke-CortexRequest POST "/mcp" @{ jsonrpc = "2.0"; id = 2; method = "tools/list"; params = @{} }
        Assert-True ($mcpTools.result.tools.Count -gt 20) "MCP tools/list returned too few tools"

        $session = Invoke-CortexRequest POST "/session/export" @{}
        Assert-True ($session.ok -and $session.path) "Session export failed"
    } finally {
        Stop-ControlledTarget $target
    }
}

Run-Scenario "instrumented-mod-crash" {
    $target = $null
    $diagnose = $null
    try {
        Remove-Item (Join-Path $CrashRoot "*") -Recurse -Force -ErrorAction SilentlyContinue
        $target = Start-ControlledTarget "instrumented-mod-crash"
        Inject-Dll $target.Process.Id $CoreDll
        Wait-CortexApi
        Inject-Dll $target.Process.Id $FakeModDll
        Start-Sleep -Seconds 2

        $diagnoseLog = Join-Path $target.Root "diagnose.log"
        $diagnose = Start-Process -FilePath $HostExe -ArgumentList @(
            "diagnose", "--pid", $target.Process.Id,
            "--output", $CrashRoot,
            "--heartbeat", "render",
            "--hang-ms", "1500",
            "--poll-ms", "100",
            "--once"
        ) -RedirectStandardOutput $diagnoseLog -RedirectStandardError (Join-Path $target.Root "diagnose-error.log") -PassThru
        Start-Sleep -Milliseconds 700
        Signal-NamedEvent $target.Manifest.crash_event

        Assert-True ($target.Process.WaitForExit(20000)) "Crash target did not exit"
        Assert-True ($diagnose.WaitForExit(20000)) "External diagnostics host did not finish crash capture"
        Assert-True ($diagnose.ExitCode -eq 0) "External diagnostics host failed during crash capture"

        $capture = Find-NewestCapture "crash_" $target.Process.Id
        foreach ($file in @(
            "crash.dmp", "external_crash.dmp", "report.json", "breadcrumbs.json",
            "mods.json", "scopes.json", "values.json", "hooks.json",
            "stack.json", "build_info.json", "report.txt", "external_report.json",
            "analysis.json", "analysis.txt"
        )) { Assert-NonEmptyFile $capture $file }

        $report = Get-Content (Join-Path $capture "report.json") -Raw
        Assert-True ($report -match 'C0000005|EXCEPTION_ACCESS_VIOLATION') "Crash report lacks access violation evidence"
        $mods = Get-Content (Join-Path $capture "mods.json") -Raw
        Assert-True ($mods -match 'CortexE2EFakeMod') "Crash report lacks fake mod registration"
        $scopes = Get-Content (Join-Path $capture "scopes.json") -Raw
        Assert-True ($scopes -match 'E2EWorker') "Crash report lacks active fake mod scope"
        $values = Get-Content (Join-Path $capture "values.json") -Raw
        Assert-True ($values -match 'e2e_counter' -and $values -match 'fixture_ready') "Crash report lacks fake mod values"
        $hooks = Get-Content (Join-Path $capture "hooks.json") -Raw
        Assert-True ($hooks -match 'E2EPrimaryHook' -and $hooks -match 'E2EOverlappingHook') "Crash report lacks registered hooks"
        Assert-True ($hooks -match 'overlap|installed_bytes_changed|jump_target_mismatch') "Hook corruption/conflict was not detected"
        $analysis = Get-Content (Join-Path $capture "analysis.json") -Raw
        Assert-True ($analysis -match 'null_dereference') "Analyzer missed the null dereference"
        Assert-True ($analysis -match 'overlapping_hooks|hook_replaced|recursive_hook') "Analyzer missed hook evidence"
    } finally {
        Stop-ProcessSafe $diagnose
        Stop-ProcessSafe $(if ($target) { $target.Process } else { $null })
        try { Wait-PortClosed } catch {}
    }
}

Run-Scenario "external-hang-watchdog" {
    $target = $null
    $diagnose = $null
    try {
        Remove-Item (Join-Path $CrashRoot "*") -Recurse -Force -ErrorAction SilentlyContinue
        $target = Start-ControlledTarget "external-hang-watchdog"
        Inject-Dll $target.Process.Id $CoreDll
        Wait-CortexApi
        Inject-Dll $target.Process.Id $FakeModDll
        Start-Sleep -Seconds 2

        $diagnose = Start-Process -FilePath $HostExe -ArgumentList @(
            "diagnose", "--pid", $target.Process.Id,
            "--output", $CrashRoot,
            "--heartbeat", "render",
            "--hang-ms", "1200",
            "--poll-ms", "100",
            "--once"
        ) -RedirectStandardOutput (Join-Path $target.Root "diagnose.log") -RedirectStandardError (Join-Path $target.Root "diagnose-error.log") -PassThru
        Start-Sleep -Milliseconds 600
        Signal-NamedEvent $target.Manifest.hang_event

        Assert-True ($diagnose.WaitForExit(20000)) "External diagnostics host did not finish hang capture"
        Assert-True ($diagnose.ExitCode -eq 0) "External diagnostics host failed during hang capture"
        $capture = Find-NewestCapture "hang_" $target.Process.Id
        foreach ($file in @("hang.dmp", "threads.json", "hang_report.json", "analysis.json", "analysis.txt")) {
            Assert-NonEmptyFile $capture $file
        }
        $hang = Get-Content (Join-Path $capture "hang_report.json") -Raw
        Assert-True ($hang -match '"kind":"hang"' -and $hang -match '"responsive":false') "Hang report did not confirm an unresponsive window"
        $threads = Get-Content (Join-Path $capture "threads.json") -Raw
        Assert-True ($threads -match 'instruction_pointer' -and $threads -match 'thread_id') "Thread capture lacks control contexts"
        $analysis = Get-Content (Join-Path $capture "analysis.json") -Raw
        Assert-True ($analysis -match 'hang_snapshot') "Analyzer missed the hang snapshot"
    } finally {
        Stop-ProcessSafe $diagnose
        Stop-ProcessSafe $(if ($target) { $target.Process } else { $null })
        try { Wait-PortClosed } catch {}
    }
}

foreach ($renderer in @(
    @{ Name = "d3d9-renderer"; Exe = $D3D9Exe },
    @{ Name = "d3d11-renderer"; Exe = $D3D11Exe }
)) {
    Run-Scenario $renderer.Name {
        $process = $null
        try {
            $process = Start-Process -FilePath $renderer.Exe -PassThru
            Start-Sleep -Seconds 2
            Assert-True (-not $process.HasExited) "$($renderer.Name) target exited before injection"
            Inject-Dll $process.Id $CoreDll
            Wait-CortexApi

            $screenshotPath = Join-Path $ResultsRoot "$($renderer.Name).png"
            $captured = $false
            for ($attempt = 0; $attempt -lt 30 -and -not $captured; ++$attempt) {
                $response = Invoke-WebRequest -Uri "$BaseUri/screenshot?mode=auto" -Headers @{ "X-Cortex-Token" = $script:Token } -TimeoutSec 3 -SkipHttpErrorCheck
                if ($response.StatusCode -eq 200 -and $response.RawContentStream.Length -gt 64) {
                    [IO.File]::WriteAllBytes($screenshotPath, $response.Content)
                    $captured = $true
                    break
                }
                Start-Sleep -Milliseconds 300
            }
            Assert-True $captured "$($renderer.Name) screenshot was not captured"
            $bytes = [IO.File]::ReadAllBytes($screenshotPath)
            Assert-True ($bytes.Length -gt 64 -and $bytes[0] -eq 0x89 -and $bytes[1] -eq 0x50 -and $bytes[2] -eq 0x4E -and $bytes[3] -eq 0x47) \
                "$($renderer.Name) screenshot is not a PNG"
        } finally {
            Stop-ProcessSafe $process
            try { Wait-PortClosed } catch {}
        }
    }
}

Run-Scenario "repeated-injection-cycles" {
    for ($cycle = 1; $cycle -le 3; ++$cycle) {
        $target = $null
        try {
            $target = Start-ControlledTarget "cycle-$cycle"
            Inject-Dll $target.Process.Id $CoreDll
            Wait-CortexApi
            $health = Invoke-CortexRequest GET "/health"
            Assert-True $health.ok "Cycle $cycle health check failed"
        } finally {
            Stop-ControlledTarget $target
        }
    }
}

$passed = @($script:Results | Where-Object status -eq "passed").Count
$failed = @($script:Results | Where-Object status -eq "failed").Count
$summary = [pscustomobject]@{
    schema_version = 1
    architecture = $Architecture
    build_root = $BuildRoot
    generated_utc = [DateTime]::UtcNow.ToString("o")
    passed = $passed
    failed = $failed
    scenarios = $script:Results
}
$summaryPath = Join-Path $ResultsRoot "e2e-summary.json"
$summary | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 $summaryPath

$junitPath = Join-Path $ResultsRoot "e2e-junit.xml"
$xml = [System.Text.StringBuilder]::new()
[void]$xml.AppendLine("<?xml version=\"1.0\" encoding=\"utf-8\"?>")
[void]$xml.AppendLine("<testsuite name=\"cortex-e2e-$Architecture\" tests=\"$($script:Results.Count)\" failures=\"$failed\">")
foreach ($result in $script:Results) {
    $name = [Security.SecurityElement]::Escape($result.name)
    [void]$xml.Append("  <testcase name=\"$name\" time=\"$([Math]::Round($result.duration_ms / 1000.0, 3))\">")
    if ($result.status -eq "failed") {
        $errorText = [Security.SecurityElement]::Escape($result.error)
        [void]$xml.Append("<failure message=\"$errorText\">$errorText</failure>")
    }
    [void]$xml.AppendLine("</testcase>")
}
[void]$xml.AppendLine("</testsuite>")
$xml.ToString() | Set-Content -Encoding utf8 $junitPath

Write-Host "`nCortex E2E result: $passed passed, $failed failed"
Write-Host "Summary: $summaryPath"
if ($failed -ne 0) { exit 1 }
