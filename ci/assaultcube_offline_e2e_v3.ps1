param(
    [Parameter(Mandatory = $true)]
    [string]$CortexDir,

    [Parameter(Mandatory = $true)]
    [string]$AssaultCubeDir,

    [Parameter(Mandatory = $true)]
    [string]$ReportPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$CortexDir = (Resolve-Path $CortexDir).Path
$AssaultCubeDir = (Resolve-Path $AssaultCubeDir).Path
$reportDirectory = Split-Path -Parent $ReportPath
if ([string]::IsNullOrWhiteSpace($reportDirectory)) {
    $reportDirectory = (Get-Location).Path
    $ReportPath = Join-Path $reportDirectory $ReportPath
}
New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null

$script:Mcp = $null
$script:McpStderrTask = $null
$script:NextId = 1
$script:TranscriptPath = Join-Path $reportDirectory "cortex-mcp-transcript.log"
$mcpStderrPath = Join-Path $reportDirectory "cortex-mcp.stderr.txt"
$serverStdout = Join-Path $reportDirectory "assaultcube-server.stdout.txt"
$serverStderr = Join-Path $reportDirectory "assaultcube-server.stderr.txt"
Remove-Item $script:TranscriptPath,$mcpStderrPath,$serverStdout,$serverStderr -Force -ErrorAction SilentlyContinue

$server = $null
$serverPort = 28799
$firewallRules = @(
    "Cortex-AssaultCube-Offline-Out-$([Guid]::NewGuid().ToString('N'))",
    "Cortex-AssaultCube-Offline-In-$([Guid]::NewGuid().ToString('N'))"
)
$createdFirewallRules = @()

$report = [ordered]@{
    schema = "cortex.assaultcube.offline-e2e.v3"
    status = "running"
    started_at_utc = [DateTime]::UtcNow.ToString("o")
    assaultcube = [ordered]@{
        version = "1.3.0.2"
        process = "ac_server.exe"
        mode = "official dedicated server"
        network_mode = "network blocked by Windows Firewall; loopback bind requested; no masterserver option"
        port = $serverPort
    }
    cortex = [ordered]@{ mode = "targetless MCP"; tools = "all" }
    checks = [ordered]@{}
}

function Write-TranscriptLine([string]$Direction, [string]$Line) {
    Add-Content -LiteralPath $script:TranscriptPath -Encoding UTF8 -Value ("{0} {1}" -f $Direction, $Line)
}

function Read-McpResponse([int]$ExpectedId, [int]$TimeoutMs = 20000) {
    if ($null -eq $script:Mcp) { throw "Cortex MCP process is not running" }
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        $task = $script:Mcp.StandardOutput.ReadLineAsync()
        if (-not $task.Wait($remaining)) { throw "Timed out waiting for Cortex MCP response id=$ExpectedId" }
        $line = $task.Result
        if ($null -eq $line) { throw "Cortex MCP stdout closed while waiting for id=$ExpectedId" }
        Write-TranscriptLine "IN " $line
        try { $message = $line | ConvertFrom-Json }
        catch { throw "Cortex MCP emitted non-JSON stdout: $line" }

        $hasId = $message.PSObject.Properties.Name -contains "id"
        if ($hasId -and [string]$message.id -eq [string]$ExpectedId) { return $message }
        if ($message.PSObject.Properties.Name -contains "method") {
            Write-Host ("MCP notification: " + [string]$message.method)
            continue
        }
        if ($hasId) { throw "Unexpected Cortex MCP response id=$($message.id) while waiting for id=$ExpectedId" }
    }
    throw "Timed out waiting for Cortex MCP response id=$ExpectedId"
}

function Send-McpRequest([string]$Method, $Params = @{}, [int]$TimeoutMs = 20000) {
    $id = $script:NextId
    $script:NextId++
    $request = [ordered]@{ jsonrpc = "2.0"; id = $id; method = $Method; params = $Params }
    $json = $request | ConvertTo-Json -Depth 20 -Compress
    Write-TranscriptLine "OUT" $json
    $script:Mcp.StandardInput.WriteLine($json)
    $script:Mcp.StandardInput.Flush()
    return Read-McpResponse -ExpectedId $id -TimeoutMs $TimeoutMs
}

function Send-McpNotification([string]$Method, $Params = @{}) {
    $request = [ordered]@{ jsonrpc = "2.0"; method = $Method; params = $Params }
    $json = $request | ConvertTo-Json -Depth 20 -Compress
    Write-TranscriptLine "OUT" $json
    $script:Mcp.StandardInput.WriteLine($json)
    $script:Mcp.StandardInput.Flush()
}

