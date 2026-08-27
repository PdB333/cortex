param(
    [string]$BaseUrl = "http://127.0.0.1:6969",
    [Parameter(Mandatory = $true)]
    [string]$TokenPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Invoke-Mcp {
    param([Parameter(Mandatory = $true)]$Payload)
    $body = ConvertTo-Json -InputObject $Payload -Depth 60 -Compress
    return Invoke-RestMethod -Method Post -Uri "$BaseUrl/mcp" -Headers $script:Headers `
        -ContentType "application/json" -Body $body -TimeoutSec 30
}

function Assert-SetEqual {
    param([object[]]$Expected, [object[]]$Actual, [string]$Message)
    $difference = Compare-Object -ReferenceObject @($Expected | Sort-Object) `
                                 -DifferenceObject @($Actual | Sort-Object)
    Assert-True ($null -eq $difference) $Message
}

function Property-Names {
    param($Object)
    return @($Object.PSObject.Properties.Name)
}

Assert-True (Test-Path $TokenPath) "token file does not exist: $TokenPath"
$token = (Get-Content $TokenPath -Raw).Trim()
Assert-True ($token.Length -ge 32) "token file is empty or malformed"
$script:Headers = @{
    "X-Cortex-Token" = $token
    "X-Cortex-MCP-Session" = "ci-http-semantic"
}

$expectedNames = @(
    "capture_runtime_state", "observe_visual_state", "record_interaction_window",
    "discover_changing_values", "discover_stable_values", "discover_event_correlations",
    "compare_runtime_states", "cluster_memory_changes", "search_value_hypotheses",
    "search_unknown_initial_value", "refine_value_candidates", "search_byte_pattern",
    "search_text_references", "search_pointer_references", "discover_pointer_paths",
    "validate_pointer_stability", "find_code_accessing_address", "find_code_writing_address",
    "find_addresses_accessed_by_code", "trace_execution_from_event", "analyze_function_behavior",
    "discover_callers_and_callees", "infer_function_purpose", "detect_state_machine",
    "infer_memory_structure", "compare_object_instances", "discover_object_relationships",
    "classify_memory_candidate", "test_candidate_causality", "apply_reversible_patch"
)

$initialize = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"; id = 1; method = "initialize"; params = @{ protocolVersion = "2025-11-25" }
})
Assert-True ($initialize.result.serverInfo.name -eq "cortex") "initialize returned wrong server name"
Assert-True ($initialize.result.protocolVersion -eq "2025-11-25") "unexpected MCP protocol version"

$list = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"; id = 2; method = "tools/list"; params = @{}
})
$allTools = @($list.result.tools)
$semanticTools = @($allTools | Where-Object {
    (Property-Names $_) -contains "_semantic" -and [bool]$_._semantic
})
Assert-True ($semanticTools.Count -eq 30) "tools/list must expose exactly 30 semantic tools"
Assert-SetEqual -Expected $expectedNames -Actual @($semanticTools.name) `
    -Message "live MCP semantic names differ from the locked catalog"

foreach ($tool in $semanticTools) {
    $name = [string]$tool.name
    $properties = Property-Names $tool.inputSchema.properties
    foreach ($requiredProperty in @(
        "objective", "observations", "constraints", "execute", "steps", "timeout_ms", "mutation_permission"
    )) {
        Assert-True ($properties -contains $requiredProperty) "$name lacks schema property $requiredProperty"
    }
    Assert-True (@($tool.inputSchema.required) -contains "objective") "$name does not require objective"
    Assert-True ([int]$tool.inputSchema.properties.steps.maxItems -eq 32) "$name steps schema is not bounded"
    Assert-True (@($tool._primitives).Count -gt 0) "$name has an empty primitive allowlist"
}

$actionsBefore = Invoke-RestMethod -Method Get -Uri "$BaseUrl/actions" -Headers $script:Headers -TimeoutSec 10
$actionsBeforeJson = ConvertTo-Json -InputObject $actionsBefore -Depth 30 -Compress

$requestId = 100
foreach ($tool in $semanticTools) {
    $name = [string]$tool.name
    $arguments = [ordered]@{
        objective = "Observe an arbitrary labelled runtime transition for $name"
        observations = @(
            [ordered]@{ label = "before"; sequence = 1 },
            [ordered]@{ label = "after"; sequence = 2 }
        )
        constraints = [ordered]@{ module = "cortex_test_target"; mutation_policy = "reversible_only" }
    }
    $response = Invoke-Mcp -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = $requestId
        method = "tools/call"
        params = [ordered]@{ name = $name; arguments = $arguments }
    })
    ++$requestId

    Assert-True (-not [bool]$response.result.isError) "$name returned isError for planning"
    $structured = $response.result.structuredContent
    Assert-True ($structured.status -eq "plan_ready") "$name did not return plan_ready"
    Assert-True ($structured.lifecycle.current -eq "planned") "$name planning lifecycle is wrong"
    Assert-SetEqual -Expected @($tool._primitives) -Actual @($structured.primitive_sequence) `
        -Message "$name returned a different primitive sequence"
    Assert-True ([bool]$structured.execution_policy.server_side_execution) `
        "$name did not advertise server-side execution"
    Assert-True ([bool]$structured.execution_policy.cancellation_supported) `
        "$name did not advertise cancellation"
}

