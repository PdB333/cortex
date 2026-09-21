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
if (-not $ResultsRoot) { $ResultsRoot = Join-Path $BuildRoot "e2e-results-$Architecture" }
New-Item -ItemType Directory -Force -Path $ResultsRoot | Out-Null
$CrashRoot = Join-Path $ResultsRoot "crashes"
New-Item -ItemType Directory -Force -Path $CrashRoot | Out-Null

$HostExe = Join-Path $BuildRoot "cortex_host.exe"
$CoreDll = Join-Path $BuildRoot "cortex_core.dll"
$FakeModDll = Join-Path $BuildRoot "cortex_e2e_mod.dll"
$TargetExe = Join-Path $BuildRoot "cortex_test_target_$Architecture.exe"
$SilentGuiExe = Join-Path $BuildRoot "cortex_silent_gui_target_$Architecture.exe"
$D3D9Exe = Join-Path $BuildRoot "cortex_test_target_d3d9_$Architecture.exe"
$D3D11Exe = Join-Path $BuildRoot "cortex_test_target_d3d11_$Architecture.exe"
$TokenPath = Join-Path $BuildRoot "cortex.token"
$ConfigPath = Join-Path $BuildRoot "cortex.ini"
$BaseUri = "http://127.0.0.1:6969"

foreach ($path in @($HostExe, $CoreDll, $FakeModDll, $TargetExe, $SilentGuiExe, $D3D9Exe, $D3D11Exe)) {
    if (-not (Test-Path $path)) { throw "Missing E2E artifact: $path" }
}

@"
http_api_enabled=true
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

$script:Token = ""
$script:Results = [System.Collections.Generic.List[object]]::new()

