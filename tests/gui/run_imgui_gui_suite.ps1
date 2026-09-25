param(
    [Parameter(Mandatory = $true)]
    [string]$CortexExe,

    [Parameter(Mandatory = $true)]
    [string]$X64Target,

    [Parameter(Mandatory = $true)]
    [string]$X86Target,

    [string]$ReportPath = "gui-test-report.json"
)

$ErrorActionPreference = "Stop"
$CortexExe = (Resolve-Path $CortexExe).Path
$X64Target = (Resolve-Path $X64Target).Path
$X86Target = (Resolve-Path $X86Target).Path
$reportFullPath = [IO.Path]::GetFullPath($ReportPath)
$reportDirectory = Split-Path -Parent $reportFullPath
if ($reportDirectory) {
    New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null
}

$results = [System.Collections.Generic.List[object]]::new()

function Add-Result(
    [string]$Name,
    [bool]$Passed,
    [double]$DurationMs,
    [string]$Stdout,
    [string]$Stderr,
    [int]$ExitCode
) {
    $results.Add([pscustomobject]@{
        name = $Name
        passed = $Passed
        duration_ms = [Math]::Round($DurationMs, 3)
        exit_code = $ExitCode
        stdout = $Stdout.Trim()
        stderr = $Stderr.Trim()
    })
}

function Save-Report {
    $payload = [ordered]@{
        schema = "cortex.imgui.gui-suite.v1"
        generated_utc = [DateTime]::UtcNow.ToString("o")
        cortex = $CortexExe
        x64_target = $X64Target
        x86_target = $X86Target
        passed = @($results | Where-Object passed).Count
        failed = @($results | Where-Object { -not $_.passed }).Count
        tests = $results
    }
    $payload | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 $reportFullPath
}

function Invoke-CortexProcess(
    [string]$Name,
    [string[]]$Arguments,
    [int]$TimeoutMs = 30000
) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $CortexExe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    foreach ($argument in $Arguments) {
        [void]$psi.ArgumentList.Add($argument)
    }

    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::Start($psi)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutMs)) {
        try { $process.Kill() } catch {}
        $watch.Stop()
        Add-Result $Name $false $watch.Elapsed.TotalMilliseconds "" "timeout" -1
        Save-Report
        throw "$Name timed out after $TimeoutMs ms"
    }
    $watch.Stop()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $passed = $process.ExitCode -eq 0
    Add-Result $Name $passed $watch.Elapsed.TotalMilliseconds $stdout $stderr $process.ExitCode
    if (-not $passed) {
        Save-Report
        $combined = $stderr + [Environment]::NewLine + $stdout
        throw "$Name failed ($($process.ExitCode)): $combined"
    }
    Write-Host ("PASS {0} ({1} ms)" -f $Name, [Math]::Round($watch.Elapsed.TotalMilliseconds, 1))
    if ($stdout) { Write-Host $stdout.Trim() }
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdout
        Stderr = $stderr
    }
}

function Wait-ForFile([string]$Path, [int]$TimeoutMs = 15000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path $Path) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

$x64A = $null
$x64B = $null
$x86 = $null
$mcp = $null

