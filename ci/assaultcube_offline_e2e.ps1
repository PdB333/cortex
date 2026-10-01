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
$serverStdout = Join-Path $reportDirectory "assaultcube-server.stdout.txt"
$serverStderr = Join-Path $reportDirectory "assaultcube-server.stderr.txt"
$mcpStderrPath = Join-Path $reportDirectory "cortex-mcp.stderr.txt"
Remove-Item $script:TranscriptPath,$serverStdout,$serverStderr,$mcpStderrPath -Force -ErrorAction SilentlyContinue

$server = $null
$firewallRuleName = "Cortex-AssaultCube-Offline-$([Guid]::NewGuid().ToString('N'))"
$firewallRuleCreated = $false
$serverPort = 28799

$report = [ordered]@{
    schema = "cortex.assaultcube.offline-e2e.v1"
    status = "running"
    started_at_utc = [DateTime]::UtcNow.ToString("o")
    assaultcube = [ordered]@{
        version = "1.3.0.2"
        process = "ac_server.exe"
        mode = "official dedicated server"
        network_mode = "loopback-only; outbound blocked by Windows Firewall; no -m masterserver option"
        port = $serverPort
    }
    cortex = [ordered]@{
        mode = "targetless MCP"
        tools = "all"
    }
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
        if (-not $task.Wait($remaining)) {
            throw "Timed out waiting for Cortex MCP response id=$ExpectedId"
        }
        $line = $task.Result
        if ($null -eq $line) {
            throw "Cortex MCP stdout closed while waiting for id=$ExpectedId"
        }
        Write-TranscriptLine "IN " $line
        try {
            $message = $line | ConvertFrom-Json
        } catch {
            throw "Cortex MCP emitted non-JSON stdout: $line"
        }

        $hasId = $message.PSObject.Properties.Name -contains "id"
        if ($hasId -and [string]$message.id -eq [string]$ExpectedId) {
            return $message
        }

        if ($message.PSObject.Properties.Name -contains "method") {
            Write-Host ("MCP notification: " + [string]$message.method)
            continue
        }

        if ($hasId) {
            throw "Unexpected Cortex MCP response id=$($message.id) while waiting for id=$ExpectedId"
        }
    }
    throw "Timed out waiting for Cortex MCP response id=$ExpectedId"
}

function Send-McpRequest([string]$Method, $Params = @{}, [int]$TimeoutMs = 20000) {
    $id = $script:NextId
    $script:NextId++
    $request = [ordered]@{
        jsonrpc = "2.0"
        id = $id
        method = $Method
        params = $Params
    }
    $json = $request | ConvertTo-Json -Depth 20 -Compress
    Write-TranscriptLine "OUT" $json
    $script:Mcp.StandardInput.WriteLine($json)
    $script:Mcp.StandardInput.Flush()
    return Read-McpResponse -ExpectedId $id -TimeoutMs $TimeoutMs
}

function Send-McpNotification([string]$Method, $Params = @{}) {
    $request = [ordered]@{
        jsonrpc = "2.0"
        method = $Method
        params = $Params
    }
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
    if (-not ($Response.PSObject.Properties.Name -contains "result")) {
        throw "$Label returned no MCP result"
    }
    if (($Response.result.PSObject.Properties.Name -contains "isError") -and $Response.result.isError) {
        throw "$Label returned tool error: $($Response.result.structuredContent | ConvertTo-Json -Depth 10 -Compress)"
    }
}

function Get-ToolStructured($Response) {
    if (-not ($Response.PSObject.Properties.Name -contains "result")) { return $null }
    return $Response.result.structuredContent
}

function Get-RouteResult($Response) {
    $structured = Get-ToolStructured $Response
    if ($null -eq $structured) { return $null }
    if ($structured.PSObject.Properties.Name -contains "result") {
        return $structured.result
    }
    return $structured
}

function Convert-HexAddressToUInt64([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw "empty address" }
    if ($Value.StartsWith("0x", [StringComparison]::OrdinalIgnoreCase)) {
        return [Convert]::ToUInt64($Value.Substring(2), 16)
    }
    return [Convert]::ToUInt64($Value, 10)
}

function Format-HexAddress([uint64]$Value) {
    return "0x{0:x}" -f $Value
}