function Assert-That([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Wait-For {
    param([scriptblock]$Condition, [int]$TimeoutMs = 15000, [int]$PollMs = 100, [string]$Description = "condition")
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        try { if (& $Condition) { return } } catch {}
        Start-Sleep -Milliseconds $PollMs
    }
    throw "Timed out waiting for $Description"
}

function Request-Json {
    param(
        [string]$Method,
        [string]$Path,
        $Body = $null,
        [AllowEmptyString()][string]$Token = $script:Token,
        [int[]]$ExpectedStatus = @(200),
        [string]$ContentType = "application/json"
    )
    $headers = @{}
    if ($Token) { $headers["X-Cortex-Token"] = $Token }
    $request = @{
        Uri = "$BaseUri$Path"
        Method = $Method
        Headers = $headers
        TimeoutSec = 10
        SkipHttpErrorCheck = $true
    }
    if ($null -ne $Body) {
        $request.ContentType = $ContentType
        $request.Body = if ($Body -is [string]) { $Body } else { $Body | ConvertTo-Json -Depth 12 -Compress }
    }
    $response = Invoke-WebRequest @request
    Assert-That ($ExpectedStatus -contains [int]$response.StatusCode) "$Method $Path returned HTTP $($response.StatusCode): $($response.Content)"
    $text = [string]$response.Content
    if (-not $text) { return $null }
    if ($text.TrimStart().StartsWith("{") -or $text.TrimStart().StartsWith("[")) { return $text | ConvertFrom-Json }
    return $text
}

function Wait-Api {
    Wait-For -TimeoutMs 20000 -PollMs 200 -Description "Cortex /health" -Condition {
        $response = Invoke-WebRequest -Uri "$BaseUri/health" -TimeoutSec 1 -SkipHttpErrorCheck
        $response.StatusCode -eq 200
    }
    Wait-For -TimeoutMs 10000 -Description "cortex.token" -Condition { Test-Path $TokenPath }
    $script:Token = (Get-Content $TokenPath -Raw).Trim()
    Assert-That ($script:Token.Length -ge 32) "Cortex token was not generated"
}

function Wait-ApiClosed {
    Wait-For -TimeoutMs 10000 -PollMs 200 -Description "Cortex API shutdown" -Condition {
        try {
            [void](Invoke-WebRequest -Uri "$BaseUri/health" -TimeoutSec 1 -SkipHttpErrorCheck)
            return $false
        } catch { return $true }
    }
}

function Inject-Dll([int]$ProcessId, [string]$DllPath) {
    $output = & $HostExe inject $ProcessId $DllPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Injection failed for $DllPath into $ProcessId`: $output" }
}

function Signal-Event([string]$Name) {
    $handle = [CortexE2EWin32]::OpenEvent(0x0002, $false, $Name)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenEvent failed for $Name" }
    try {
        if (-not [CortexE2EWin32]::SetEvent($handle)) { throw "SetEvent failed for $Name" }
    } finally { [void][CortexE2EWin32]::CloseHandle($handle) }
}

function Start-Fixture([string]$Name) {
    $root = Join-Path $ResultsRoot $Name
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $manifestPath = Join-Path $root "target-manifest.json"
    Remove-Item $manifestPath -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $TargetExe -ArgumentList "--e2e-manifest=$manifestPath" -PassThru
    Wait-For -TimeoutMs 10000 -Description "$Name manifest" -Condition { Test-Path $manifestPath }
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    Assert-That ([int]$manifest.pid -eq $process.Id) "Target manifest PID mismatch"
    $expected = if ($Architecture -eq "x64") { 8 } else { 4 }
    Assert-That ([int]$manifest.pointer_size -eq $expected) "Target pointer size mismatch"
    [pscustomobject]@{ Process = $process; Manifest = $manifest; Root = $root }
}

function Stop-ProcessSafe($Process) {
    if ($Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        [void]$Process.WaitForExit(3000)
    }
}

function Stop-Fixture($Fixture) {
    if (-not $Fixture) { return }
    if (-not $Fixture.Process.HasExited) {
        try { Signal-Event $Fixture.Manifest.stop_event } catch {}
        if (-not $Fixture.Process.WaitForExit(3000)) { Stop-ProcessSafe $Fixture.Process }
    }
    try { Wait-ApiClosed } catch {}
}

function Address-Number([string]$Address) { [Convert]::ToUInt64($Address.Substring(2), 16) }

function Newest-Capture([string]$Prefix) {
    $directory = Get-ChildItem $CrashRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object Name -Like "$Prefix*" |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if (-not $directory) { throw "No $Prefix capture directory was generated" }
    $directory.FullName
}

function Require-File([string]$Directory, [string]$Name) {
    $path = Join-Path $Directory $Name
    Assert-That (Test-Path $path) "Missing artifact: $path"
    Assert-That ((Get-Item $path).Length -gt 0) "Empty artifact: $path"
}

function Run-Scenario([string]$Name, [scriptblock]$Body) {
    Write-Host "`n=== $Name ==="
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        & $Body
        $watch.Stop()
        $script:Results.Add([pscustomobject]@{ name = $Name; status = "passed"; duration_ms = $watch.ElapsedMilliseconds; error = "" })
        Write-Host "PASS $Name"
    } catch {
        $watch.Stop()
        $message = $_.Exception.Message
        $script:Results.Add([pscustomobject]@{ name = $Name; status = "failed"; duration_ms = $watch.ElapsedMilliseconds; error = $message })
        Write-Warning "FAIL $Name`: $message"
    }
}

Run-Scenario "silent-runtime-gui-injection" {
    $process = $null
    $statePath = Join-Path $ResultsRoot "silent-runtime-state.json"
    $previousState = $env:CORTEX_SILENT_STATE
    try {
        Remove-Item $statePath -ErrorAction SilentlyContinue
        $env:CORTEX_SILENT_STATE = $statePath
        $process = Start-Process -FilePath $SilentGuiExe -PassThru
        Wait-For -TimeoutMs 10000 -Description "silent GUI state" -Condition { Test-Path $statePath }
        $before = Get-Content $statePath -Raw | ConvertFrom-Json
        Assert-That (-not [bool]$before.console_window) "GUI target unexpectedly started with a console"

        Inject-Dll $process.Id $CoreDll
        $mcpTokenPath = Join-Path $BuildRoot "cortex.mcp.$($process.Id).token"
        Wait-For -TimeoutMs 15000 -Description "native MCP startup in silent GUI target" -Condition { Test-Path $mcpTokenPath }
        Start-Sleep -Milliseconds 300

        $after = Get-Content $statePath -Raw | ConvertFrom-Json
        Assert-That (-not [bool]$after.console_window) "Cortex injection created a console window in a GUI target"
        Assert-That ([string]$after.console_class -ne "ConsoleWindowClass") "Cortex injection created ConsoleWindowClass"
    } finally {
        Stop-ProcessSafe $process
        if ($null -eq $previousState) { Remove-Item Env:CORTEX_SILENT_STATE -ErrorAction SilentlyContinue }
        else { $env:CORTEX_SILENT_STATE = $previousState }
    }
}

