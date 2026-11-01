# Evidence records

Cortex keeps one evidence record type for what a trial, a scan, a breakpoint
or a static analysis showed. Records live with the MCP host, not in the target
process, so they stay readable after the process has exited. The default store
is the user data folder (`%LOCALAPPDATA%\Cortex\evidence`); the operator can
choose another with `cortex.exe mcp --evidence-store <dir>`. There is one JSON
file per record, written atomically.

A record is what was recorded and how Cortex checked it. It is not proof of
semantics or causality.

## Record (`cortex.evidence.v1`)

| Field | Set by | Meaning |
|---|---|---|
| `id` | Cortex | `ev_` + 20 hex digits |
| `revision`, `history` | Cortex | +1 on each change; up to 16 previous versions |
| `class` | caller, checked | see below |
| `origin` | caller | `static`, `decompiled` or `observed` |
| `statement`, `notes` | caller | the claim, in words |
| `subject` | Cortex or caller | `sha256`, `name`, `format`, `location`, `identity`, `fingerprint` |
| `produced_by` | caller | `{tool, arguments, expect: [{pointer, value}]}`: a read-only artifact tool Cortex reruns |
| `references` | caller | up to 16 `{kind, id, detail}`; kinds: trial, report, artifact, knowledge, evidence, scan, breakpoint, trace, context |
| `relations` | caller | up to 16 `{type, to, origin, note}`; `to` is `{evidence: id}` or `{sha256, location}` |
| `verification` | Cortex | `status` (`reproduced`, `not_reproduced`, `not_checked`, `unavailable`), `output_sha256`, `mismatches`, `trials` |
| `stale` | Cortex | `true` when a later verification no longer holds |
| `knowledge_status` | Cortex | the project knowledge status the class maps to |
| `recorded_by`, `updated_by` | Cortex | `agent`, `desktop` or `cli` |

**Subject.**
- When given a `path` inside `--artifact-root`, Cortex reads the file and
  records its SHA-256, name and format (`identity: computed_by_cortex`).
- For an `rva:` location in a PE, it also stores a fingerprint of the code
  there.
- A subject given only by `sha256` is marked `client_asserted`.

**Locations:**
- `rva:0x…`: native code or data;
- `token:0x…`: .NET metadata;
- `js:file:line:col`: JavaScript;
- `module:name+0x…`: a module of a running process.

**Relation types:** `calls`, `reads`, `writes`, `handled_by`, `exposes`,
`loads`, `compiled_to`, `same_as`, `correlates`, `contradicts`,
`depends_on`, `related_to`.

A relation between subjects can cross runtimes. For example, an IPC
channel at `js:main.js:25:16` is `handled_by` a function in the same ASAR;
that function `calls` an export of a `.node` PE at `rva:0x…`.

## Classes and their requirements

The store checks each class when a record is created or updated. An unmet
requirement is refused with `class_requirements_not_met` and a `reason`. A
class is never changed silently.

| Class | Requirement |
|---|---|
| `verified_observation` | `origin: static/decompiled`: `produced_by`, which Cortex reruns with every `expect` value equal (`verification.status: reproduced`), and whose arguments name the same path as the subject. `origin: observed`: a `trial` reference that the test runner reports `completed` with an outcome other than `inconclusive` |
| `correlation` | at least two references or relations |
| `hypothesis` | none |
| `contradictory` | a `contradicts` relation to an existing record |
| `inconclusive` | a reference or `produced_by` |
| `refuted` | a reference, a reproduced verification or a valid trial |

`cortex_evidence_verify` reruns the check later. If the file changed and the
expectations no longer hold, the record becomes `stale: true` and keeps its
class. Reclassifying it is an explicit `cortex_evidence_update`.

### Mapping to project knowledge statuses

There are no silent upgrades in either direction:

| Evidence class | Knowledge status |
|---|---|
| `verified_observation` | `observed` |
| `refuted` | `refuted` |
| any other | `hypothesis` |

| Knowledge status (a client assertion) | Evidence class at most |
|---|---|
| `observed` | `correlation` |
| `refuted` | `refuted` |
| `hypothesis`, `stale` | `hypothesis` |

## Comparing builds

`cortex_evidence_compare {sha256, path}` places the records about an older
build in another build of the artifact. For each record it returns one of:
- `same_binary`;
- `unchanged_at_rva`: same code fingerprint at the same RVA;
- `moved`: the only function with that fingerprint, with `new_location`;
- `ambiguous`: several candidates;
- `not_found`;
- `not_comparable`: no `rva:` location or no fingerprint.

The fingerprint is the SHA-256 of a function's first instructions (up to 64
bytes). Relative branch targets, RIP-relative displacements and in-image
absolute addresses are zeroed, so moved code still matches. Matching code
does not prove matching behaviour. Records are never modified; re-record
them on the new build.

## Export

`cortex_evidence_export {format: json|markdown, sha256?}` returns:
- `cortex.evidence.export.v1`: `{schema, exported_ms, records}`;
- or a Markdown report grouped by artifact, with each record's class,
  origin, location, verification, trials, references and relations.

The CLI writes the same export without size limits:

```bash
cortex.exe evidence export --format markdown --out evidence.md
```

```bash
cortex.exe evidence list --sha256 <sha256>
```

## Permissions

`cortex_evidence_record`, `_update` and `_verify` write Cortex state. They
need `mutation_permission: true` and the operator's `--allow-state` (or
`--allow-writes`). `_get`, `_list`, `_export` and `_compare` are read-only.
None of them reads or changes a target process.
