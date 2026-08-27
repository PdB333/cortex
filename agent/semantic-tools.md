# Cortex semantic tools

Cortex v0.6 includes a semantic layer for AI agents with bounded server-side execution. These tools describe goals in terms of observable runtime behaviour rather than game-specific concepts such as health, ammunition, money, or score.

The semantic layer does not invent domain objects. Every conclusion must include evidence, confidence, alternatives, and a recommended next action. When evidence is insufficient, return `status: not_found` or `status: inconclusive` instead of guessing.

## Common result contract

```json
{
  "status": "candidate_found | confirmed | not_found | inconclusive | failed | cancelled | timed_out",
  "confidence": 0.0,
  "summary": "human-readable conclusion",
  "evidence": [],
  "candidates": [],
  "alternative_hypotheses": [],
  "tested_hypotheses": [],
  "recommended_next_tool": "tool_name",
  "artifacts": {},
  "reversible_actions": []
}
```

Confidence is not a substitute for validation. A value scan only identifies correlation. Use `test_candidate_causality` or another controlled experiment before persisting a finding.

## Server-side execution

Semantic calls are plan-only by default. Set `execute: true` with an explicit non-empty `steps` array to run a bounded primitive sequence inside Cortex.

Execution rules:

- at most 32 primitive steps per semantic request;
- every step must belong to that semantic tool's `_primitives` allowlist;
- nested semantic execution is rejected;
- `timeout_ms` is a cooperative orchestration deadline from 100 ms to 120000 ms, defaulting to 30000 ms;
- `mutation_permission: true` is required before control, mutation, or native-call operations can run;
- active operations without a known rollback contract are rejected before execution;
- supported mutations run inside an action transaction and roll back on failure, observed cancellation, or observed timeout;
- `rollback_on_success: true` can be used for reversible causal experiments;
- later steps can reference prior evidence with `{"$from_step": 0, "pointer": "/result/..."}`;
- `notifications/cancelled` is scoped to the MCP session and observed between primitive calls.

Cancellation and deadlines are cooperative between primitive calls. A blocking primitive still needs its own internal timeout/cancellation mechanism to be interrupted in the middle of that call.

## Observation and discovery

1. `capture_runtime_state` — capture modules, memory regions, threads, project state, watches, breakpoints, patches, freezes, and window state.
2. `observe_visual_state` — capture a frame and optionally run OCR to expose visible state to an agent.
3. `record_interaction_window` — record input, screenshots, watches, and snapshot evidence during a bounded action window.
4. `discover_changing_values` — create or refine unknown-value scans around an observed action.
5. `discover_stable_values` — retain candidates that remain unchanged during control observations.
6. `discover_event_correlations` — rank memory or execution changes by repeated temporal correlation with labelled events.
7. `compare_runtime_states` — compare snapshots, modules, project data, patches, and selected memory ranges.
8. `cluster_memory_changes` — group changed addresses by region, allocation, proximity, type interpretation, and write instruction.

## Search and addressing

9. `search_value_hypotheses` — test multiple numeric encodings and ranges for an observed value.
10. `search_unknown_initial_value` — start a bounded unknown-value scan without assuming a representation.
11. `refine_value_candidates` — apply changed, unchanged, increased, decreased, delta, range, or threshold filters.
12. `search_byte_pattern` — search an AOB signature with wildcards and module restriction.
13. `search_text_references` — locate ASCII/UTF-16 strings and optionally find code/data references.
14. `search_pointer_references` — find direct pointers to or near a target.
15. `discover_pointer_paths` — search stable module-rooted multi-level pointer paths.
16. `validate_pointer_stability` — intersect pointer maps captured across sessions and rank stable paths.

## Code and execution analysis

17. `find_code_accessing_address` — use a page-access watch or read/write breakpoint to identify code touching a region.
18. `find_code_writing_address` — use a hardware write breakpoint and capture registers, stack, and hit history.
19. `find_addresses_accessed_by_code` — trace or capture effective addresses produced by a selected instruction.
20. `trace_execution_from_event` — start a bounded trace linked to a prompt, input sequence, breakpoint, or observation marker.
21. `analyze_function_behavior` — combine disassembly, CFG, structure hints, symbols, trace coverage, and call graph.
22. `discover_callers_and_callees` — combine static xrefs with dynamic trace call graphs.
23. `infer_function_purpose` — produce a labelled hypothesis from evidence without renaming or persisting unless requested.
24. `detect_state_machine` — identify candidate state variables and conditional branches using snapshots, watches, traces, and CFG analysis.

## Objects and structures

25. `infer_memory_structure` — infer a typed structure from several candidate object instances.
26. `compare_object_instances` — snapshot and compare instances to identify distinguishing fields.
27. `discover_object_relationships` — follow pointers, vtables, arrays, managers, and allocation events to build an object graph.
28. `classify_memory_candidate` — classify a candidate as counter, timer, coordinate, flag, pointer, string, enum, state, or unknown.

## Validation and controlled mutation

29. `test_candidate_causality` — perform a bounded reversible write or freeze, observe the result, then roll back automatically.
30. `apply_reversible_patch` — assemble or write a tracked patch, verify bytes, expose the action checkpoint, and support rollback.

## Agent rules

- Start from observations and events, not game-specific assumptions.
- Prefer `module+RVA`, pointer paths, and AOB signatures over absolute addresses.
- Keep scans bounded by module or explicit range where possible.
- Separate correlation, causality, and stability into distinct evidence.
- Use the mutation journal for every experiment that changes memory or code.
- Do not persist a candidate as fact until it survives a controlled validation.
- Store rejected hypotheses so later agents do not repeat failed experiments.
- Explicitly report when a concept does not appear to exist or cannot be observed locally.

## Validation battery

Every pull request touching the semantic or MCP layer runs validation on Windows x86 and x64:

- a standalone C++ catalog test locks the count and names of all 30 tools;
- every schema must require a non-empty `objective` and expose the bounded execution fields;
- every dependency must resolve to a live primitive or another semantic tool;
- the semantic dependency graph must be acyclic and eventually reach a primitive tool;
- malformed arguments and unknown tools must return stable machine-readable errors;
- MCP protocol tests cover legacy negotiation, 2026-07-28 discovery/list behaviour, notifications, batching, and calls;
- native pipe tests lock token-derived rendezvous names and frame-size limits;
- real DLL injection validates HTTP MCP planning and server-side read-only execution;
- the native stdio path validates `cortex_host mcp` -> authenticated Named Pipe -> semantic executor -> native route dispatcher;
- mutation permission gates are checked before dangerous arguments reach a primitive;
- the action journal is compared around plan-only and read-only execution;
- CTest, unified-host checks, and release packaging still run.

The v0.6.0 release workflow performs the same release gate on both x86 and x64 and includes the native stdio MCP E2E before publication. A failing build, semantic test, native transport test, or packaging check prevents publication.
