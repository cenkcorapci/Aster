# Start the tasks — parallel agent playbook

This is the kickoff document for humans and AI agents working on Aster
**at the same time**. The task list lives in [`tasks.md`](tasks.md). The
roadmap lives in [`development-plan.md`](development-plan.md). Normative
behavior lives in [`indexing.md`](indexing.md) and `tla/`.

If you are an agent: read this file fully, then claim one task, then work.

---

## 1. Goals of this protocol

- Many agents make progress without stomping each other's files.
- Every change maps to a task ID (traceability).
- Specs stay ahead of semantics-changing code.
- The repo stays buildable (`bazel test //aster/...` green) on `main`.

---

## 2. First-time agent checklist

1. Read `README.md`, then `docs/development-plan.md` (milestones + gates).
2. Skim `docs/tasks.md` — find `Status: open` tasks whose `Depends` are all
   `done`.
3. Prefer a task in an **idle lane** (see §4). If every open task in a lane
   is claimed, pick another lane.
4. Claim exactly **one** task (§5). Do not claim a second until the first is
   `done` or you unclaim it.
5. Create a branch named `task/<ID>-short-slug` (example: `task/M1-T01-sstable-rfc`).
6. Implement; keep scope inside the task's **Touch** paths.
7. Run the relevant tests before opening a PR (§7).
8. Open a PR whose title starts with the task ID: `M1-T01: Spec SSTable layout`.
9. Mark the task `done` only after the PR is merged (or your team’s equivalent).

---

## 3. What “done” means

A task is done only when **all** of its **Done when** criteria are met,
tests exist (or are explicitly waived in the PR), and docs/TLA updates
required by §6 are included in the same PR.

Do not mark `done` for “mostly working” or “follow-up later” — split a
follow-up into a new task ID instead.

---

## 4. Lanes and file ownership

Lanes reduce merge conflicts. While a task is `claimed` / `in_progress`:

| You own (exclusive write) | You may read |
| --- | --- |
| Paths listed in the task **Touch** column | Anything else |

### Soft locks

- If you must edit outside **Touch**, either:
  - expand **Touch** in `tasks.md` when claiming, or
  - stop and pick a different task / ask for a coordinating agent.
- Never edit another agent's claimed **Touch** set.
- Shared files need extra care (serialize or co-claim):

  | Hot file | Rule |
  | --- | --- |
  | `MODULE.bazel` / `.bazelrc` | Only `release` lane, or tiny additive deps with PR note |
  | `aster/rpc/aster.thrift` | Only after `M5-T01` freeze protocol; one changer at a time |
  | `tla/*.tla` | Spec-first: land TLC-green PR before code that relies on new semantics |
  | `docs/tasks.md` | Allowed for claim/status edits only; don't reorder wholesale in feature PRs |
  | `aster/core/types.h` | Prefer additive changes; breaking changes need a dedicated task |

### Safe parallel sets (examples)

These can run concurrently with low collision:

```
Agent A: M1-T01  (storage RFC)           lane storage
Agent B: M3-T05  (compile-time profiles) lane platform
Agent C: M4-T06  (metrics enrichment)    lane obs
Agent D: docs/spec polish                lane spec
```

After M1-T03 lands, typical fan-out:

```
Agent A: M1-T09/T10  (flush/compaction)  lane db
Agent B: M2-T01/T02  (HNSW structures)   lane index
Agent C: M3-T03      (PosixStorage)      lane platform
```

After M4-T02 + M5-T01:

```
Agents: M5-T04 … M5-T10  (one language each)  lanes-* lanes
Agent:  M5-T03           (conformance corpus) lane qa
```

### Hard gates (do not bypass)

| Gate | Rule |
| --- | --- |
| M2 exit | No M7 coding until M2 exit criteria are met (plan). |
| Spec before semantics | Lifecycle / replication behavior changes: update TLA+ + TLC green in a PR **before** or **with** the code PR, never after silently. |
| IDL freeze | No client transport PRs until `M5-T01` is done. |
| Milestone exit tasks | `M1-T14`, `M2-T10/T11`, etc. are QA gates — features aren't “milestone done” until those pass. |

---

## 5. Claiming protocol

Edit `docs/tasks.md` in a tiny PR or the first commit of your branch:

1. Set **Status** from `open` → `claimed`.
2. Add a claim line under the task (or in a nearby comment block):

```markdown
<!-- claim: M1-T01 | agent: <name-or-model> | branch: task/M1-T01-sstable-rfc | since: 2026-08-07 -->
```

3. When you start coding: `claimed` → `in_progress`.
4. If blocked on another task/PR: `in_progress` → `blocked` and note the blocker in the claim comment.
5. If you abandon: revert status to `open`, delete the claim comment, push.

### Stale claims

If a claim is older than **72 hours** with no commits on the branch, any
agent may unclaim it (status → `open`, remove claim comment) and take it.
Prefer pinging first if a human owner is named.

### One task per agent

Parallelism comes from **multiple agents**, not one agent juggling five
IDs. Exception: a single agent may hold a **parent** design task and a
tiny dependent doc task in the same branch if **Touch** sets are nested.

