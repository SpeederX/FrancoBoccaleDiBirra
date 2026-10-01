# Harness foundation and project mode

Status: first adapter milestone, 2026-10-01, `feature/harness-foundation`.
The release checkout still has command, scratchpad, and diary. The existing
`feature/project-mode` worktree has only a selectable fourth-mode UI scaffold.

The user clarified that the agent is [Pi](https://github.com/earendil-works/pi).
Pi owns the tool loop and session machinery. Franco owns a shared agent service
that project mode and other modes can call. The same backend/model selected by
Franco must be used for Pi inference and compaction.

## Agreed work and current evidence

| Work | Current state | Completion evidence still needed |
| --- | --- | --- |
| Canonical instructions and checkout status | Exact human AGENTS.md versioned; CLAUDE.md routes to it; README corrected; commands in CONTRIBUTING.md | Review instruction usability in a real project task |
| Release checks, verify command, CI | Always-active native checks, negative check, Windows Debug/Release pass; Windows/Linux CI configured | Hosted CI results and Linux build execution |
| Observed intent/persistence regressions | Findings recorded in the root readiness report | Synthetic replay tests; demonstrated failure before fix; persistence write failures and truthful UI feedback |
| Bounded project workflow | Shared Pi adapter implemented and tested separately from the native UI | Router lifecycle, process pipes, UI controls; bug reproduction, patch, regression run, reviewed diff |
| 4B baseline/graph/agent/combined comparison | Evaluation shape defined below | Select exact model/quantization and run the fixed tasks; no reliability measurement yet |

AGENTS.md was copied byte-for-byte from the human-maintained root file. Future
operational documentation belongs outside that policy file. The root README logo
change remains separate from the harness branch.

## First native integration

1. Backend manager starts or attaches to one llama.cpp router using the selected
   INI file. Model inventory comes from the router; loading and unloading are
   jobs with progress and errors. Resolve whether `/add` and `/remove` edit the
   INI or the user maintains it; do not keep two authoritative registries.
2. Add bidirectional process pipes while preserving the existing process-tree
   lifetime behavior. Start the Node worker for an explicit project working copy.
3. Bind the exact loaded endpoint/preset ID. Coordinate unload/reload with active
   sessions so model changes cannot race a tool run or compaction.
4. Expose session identity, loaded context, requested/effective thinking, output
   cap, compaction settings and events, active tools, cancellation, and failures.
   Store user-facing defaults in application configuration, not feature env vars.
5. Run one well-specified bug workflow: reproduce the failing case, make the
   patch, run the regression and repository verification, present the resulting
   diff and measured checks. Agent text alone never establishes completion.

The adapter protocol and current limitations are in
[tools/agent-service/README.md](../tools/agent-service/README.md).

## Graphify role

The project scaffold names [Graphify-Labs/graphify](https://github.com/Graphify-Labs/graphify).
Its repository describes local deterministic code parsing and graph queries,
with extracted/inferred edge labels; documents/media can use a model-backed
semantic pass. That is a context provider for project tools, while Pi runs the
agent. No Graphify dependency has been installed in this milestone.

Qualify and pin its CLI/version, Windows support, C symbol/call coverage,
incremental update behavior and source provenance before wiring it. Indexing
must run as a visible job. Queries should return a bounded subgraph with file
and line references, graph revision, and extracted/inferred labels. A stale or
failed index must be visible. Any optional semantic pass must use the selected
local backend; additional providers need explicit user agreement.

First integration should retain ordinary file/search tools and compare their
results against graph-assisted retrieval. A graph may help navigation; only the
task comparison can establish whether its overhead improves this harness.

## Small-model qualification

Record the exact GGUF identity and quantization, model/preset ID, llama.cpp build,
Pi version, template, loaded context, thinking mode, output/compaction budgets,
tool list, graph version, task fixtures, and trace identity for every run.

Use the same model and task set across baseline file/search, Graphify context,
bounded agent workflow, and combined workflow. Include ordinary fixes, unrelated
commands, malformed tool arguments, failed tools, overflowing context, summary
fact retention, resumed sessions, cancellation, and a case that needs a targeted
clarification. Check artifacts and executed commands, not instruction recall.

Measure task/test completion, unintended modifications, tool-call failures,
retries, context/compaction behavior, token counts and elapsed time with units.
Retain all failures and compare repeated runs within equal budgets. Human review
calibrates qualitative judgments. Define acceptance from those observed tasks
and the user's tolerance for errors, rather than asserting a reliability
percentage before collecting evidence. Use synthetic data in the fixtures.