Run-Scenario "api-memory-security" {
    $fixture = $null
    try {
        $fixture = Start-Fixture "api-memory-security"
        Inject-Dll $fixture.Process.Id $CoreDll
        Wait-Api

        $health = Request-Json GET "/health"
        Assert-That $health.ok "Health endpoint failed"
        $status = Request-Json GET "/status"
        Assert-That ([int]$status.pid -eq $fixture.Process.Id) "Status PID mismatch"
        $tools = Request-Json GET "/tools"
        Assert-That (($tools | ConvertTo-Json -Depth 8) -match "memory_read") "Tools manifest is incomplete"
        $openApi = Request-Json GET "/openapi.json"
        Assert-That (($openApi | ConvertTo-Json -Depth 12) -match "/memory/read") "OpenAPI is incomplete"

        [void](Request-Json GET "/modules" -Token "" -ExpectedStatus @(401))
        [void](Request-Json GET "/modules" -Token "wrong-token" -ExpectedStatus @(401))
        [void](Request-Json POST "/memory/read" -Body '{"address":"0x1","type":"u8"}' -ContentType "text/plain" -ExpectedStatus @(415))

        $modules = Request-Json GET "/modules"
        $moduleText = $modules | ConvertTo-Json -Depth 8
        Assert-That ($moduleText -match "cortex_core.dll") "cortex_core.dll is absent from /modules"
        Assert-That ($moduleText -match "cortex_test_target_$Architecture.exe") "Test target is absent from /modules"

        $u32 = Request-Json POST "/memory/read" @{ address = $fixture.Manifest.u32; type = "u32" }
        Assert-That ($u32.ok -and [uint64]$u32.value -eq [uint64]3735928559) "u32 read failed: value=$($u32.value)"
        $string = Request-Json POST "/memory/read" @{ address = $fixture.Manifest.string; type = "string"; count = 32 }
        Assert-That ($string.ok -and $string.value -eq "cortex-canary") "String read failed"
        $batch = Request-Json POST "/memory/read_batch" @{ reads = @(
            @{ address = $fixture.Manifest.u32; type = "u32" },
            @{ address = $fixture.Manifest.u64; type = "u64" },
            @{ address = $fixture.Manifest.float; type = "float" }
        ) }
        Assert-That ($batch.results.Count -eq 3) "Batch read count mismatch"
        Assert-That (@($batch.results | Where-Object { -not $_.ok }).Count -eq 0) "Batch read failed"

        $write = Request-Json POST "/memory/write" @{ address = $fixture.Manifest.health; type = "u32"; value = 777 }
        Assert-That $write.ok "Memory write failed"
        $written = Request-Json POST "/memory/read" @{ address = $fixture.Manifest.health; type = "u32" }
        Assert-That ([int]$written.value -eq 777) "Memory write was not visible"

        $freeze = Request-Json POST "/freeze" @{ address = $fixture.Manifest.health; type = "u32"; value = 4242; label = "e2e-health" }
        Assert-That ($freeze.ok -and [int]$freeze.id -gt 0) "Freeze creation failed"
        Wait-For -TimeoutMs 2000 -PollMs 20 -Description "initial frozen value" -Condition {
            $sample = Request-Json POST "/memory/read" @{ address = $fixture.Manifest.health; type = "u32" }
            [int]$sample.value -eq 4242
        }
        1..3 | ForEach-Object {
            $perturbedValue = 5000 + $_
            $perturb = Request-Json POST "/memory/write" @{ address = $fixture.Manifest.health; type = "u32"; value = $perturbedValue }
            Assert-That $perturb.ok "Failed to perturb frozen value"
            Wait-For -TimeoutMs 2000 -PollMs 20 -Description "freeze restoration $($_)" -Condition {
                $sample = Request-Json POST "/memory/read" @{ address = $fixture.Manifest.health; type = "u32" }
                [int]$sample.value -eq 4242
            }
        }
        [void](Request-Json DELETE "/freeze/$($freeze.id)")
        $unfrozen = Request-Json POST "/memory/write" @{ address = $fixture.Manifest.health; type = "u32"; value = 7331 }
        Assert-That $unfrozen.ok "Failed to write after unfreeze"
        Start-Sleep -Milliseconds 100
        $afterFreeze = Request-Json POST "/memory/read" @{ address = $fixture.Manifest.health; type = "u32" }
        Assert-That ([int]$afterFreeze.value -ne 4242) "Value remained frozen after deletion"

        $start = Address-Number $fixture.Manifest.u32
        $end = $start + 4
        $scan = Request-Json POST "/scan/new" @{
            type = "u32"; value = "3735928559"
            start = "0x$($start.ToString('x'))"; end = "0x$($end.ToString('x'))"
            writable_only = $false; alignment = 1
        }
        Assert-That ($scan.ok -and [int]$scan.count -ge 1) "Range scan missed the canary"
        $scanResults = Request-Json GET "/scan/results/$($scan.scan_id)?limit=10"
        Assert-That ([int]$scanResults.total -ge 1) "Scan results are empty"
        [void](Request-Json DELETE "/scan/$($scan.scan_id)")

        $symbolModule = Request-Json GET "/symbols/module?address=$($fixture.Manifest.anchor)"
        Assert-That ($symbolModule.ok -and $symbolModule.module -match "cortex_test_target") "Module/RVA resolution failed"
        $lua = Request-Json POST "/lua/exec" @{ code = "return 6 * 7"; timeout_ms = 2000 }
        Assert-That ($lua.ok -and [int]$lua.result -eq 42) "Lua execution failed"

        $mcpInit = Request-Json POST "/mcp" @{
            jsonrpc = "2.0"; id = 1; method = "initialize"
            params = @{ protocolVersion = "2024-11-05"; capabilities = @{}; clientInfo = @{ name = "cortex-e2e"; version = "1.0" } }
        }
        Assert-That ($mcpInit.jsonrpc -eq "2.0" -and $null -ne $mcpInit.result) "MCP initialize failed"
        $mcpTools = Request-Json POST "/mcp" @{ jsonrpc = "2.0"; id = 2; method = "tools/list"; params = @{} }
        Assert-That ($mcpTools.result.tools.Count -gt 20) "MCP tools/list is incomplete"
        $session = Request-Json POST "/session/export" @{}
        Assert-That ($session.ok -and $session.path) "Session export failed"
    } finally { Stop-Fixture $fixture }
}

