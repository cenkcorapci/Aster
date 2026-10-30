# Contributor protocol

Use this protocol when claiming work in Aster. The
[development plan](development-plan.md) is the canonical record for milestone
status and gates; [tasks.md](tasks.md) contains claimable work and completed
task history. Normative indexing and replication behavior lives in
[indexing.md](indexing.md) and `tla/`.

## Before claiming work

1. Read `README.md`, the relevant milestone and gate in
   [development-plan.md](development-plan.md), and the task's dependencies in
   [tasks.md](tasks.md).
2. Select one `open` task whose dependencies are `done`. Do not bypass a
   milestone gate required by the development plan.
3. Check its lane and **Touch** paths. Avoid paths held by another
   `claimed` or `in_progress` task.
4. Create a branch named `task/<ID>-short-slug`.

## Claim and complete a task

Make the smallest possible edit to `docs/tasks.md` in the first commit of the
branch:

1. Change the task status from `open` to `claimed`.
2. Add a claim comment adjacent to the task:

   ```markdown
   <!-- claim: M?-T?? | contributor: <name> | branch: task/M?-T??-slug -->
   ```

3. Change `claimed` to `in_progress` when implementation begins.
4. If blocked, change the status to `blocked` and record the blocking task or
   prerequisite in the claim comment.
5. If abandoning work, restore `open` and remove the claim comment.
6. Mark a task `done` only after its acceptance criteria are met and the
   change has merged (or under the team's equivalent merge policy).

Claim one task at a time. A stale claim may be released only after checking
the branch or contacting its named contributor when possible.

## Ownership and coordination

The task's **Touch** paths are an exclusive write reservation while it is
claimed or in progress. If a necessary change falls outside them, update the
task before editing or coordinate with the task owner. Do not reorder the task
board while working on a feature.

Take particular care with shared files:

| Path | Coordination rule |
| --- | --- |
| `MODULE.bazel` / `.bazelrc` | Serialize dependency and build-setting changes. |
| `aster/rpc/aster.thrift` | Change only under the IDL compatibility policy. |
| `tla/*.tla` | Keep specification and semantics-changing code aligned. |
| `docs/tasks.md` | Limit feature-branch edits to the claimed task's status and claim. |
| `aster/core/types.h` | Make additive changes when possible; use a dedicated task for breaking changes. |

## Specification and test discipline

For behavior covered by `indexing.md` or TLA+:

1. Update the relevant specification and prose contract before or with code.
2. Run TLC as described in [`../tla/README.md`](../tla/README.md).
3. State the affected property and test evidence in the pull request.

Before review, run the narrowest relevant tests. The core baseline is:

```bash
bazel test //aster/...
```

Run the relevant client checks for client work, and include QA evidence for
soak, fuzz, recall, or benchmark tasks. Keep changes within the task's
acceptance criteria; create a new task for follow-up work rather than calling
an incomplete task done.

## Pull request checklist

```markdown
## Task
- ID: M?-T??
- Lane: …

## Summary
- …

## Spec / docs
- [ ] No semantic change
- [ ] TLA+ updated and TLC green, if applicable
- [ ] Relevant documentation updated

## Test plan
- [ ] Narrowest applicable tests pass
- [ ] Acceptance criteria evidence included

## Touch compliance
- Paths edited: …
- Outside original Touch? no / yes (reason: …)
```

## References

| Doc | Role |
| --- | --- |
| [tasks.md](tasks.md) | Claimable work and task history |
| [development-plan.md](development-plan.md) | Milestones, gates, and dependencies |
| [indexing.md](indexing.md) | Indexing and distributed-search contract |
| [design.md](design.md) | Architecture |
| [code-structure.md](code-structure.md) | Layers and profiles |
| [client-api.md](client-api.md) | Collection API |
| [`../tla/README.md`](../tla/README.md) | TLC instructions |