---

## 6. Spec / docs discipline

Before changing behavior described in `indexing.md` or checked by TLA+:

1. Open or update a `spec` lane task if the change is non-trivial.
2. Edit `tla/*.tla` / `.cfg` as needed.
3. Run TLC until green (see `tla/README.md`).
4. Update `indexing.md` if the prose contract changed.
5. Then (or in the same PR if small) change C++.

For the classic trap: tombstone purge, LWW visibility, SegState
PENDING→READY, quorum visibility — **always** map the test name to the
TLA property in the PR body (e.g. “pins `NoResurrection`”).

---

## 7. Build, test, PR bar

Minimum before asking for review:

```bash
bazel test //aster/...
```

If you touched TLA+:

```bash
cd tla && java -jar tla2tools.jar -workers auto AsterLsmIndex.tla
# and/or AsterReplication.tla if that module changed
```

If you touched a client package, run that language’s local checks too
(once M5/M6 tooling exists).

### PR template (paste into description)

```markdown
## Task
- ID: M?-T??
- Lane: …
- Claim comment updated: yes

## Summary
- …

## Spec / docs
- [ ] No semantic change
- [ ] TLA+ updated + TLC green
- [ ] indexing.md / plan / tasks updated

## Test plan
- [ ] `bazel test //aster/...`
- [ ] New/updated unit tests for Done-when criteria
- [ ] (if QA task) soak/fuzz/recall evidence linked

## Touch compliance
- Paths edited: …
- Outside original Touch? no / yes (reason: …)
```

---

## 8. Coordinator agent (optional)

When many agents run, designate one **coordinator** (human or agent) who:

- Watches `tasks.md` for duplicate claims and stale locks.
- Unblocks `blocked` tasks by sequencing merges.
- Keeps the “Parallelism snapshot” section of `tasks.md` current.
- Does **not** implement large features while coordinating (conflict of interest with lane ownership).

Coordinator commands (natural language is fine):

- “Unclaim stale M1-T04”
- “What open tasks are unblocked in lane index?”
- “After M1-T03 merges, spawn agents on M1-T09 and M2-T01”

---

## 9. Spawning parallel agents — prompt recipe

Give each agent a copy of this prompt shape:

```text
You are an Aster coding agent. Follow docs/start-the-tasks.md strictly.

1. Open docs/tasks.md.
2. Claim task <ID> (or pick the highest-priority open task in lane <LANE>
   whose Depends are all done). Prefer: <ID>.
3. Branch: task/<ID>-<slug>
4. Implement only that task’s Done-when criteria.
5. Do not edit files outside Touch unless you update the claim.
6. Run bazel test //aster/... (and TLC if specs change).
7. Open a PR titled "<ID>: …" using the PR template in start-the-tasks.md.
8. Leave the task in_progress until merge; then mark done.

Repo rules: C++20, Bazel, no drive-by refactors, match existing style.
Normative: docs/indexing.md and tla/ for indexing/replication semantics.
```

Launch **different lanes** in one wave. Example first wave after M0:

| Agent | Task | Lane |
| --- | --- | --- |
| 1 | M1-T01 | storage |
| 2 | M3-T05 | platform |
| 3 | M4-T06 | obs |
| 4 | (optional) expand TLC bounds / docs | spec |

Second wave (after M1-T01 + M1-T02 path is moving):

| Agent | Task | Lane |
| --- | --- | --- |
| 1 | M1-T03 / M1-T05 (same agent OK if sequential) | storage |
| 2 | M2-T01 (format + structs; build waits for disk segment) | index |
| 3 | M3-T03 | platform |

---

## 10. Priority order when choosing freely

If no ID was assigned, pick the first match:

1. Unblock the critical path: **M1 → M2 → M7 → M8**.
2. Open tasks with empty Depends in an idle lane.
3. QA gate tasks for a milestone that is otherwise feature-complete.
4. Client tasks only after server + IDL freeze.
5. Nice-to-haves (optional opts like M2-T06) last.

---

## 11. Anti-patterns

- Claiming a whole milestone (“I’ll do M1”).
- Rewriting the ring / Db facade while holding a storage format task.
- Implementing HNSW by mutating a global graph (forbidden by indexing.md).
- Purging tombstones in partial compaction (TLA counterexample exists).
- Publishing clients against an unfrozen IDL.
- Force-pushing shared branches; rewriting `main` history.
- Marking `done` without tests for **Done when**.

---

## 12. Quick links

| Doc | Role |
| --- | --- |
| [`tasks.md`](tasks.md) | Claimable work |
| [`development-plan.md`](development-plan.md) | Milestones & exits |
| [`indexing.md`](indexing.md) | Indexing / distributed search contract |
| [`design.md`](design.md) | Architecture |
| [`code-structure.md`](code-structure.md) | Layering & platforms |
| [`client-api.md`](client-api.md) | Collection API |
| [`../tla/README.md`](../tla/README.md) | How to run TLC |
| [`../clients/README.md`](../clients/README.md) | Client contract & release train |
