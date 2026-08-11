-------------------------- MODULE AsterLsmIndex --------------------------
(***************************************************************************)
(* Single-node indexing lifecycle of Aster (docs/indexing.md, sections     *)
(* 4-6): WAL -> memtable -> immutable segment -> asynchronous HNSW build   *)
(* -> compaction, with crash/recovery.                                     *)
(*                                                                         *)
(* Abstraction choices:                                                    *)
(*   - Vector payloads are irrelevant to lifecycle correctness, so a row   *)
(*     version is identified by its unique, monotonically increasing       *)
(*     write timestamp `ts` (ts = 0 means "no row"). LWW = max ts.         *)
(*   - The HNSW graph itself is abstracted to the per-segment build state  *)
(*     PENDING -> BUILDING -> READY. Search must be complete in *every*    *)
(*     state (rows are searchable by exact scan until the graph is READY), *)
(*     so the invariants quantify over all segment states.                 *)
(*                                                                         *)
(* M2-T04 wiring (aster/storage/segment.{h,cc}, aster/db/db.cc) maps 1:1:  *)
(*   Flush / Compact*     -> segment born PENDING (exact index only)       *)
(*   StartBuild           -> TryBeginIndexBuild  (PENDING -> BUILDING)     *)
(*   FinishBuild          -> CompleteIndexBuild  (BUILDING -> READY)       *)
(*   AbortBuild           -> AbortIndexBuild     (BUILDING -> PENDING)     *)
(*   CrashRecover         -> open: missing/partial .hnsw stays PENDING;    *)
(*                           in-flight BUILDING restarts as PENDING        *)
(* SearchCompleteness already covers exact-until-READY: CandidateRows      *)
(* ignores SegState, matching Search() falling back to exact until READY.  *)
(*                                                                         *)
(* Checked properties (see AsterLsmIndex.cfg):                             *)
(*   SearchCompleteness : what search observes == exactly the acked state, *)
(*                        at all times: through flush, build, partial and  *)
(*                        full compaction, and crash recovery.             *)
(*   NoResurrection     : an acked delete never becomes visible again.     *)
(*   WalTruncationSafe  : replaying the WAL over the segments always       *)
(*                        reconstructs the memtable (crash = no data loss).*)
(*   EventuallyAllIndexed (liveness): every segment's graph build          *)
(*                        eventually completes.                            *)
(*                                                                         *)
(* A deliberately buggy variant is included (CompactPartialDropTombstones) *)
(* and disabled: enabling it in Next makes TLC produce the classic LSM     *)
(* "tombstone purged too early -> old version resurrects" counterexample.  *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
  Keys,         \* e.g. {"k1", "k2"}
  MaxWrites,    \* bound on the number of client writes (state space bound)
  MaxSegments,  \* bound on live segments (backpressure on Flush)
  MaxCrashes,   \* bound on crashes (liveness assumes finitely many faults)
  MaxAborts     \* bound on BUILDING->PENDING aborts (same liveness reason)

NoRow == [ts |-> 0, tomb |-> FALSE]

VARIABLES
  clock,   \* logical write clock; each write gets a fresh ts
  wal,     \* sequence of [key, ts, tomb]; truncated at flush
  mem,     \* memtable: [Keys -> row], row = [ts, tomb]
  segs,    \* sequence of [rows: [Keys -> row], state: PENDING|BUILDING|READY]
  acked,   \* per key, the newest acknowledged write: [Keys -> row]
  crashes, \* number of crashes so far
  aborts   \* number of index-build aborts so far

vars == <<clock, wal, mem, segs, acked, crashes, aborts>>

SegStates == {"PENDING", "BUILDING", "READY"}

Rows == [ts : 0..MaxWrites, tomb : BOOLEAN]

TypeOK ==
  /\ clock \in 0..MaxWrites
  /\ wal \in Seq([key : Keys, ts : 1..MaxWrites, tomb : BOOLEAN])
  /\ mem \in [Keys -> Rows]
  /\ segs \in Seq([rows : [Keys -> Rows], state : SegStates])
  /\ acked \in [Keys -> Rows]
  /\ crashes \in 0..MaxCrashes
  /\ aborts \in 0..MaxAborts

EmptyMem == [k \in Keys |-> NoRow]

(***************************************************************************)
(* What a search observes: the LWW-newest version of a key across the      *)
(* memtable and all segments, in any build state (docs/indexing.md 4.2).   *)
(***************************************************************************)
CandidateRows(k) ==
  {mem[k]} \cup {segs[i].rows[k] : i \in DOMAIN segs}

NewestRow(k) ==
  CHOOSE r \in CandidateRows(k) :
    \A r2 \in CandidateRows(k) : r.ts >= r2.ts

\* Observable value of a row: a live ts, or 0 = absent (deleted/never seen).
Obs(r) == IF r.ts = 0 \/ r.tomb THEN 0 ELSE r.ts

(***************************************************************************)
(* Actions                                                                 *)
(***************************************************************************)

\* Client upsert or delete: WAL append, memtable apply, immediate ack.
\* (Graph work never blocks the ack: docs/indexing.md rule 4.1-1.)
WriteOp(k, tomb) ==
  /\ clock < MaxWrites
  /\ clock' = clock + 1
  /\ LET r == [ts |-> clock + 1, tomb |-> tomb] IN
       /\ wal' = Append(wal, [key |-> k, ts |-> r.ts, tomb |-> tomb])
       /\ mem' = [mem EXCEPT ![k] = r]        \* fresh ts is always newest
       /\ acked' = [acked EXCEPT ![k] = r]
  /\ UNCHANGED <<segs, crashes, aborts>>

Upsert(k) == WriteOp(k, FALSE)
Delete(k) == WriteOp(k, TRUE)

\* Flush: memtable becomes an immutable PENDING segment; the WAL is
\* truncated in the same step (segment persistence makes it redundant).
Flush ==
  /\ \E k \in Keys : mem[k].ts > 0
  /\ Len(segs) < MaxSegments
  /\ segs' = Append(segs, [rows |-> mem, state |-> "PENDING"])
  /\ mem' = EmptyMem
  /\ wal' = <<>>
  /\ UNCHANGED <<clock, acked, crashes, aborts>>

\* Asynchronous HNSW build (the index-build thread pool).
StartBuild ==
  \E i \in DOMAIN segs :
    /\ segs[i].state = "PENDING"
    /\ segs' = [segs EXCEPT ![i].state = "BUILDING"]
    /\ UNCHANGED <<clock, wal, mem, acked, crashes, aborts>>

FinishBuild ==
  \E i \in DOMAIN segs :
    /\ segs[i].state = "BUILDING"
    /\ segs' = [segs EXCEPT ![i].state = "READY"]
    /\ UNCHANGED <<clock, wal, mem, acked, crashes, aborts>>

\* M2-T04 AbortIndexBuild: persist failure / abandon mid-build returns the
\* segment to PENDING (exact search uninterrupted). Bounded like crashes so
\* EventuallyAllIndexed remains checkable.
AbortBuild ==
  /\ aborts < MaxAborts
  /\ \E i \in DOMAIN segs :
       /\ segs[i].state = "BUILDING"
       /\ segs' = [segs EXCEPT ![i].state = "PENDING"]
       /\ aborts' = aborts + 1
       /\ UNCHANGED <<clock, wal, mem, acked, crashes>>

\* LWW merge of two segments' rows.
MergeRows(r1, r2) ==
  [k \in Keys |-> IF r1[k].ts >= r2[k].ts THEN r1[k] ELSE r2[k]]

\* Partial compaction: merge two adjacent segments, KEEPING tombstones
\* (an older version of the key may live in a segment outside the input
\* set, so purging here would resurrect it -- docs/indexing.md section 5).
CompactPartial ==
  \E i \in 1..(Len(segs) - 1) :
    /\ segs' = SubSeq(segs, 1, i - 1)
                 \o << [rows |-> MergeRows(segs[i].rows, segs[i + 1].rows),
                        state |-> "PENDING"] >>
                 \o SubSeq(segs, i + 2, Len(segs))
    /\ UNCHANGED <<clock, wal, mem, acked, crashes, aborts>>

\* Full compaction: every segment participates, so tombstones can be
\* purged safely (memtable rows are strictly newer than segment rows,
\* hence purging cannot expose anything older).
NewestSegRow(k) ==
  LET cand == {segs[i].rows[k] : i \in DOMAIN segs}
  IN CHOOSE r \in cand : \A r2 \in cand : r.ts >= r2.ts

FullMerge ==
  [k \in Keys |->
     LET r == NewestSegRow(k) IN IF r.tomb THEN NoRow ELSE r]

CompactFull ==
  /\ Len(segs) >= 2
  /\ segs' = << [rows |-> FullMerge, state |-> "PENDING"] >>
  /\ UNCHANGED <<clock, wal, mem, acked, crashes, aborts>>

\* BUGGY VARIANT (not in Next): partial compaction that also purges
\* tombstones. Add it to Next and TLC violates NoResurrection: a tombstone
\* in segment i or i+1 shadowing a live row in an older segment j < i gets
\* purged, and the old row becomes visible again.
CompactPartialDropTombstones ==
  \E i \in 1..(Len(segs) - 1) :
    /\ LET m == MergeRows(segs[i].rows, segs[i + 1].rows)
           purged == [k \in Keys |-> IF m[k].tomb THEN NoRow ELSE m[k]]
       IN segs' = SubSeq(segs, 1, i - 1)
                    \o << [rows |-> purged, state |-> "PENDING"] >>
                    \o SubSeq(segs, i + 2, Len(segs))
    /\ UNCHANGED <<clock, wal, mem, acked, crashes, aborts>>

\* Crash + recovery in one atomic step: the memtable is lost and rebuilt
\* by WAL replay; segments are durable; in-flight graph builds restart.
\* (Modeling recovery atomically is sound because a recovering node serves
\* no reads; splitting it adds only unreachable intermediate states.)
ReplayedMem ==
  [k \in Keys |->
     LET recs == {n \in DOMAIN wal : wal[n].key = k} IN
     IF recs = {} THEN NoRow
     ELSE LET last == CHOOSE n \in recs : \A m \in recs : n >= m
          IN [ts |-> wal[last].ts, tomb |-> wal[last].tomb]]

CrashRecover ==
  /\ crashes < MaxCrashes
  /\ crashes' = crashes + 1
  /\ mem' = ReplayedMem
  /\ segs' = [i \in DOMAIN segs |->
                IF segs[i].state = "BUILDING"
                THEN [segs[i] EXCEPT !.state = "PENDING"]
                ELSE segs[i]]
  /\ UNCHANGED <<clock, wal, acked, aborts>>

Init ==
  /\ clock = 0
  /\ wal = <<>>
  /\ mem = EmptyMem
  /\ segs = <<>>
  /\ acked = EmptyMem
  /\ crashes = 0
  /\ aborts = 0

Next ==
  \/ \E k \in Keys : Upsert(k)
  \/ \E k \in Keys : Delete(k)
  \/ Flush
  \/ StartBuild
  \/ FinishBuild
  \/ AbortBuild
  \/ CompactPartial
  \/ CompactFull
  \/ CrashRecover

\* Fairness: flushes and builds eventually happen; writes, compactions,
\* crashes, and aborts are never forced.
Fairness ==
  /\ WF_vars(Flush)
  /\ WF_vars(StartBuild)
  /\ WF_vars(FinishBuild)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Safety properties                                                       *)
