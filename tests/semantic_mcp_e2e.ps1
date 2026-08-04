param(
    [string]$BaseUrl = "http://127.0.0.1:6969",
    [Parameter(Mandatory = $true)]
    [string]$TokenPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

function Invoke-Mcp {
    param([Parameter(Mandatory = $true)]$Payload)
    $body = ConvertTo-Json -InputObject $Payload -Depth 40 -Compress
    return Invoke-RestMethod -Method Post -Uri "$BaseUrl/mcp" -Headers $script:Headers `
        -ContentType "application/json" -Body $body -TimeoutSec 30
}

function Assert-SetEqual {
    param(
        [object[]]$Expected,
        [object[]]$Actual,
        [string]$Message
    )
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
$script:Headers = @{ "X-Cortex-Token" = $token }

$expectedNames = @(
    "capture_runtime_state",
    "observe_visual_state",
    "record_interaction_window",
    "discover_changing_values",
    "discover_stable_values",
    "discover_event_correlations",
    "compare_runtime_states",
    "cluster_memory_changes",
    "search_value_hypotheses",
    "search_unknown_initial_value",
    "refine_value_candidates",
    "search_byte_pattern",
    "search_text_references",
    "search_pointer_references",
    "discover_pointer_paths",
    "validate_pointer_stability",
    "find_code_accessing_address",
    "find_code_writing_address",
    "find_addresses_accessed_by_code",
    "trace_execution_from_event",
    "analyze_function_behavior",
    "discover_callers_and_callees",
    "infer_function_purpose",
    "detect_state_machine",
    "infer_memory_structure",
    "compare_object_instances",
    "discover_object_relationships",
    "classify_memory_candidate",
    "test_candidate_causality",
    "apply_reversible_patch"
)

$initialize = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"
    id = 1
    method = "initialize"
    params = @{}
})
Assert-True ($initialize.result.serverInfo.name -eq "cortex") "initialize returned wrong server name"
Assert-True ($initialize.result.serverInfo.version -eq "0.4.0") "initialize did not report v0.4.0"
Assert-True ($initialize.result.protocolVersion -eq "2024-11-05") "unexpected MCP protocol version"

$list = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"
    id = 2
    method = "tools/list"
    params = @{}
})
$allTools = @($list.result.tools)
$semanticTools = @($allTools | Where-Object {
    (Property-Names $_) -contains "_semantic" -and [bool]$_._semantic
})
Assert-True ($semanticTools.Count -eq 30) "tools/list must expose exactly 30 semantic tools"
Assert-SetEqual -Expected $expectedNames -Actual @($semanticTools.name) `
    -Message "live MCP semantic names differ from the locked v0.4.0 catalog"

$uniqueNames = @($semanticTools.name | Sort-Object -Unique)
Assert-True ($uniqueNames.Count -eq 30) "semantic tool names are not unique"

$allToolNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
foreach ($tool in $allTools) {
    [void]$allToolNames.Add([string]$tool.name)
}

foreach ($tool in $semanticTools) {
    $name = [string]$tool.name
    Assert-True ($tool.inputSchema.type -eq "object") "$name has an invalid input schema type"
    Assert-True ((Property-Names $tool.inputSchema.properties) -contains "objective") "$name lacks objective"
    Assert-True ((Property-Names $tool.inputSchema.properties) -contains "observations") "$name lacks observations"
    Assert-True ((Property-Names $tool.inputSchema.properties) -contains "constraints") "$name lacks constraints"
    Assert-True ((Property-Names $tool.inputSchema.properties) -contains "execute") "$name lacks execute"
    Assert-True (@($tool.inputSchema.required) -contains "objective") "$name does not require objective"

    $dependencies = @($tool._primitives)
    Assert-True ($dependencies.Count -gt 0) "$name has an empty primitive sequence"
    Assert-True (@($dependencies | Sort-Object -Unique).Count -eq $dependencies.Count) `
        "$name contains duplicate dependencies"
    foreach ($dependency in $dependencies) {
        Assert-True ($allToolNames.Contains([string]$dependency)) `
            "$name references a tool absent from live tools/list: $dependency"
    }
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
        constraints = [ordered]@{
            module = "cortex_test_target"
            max_duration_ms = 1500
            mutation_policy = "reversible_only"
        }
    }

    $response = Invoke-Mcp -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = $requestId
        method = "tools/call"
        params = [ordered]@{
            name = $name
            arguments = $arguments
        }
    })
    ++$requestId

    Assert-True (-not [bool]$response.result.isError) "$name returned isError for a valid planning call"
    $structured = $response.result.structuredContent
    Assert-True ($structured.status -eq "plan_ready") "$name did not return plan_ready"
    Assert-True ([double]$structured.confidence -eq 1.0) "$name returned unexpected planning confidence"
    Assert-True ($structured.objective -eq $arguments.objective) "$name did not preserve objective"
    Assert-SetEqual -Expected @($tool._primitives) -Actual @($structured.primitive_sequence) `
        -Message "$name returned a primitive sequence different from tools/list"

    $contractNames = Property-Names $structured.result_contract
    foreach ($requiredField in @(
        "status", "confidence", "evidence", "candidates", "alternative_hypotheses",
        "tested_hypotheses", "recommended_next_tool", "reversible_actions"
    )) {
        Assert-True ($contractNames -contains $requiredField) "$name result contract lacks $requiredField"
    }
    Assert-True (@($structured.rules).Count -ge 5) "$name returned too few evidence/safety rules"

    $textContent = [string]$response.result.content[0].text
    $textDecoded = $textContent | ConvertFrom-Json
    Assert-True ($textDecoded.status -eq $structured.status) "$name text and structured status differ"
    Assert-True ($textDecoded.objective -eq $structured.objective) "$name text and structured objective differ"
}

$actionsAfterPlanning = Invoke-RestMethod -Method Get -Uri "$BaseUrl/actions" -Headers $script:Headers -TimeoutSec 10
$actionsAfterPlanningJson = ConvertTo-Json -InputObject $actionsAfterPlanning -Depth 30 -Compress
Assert-True ($actionsBeforeJson -eq $actionsAfterPlanningJson) `
    "semantic planning calls changed the action journal; plans must be side-effect free"

