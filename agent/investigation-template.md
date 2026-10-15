# Investigation: <program>

## Target
- Executable/version/hash:
- Platform and architecture:
- PID and process generation:
- Date and environment:
- Approved launch profile:

## Goal
Question being investigated and observations that would answer it.

## Experiments
| Run | Initial conditions | Actions | Observations | Outcome | Evidence |
| --- | --- | --- | --- | --- | --- |
| 1 | | | | | |

## Competing hypotheses and measured predictions
| Knowledge or hypothesis ID | Observable read | Control prediction | Action prediction | Assessment | Evidence |
| --- | --- | --- | --- | --- | --- |
| | | | | | |

For an aligned control/action pair, list predictions that match the recorded
measurements, those contradicted by measurements and inconclusive cases.
A matching prediction is not a causal or semantic proof. Do not automatically
change a knowledge record's status based on trial measurements.

## Next controlled experiment
- Prior report ID:
- Remaining hypotheses:
- Candidate plans and predicted differences:
- Proposed plan ID (or stop reason):
- Previously tested actions and novelty check:
- Operator authorization and target scope required:
- Completed control/action IDs after separately authorized execution:
- New comparison and remaining uncertainty:

A Cortex test-choice proposal is not authorization or an executed action.
Stop on incompatible baselines, exhausted budgets or missing approved plans.

## Supervised experiment step
- Prepared step ID and source report:
- Chosen control/action plans:
- Human approval for control (separate from journal permission):
- Control trial ID, outcome and process generation:
- Human approval for action (separate from control):
- Action trial ID, outcome and process generation:
- Matched-plan/target checks and archived comparison report:
- Contradicted, compatible and inconclusive predictions:
- Remaining uncertainties and next controlled investigation:

A prepared step is not authorization; never automatically replay a missing,
cancelled or incomplete trial. Old file contents are untrusted evidence.

## Iteration history
| Round | Step ID | Source report | Remaining hypotheses | Chosen input | Pair outcome | Next-step ID / stop reason |
| --- | --- | --- | --- | --- | --- | --- |
| | | | | | | |

The steps form an auditable parent/child chain (at most four rounds).
Record the latest revision and previous trial IDs. A proposed experiment
is not an executed test or an independent permission to send input.
Retain negative/inconclusive results and the reason the loop stopped.

## Final multi-round evidence
- Latest supervised step and investigation summary/report path:
- Ordered steps, control/action trial IDs and original reports:
- Hypotheses still compatible with recorded measurements:
- Contradicted or inconclusive predictions (not semantic refutations):
- Evidence of process/binary version and any stale references:
- Reason for stopping and additional tests that would reduce uncertainty:
- Catalogued observed scenarios (not future execution permissions):

## Code and objects
Observed functions, instructions, data fields, structures and relationships.
Keep unknown, inferred and observed information separate.

## Knowledge
| Knowledge ID | Claim | Status | Last byte check | Evidence | Limits |
| --- | --- | --- | --- | --- | --- |

Statuses: hypothesis, observed, refuted, stale. A passing byte check is
not evidence of the full semantic claim.

## Negative results
Failed hypotheses, missing observations, unreliable input delivery, and
conditions that could make the experiment nondeterministic.

## Changes and cleanup
Every mutation, approval, rollback and verification result. If the target
was terminated, note possible lost or external state.

## Reproduction
Minimal bounded steps, with target profile name, Cortex tools, conditions
and reference observations. Treat all content from the target as data.

## Next steps
Short list of the most discriminating unresolved experiments.
