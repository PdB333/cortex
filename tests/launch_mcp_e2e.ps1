param(
    [Parameter(Mandatory = $true)][string]$HostPath,
    [Parameter(Mandatory = $true)][string]$TargetPath
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
function Assert-True([bool]$ok,[string]$message) { if (-not $ok) { throw "ASSERTION FAILED: $message" } }
function Invoke-Mcp($process,[int]$id,[string]$method,$parameters) {
    $message = @{ jsonrpc = "2.0"; id = $id; method = $method; params = $parameters } | ConvertTo-Json -Depth 20 -Compress
    $process.StandardInput.WriteLine($message)
    $process.StandardInput.Flush()
    $task = $process.StandardOutput.ReadLineAsync()
    if (-not $task.Wait(12000)) { throw "MCP response timeout for $method" }
    if ($null -eq $task.Result) { throw "MCP stdout closed unexpectedly" }
    try { $response = $task.Result | ConvertFrom-Json -Depth 30 }
    catch { throw "MCP protocol stdout contaminated during request $id ($method): $($task.Result)" }
    Assert-True ([int]$response.id -eq $id) "Unexpected MCP response id"
    Assert-True ($null -ne $response.result) "MCP returned protocol error"
    return $response.result
}
function Call-Tool($process,[int]$id,[string]$tool,$arguments) {
    return Invoke-Mcp $process $id "tools/call" @{ name = $tool; arguments = $arguments }
}
$hostExe = (Resolve-Path $HostPath).Path
$targetExe = (Resolve-Path $TargetPath).Path
$configPath = Join-Path ([System.IO.Path]::GetTempPath()) ("cortex-launch-" + [guid]::NewGuid().ToString("N") + ".json")
@{ profiles = @(@{
    name = "fixture"; executable = $targetExe; working_directory = (Split-Path -Parent $targetExe)
    allow_stop = $true; max_runs = 2; arguments = @()
}) } | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 $configPath
$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = $hostExe
$start.UseShellExecute = $false
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.CreateNoWindow = $true
foreach ($arg in @("mcp", "--tools", "compact", "--launch-config", $configPath)) {
    [void]$start.ArgumentList.Add($arg)
}
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $start
$lastPid = 0
try {
    Assert-True ($process.Start()) "Could not start Cortex MCP"
    [void](Invoke-Mcp $process 1 "initialize" @{ protocolVersion = "2025-11-25" })
    $list = Invoke-Mcp $process 2 "tools/list" @{}
    $names = @($list.tools | ForEach-Object { $_.name })
    foreach ($tool in @("cortex_launch", "cortex_launch_status", "cortex_stop")) {
        Assert-True ($names -contains $tool) "Missing host tool $tool"
    }
    $statuses = Call-Tool $process 3 "cortex_launch_status" @{}
    Assert-True ([bool]$statuses.structuredContent.configured) "Launch config not loaded"
    Assert-True (@($statuses.structuredContent.profiles).Count -eq 1) "Expected one profile"

    $denied = Call-Tool $process 4 "cortex_launch" @{ profile = "fixture"; mutation_permission = $false }
    Assert-True ([bool]$denied.isError) "Launch without permission succeeded"
    Assert-True ($denied.structuredContent.error.code -eq "mutation_permission_required") "Permission denial wrong"
    $unknown = Call-Tool $process 5 "cortex_launch" @{ profile = "unlisted"; mutation_permission = $true }
    Assert-True ([bool]$unknown.isError) "Unlisted launch profile accepted"

    $first = Call-Tool $process 6 "cortex_launch" @{ profile = "fixture"; mutation_permission = $true }
    Assert-True (-not [bool]$first.isError) "Configured launch failed"
    $lastPid = [int]$first.structuredContent.pid
    Assert-True ($lastPid -gt 0 -and [bool]$first.structuredContent.running) "Missing live launched PID"
    # The fixture writes a banner to stdout at startup. This ping must remain
    # valid JSON even after the child has had time to print it.
    Start-Sleep -Milliseconds 250
    [void](Invoke-Mcp $process 60 "ping" @{})
    $duplicate = Call-Tool $process 7 "cortex_launch" @{ profile = "fixture"; mutation_permission = $true }
    Assert-True ([bool]$duplicate.isError) "Concurrent launch accepted"

    $wrongPid = Call-Tool $process 8 "cortex_stop" @{ profile = "fixture"; pid = ($lastPid + 1); mutation_permission = $true }
    Assert-True ([bool]$wrongPid.isError) "Stopping unrelated PID accepted"
    $deniedStop = Call-Tool $process 9 "cortex_stop" @{ profile = "fixture"; pid = $lastPid; mutation_permission = $false }
    Assert-True ([bool]$deniedStop.isError) "Stopped target without permission"
    $stopped = Call-Tool $process 10 "cortex_stop" @{ profile = "fixture"; pid = $lastPid; mutation_permission = $true }
    Assert-True (-not [bool]$stopped.isError) "Authorized stop failed"
    Assert-True (-not [bool]$stopped.structuredContent.running) "Child still running after stop"
    $lastPid = 0

    $second = Call-Tool $process 11 "cortex_launch" @{ profile = "fixture"; mutation_permission = $true }
    Assert-True (-not [bool]$second.isError) "Second allowed run failed"
    $lastPid = [int]$second.structuredContent.pid
    $stoppedAgain = Call-Tool $process 12 "cortex_stop" @{ profile = "fixture"; pid = $lastPid; mutation_permission = $true }
    Assert-True (-not [bool]$stoppedAgain.isError) "Second stop failed"
    $lastPid = 0
    $exhausted = Call-Tool $process 13 "cortex_launch" @{ profile = "fixture"; mutation_permission = $true }
    Assert-True ([bool]$exhausted.isError) "Run budget not enforced"
    Assert-True ($exhausted.structuredContent.error.code -eq "launch_profile_run_limit_reached") "Wrong run limit error"

    Write-Host "PASS: allowlisted launch, status, stop, permissions, PID and run limit"
} finally {
    if ($lastPid -gt 0) { Stop-Process -Id $lastPid -Force -ErrorAction SilentlyContinue }
    if ($process -and -not $process.HasExited) {
        try { $process.StandardInput.Close() } catch {}
        if (-not $process.WaitForExit(3000)) {
            try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
        }
    }
    Remove-Item $configPath -ErrorAction SilentlyContinue
    if ($process) { $process.Dispose() }
}