function Invoke-CortexTool([string]$Name, $Arguments = @{}, [int]$TimeoutMs = 20000) {
    return Send-McpRequest -Method "tools/call" -Params @{ name = $Name; arguments = $Arguments } -TimeoutMs $TimeoutMs
}

function Assert-ToolSuccess($Response, [string]$Label) {
    if ($Response.PSObject.Properties.Name -contains "error") {
        throw "$Label returned JSON-RPC error: $($Response.error | ConvertTo-Json -Depth 8 -Compress)"
    }
    if (-not ($Response.PSObject.Properties.Name -contains "result")) { throw "$Label returned no MCP result" }
    if (($Response.result.PSObject.Properties.Name -contains "isError") -and $Response.result.isError) {
        throw "$Label returned tool error: $($Response.result.structuredContent | ConvertTo-Json -Depth 12 -Compress)"
    }
}

function Get-Structured($Response) {
    if ($null -eq $Response -or -not ($Response.PSObject.Properties.Name -contains "result")) { return $null }
    return $Response.result.structuredContent
}

function Get-RouteResult($Response) {
    $structured = Get-Structured $Response
    if ($null -eq $structured) { return $null }
    if ($structured.PSObject.Properties.Name -contains "result") { return $structured.result }
    return $structured
}

function Format-Hex([uint64]$Value) { return "0x{0:x}" -f $Value }
function Parse-Hex([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw "empty address" }
    if ($Value.StartsWith("0x", [StringComparison]::OrdinalIgnoreCase)) { return [Convert]::ToUInt64($Value.Substring(2), 16) }
    return [Convert]::ToUInt64($Value, 10)
}

function Read-TargetValue([uint64]$Address, [string]$Type) {
    $response = Invoke-CortexTool "memory_read" @{ address = (Format-Hex $Address); type = $Type }
    Assert-ToolSuccess $response "memory_read $Type @ $(Format-Hex $Address)"
    $result = Get-RouteResult $response
    if (-not $result.ok) { throw "memory_read failed at $(Format-Hex $Address)" }
    return $result.value
}

function Assert-ServerAlive([string]$Stage) {
    $server.Refresh()
    if ($server.HasExited) { throw "AssaultCube server exited during $Stage with code $($server.ExitCode)" }
}

function Resume-ThreadBestEffort([uint64]$ThreadId) {
    try {
        [void](Invoke-CortexTool "debug_continue" @{ thread_id = $ThreadId; mutation_permission = $true } 8000)
    } catch {
        Write-Warning "Best-effort continue failed for TID ${ThreadId}: $($_.Exception.Message)"
    }
}

