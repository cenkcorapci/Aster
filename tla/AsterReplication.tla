------------------------- MODULE AsterReplication -------------------------
(***************************************************************************)
(* Distributed indexing/search protocol of Aster (docs/indexing.md,        *)
(* section 8): coordinator writes with tunable consistency levels,         *)
(* last-write-wins reconciliation, asynchronous replication, message loss, *)
(* anti-entropy repair, and quorum reads/searches.                         *)
(*                                                                         *)
(* Abstraction choices:                                                    *)
(*   - One token range, replicated on every node in `Nodes` (RF = N).      *)
(*     Ring placement/coverage is a per-range property tested in code      *)
(*     (aster/distributed/ring_test.cc); the protocol below is what runs   *)
(*     identically inside each range's replica set.                        *)
(*   - Each replica's LSM/segment machinery is abstracted to its LWW row   *)
(*     store: AsterLsmIndex.tla proves the store observed by search equals *)
(*     the rows applied on that node, so composing the two specs is sound. *)
(*     "Search on node n sees k" therefore means store[n][k] is live.      *)
(*   - Graphs are never replicated -- only rows travel between nodes --    *)
(*     which is why replica divergence in segment layout cannot appear in  *)
(*     this model at all: converged row stores imply converged search.     *)
(*   - M2-T04 SegState (PENDING/BUILDING/READY) stays inside AsterLsmIndex *)
(*     per node. This module needs no new actions for READY wiring: a      *)
(*     replica's search observation is still Obs(store[n][k]), justified   *)
(*     by SearchCompleteness under every SegState.                         *)
(*                                                                         *)
(* Checked properties (see AsterReplication.cfg):                          *)
(*   QuorumReadConsistent   : W+R > N ==> every quorum read/search of a    *)
(*                            key returns exactly its latest acked state.  *)
(*   NoResurrectionQuorum   : a quorum-acked delete is invisible to every  *)
(*                            quorum search.                               *)
(*   AckedWriteSomewhere    : an acked write (any CL) survives on at least *)
(*                            one replica despite message loss.            *)
(*   EventualConsistency    : all replicas converge (liveness, assuming    *)
(*                            fair delivery + anti-entropy repair).        *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
  Nodes,      \* replica set of the token range, e.g. {"n1","n2","n3"}
  Keys,       \* e.g. {"k1"}
  MaxWrites,  \* bound on client writes
  WriteCLs    \* consistency levels exercised, subset of {"ONE","QUORUM","ALL"}

N == Cardinality(Nodes)
Quorum == (N \div 2) + 1

AckCount(cl) == CASE cl = "ONE"    -> 1
                  [] cl = "QUORUM" -> Quorum
                  [] cl = "ALL"    -> N

QuorumSets == {S \in SUBSET Nodes : Cardinality(S) >= Quorum}

NoRow == [ts |-> 0, tomb |-> FALSE]

VARIABLES
  clock,     \* coordinator hybrid-timestamp source (unique, monotone)
  store,     \* store[n][k] : the LWW row each replica's engine exposes
  inflight,  \* undelivered replication messages [node, key, ts, tomb]
  acked      \* latest acknowledged write per key: [ts, tomb, cl]

vars == <<clock, store, inflight, acked>>

Rows == [ts : 0..MaxWrites, tomb : BOOLEAN]

TypeOK ==
  /\ clock \in 0..MaxWrites
  /\ store \in [Nodes -> [Keys -> Rows]]
  /\ inflight \subseteq [node : Nodes, key : Keys,
                         ts : 1..MaxWrites, tomb : BOOLEAN]
  /\ acked \in [Keys -> [ts : 0..MaxWrites, tomb : BOOLEAN,
                         cl : WriteCLs \cup {"ONE"}]]

Obs(r) == IF r.ts = 0 \/ r.tomb THEN 0 ELSE r.ts

\* LWW apply: a replica accepts a version only if it is newer.
Apply(rowmap, k, r) ==
  [rowmap EXCEPT ![k] = IF r.ts > rowmap[k].ts THEN r ELSE rowmap[k]]

(***************************************************************************)
(* Actions                                                                 *)
(***************************************************************************)

\* Coordinator write (upsert or delete) at consistency level cl:
\* the row reaches `ackSet` replicas synchronously (those acks gate the
\* client response); the remaining replicas get an in-flight message that
\* may be delayed or lost (hinted handoff / repair recovers it).
Write(k, tomb, cl) ==
  /\ clock < MaxWrites
  /\ clock' = clock + 1
  /\ LET r == [ts |-> clock + 1, tomb |-> tomb] IN
     \E ackSet \in SUBSET Nodes :
       /\ Cardinality(ackSet) = AckCount(cl)
       /\ store' = [n \in Nodes |->
                      IF n \in ackSet THEN Apply(store[n], k, r)
                      ELSE store[n]]
       /\ inflight' = inflight \cup
            {[node |-> n, key |-> k, ts |-> r.ts, tomb |-> tomb] :
             n \in Nodes \ ackSet}
       /\ acked' = [acked EXCEPT ![k] =
                      [ts |-> r.ts, tomb |-> tomb, cl |-> cl]]

\* Asynchronous replication delivery (possibly reordered; LWW makes
\* reordering harmless).
Deliver ==
  \E m \in inflight :
    /\ store' = [store EXCEPT ![m.node] =
                   Apply(store[m.node], m.key,
                         [ts |-> m.ts, tomb |-> m.tomb])]
    /\ inflight' = inflight \ {m}
    /\ UNCHANGED <<clock, acked>>

\* Message loss: replica down longer than the hint window, network drop...
Lose ==
  \E m \in inflight :
    /\ inflight' = inflight \ {m}
    /\ UNCHANGED <<clock, store, acked>>

\* Anti-entropy repair round for one key (Merkle-tree repair in the
\* implementation): all replicas adopt the LWW-newest version among them.
NewestAmong(S, k) ==
  CHOOSE r \in {store[n][k] : n \in S} :
    \A n \in S : r.ts >= store[n][k].ts

Repair ==
  \E k \in Keys :
    /\ \E n1, n2 \in Nodes : store[n1][k] # store[n2][k]  \* only if divergent
    /\ LET r == NewestAmong(Nodes, k) IN
         store' = [n \in Nodes |-> Apply(store[n], k, r)]
    /\ UNCHANGED <<clock, inflight, acked>>

Init ==
  /\ clock = 0
  /\ store = [n \in Nodes |-> [k \in Keys |-> NoRow]]
  /\ inflight = {}
  /\ acked = [k \in Keys |-> [ts |-> 0, tomb |-> FALSE, cl |-> "ONE"]]

Next ==
  \/ \E k \in Keys, tomb \in BOOLEAN, cl \in WriteCLs : Write(k, tomb, cl)
  \/ Deliver
  \/ Lose
  \/ Repair

\* Fairness: replication delivery and repair keep happening; writes and
\* message loss are never forced.
Fairness == WF_vars(Deliver) /\ WF_vars(Repair)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Safety properties                                                       *)
(***************************************************************************)