Run-Scenario "instrumented-mod-crash" {
    $fixture = $null
    $diagnose = $null
    try {
        Remove-Item (Join-Path $CrashRoot "*") -Recurse -Force -ErrorAction SilentlyContinue
        $fixture = Start-Fixture "instrumented-mod-crash"
        Inject-Dll $fixture.Process.Id $CoreDll
        Wait-Api
        Inject-Dll $fixture.Process.Id $FakeModDll
        Start-Sleep -Seconds 2
        $diagnose = Start-Process -FilePath $HostExe -ArgumentList @(
            "diagnose", "--pid", $fixture.Process.Id, "--output", $CrashRoot,
            "--heartbeat", "render", "--hang-ms", "1500", "--poll-ms", "100", "--once"
        ) -RedirectStandardOutput (Join-Path $fixture.Root "diagnose.log") -RedirectStandardError (Join-Path $fixture.Root "diagnose-error.log") -PassThru
        Start-Sleep -Milliseconds 700
        Signal-Event $fixture.Manifest.crash_event
        Assert-That ($fixture.Process.WaitForExit(20000)) "Crash target did not exit"
        Assert-That ($diagnose.WaitForExit(20000)) "Diagnostics host did not finish crash capture"
        Assert-That ($diagnose.ExitCode -eq 0) "Diagnostics host failed during crash capture"

        $capture = Newest-Capture "crash_"
        foreach ($name in @(
            "crash.dmp", "external_crash.dmp", "report.json", "breadcrumbs.json",
            "mods.json", "scopes.json", "values.json", "hooks.json", "stack.json",
            "build_info.json", "report.txt", "external_report.json", "analysis.json", "analysis.txt"
        )) { Require-File $capture $name }

        $report = Get-Content (Join-Path $capture "report.json") -Raw
        Assert-That ($report -match "C0000005|EXCEPTION_ACCESS_VIOLATION") "Crash evidence is missing"
        Assert-That ((Get-Content (Join-Path $capture "mods.json") -Raw) -match "CortexE2EFakeMod") "Mod registration is missing"
        Assert-That ((Get-Content (Join-Path $capture "scopes.json") -Raw) -match "E2EWorker") "Active scope is missing"
        $values = Get-Content (Join-Path $capture "values.json") -Raw
        Assert-That ($values -match "e2e_counter" -and $values -match "fixture_ready") "Diagnostic values are missing"
        $hooks = Get-Content (Join-Path $capture "hooks.json") -Raw
        Assert-That ($hooks -match "E2EPrimaryHook" -and $hooks -match "E2EOverlappingHook") "Registered hooks are missing"
        Assert-That ($hooks -match "overlap|installed_bytes_changed|jump_target_mismatch") "Hook conflict/corruption was not detected"
        $analysis = Get-Content (Join-Path $capture "analysis.json") -Raw
        Assert-That ($analysis -match "null_dereference") "Analyzer missed the null dereference"
        Assert-That ($analysis -match "overlapping_hooks|hook_replaced|recursive_hook") "Analyzer missed hook evidence"
    } finally {
        Stop-ProcessSafe $diagnose
        if ($fixture) { Stop-ProcessSafe $fixture.Process }
        try { Wait-ApiClosed } catch {}
    }
}