try {
    $cortexExe = Join-Path $CortexDir "cortex.exe"
    $serverExe = Join-Path $AssaultCubeDir "bin_win32\ac_server.exe"
    foreach ($required in @($cortexExe,$serverExe,(Join-Path $CortexDir "runtime\x86\cortex_core.dll"),(Join-Path $CortexDir "runtime\x86\cortex_runtime_helper.exe"))) {
        if (-not (Test-Path -LiteralPath $required)) { throw "missing required test asset: $required" }
    }

    Get-ChildItem (Join-Path $CortexDir "runtime\x86") -Filter "cortex.mcp.*.token" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $CortexDir "runtime\x86\cortex.token") -Force -ErrorAction SilentlyContinue

    New-NetFirewallRule -DisplayName $firewallRules[0] -Direction Outbound -Program $serverExe -Action Block -Profile Any | Out-Null
    $createdFirewallRules += $firewallRules[0]
    New-NetFirewallRule -DisplayName $firewallRules[1] -Direction Inbound -Program $serverExe -Action Block -Profile Any | Out-Null
    $createdFirewallRules += $firewallRules[1]
    $report.checks.network_isolation = [ordered]@{ outbound_block = $true; inbound_block = $true; masterserver_option = $false }

    $serverArguments = @("-i127.0.0.1", "-f$serverPort", "-c2", "-LF5", "-LS5", "-V")
    $report.assaultcube.arguments = @($serverArguments)
    $server = Start-Process -FilePath $serverExe -ArgumentList $serverArguments -WorkingDirectory $AssaultCubeDir -RedirectStandardOutput $serverStdout -RedirectStandardError $serverStderr -PassThru
    Start-Sleep -Seconds 2
    Assert-ServerAlive "startup"
    $report.assaultcube.pid = [int64]$server.Id
    $report.checks.server_process_alive = $true

    $preAttachThreadIds = @($server.Threads | ForEach-Object { [uint64]$_.Id })
    if ($preAttachThreadIds.Count -lt 1) { throw "AssaultCube server has no visible pre-attach threads" }
    $report.checks.pre_attach_threads = $preAttachThreadIds.Count

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $cortexExe
    $startInfo.Arguments = "mcp --tools all"
    $startInfo.WorkingDirectory = $CortexDir
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    $script:Mcp = [System.Diagnostics.Process]::new()
    $script:Mcp.StartInfo = $startInfo
    if (-not $script:Mcp.Start()) { throw "failed to start Cortex targetless MCP" }
    $script:McpStderrTask = $script:Mcp.StandardError.ReadToEndAsync()

    $init = Send-McpRequest "initialize" @{ protocolVersion = "2025-11-25"; capabilities = @{}; clientInfo = @{ name = "assaultcube-offline-ci-v3"; version = "3" } }
    if ($init.result.protocolVersion -ne "2025-11-25") { throw "Cortex MCP negotiation failed" }
    Send-McpNotification "notifications/initialized"

    $targetlessTools = Send-McpRequest "tools/list" @{}
    $targetlessNames = @($targetlessTools.result.tools.name)
    foreach ($requiredTool in @("cortex_processes", "cortex_attach", "cortex_detach", "cortex_targets")) {
        if (-not ($targetlessNames -contains $requiredTool)) { throw "targetless MCP catalog is missing $requiredTool" }
    }
    $report.checks.targetless_catalog = $true

    $processResponse = Invoke-CortexTool "cortex_processes" @{ query = "ac_server.exe"; limit = 64 }
    Assert-ToolSuccess $processResponse "cortex_processes"
    $processResult = Get-Structured $processResponse
    $processRow = @($processResult.processes | Where-Object { [int64]$_.pid -eq [int64]$server.Id })
    if ($processRow.Count -ne 1) { throw "cortex_processes did not discover the running AssaultCube server" }
    if ([string]$processRow[0].architecture -ne "x86") { throw "AssaultCube was not detected as x86: $($processRow[0].architecture)" }
    if ($processRow[0].attached) { throw "AssaultCube appeared attached before cortex_attach" }
    $report.checks.process_discovery = [ordered]@{ architecture = "x86"; target_id = [string]$processRow[0].id }

    $attachResponse = Invoke-CortexTool "cortex_attach" @{ pid = [int64]$server.Id } 30000
    Assert-ToolSuccess $attachResponse "cortex_attach"
    $attachResult = Get-Structured $attachResponse
    if (-not $attachResult.ok -or [string]$attachResult.status -notin @("attached", "already_attached")) { throw "Cortex failed to attach AssaultCube" }
    Assert-ServerAlive "attach"
    $report.checks.attach = [string]$attachResult.status

    $attachedTools = Send-McpRequest "tools/list" @{}
    $attachedNames = @($attachedTools.result.tools.name)
    foreach ($requiredTool in @("status", "modules", "memory_read", "scan_aob", "disasm", "debug_threads", "debug_pause", "debug_step", "debug_continue")) {
        if (-not ($attachedNames -contains $requiredTool)) { throw "attached MCP catalog is missing $requiredTool" }
    }
    $report.checks.attached_catalog = $true

    $statusResponse = Invoke-CortexTool "status" @{}
    Assert-ToolSuccess $statusResponse "status"
    $statusResult = Get-RouteResult $statusResponse
    if ([int64]$statusResult.pid -ne [int64]$server.Id) { throw "Cortex status reports the wrong PID" }
    $report.checks.status_pid = [int64]$statusResult.pid

    $modulesResponse = Invoke-CortexTool "modules" @{}
    Assert-ToolSuccess $modulesResponse "modules"
    $modules = @(Get-RouteResult $modulesResponse)
    $moduleRows = @($modules | Where-Object { [string]$_.name -ieq "ac_server.exe" })
    if ($moduleRows.Count -ne 1) { throw "Cortex module list did not expose ac_server.exe" }
    $moduleBase = Parse-Hex ([string]$moduleRows[0].base)
    $moduleSize = [uint64]$moduleRows[0].size
    if ($moduleBase -eq 0 -or $moduleSize -lt 4096) { throw "invalid AssaultCube module range" }
    $report.checks.module = [ordered]@{ base = (Format-Hex $moduleBase); size = $moduleSize }

    $mz = [uint64](Read-TargetValue $moduleBase "u16")
    if ($mz -ne 0x5A4D) { throw "AssaultCube module does not begin with MZ" }
    $eLfanew = [uint64](Read-TargetValue ($moduleBase + 0x3c) "u32")
    if ($eLfanew -lt 0x40 -or $eLfanew -gt [Math]::Min([double]$moduleSize, 0x100000)) { throw "invalid PE e_lfanew: $eLfanew" }
    $peSignature = [uint64](Read-TargetValue ($moduleBase + $eLfanew) "u32")
    if ($peSignature -ne 0x00004550) { throw "AssaultCube PE signature mismatch" }
    $entryRva = [uint64](Read-TargetValue ($moduleBase + $eLfanew + 0x28) "u32")
    if ($entryRva -eq 0 -or $entryRva -ge $moduleSize) { throw "invalid PE entry RVA: $entryRva" }
    $entryAddress = $moduleBase + $entryRva
    $report.checks.pe_headers = [ordered]@{ mz = "0x5a4d"; pe = "0x4550"; entry = (Format-Hex $entryAddress) }

    $aobResponse = Invoke-CortexTool "scan_aob" @{ pattern = "4D 5A"; module = "ac_server.exe" } 30000
    Assert-ToolSuccess $aobResponse "scan_aob"
    $aobResult = Get-RouteResult $aobResponse
    $moduleBaseText = (Format-Hex $moduleBase).ToLowerInvariant()
    $aobAddresses = @($aobResult.addresses | ForEach-Object { ([string]$_).ToLowerInvariant() })
    if (-not ($aobAddresses -contains $moduleBaseText)) { throw "scan_aob did not find the PE MZ signature at the AssaultCube module base" }
    $report.checks.aob_scan = [ordered]@{ pattern = "4D 5A"; matches = $aobAddresses.Count; module_base_found = $true }

    $disasmResponse = Invoke-CortexTool "disasm" @{ _query = @{ address = (Format-Hex $entryAddress); count = 16 } }
    Assert-ToolSuccess $disasmResponse "disasm"
    $disasmResult = Get-RouteResult $disasmResponse
    $instructions = @($disasmResult.instructions)
    if (-not $disasmResult.ok -or $instructions.Count -lt 1) { throw "Cortex could not disassemble the AssaultCube entry point" }
    $report.checks.disassembly = [ordered]@{ instruction_count = $instructions.Count; first = [string]$instructions[0].text }

    $threadsResponse = Invoke-CortexTool "debug_threads" @{}
    Assert-ToolSuccess $threadsResponse "debug_threads"
    $threadsResult = Get-RouteResult $threadsResponse
    $debugThreadIds = @($threadsResult.thread_ids | ForEach-Object { [uint64]$_ })
    $candidateThreads = @($preAttachThreadIds | Where-Object { $debugThreadIds -contains [uint64]$_ })
    if ($candidateThreads.Count -lt 1) { throw "Cortex debugger did not expose any original AssaultCube thread" }
    $report.checks.debug_threads = [ordered]@{ total = $debugThreadIds.Count; original_candidates = $candidateThreads.Count }

    $permissionProbeTid = [uint64]$candidateThreads[0]
    $permissionProbe = Invoke-CortexTool "debug_pause" @{ thread_id = $permissionProbeTid }
    $permissionRoute = Get-RouteResult $permissionProbe
    $permissionDenied = ($permissionProbe.PSObject.Properties.Name -contains "result") -and ($permissionProbe.result.PSObject.Properties.Name -contains "isError") -and $permissionProbe.result.isError -and ($null -ne $permissionRoute) -and (-not $permissionRoute.ok) -and ([string]$permissionRoute.error -eq "mutation_permission_required")
    if (-not $permissionDenied) {

        if ($null -ne $permissionRoute -and $permissionRoute.ok) { Resume-ThreadBestEffort $permissionProbeTid }
        throw "debug_pause bypassed mutation_permission"
    }
    $report.checks.mutation_permission = $true

    $debugTid = [uint64]0
    $debugStepAddress = ""
    foreach ($tid in $candidateThreads) {
        $tid64 = [uint64]$tid
        $paused = $false
        try {
            $pauseResponse = Invoke-CortexTool "debug_pause" @{ thread_id = $tid64; mutation_permission = $true } 10000
            $pauseRoute = Get-RouteResult $pauseResponse
            if ($null -eq $pauseRoute -or -not $pauseRoute.ok) { continue }
            $paused = $true

            $stepResponse = Invoke-CortexTool "debug_step" @{ thread_id = $tid64; timeout_ms = 4000; mutation_permission = $true } 10000
            $stepRoute = Get-RouteResult $stepResponse
            if ($null -eq $stepRoute -or -not $stepRoute.ok) { continue }
            $paused = $true
            $registers = $stepRoute.registers
            $debugStepAddress = if ($registers.PSObject.Properties.Name -contains "eip") { [string]$registers.eip } else { [string]$registers.rip }

            $resumeResponse = Invoke-CortexTool "debug_continue" @{ thread_id = $tid64; mutation_permission = $true } 10000
            $resumeRoute = Get-RouteResult $resumeResponse
            if ($null -eq $resumeRoute -or -not $resumeRoute.ok) { throw "debug_continue failed after successful step on TID $tid64" }
            $paused = $false
            $debugTid = $tid64
            break
        } catch {
            Write-Warning "Debugger candidate TID $tid64 failed: $($_.Exception.Message)"
        } finally {
            if ($paused) { Resume-ThreadBestEffort $tid64 }
        }
    }
    if ($debugTid -eq 0) { throw "Cortex could not complete Pause -> Step -> Continue on any original AssaultCube thread" }
    Assert-ServerAlive "Pause/Step/Continue"
    $report.checks.debugger = [ordered]@{ thread_id = $debugTid; pause = $true; step = $true; continue = $true; step_ip = $debugStepAddress; target_alive = $true }

    $detachResponse = Invoke-CortexTool "cortex_detach" @{}
    Assert-ToolSuccess $detachResponse "cortex_detach"
    $detachResult = Get-Structured $detachResponse
    if (-not $detachResult.ok -or [string]$detachResult.status -ne "detached" -or [int]$detachResult.count -ne 0) { throw "Cortex detach did not return to targetless state" }
    Assert-ServerAlive "detach"

    $targetsAfterDetach = Invoke-CortexTool "cortex_targets" @{}
    Assert-ToolSuccess $targetsAfterDetach "cortex_targets after detach"
    $targetsAfterDetachResult = Get-Structured $targetsAfterDetach
    if ([int]$targetsAfterDetachResult.count -ne 0) { throw "cortex_targets still reports AssaultCube after detach" }

    $reattachResponse = Invoke-CortexTool "cortex_attach" @{ pid = [int64]$server.Id } 30000
    Assert-ToolSuccess $reattachResponse "cortex_attach reattach"
    $reattachResult = Get-Structured $reattachResponse
    if (-not $reattachResult.ok -or [string]$reattachResult.status -notin @("attached", "already_attached")) { throw "Cortex re-attach failed" }
    Assert-ServerAlive "re-attach"

    $statusAfterReattach = Invoke-CortexTool "status" @{}
    Assert-ToolSuccess $statusAfterReattach "status after re-attach"
    $statusAfterReattachResult = Get-RouteResult $statusAfterReattach
    if ([int64]$statusAfterReattachResult.pid -ne [int64]$server.Id) { throw "status after re-attach reports the wrong PID" }

    $finalDetach = Invoke-CortexTool "cortex_detach" @{}
    Assert-ToolSuccess $finalDetach "final cortex_detach"
    Assert-ServerAlive "final detach"
    $report.checks.detach_reattach = [ordered]@{ detach = $true; reattach = $true; final_detach = $true; target_alive = $true }

    $report.status = "PASS"
    $report.finished_at_utc = [DateTime]::UtcNow.ToString("o")
    Write-Host "AssaultCube offline real-target Cortex E2E PASSED"
} catch {
    $report.status = "FAIL"
    $report.error = $_.Exception.Message
    $report.finished_at_utc = [DateTime]::UtcNow.ToString("o")
    Write-Error $_
    throw
} finally {
    try {
        if ($null -ne $script:Mcp) {
            if (-not $script:Mcp.HasExited) {
                try { $script:Mcp.StandardInput.Close() } catch {}
                if (-not $script:Mcp.WaitForExit(5000)) { $script:Mcp.Kill($true) }
            }
            if ($null -ne $script:McpStderrTask) {
                try { $script:McpStderrTask.Wait(3000) | Out-Null; $script:McpStderrTask.Result | Set-Content -Encoding UTF8 $mcpStderrPath } catch {}
            }
        }
    } catch {}

    try {
        if ($null -ne $server) {
            $server.Refresh()
            if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
        }
    } catch {}

    foreach ($rule in $createdFirewallRules) {
        try { Remove-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue } catch {}
    }

    try { $report | ConvertTo-Json -Depth 20 | Set-Content -Encoding UTF8 -LiteralPath $ReportPath } catch {}
    if (Test-Path $serverStdout) { Write-Host "--- AssaultCube stdout ---"; Get-Content $serverStdout }
    if (Test-Path $serverStderr) { Write-Host "--- AssaultCube stderr ---"; Get-Content $serverStderr }
    if (Test-Path $mcpStderrPath) { Write-Host "--- Cortex MCP stderr ---"; Get-Content $mcpStderrPath }
    Write-Host "AssaultCube Cortex report: $ReportPath"
}