\* A quorum read (point lookup) or quorum search merges the replies of any
\* >= Quorum replicas by LWW. This is its per-key result:
QuorumMerged(S, k) == NewestAmong(S, k)

\* P5 (docs/indexing.md 8.4): if the latest write to k was acked at QUORUM
\* or ALL, then EVERY quorum read/search observes exactly that write --
\* the live row for an upsert, absence for a delete. (W + R > N.)
QuorumReadConsistent ==
  \A k \in Keys :
    (acked[k].ts > 0 /\ acked[k].cl # "ONE") =>
      \A S \in QuorumSets :
        Obs(QuorumMerged(S, k)) = Obs(acked[k])

\* P3 distributed: a quorum-acked delete never surfaces in quorum searches.
NoResurrectionQuorum ==
  \A k \in Keys :
    (acked[k].tomb /\ acked[k].cl # "ONE") =>
      \A S \in QuorumSets : Obs(QuorumMerged(S, k)) = 0

\* P1 distributed: any acked write (even CL=ONE) is durable on at least one
\* replica, no matter which in-flight messages are lost.
AckedWriteSomewhere ==
  \A k \in Keys :
    acked[k].ts > 0 =>
      \E n \in Nodes : store[n][k].ts = acked[k].ts

(***************************************************************************)
(* Liveness: replicas converge -- and since AsterLsmIndex.tla shows        *)
(* per-node search equals the per-node store, converged stores mean every  *)
(* replica answers searches identically (P4).                              *)
(***************************************************************************)
EventualConsistency ==
  <>[](\A n1, n2 \in Nodes, k \in Keys : store[n1][k] = store[n2][k])

===========================================================================