function Read-TargetValue([uint64]$Address, [string]$Type) {
    $response = Invoke-CortexTool "memory_read" @{ address = (Format-HexAddress $Address); type = $Type }
    Assert-ToolSuccess $response "memory_read $Type @ $(Format-HexAddress $Address)"
    $result = Get-RouteResult $response
    if (-not $result.ok) { throw "memory_read failed at $(Format-HexAddress $Address)" }
    return $result.value
}

function Assert-ServerAlive([string]$Stage) {
    $server.Refresh()
    if ($server.HasExited) {
        throw "AssaultCube server exited during $Stage with code $($server.ExitCode)"
    }
}

try {
    $cortexExe = Join-Path $CortexDir "cortex.exe"
    $serverExe = Join-Path $AssaultCubeDir "bin_win32\ac_server.exe"
    foreach ($required in @($cortexExe, $serverExe, (Join-Path $CortexDir "runtime\x86\cortex_core.dll"), (Join-Path $CortexDir "runtime\x86\cortex_runtime_helper.exe"))) {
        if (-not (Test-Path -LiteralPath $required)) { throw "missing required test asset: $required" }
    }

    Get-ChildItem (Join-Path $CortexDir "runtime\x86") -Filter "cortex.mcp.*.token" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $CortexDir "runtime\x86\cortex.token") -Force -ErrorAction SilentlyContinue

    New-NetFirewallRule -DisplayName $firewallRuleName -Direction Outbound -Program $serverExe -Action Block -Profile Any | Out-Null
    $firewallRuleCreated = $true
    $report.checks.firewall_outbound_block = $true

    $serverArguments = @(
        "-i127.0.0.1",
        "-f$serverPort",
        "-c2",
        "-LF5",
        "-LS5",
        "-V"
    )
    $server = Start-Process -FilePath $serverExe -ArgumentList $serverArguments -WorkingDirectory $AssaultCubeDir `
        -RedirectStandardOutput $serverStdout -RedirectStandardError $serverStderr -PassThru

    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        Start-Sleep -Milliseconds 100
        $server.Refresh()
        if ($server.HasExited) {
            throw "AssaultCube server exited during startup with code $($server.ExitCode)"
        }
        $endpoint = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object {
            [int64]$_.OwningProcess -eq [int64]$server.Id -and [int]$_.LocalPort -eq $serverPort
        })
        if ($endpoint.Count -gt 0) { break }
    }
    if ($endpoint.Count -lt 1) { throw "AssaultCube server never exposed its local UDP endpoint" }
    $nonLoopback = @($endpoint | Where-Object { $_.LocalAddress -ne "127.0.0.1" -and $_.LocalAddress -ne "::1" })
    if ($nonLoopback.Count -gt 0) {
        throw "AssaultCube server bound a non-loopback address: $($nonLoopback.LocalAddress -join ', ')"
    }
    $report.assaultcube.pid = [int64]$server.Id
    $report.checks.loopback_binding = @($endpoint.LocalAddress)

    $preAttachThreadIds = @($server.Threads | ForEach-Object { [uint64]$_.Id })
    if ($preAttachThreadIds.Count -lt 1) { throw "AssaultCube server has no visible pre-attach threads" }

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

    $init = Send-McpRequest "initialize" @{
        protocolVersion = "2025-11-25"
        capabilities = @{}
        clientInfo = @{ name = "assaultcube-offline-ci"; version = "1" }
    }
    if ($init.result.protocolVersion -ne "2025-11-25") { throw "Cortex MCP legacy negotiation failed" }
    Send-McpNotification "notifications/initialized"

    $targetlessTools = Send-McpRequest "tools/list" @{}
    $targetlessNames = @($targetlessTools.result.tools.name)
    foreach ($requiredTool in @("cortex_processes", "cortex_attach", "cortex_detach", "cortex_targets")) {
        if (-not ($targetlessNames -contains $requiredTool)) { throw "targetless MCP catalog is missing $requiredTool" }
    }
    $report.checks.targetless_catalog = $true

    $processResponse = Invoke-CortexTool "cortex_processes" @{ query = "ac_server.exe"; limit = 64 }
    Assert-ToolSuccess $processResponse "cortex_processes"
    $processResult = Get-ToolStructured $processResponse
    $processRow = @($processResult.processes | Where-Object { [int64]$_.pid -eq [int64]$server.Id })
    if ($processRow.Count -ne 1) { throw "cortex_processes did not discover the running AssaultCube server" }
    if ([string]$processRow[0].architecture -ne "x86") {
        throw "AssaultCube target was not detected as x86: $($processRow[0].architecture)"
    }
    if ($processRow[0].attached) { throw "AssaultCube unexpectedly appeared attached before cortex_attach" }
    $report.checks.process_discovery = [ordered]@{ architecture = "x86"; target_id = [string]$processRow[0].id }

    $attachResponse = Invoke-CortexTool "cortex_attach" @{ pid = [int64]$server.Id } -TimeoutMs 30000
    Assert-ToolSuccess $attachResponse "cortex_attach"
    $attachResult = Get-ToolStructured $attachResponse
    if (-not $attachResult.ok -or [string]$attachResult.status -notin @("attached", "already_attached")) {
        throw "Cortex failed to attach AssaultCube"
    }
    Assert-ServerAlive "initial attach"

    $targetsResponse = Invoke-CortexTool "cortex_targets" @{}
    Assert-ToolSuccess $targetsResponse "cortex_targets"
    $targetsResult = Get-ToolStructured $targetsResponse
    if ([int]$targetsResult.count -ne 1 -or [int64]$targetsResult.targets[0].pid -ne [int64]$server.Id) {
        throw "cortex_targets did not report the attached AssaultCube server"
    }

    $attachedTools = Send-McpRequest "tools/list" @{}
    $attachedNames = @($attachedTools.result.tools.name)
    foreach ($requiredTool in @("status", "modules", "memory_read", "scan_aob", "disasm", "debug_threads", "debug_pause", "debug_step", "debug_step_over", "debug_continue")) {
        if (-not ($attachedNames -contains $requiredTool)) { throw "attached MCP catalog is missing $requiredTool" }
    }
    $report.checks.attached_catalog = $true

    $statusResponse = Invoke-CortexTool "status" @{}
    Assert-ToolSuccess $statusResponse "status"
    $statusResult = Get-RouteResult $statusResponse
    if ([int64]$statusResult.pid -ne [int64]$server.Id) { throw "Cortex status reports the wrong PID" }

    $modulesResponse = Invoke-CortexTool "modules" @{}
    Assert-ToolSuccess $modulesResponse "modules"
    $modules = @(Get-RouteResult $modulesResponse)
    $moduleRows = @($modules | Where-Object { [string]$_.name -ieq "ac_server.exe" })
    if ($moduleRows.Count -ne 1) { throw "Cortex module list did not expose ac_server.exe" }
    $module = $moduleRows[0]
    $moduleBase = Convert-HexAddressToUInt64 ([string]$module.base)
    $moduleSize = [uint64]$module.size
    if ($moduleBase -eq 0 -or $moduleSize -lt 4096) { throw "invalid AssaultCube module range" }

    $mz = [uint64](Read-TargetValue $moduleBase "u16")
    if ($mz -ne 0x5A4D) { throw "AssaultCube module does not begin with MZ" }
    $eLfanew = [uint64](Read-TargetValue ($moduleBase + 0x3c) "u32")
    if ($eLfanew -lt 0x40 -or $eLfanew -gt [Math]::Min([double]$moduleSize, 0x100000)) {
        throw "invalid PE e_lfanew in AssaultCube module: $eLfanew"
    }
    $peSignature = [uint64](Read-TargetValue ($moduleBase + $eLfanew) "u32")
    if ($peSignature -ne 0x00004550) { throw "AssaultCube module PE signature mismatch" }
    $entryRva = [uint64](Read-TargetValue ($moduleBase + $eLfanew + 40) "u32")
    $entryPoint = $moduleBase + $entryRva
    if ($entryPoint -lt $moduleBase -or $entryPoint -ge ($moduleBase + $moduleSize)) {
        throw "AssaultCube PE entrypoint is outside the main module"
    }
    $report.checks.pe_read = [ordered]@{
        module_base = Format-HexAddress $moduleBase
        module_size = $moduleSize
        e_lfanew = Format-HexAddress $eLfanew
        entrypoint = Format-HexAddress $entryPoint
        mz = $true
        pe_signature = $true
    }

    $aobResponse = Invoke-CortexTool "scan_aob" @{ pattern = "4D 5A"; module = "ac_server.exe" } -TimeoutMs 30000
    Assert-ToolSuccess $aobResponse "scan_aob"
    $aobResult = Get-RouteResult $aobResponse
    $baseHex = (Format-HexAddress $moduleBase).ToLowerInvariant()
    $aobAddresses = @($aobResult.addresses | ForEach-Object { ([string]$_).ToLowerInvariant() })
    if (-not ($aobAddresses -contains $baseHex)) { throw "AOB scan did not rediscover the ac_server.exe MZ header" }
    $report.checks.aob_scan = [ordered]@{ pattern = "4D 5A"; matches = $aobAddresses.Count; module_base_found = $true }

    $entryDisasmResponse = Invoke-CortexTool "disasm" @{ _query = @{ address = (Format-HexAddress $entryPoint); count = 12 } }
    Assert-ToolSuccess $entryDisasmResponse "disasm entrypoint"
    $entryDisasm = Get-RouteResult $entryDisasmResponse
    if (-not $entryDisasm.ok -or @($entryDisasm.instructions).Count -lt 1) { throw "could not disassemble the AssaultCube entrypoint" }
    $report.checks.disassembly = [ordered]@{ instruction_count = @($entryDisasm.instructions).Count; first = [string]$entryDisasm.instructions[0].text }

    $threadsResponse = Invoke-CortexTool "debug_threads" @{}
    Assert-ToolSuccess $threadsResponse "debug_threads"
    $threadsResult = Get-RouteResult $threadsResponse
    $debugThreadIds = @($threadsResult.thread_ids | ForEach-Object { [uint64]$_ })
    $candidateThreadIds = @($preAttachThreadIds | Where-Object { $debugThreadIds -contains [uint64]$_ } | Sort-Object)
    if ($candidateThreadIds.Count -lt 1) { throw "no original AssaultCube thread remains visible after attach" }
    $debugThreadId = [uint64]$candidateThreadIds[0]

    $deniedPause = Invoke-CortexTool "debug_pause" @{ thread_id = $debugThreadId }
    if (-not $deniedPause.result.isError -or [string]$deniedPause.result.structuredContent.error -ne "mutation_permission_required") {
        throw "debug_pause without mutation_permission was not rejected"
    }
    $report.checks.mutation_permission_gate = $true

    $pauseResponse = Invoke-CortexTool "debug_pause" @{ thread_id = $debugThreadId; mutation_permission = $true } -TimeoutMs 10000
    Assert-ToolSuccess $pauseResponse "debug_pause"
    $pauseResult = Get-RouteResult $pauseResponse
    if (-not $pauseResult.ok) { throw "debug_pause failed on the real AssaultCube target" }
    $pausedEip = [string]$pauseResult.registers.eip
    if ([string]::IsNullOrWhiteSpace($pausedEip)) { throw "debug_pause returned no x86 EIP" }

    $pausedDisasmResponse = Invoke-CortexTool "disasm" @{ _query = @{ address = $pausedEip; count = 1 } }
    Assert-ToolSuccess $pausedDisasmResponse "disasm paused EIP"
    $pausedDisasm = Get-RouteResult $pausedDisasmResponse
    if (@($pausedDisasm.instructions).Count -ne 1) { throw "could not disassemble paused AssaultCube EIP" }
    $pausedMnemonic = ([string]$pausedDisasm.instructions[0].mnemonic).ToLowerInvariant()

    if ($pausedMnemonic -eq "call") {
        $stepResponse = Invoke-CortexTool "debug_step_over" @{ thread_id = $debugThreadId; timeout_ms = 5000; mutation_permission = $true } -TimeoutMs 10000
        $stepOperation = "step_over"
    } else {
        $stepResponse = Invoke-CortexTool "debug_step" @{ thread_id = $debugThreadId; timeout_ms = 3000; mutation_permission = $true } -TimeoutMs 10000
        $stepOperation = "step_into"
    }
    Assert-ToolSuccess $stepResponse $stepOperation
    $stepResult = Get-RouteResult $stepResponse
    if (-not $stepResult.ok) { throw "$stepOperation failed on the real AssaultCube target" }
    $steppedEip = [string]$stepResult.registers.eip

    $continueResponse = Invoke-CortexTool "debug_continue" @{ thread_id = $debugThreadId; mutation_permission = $true } -TimeoutMs 10000
    Assert-ToolSuccess $continueResponse "debug_continue"
    $continueResult = Get-RouteResult $continueResponse
    if (-not $continueResult.ok) { throw "debug_continue failed on the real AssaultCube target" }
    Start-Sleep -Milliseconds 500
    Assert-ServerAlive "debugger Pause/Step/Continue"
    $report.checks.debugger = [ordered]@{
        thread_id = $debugThreadId
        paused_eip = $pausedEip
        paused_mnemonic = $pausedMnemonic
        operation = $stepOperation
        stepped_eip = $steppedEip
        continue_ok = $true
        target_survived = $true
    }

    $detachResponse = Invoke-CortexTool "cortex_detach" @{}
    Assert-ToolSuccess $detachResponse "cortex_detach"
    $detachResult = Get-ToolStructured $detachResponse
    if (-not $detachResult.ok -or [int]$detachResult.count -ne 0) { throw "Cortex did not detach AssaultCube cleanly" }
    Assert-ServerAlive "detach"

    $detachedTargets = Invoke-CortexTool "cortex_targets" @{}
    Assert-ToolSuccess $detachedTargets "cortex_targets after detach"
    if ([int](Get-ToolStructured $detachedTargets).count -ne 0) { throw "target remained routed after detach" }

    $rediscovery = Invoke-CortexTool "cortex_processes" @{ query = "ac_server.exe"; limit = 64 }
    Assert-ToolSuccess $rediscovery "cortex_processes after detach"
    $rediscoveryRow = @((Get-ToolStructured $rediscovery).processes | Where-Object { [int64]$_.pid -eq [int64]$server.Id })
    if ($rediscoveryRow.Count -ne 1 -or $rediscoveryRow[0].attached) { throw "AssaultCube rediscovery after detach is incorrect" }

    $reattachResponse = Invoke-CortexTool "cortex_attach" @{ pid = [int64]$server.Id } -TimeoutMs 30000
    Assert-ToolSuccess $reattachResponse "cortex_attach reattach"
    $reattachStatus = Invoke-CortexTool "status" @{}
    Assert-ToolSuccess $reattachStatus "status after reattach"
    if ([int64](Get-RouteResult $reattachStatus).pid -ne [int64]$server.Id) { throw "reattach routed status to the wrong PID" }
    Assert-ServerAlive "reattach"

    $finalDetach = Invoke-CortexTool "cortex_detach" @{}
    Assert-ToolSuccess $finalDetach "final cortex_detach"
    if ([int](Get-ToolStructured $finalDetach).count -ne 0) { throw "final detach left a routed target" }
    $report.checks.detach_reattach = [ordered]@{ detached = $true; rediscovered = $true; reattached = $true; final_detach = $true; target_survived = $true }

    $script:Mcp.StandardInput.Close()
    if (-not $script:Mcp.WaitForExit(15000)) {
        $script:Mcp.Kill($true)
        throw "Cortex MCP did not exit after stdin closed"
    }
    if ($script:Mcp.ExitCode -ne 0) { throw "Cortex MCP exited with code $($script:Mcp.ExitCode)" }

    Assert-ServerAlive "completed Cortex validation"
    $report.status = "PASS"
    $report.finished_at_utc = [DateTime]::UtcNow.ToString("o")
    Write-Host "AssaultCube offline real-target E2E passed"
} catch {
    $report.status = "FAIL"
    $report.failure = $_.Exception.Message
    $report.finished_at_utc = [DateTime]::UtcNow.ToString("o")
    Write-Host ("AssaultCube offline E2E failure: " + $_.Exception.Message)
    throw
} finally {
    if ($null -ne $script:Mcp -and -not $script:Mcp.HasExited) {
        try { $script:Mcp.Kill($true) } catch {}
        try { $script:Mcp.WaitForExit(5000) | Out-Null } catch {}
    }
    if ($null -ne $script:McpStderrTask) {
        try {
            if ($script:McpStderrTask.Wait(5000)) {
                [IO.File]::WriteAllText($mcpStderrPath, [string]$script:McpStderrTask.Result, [Text.UTF8Encoding]::new($false))
            }
        } catch {}
    }
    if ($null -ne $server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
    if ($firewallRuleCreated) {
        Remove-NetFirewallRule -DisplayName $firewallRuleName -ErrorAction SilentlyContinue
    }
    if (-not ($report.Contains("finished_at_utc"))) {
        $report.finished_at_utc = [DateTime]::UtcNow.ToString("o")
    }
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
    Write-Host "AssaultCube Cortex report: $ReportPath"
}