(***************************************************************************)

\* P2 (docs/indexing.md section 9): search observes exactly the acked
\* per-key state in every reachable state -- including mid-build,
\* mid-compaction and immediately after crash recovery.
SearchCompleteness ==
  \A k \in Keys : Obs(NewestRow(k)) = Obs(acked[k])

\* P3: an acked delete is never visible.
NoResurrection ==
  \A k \in Keys : acked[k].tomb => Obs(NewestRow(k)) = 0

\* P1: the WAL + segments alone (i.e. after losing the memtable) still
\* reconstruct the acked state -- this is what makes WAL truncation at
\* flush and crash recovery safe.
NewestDurableRow(k) ==
  LET cand == {ReplayedMem[k]} \cup {segs[i].rows[k] : i \in DOMAIN segs}
  IN CHOOSE r \in cand : \A r2 \in cand : r.ts >= r2.ts

WalTruncationSafe ==
  \A k \in Keys : Obs(NewestDurableRow(k)) = Obs(acked[k])

(***************************************************************************)
(* Liveness: every segment eventually gets its graph built (assuming       *)
(* finitely many crashes and aborts: MaxCrashes / MaxAborts).              *)
(***************************************************************************)
EventuallyAllIndexed ==
  <>[](\A i \in DOMAIN segs : segs[i].state = "READY")

===========================================================================
