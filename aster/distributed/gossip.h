#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "aster/distributed/ring.h"

namespace aster {

// Deterministic time source so unit tests can advance "now" without sleeping.
class GossipClock {
 public:
  virtual ~GossipClock() = default;
  virtual int64_t NowMs() const = 0;
};

// Wall-clock backed by steady_clock milliseconds since an arbitrary epoch.
class SteadyGossipClock final : public GossipClock {
 public:
  int64_t NowMs() const override;
};

// Test / simulation clock. Starts at 0; Advance/Set control time.
class ManualGossipClock final : public GossipClock {
 public:
  explicit ManualGossipClock(int64_t start_ms = 0) : now_ms_(start_ms) {}

  int64_t NowMs() const override { return now_ms_; }
  void Advance(int64_t delta_ms);
  void Set(int64_t now_ms);

 private:
  int64_t now_ms_;
};

// Phi-accrual failure detector (Hayashibara et al. / Cassandra-style).
// Tracks heartbeat arrival intervals per endpoint and reports suspicion as
// phi = -log10(P(heartbeat still pending)). Convict when phi >= threshold.
struct PhiAccrualConfig {
  double threshold = 8.0;
  size_t window_size = 100;
  // Floor on stddev / mean scale so a single regular interval cannot make
  // phi explode after one missed beat.
  int64_t min_std_deviation_ms = 100;
  // Grace period subtracted from silence before phi accrues (local GC pause).
  int64_t acceptable_heartbeat_pause_ms = 0;
  // Mean used before the first inter-arrival sample exists.
  int64_t first_heartbeat_estimate_ms = 1000;
};

class PhiAccrualFailureDetector {
 public:
  explicit PhiAccrualFailureDetector(PhiAccrualConfig config = {});

  // Record a heartbeat arrival from `endpoint` at `now_ms`.
  void Report(const NodeId& endpoint, int64_t now_ms);

  // Suspicion level; 0 if never reported.
  double Phi(const NodeId& endpoint, int64_t now_ms) const;

  // True while phi < threshold (or endpoint never seen).
  bool IsAvailable(const NodeId& endpoint, int64_t now_ms) const;

  void Remove(const NodeId& endpoint);
  void Clear();

  const PhiAccrualConfig& config() const { return config_; }

 private:
  struct ArrivalWindow {
    std::deque<int64_t> intervals_ms;
    int64_t last_arrival_ms = -1;
  };

  double MeanIntervalMs(const ArrivalWindow& w) const;
  double PhiWithMean(int64_t silence_ms, double mean_ms) const;

  PhiAccrualConfig config_;
  std::map<NodeId, ArrivalWindow> windows_;
};

enum class MemberStatus : uint8_t {
  kAlive = 0,
  kDead = 1,
  kLeft = 2,
};

// Cassandra-style heartbeat: generation bumps on restart; version on each beat.
struct HeartbeatState {
  int32_t generation = 0;
  int32_t version = 0;
};

struct EndpointState {
  HeartbeatState heartbeat;
  MemberStatus status = MemberStatus::kAlive;
};

// Full membership map carried in a gossip round (digest optimization is
// deferred; small clusters exchange state directly for M7-T01).
struct GossipMessage {
  NodeId from;
  std::map<NodeId, EndpointState> states;
};

// Peer-to-peer membership gossiper with an embedded phi-accrual detector.
// Network I/O is out of scope: callers drive Heartbeat / GossipWith / Tick
// (tests wire nodes in-process; a later transport will call the same APIs).
class Gossiper {
 public:
  Gossiper(NodeId local_id, GossipClock* clock,
           PhiAccrualConfig fd_config = {});

  const NodeId& local_id() const { return local_id_; }

  // Publish this node as Alive at the current generation/version.
  void Start(int32_t generation = 1);

  // Explicit leave: mark local status Left and bump heartbeat so peers learn.
  void Leave();

  // Increment local heartbeat version (call on the gossip period).
  void Heartbeat();

  // Exchange state with a peer and update failure-detector samples.
  void GossipWith(Gossiper& peer);

  // Apply a received message (same merge rules as GossipWith).
  void Ingest(const GossipMessage& msg);

  // Snapshot for outbound gossip.
  GossipMessage MakeMessage() const;

  // Recompute liveness from the detector; marks Alive→Dead when phi crosses
  // the configured threshold ("detector window").
  void Tick();

  bool Has(const NodeId& id) const;
  MemberStatus StatusOf(const NodeId& id) const;
  bool IsAlive(const NodeId& id) const;
  bool IsDead(const NodeId& id) const;
  bool IsLeft(const NodeId& id) const;

  std::vector<NodeId> Members() const;
  std::vector<NodeId> AliveMembers() const;

  const std::map<NodeId, EndpointState>& endpoint_states() const {
    return endpoints_;
  }

  PhiAccrualFailureDetector& failure_detector() { return fd_; }
  const PhiAccrualFailureDetector& failure_detector() const { return fd_; }

 private:
  static bool IsNewer(const HeartbeatState& a, const HeartbeatState& b);
  void MergeState(const NodeId& id, const EndpointState& remote,
                  bool from_live_peer);
  void MaybeReportHeartbeat(const NodeId& id, const EndpointState& before,
                            const EndpointState& after);

  NodeId local_id_;
  GossipClock* clock_;  // not owned
  PhiAccrualFailureDetector fd_;
  std::map<NodeId, EndpointState> endpoints_;
  bool started_ = false;
};

}  // namespace aster