try {
    Invoke-CortexProcess "workspace-layout" @("--gui-workspace-suite") 30000 | Out-Null
    Invoke-CortexProcess "native-window-d3d11" @("--window-smoke-test") 30000 | Out-Null
    Invoke-CortexProcess "ai-activity" @("--ai-activity-smoke") 30000 | Out-Null

    $x64A = Start-Process -FilePath $X64Target -PassThru
    $x64B = Start-Process -FilePath $X64Target -PassThru
    $x86 = Start-Process -FilePath $X86Target -PassThru
    Start-Sleep -Milliseconds 900

    Invoke-CortexProcess "attached-x64" @(
        "--gui-attached-suite", "--pid", "$($x64A.Id)"
    ) 45000 | Out-Null

    Invoke-CortexProcess "multi-session-x64" @(
        "--gui-multi-session-suite",
        "--pid-a", "$($x64A.Id)",
        "--pid-b", "$($x64B.Id)"
    ) 45000 | Out-Null

    Invoke-CortexProcess "cross-bitness-x86" @(
        "--gui-cross-bitness-suite", "--pid", "$($x86.Id)"
    ) 45000 | Out-Null

    $appRoot = Split-Path -Parent $CortexExe
    $corePath = Join-Path $appRoot "cortex_core.dll"
    if (-not (Test-Path $corePath)) {
        throw "missing x64 runtime beside cortex.exe: $corePath"
    }

    Invoke-CortexProcess "inject-x64-private-channel" @(
        "inject", "$($x64A.Id)", $corePath
    ) 30000 | Out-Null

    $tokenPath = Join-Path $appRoot ("cortex.mcp.{0}.token" -f $x64A.Id)
    if (-not (Wait-ForFile $tokenPath 15000)) {
        throw "private MCP token was not created: $tokenPath"
    }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $CortexExe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    foreach ($argument in @(
        "mcp", "--transport", "native", "--tools", "all",
        "--token-file", $tokenPath
    )) {
        [void]$psi.ArgumentList.Add($argument)
    }
    $mcp = [System.Diagnostics.Process]::Start($psi)

    function Read-McpLine([int]$TimeoutMs = 15000) {
        $task = $mcp.StandardOutput.ReadLineAsync()
        if (-not $task.Wait($TimeoutMs)) {
            throw "timed out waiting for native MCP response"
        }
        $line = $task.Result
        if (-not $line) {
            throw "native MCP stdout closed: $($mcp.StandardError.ReadToEnd())"
        }
        return $line
    }

    $mcp.StandardInput.WriteLine(
        '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"imgui-gui-suite","version":"1"}}}'
    )
    $initLine = Read-McpLine
    $init = $initLine | ConvertFrom-Json
    if ($init.id -ne 1 -or -not $init.result) {
        throw "invalid native MCP initialize response: $initLine"
    }
    $mcp.StandardInput.WriteLine(
        '{"jsonrpc":"2.0","method":"notifications/initialized"}'
    )

    $promptRequest = @{
        jsonrpc = "2.0"
        id = 2
        method = "tools/call"
        params = @{
            name = "prompt_value_change"
            arguments = @{
                label = "GUI full-suite prompt"
                current_value = "before"
                target_value = "after"
            }
        }
    } | ConvertTo-Json -Compress -Depth 8

    # Start the desktop presenter first. Its /prompt/active polling renews the
    # external-presenter lease, so the test matches the real GUI lifecycle
    # instead of racing prompt creation against presenter startup.
    $promptPsi = [System.Diagnostics.ProcessStartInfo]::new()
    $promptPsi.FileName = $CortexExe
    $promptPsi.UseShellExecute = $false
    $promptPsi.RedirectStandardOutput = $true
    $promptPsi.RedirectStandardError = $true
    $promptPsi.CreateNoWindow = $true
    foreach ($argument in @(
        "--prompt-channel-smoke", "--pid", "$($x64A.Id)",
        "--answer", "gui-suite-answer"
    )) {
        [void]$promptPsi.ArgumentList.Add($argument)
    }

    $promptWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $promptProcess = [System.Diagnostics.Process]::Start($promptPsi)
    $promptStdoutTask = $promptProcess.StandardOutput.ReadToEndAsync()
    $promptStderrTask = $promptProcess.StandardError.ReadToEndAsync()

    Start-Sleep -Milliseconds 300
    $mcp.StandardInput.WriteLine($promptRequest)

    if (-not $promptProcess.WaitForExit(30000)) {
        try { $promptProcess.Kill() } catch {}
        $promptWatch.Stop()
        Add-Result "prompt-private-channel" $false $promptWatch.Elapsed.TotalMilliseconds "" "timeout" -1
        Save-Report
        throw "prompt-private-channel timed out"
    }

    $promptWatch.Stop()
    $promptStdout = $promptStdoutTask.Result
    $promptStderr = $promptStderrTask.Result
    $promptPassed =
        $promptProcess.ExitCode -eq 0 -and
        $promptStdout -match "PASS: ImGui prompt channel answered prompt"
    Add-Result "prompt-private-channel" $promptPassed $promptWatch.Elapsed.TotalMilliseconds $promptStdout $promptStderr $promptProcess.ExitCode
    if (-not $promptPassed) {
        Save-Report
        throw "prompt-private-channel failed ($($promptProcess.ExitCode)): $promptStderr $promptStdout"
    }
    Write-Host ("PASS prompt-private-channel ({0} ms)" -f [Math]::Round($promptWatch.Elapsed.TotalMilliseconds, 1))
    if ($promptStdout) { Write-Host $promptStdout.Trim() }

    $promptLine = Read-McpLine
    $promptResponse = $promptLine | ConvertFrom-Json
    if ($promptResponse.id -ne 2 -or $promptResponse.result.isError) {
        throw "prompt MCP response failed: $promptLine"
    }
    Add-Result "prompt-mcp-completion" $true 0 $promptLine "" 0

    Invoke-CortexProcess "event-private-channel" @(
        "--event-channel-smoke", "--pid", "$($x64A.Id)",
        "--expect", "prompt.answered"
    ) 30000 | Out-Null

    Save-Report
    Write-Host ("GUI suite complete: {0} passed, {1} failed" -f
        @($results | Where-Object passed).Count,
        @($results | Where-Object { -not $_.passed }).Count)
}
catch {
    if (-not (Test-Path $reportFullPath)) {
        Add-Result "suite-exception" $false 0 "" $_.Exception.Message -1
        Save-Report
    }
    throw
}
finally {
    if ($mcp) {
        try { $mcp.StandardInput.Close() } catch {}
        try {
            if (-not $mcp.WaitForExit(3000)) { $mcp.Kill() }
        } catch {}
        try { $mcp.Dispose() } catch {}
    }
    foreach ($process in @($x64A, $x64B, $x86)) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