Run-Scenario "external-hang-watchdog" {
    $fixture = $null
    $diagnose = $null
    try {
        Remove-Item (Join-Path $CrashRoot "*") -Recurse -Force -ErrorAction SilentlyContinue
        $fixture = Start-Fixture "external-hang-watchdog"
        Inject-Dll $fixture.Process.Id $CoreDll
        Wait-Api
        Inject-Dll $fixture.Process.Id $FakeModDll
        Start-Sleep -Seconds 2
        $diagnose = Start-Process -FilePath $HostExe -ArgumentList @(
            "diagnose", "--pid", $fixture.Process.Id, "--output", $CrashRoot,
            "--heartbeat", "render", "--hang-ms", "1200", "--poll-ms", "100", "--once"
        ) -RedirectStandardOutput (Join-Path $fixture.Root "diagnose.log") -RedirectStandardError (Join-Path $fixture.Root "diagnose-error.log") -PassThru
        Start-Sleep -Milliseconds 600
        Signal-Event $fixture.Manifest.hang_event
        Assert-That ($diagnose.WaitForExit(20000)) "Diagnostics host did not finish hang capture"
        Assert-That ($diagnose.ExitCode -eq 0) "Diagnostics host failed during hang capture"
        $capture = Newest-Capture "hang_"
        foreach ($name in @("hang.dmp", "threads.json", "hang_report.json", "analysis.json", "analysis.txt")) { Require-File $capture $name }
        $hang = Get-Content (Join-Path $capture "hang_report.json") -Raw
        Assert-That ($hang -match '"kind":"hang"' -and $hang -match '"responsive":false') "Hang report did not confirm the freeze"
        Assert-That ((Get-Content (Join-Path $capture "threads.json") -Raw) -match '"threads"') "Thread snapshot is invalid"
        Assert-That ((Get-Content (Join-Path $capture "analysis.json") -Raw) -match "hang_snapshot") "Analyzer missed the hang"
    } finally {
        Stop-ProcessSafe $diagnose
        if ($fixture) { Stop-ProcessSafe $fixture.Process }
        try { Wait-ApiClosed } catch {}
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
            Assert-That (-not $process.HasExited) "$($renderer.Name) target exited before injection"
            Inject-Dll $process.Id $CoreDll
            Wait-Api
            $screenshotPath = Join-Path $ResultsRoot "$($renderer.Name).png"
            $captured = $false
            $client = [Net.Http.HttpClient]::new()
            $client.DefaultRequestHeaders.Add("X-Cortex-Token", $script:Token)
            try {
                for ($attempt = 0; $attempt -lt 30; ++$attempt) {
                    $response = $client.GetAsync("$BaseUri/screenshot?mode=auto").Result
                    if ([int]$response.StatusCode -eq 200) {
                        $bytes = $response.Content.ReadAsByteArrayAsync().Result
                        if ($bytes.Length -gt 64) {
                            [IO.File]::WriteAllBytes($screenshotPath, $bytes)
                            $captured = $true
                            break
                        }
                    }
                    Start-Sleep -Milliseconds 300
                }
            } finally { $client.Dispose() }
            Assert-That $captured "$($renderer.Name) screenshot was not captured"
            $png = [IO.File]::ReadAllBytes($screenshotPath)
            $isPng = $png.Length -gt 64 -and $png[0] -eq 0x89 -and $png[1] -eq 0x50 -and $png[2] -eq 0x4E -and $png[3] -eq 0x47
            Assert-That $isPng "$($renderer.Name) screenshot is not a PNG"
        } finally {
            Stop-ProcessSafe $process
            try { Wait-ApiClosed } catch {}
        }
    }
}