$actionsAfterPlanning = Invoke-RestMethod -Method Get -Uri "$BaseUrl/actions" -Headers $script:Headers -TimeoutSec 10
$actionsAfterPlanningJson = ConvertTo-Json -InputObject $actionsAfterPlanning -Depth 30 -Compress
Assert-True ($actionsBeforeJson -eq $actionsAfterPlanningJson) `
    "semantic planning calls changed the action journal"

# Actual read-only server-side orchestration over the native route dispatcher.
$execute = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"
    id = 5000
    method = "tools/call"
    params = [ordered]@{
        name = "capture_runtime_state"
        arguments = [ordered]@{
            objective = "Validate bounded read-only server-side execution"
            execute = $true
            timeout_ms = 5000
            steps = @(
                [ordered]@{ tool = "health"; arguments = @{} },
                [ordered]@{ tool = "modules"; arguments = @{} }
            )
        }
    }
})
Assert-True (-not [bool]$execute.result.isError) "read-only execute=true returned isError"
$execution = $execute.result.structuredContent
Assert-True ($execution.status -eq "completed") "read-only semantic execution did not complete"
Assert-True ($execution.lifecycle.current -eq "completed") "read-only semantic execution lifecycle is wrong"
Assert-True (@($execution.evidence).Count -eq 2) "read-only semantic execution lacks evidence"
Assert-True ([int]$execution.evidence[0].output.status -eq 200) "health step failed"
Assert-True ([int]$execution.evidence[1].output.status -eq 200) "modules step failed"

$actionsAfterReadOnlyExecute = Invoke-RestMethod -Method Get -Uri "$BaseUrl/actions" -Headers $script:Headers -TimeoutSec 10
$actionsAfterReadOnlyExecuteJson = ConvertTo-Json -InputObject $actionsAfterReadOnlyExecute -Depth 30 -Compress
Assert-True ($actionsBeforeJson -eq $actionsAfterReadOnlyExecuteJson) `
    "read-only semantic execution changed the action journal"

# Mutation permission must be enforced before malformed/dangerous arguments
# can reach the primitive handler.
$mutationDenied = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"
    id = 5001
    method = "tools/call"
    params = [ordered]@{
        name = "apply_reversible_patch"
        arguments = [ordered]@{
            objective = "Validate mutation permission preflight"
            execute = $true
            steps = @(
                [ordered]@{ tool = "patch_write"; arguments = @{ address = "0x1"; bytes = "90" } }
            )
        }
    }
})
Assert-True ([bool]$mutationDenied.result.isError) "mutation without permission was not rejected"
Assert-True ($mutationDenied.result.structuredContent.error -eq "mutation_permission_required") `
    "mutation permission gate returned wrong error"

$actionsAfterDeniedMutation = Invoke-RestMethod -Method Get -Uri "$BaseUrl/actions" -Headers $script:Headers -TimeoutSec 10
$actionsAfterDeniedMutationJson = ConvertTo-Json -InputObject $actionsAfterDeniedMutation -Depth 30 -Compress
Assert-True ($actionsBeforeJson -eq $actionsAfterDeniedMutationJson) `
    "denied mutation changed the action journal"

$invalidCases = @(
    [ordered]@{ expected = "invalid_objective"; arguments = @{} },
    [ordered]@{ expected = "invalid_observations"; arguments = @{ objective = "observe"; observations = @{} } },
    [ordered]@{ expected = "invalid_constraints"; arguments = @{ objective = "observe"; constraints = @() } },
    [ordered]@{ expected = "invalid_execute"; arguments = @{ objective = "observe"; execute = "yes" } },
    [ordered]@{ expected = "missing_execution_steps"; arguments = @{ objective = "observe"; execute = $true } },
    [ordered]@{ expected = "invalid_timeout"; arguments = @{ objective = "observe"; timeout_ms = 99 } },
    [ordered]@{ expected = "invalid_mutation_permission"; arguments = @{ objective = "observe"; mutation_permission = "yes" } }
)
foreach ($case in $invalidCases) {
    $response = Invoke-Mcp -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = $requestId
        method = "tools/call"
        params = [ordered]@{ name = "capture_runtime_state"; arguments = $case.arguments }
    })
    ++$requestId
    Assert-True ([bool]$response.result.isError) "$($case.expected) was not marked as a tool error"
    Assert-True ($response.result.structuredContent.error -eq $case.expected) `
        "wrong validation error: expected $($case.expected)"
}

$unknown = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"
    id = 9000
    method = "tools/call"
    params = [ordered]@{ name = "semantic_tool_that_does_not_exist"; arguments = @{ objective = "observe" } }
})
Assert-True ([bool]$unknown.result.isError) "unknown tool was not marked as a tool error"
Assert-True ($unknown.result.structuredContent.error -eq "unknown_tool") "unknown tool returned wrong error"

$primitive = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"
    id = 9001
    method = "tools/call"
    params = [ordered]@{ name = "health"; arguments = @{} }
})
Assert-True (-not [bool]$primitive.result.isError) "primitive MCP dispatch regressed"
Assert-True ([int]$primitive.result.structuredContent.status -eq 200) "health primitive returned non-200 status"
Assert-True ([bool]$primitive.result.structuredContent.result.ok) "health primitive did not return ok=true"

Write-Host "PASS: live HTTP MCP planning, native-route execution, permission gates, validation, and primitive compatibility."