$executeBatch = [System.Collections.Generic.List[object]]::new()
$batchId = 1000
foreach ($tool in $semanticTools) {
    $executeBatch.Add([ordered]@{
        jsonrpc = "2.0"
        id = $batchId
        method = "tools/call"
        params = [ordered]@{
            name = [string]$tool.name
            arguments = [ordered]@{
                objective = "Attempt bounded execution"
                execute = $true
            }
        }
    })
    ++$batchId
}

$executePayload = $executeBatch.ToArray()
$executeResponses = @(Invoke-Mcp -Payload $executePayload)
Assert-True ($executeResponses.Count -eq 30) "batched execute test did not return 30 responses"
foreach ($response in $executeResponses) {
    Assert-True ($response.result.structuredContent.status -eq "execution_not_available") `
        "execute=true was not rejected explicitly"
    Assert-True (@($response.result.structuredContent.primitive_sequence).Count -gt 0) `
        "execute=true response did not retain the safe plan"
}

$actionsAfterExecute = Invoke-RestMethod -Method Get -Uri "$BaseUrl/actions" -Headers $script:Headers -TimeoutSec 10
$actionsAfterExecuteJson = ConvertTo-Json -InputObject $actionsAfterExecute -Depth 30 -Compress
Assert-True ($actionsBeforeJson -eq $actionsAfterExecuteJson) `
    "execute=true changed the action journal even though server-side execution is unavailable"

$invalidCases = @(
    [ordered]@{ expected = "invalid_objective"; arguments = @{} },
    [ordered]@{ expected = "invalid_objective"; arguments = @{ objective = 42 } },
    [ordered]@{ expected = "invalid_observations"; arguments = @{ objective = "observe"; observations = @{} } },
    [ordered]@{ expected = "invalid_constraints"; arguments = @{ objective = "observe"; constraints = @() } },
    [ordered]@{ expected = "invalid_execute"; arguments = @{ objective = "observe"; execute = "yes" } }
)

foreach ($case in $invalidCases) {
    $response = Invoke-Mcp -Payload ([ordered]@{
        jsonrpc = "2.0"
        id = $requestId
        method = "tools/call"
        params = [ordered]@{
            name = "capture_runtime_state"
            arguments = $case.arguments
        }
    })
    ++$requestId
    Assert-True ([bool]$response.result.isError) "$($case.expected) was not marked as an MCP tool error"
    Assert-True ($response.result.structuredContent.status -eq "failed") "$($case.expected) did not fail"
    Assert-True ($response.result.structuredContent.error -eq $case.expected) `
        "wrong validation error: expected $($case.expected)"
}

$unknown = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"
    id = $requestId
    method = "tools/call"
    params = [ordered]@{
        name = "semantic_tool_that_does_not_exist"
        arguments = @{ objective = "observe" }
    }
})
Assert-True ([int]$unknown.error.code -eq -32601) "unknown tool did not return JSON-RPC -32601"

$primitive = Invoke-Mcp -Payload ([ordered]@{
    jsonrpc = "2.0"
    id = 9000
    method = "tools/call"
    params = [ordered]@{
        name = "health"
        arguments = @{}
    }
})
Assert-True (-not [bool]$primitive.result.isError) "primitive MCP dispatch regressed"
Assert-True ([int]$primitive.result.structuredContent.status -eq 200) "health primitive returned non-200 status"
Assert-True ([bool]$primitive.result.structuredContent.result.ok) "health primitive did not return ok=true"
Assert-True ($null -ne $primitive.result.structuredContent) "primitive MCP result lacks structuredContent"

Write-Host "PASS: live MCP validation covered all 30 semantic tools, schemas, dependencies, calls, batch mode, validation failures, side-effect freedom, and primitive compatibility."
