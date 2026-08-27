param(
    [Parameter(Mandatory = $true)]
    [string]$HostPath,
    [Parameter(Mandatory = $true)]
    [string]$TokenPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Read-McpLine {
    param(
        [Parameter(Mandatory = $true)]$Process,
        [int]$TimeoutMs = 10000
    )
    $task = $Process.StandardOutput.ReadLineAsync()
    if (-not $task.Wait($TimeoutMs)) {
        throw "Timed out waiting for MCP stdio response"
    }
    $line = $task.Result
    if ($null -eq $line) {
        $stderr = $Process.StandardError.ReadToEnd()
        throw "MCP stdio closed unexpectedly. stderr: $stderr"
    }
    return ($line | ConvertFrom-Json -Depth 60)
}

function Write-Mcp {
    param(
        [Parameter(Mandatory = $true)]$Process,
        [Parameter(Mandatory = $true)]$Payload
    )
    $line = ConvertTo-Json -InputObject $Payload -Depth 60 -Compress
    $Process.StandardInput.WriteLine($line)
    $Process.StandardInput.Flush()
}

function Invoke-Mcp {
    param(
        [Parameter(Mandatory = $true)]$Process,
        [Parameter(Mandatory = $true)]$Payload
    )
    Write-Mcp -Process $Process -Payload $Payload
    return Read-McpLine -Process $Process
}

Assert-True (Test-Path $HostPath) "cortex_host does not exist: $HostPath"
Assert-True (Test-Path $TokenPath) "token file does not exist: $TokenPath"
$resolvedHost = (Resolve-Path $HostPath).Path
$resolvedToken = (Resolve-Path $TokenPath).Path

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $resolvedHost
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.CreateNoWindow = $true
[void]$startInfo.ArgumentList.Add("mcp")
[void]$startInfo.ArgumentList.Add("--transport")
[void]$startInfo.ArgumentList.Add("native")
[void]$startInfo.ArgumentList.Add("--tools")
[void]$startInfo.ArgumentList.Add("compact")
[void]$startInfo.ArgumentList.Add("--token-file")
[void]$startInfo.ArgumentList.Add($resolvedToken)

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
Assert-True ($process.Start()) "failed to start cortex_host mcp"

try {
    $initialize = Invoke-Mcp -Process $process -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = 1
        method = "initialize"
        params = @{ protocolVersion = "2025-11-25" }
    })
    Assert-True ($initialize.id -eq 1) "initialize response id mismatch"
    Assert-True ($initialize.result.serverInfo.name -eq "cortex") "wrong MCP server name"

    $list = Invoke-Mcp -Process $process -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = 2
        method = "tools/list"
        params = @{}
    })
    $tools = @($list.result.tools)
    Assert-True ($tools.Count -eq 30) "compact native stdio profile must expose exactly 30 semantic tools"
    Assert-True (-not (@($tools.name) -contains "health")) "compact native stdio profile leaked primitive tools"

    # Notifications must not create stdout. Send two notifications followed by
    # ping; the next line must be the ping response, not a spurious id:null row.
    Write-Mcp -Process $process -Payload ([ordered]@{
        jsonrpc = "2.0"
        method = "notifications/initialized"
    })
    Write-Mcp -Process $process -Payload ([ordered]@{
        jsonrpc = "2.0"
        method = "notifications/cancelled"
        params = @{ requestId = 999999 }
    })
    Write-Mcp -Process $process -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = 3
        method = "ping"
    })
    $ping = Read-McpLine -Process $process
    Assert-True ($ping.id -eq 3) "notification emitted an unexpected stdio response"

    $execute = Invoke-Mcp -Process $process -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = 4
        method = "tools/call"
        params = [ordered]@{
            name = "capture_runtime_state"
            arguments = [ordered]@{
                objective = "Validate native server-side semantic execution"
                execute = $true
                timeout_ms = 5000
                steps = @(
                    [ordered]@{ tool = "health"; arguments = @{} },
                    [ordered]@{ tool = "modules"; arguments = @{} }
                )
            }
        }
    })
    Assert-True (-not [bool]$execute.result.isError) "read-only semantic execution returned isError"
    $structured = $execute.result.structuredContent
    Assert-True ($structured.status -eq "completed") "native semantic execution did not complete"
    Assert-True ($structured.lifecycle.current -eq "completed") "execution lifecycle did not complete"
    Assert-True (@($structured.evidence).Count -eq 2) "execution did not capture both primitive outputs as evidence"
    Assert-True ([int]$structured.evidence[0].output.status -eq 200) "health primitive did not execute through native dispatcher"
    Assert-True ([int]$structured.evidence[1].output.status -eq 200) "modules primitive did not execute through native dispatcher"

    $permissionGate = Invoke-Mcp -Process $process -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = 5
        method = "tools/call"
        params = [ordered]@{
            name = "apply_reversible_patch"
            arguments = [ordered]@{
                objective = "Verify mutation permission preflight"
                execute = $true
                steps = @(
                    [ordered]@{
                        tool = "patch_write"
                        arguments = @{ address = "0x1"; bytes = "90" }
                    }
                )
            }
        }
    })
    Assert-True ([bool]$permissionGate.result.isError) "mutation without permission was not rejected"
    Assert-True ($permissionGate.result.structuredContent.error -eq "mutation_permission_required") `
        "mutation permission gate returned wrong error"

    $missingSteps = Invoke-Mcp -Process $process -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = 6
        method = "tools/call"
        params = [ordered]@{
            name = "capture_runtime_state"
            arguments = @{ objective = "validate execution contract"; execute = $true }
        }
    })
    Assert-True ([bool]$missingSteps.result.isError) "execute=true without steps was not rejected"
    Assert-True ($missingSteps.result.structuredContent.error -eq "missing_execution_steps") `
        "missing execution steps returned wrong error"

    Write-Host "PASS: native MCP stdio -> named pipe -> semantic executor -> native route dispatcher"
} finally {
    try { $process.StandardInput.Close() } catch {}
    if (-not $process.WaitForExit(5000)) {
        try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
    }
    $process.Dispose()
}