Run-Scenario "repeated-injection-cycles" {
    foreach ($cycle in 1..3) {
        $fixture = $null
        try {
            $fixture = Start-Fixture "cycle-$cycle"
            Inject-Dll $fixture.Process.Id $CoreDll
            Wait-Api
            Assert-That (Request-Json GET "/health").ok "Cycle $cycle health check failed"
        } finally { Stop-Fixture $fixture }
    }
}

$passed = @($script:Results | Where-Object status -eq "passed").Count
$failed = @($script:Results | Where-Object status -eq "failed").Count
$summary = [pscustomobject]@{
    schema_version = 1
    architecture = $Architecture
    generated_utc = [DateTime]::UtcNow.ToString("o")
    passed = $passed
    failed = $failed
    scenarios = $script:Results
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 (Join-Path $ResultsRoot "e2e-summary.json")

$xml = [Text.StringBuilder]::new()
[void]$xml.AppendLine('<?xml version="1.0" encoding="utf-8"?>'.Replace('\"', '"'))
[void]$xml.AppendLine(('<testsuite name="cortex-e2e-{0}" tests="{1}" failures="{2}">' -f $Architecture, $script:Results.Count, $failed))
foreach ($result in $script:Results) {
    $name = [Security.SecurityElement]::Escape($result.name)
    $seconds = [Math]::Round($result.duration_ms / 1000.0, 3)
    [void]$xml.Append(('  <testcase name="{0}" time="{1}">' -f $name, $seconds))
    if ($result.status -eq "failed") {
        $errorText = [Security.SecurityElement]::Escape($result.error)
        [void]$xml.Append(('<failure message="{0}">{0}</failure>' -f $errorText))
    }
    [void]$xml.AppendLine('</testcase>')
}
[void]$xml.AppendLine('</testsuite>')
$xml.ToString() | Set-Content -Encoding utf8 (Join-Path $ResultsRoot "e2e-junit.xml")

Write-Host "`nCortex E2E: $passed passed, $failed failed"
if ($failed -ne 0) { exit 1 }
