#include "aster/distributed/gossip.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace aster {
namespace {

constexpr double kLog10e = 0.4342944819032518;  // log10(e)

}  // namespace

int64_t SteadyGossipClock::NowMs() const {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

void ManualGossipClock::Advance(int64_t delta_ms) {
  if (delta_ms < 0) return;
  now_ms_ += delta_ms;
}

void ManualGossipClock::Set(int64_t now_ms) {
  if (now_ms < now_ms_) return;
  now_ms_ = now_ms;
}

PhiAccrualFailureDetector::PhiAccrualFailureDetector(PhiAccrualConfig config)
    : config_(std::move(config)) {
  if (config_.window_size == 0) config_.window_size = 1;
  if (config_.threshold <= 0.0) config_.threshold = 8.0;
  if (config_.first_heartbeat_estimate_ms <= 0) {
    config_.first_heartbeat_estimate_ms = 1000;
  }
  if (config_.min_std_deviation_ms < 0) config_.min_std_deviation_ms = 0;
  if (config_.acceptable_heartbeat_pause_ms < 0) {
    config_.acceptable_heartbeat_pause_ms = 0;
  }
}

void PhiAccrualFailureDetector::Report(const NodeId& endpoint,
                                       int64_t now_ms) {
  ArrivalWindow& w = windows_[endpoint];
  if (w.last_arrival_ms >= 0) {
    const int64_t interval = now_ms - w.last_arrival_ms;
    if (interval > 0) {
      w.intervals_ms.push_back(interval);
      while (w.intervals_ms.size() > config_.window_size) {
        w.intervals_ms.pop_front();
      }
    }
  }
  w.last_arrival_ms = now_ms;
}

double PhiAccrualFailureDetector::MeanIntervalMs(
    const ArrivalWindow& w) const {
  if (w.intervals_ms.empty()) {
    return static_cast<double>(config_.first_heartbeat_estimate_ms);
  }
  double sum = 0.0;
  for (int64_t v : w.intervals_ms) sum += static_cast<double>(v);
  const double mean = sum / static_cast<double>(w.intervals_ms.size());
  const double floor =
      static_cast<double>(std::max<int64_t>(1, config_.min_std_deviation_ms));
  return std::max(mean, floor);
}

double PhiAccrualFailureDetector::PhiWithMean(int64_t silence_ms,
                                              double mean_ms) const {
  if (silence_ms <= 0 || mean_ms <= 0.0) return 0.0;
  // Exponential inter-arrival: P(T > t) = e^(-t/µ); phi = -log10(P).
  return (static_cast<double>(silence_ms) / mean_ms) * kLog10e;
}

double PhiAccrualFailureDetector::Phi(const NodeId& endpoint,
                                      int64_t now_ms) const {
  const auto it = windows_.find(endpoint);
  if (it == windows_.end() || it->second.last_arrival_ms < 0) return 0.0;

  int64_t silence =
      now_ms - it->second.last_arrival_ms - config_.acceptable_heartbeat_pause_ms;
  if (silence < 0) silence = 0;
  return PhiWithMean(silence, MeanIntervalMs(it->second));
}

bool PhiAccrualFailureDetector::IsAvailable(const NodeId& endpoint,
                                            int64_t now_ms) const {
  const auto it = windows_.find(endpoint);
  if (it == windows_.end() || it->second.last_arrival_ms < 0) {
    // Never heard from them — membership layer decides; FD does not convict.
    return true;
  }
  return Phi(endpoint, now_ms) < config_.threshold;
}

void PhiAccrualFailureDetector::Remove(const NodeId& endpoint) {
  windows_.erase(endpoint);
}

void PhiAccrualFailureDetector::Clear() { windows_.clear(); }

Gossiper::Gossiper(NodeId local_id, GossipClock* clock,
                   PhiAccrualConfig fd_config)
    : local_id_(std::move(local_id)),
      clock_(clock),
      fd_(std::move(fd_config)) {}

void Gossiper::Start(int32_t generation) {
  EndpointState self;
  self.heartbeat.generation = generation;
  self.heartbeat.version = 1;
  self.status = MemberStatus::kAlive;
  endpoints_[local_id_] = self;
  started_ = true;
  fd_.Report(local_id_, clock_->NowMs());
}

void Gossiper::Leave() {
  if (!started_) Start();
  EndpointState& self = endpoints_[local_id_];
  self.status = MemberStatus::kLeft;
  ++self.heartbeat.version;
}

void Gossiper::Heartbeat() {
  if (!started_) Start();
  EndpointState& self = endpoints_[local_id_];
  if (self.status == MemberStatus::kLeft) return;
  self.status = MemberStatus::kAlive;
  ++self.heartbeat.version;
  fd_.Report(local_id_, clock_->NowMs());
}

GossipMessage Gossiper::MakeMessage() const {
  GossipMessage msg;
  msg.from = local_id_;
  msg.states = endpoints_;
  return msg;
}

bool Gossiper::IsNewer(const HeartbeatState& a, const HeartbeatState& b) {
  if (a.generation != b.generation) return a.generation > b.generation;
  return a.version > b.version;
}

void Gossiper::MaybeReportHeartbeat(const NodeId& id,
                                    const EndpointState& /*before*/,
                                    const EndpointState& after) {
  if (id == local_id_) return;
  if (after.status != MemberStatus::kAlive) return;
  fd_.Report(id, clock_->NowMs());
}

void Gossiper::MergeState(const NodeId& id, const EndpointState& remote,
                          bool /*from_live_peer*/) {
  auto it = endpoints_.find(id);
  if (it == endpoints_.end()) {
    endpoints_.emplace(id, remote);
    if (id != local_id_ && remote.status == MemberStatus::kAlive) {
      fd_.Report(id, clock_->NowMs());
    } else if (remote.status == MemberStatus::kLeft) {
      fd_.Remove(id);
    }
    return;
  }

  EndpointState& local = it->second;
  // Each node is authoritative for its own endpoint record.
  if (id == local_id_) return;

  if (!IsNewer(remote.heartbeat, local.heartbeat)) {
    // Same heartbeat: propagate explicit Leave; do not clear local Dead via a
    // stale Alive echo (conviction is local until a newer heartbeat arrives).
    if (remote.heartbeat.generation == local.heartbeat.generation &&
        remote.heartbeat.version == local.heartbeat.version &&
        remote.status == MemberStatus::kLeft &&
        local.status != MemberStatus::kLeft) {
      local.status = MemberStatus::kLeft;
      fd_.Remove(id);
    }
    return;
  }

  const EndpointState before = local;
  // Adopting a newer Alive heartbeat clears any local Dead conviction.
  local = remote;
  MaybeReportHeartbeat(id, before, local);
  if (local.status == MemberStatus::kLeft) {
    fd_.Remove(id);
  }
}

void Gossiper::Ingest(const GossipMessage& msg) {
  for (const auto& [id, state] : msg.states) {
    MergeState(id, state, /*from_live_peer=*/true);
  }
  // Seeing the sender's message is itself evidence of liveness when they
  // claim Alive.
  const auto it = msg.states.find(msg.from);
  if (it != msg.states.end() && it->second.status == MemberStatus::kAlive &&
      msg.from != local_id_) {
    fd_.Report(msg.from, clock_->NowMs());
  }
}

void Gossiper::GossipWith(Gossiper& peer) {
  const GossipMessage mine = MakeMessage();
  const GossipMessage theirs = peer.MakeMessage();
  Ingest(theirs);
  peer.Ingest(mine);
}

void Gossiper::Tick() {
  const int64_t now = clock_->NowMs();
  for (auto& [id, state] : endpoints_) {
    if (id == local_id_) continue;
    if (state.status != MemberStatus::kAlive) continue;
    if (!fd_.IsAvailable(id, now)) {
      // Local conviction only — do not bump heartbeat (peers still own that).
      state.status = MemberStatus::kDead;
    }
  }
}

bool Gossiper::Has(const NodeId& id) const {
  return endpoints_.count(id) > 0;
}

MemberStatus Gossiper::StatusOf(const NodeId& id) const {
  const auto it = endpoints_.find(id);
  if (it == endpoints_.end()) return MemberStatus::kDead;
  return it->second.status;
}

bool Gossiper::IsAlive(const NodeId& id) const {
  return StatusOf(id) == MemberStatus::kAlive;
}

bool Gossiper::IsDead(const NodeId& id) const {
  return Has(id) && StatusOf(id) == MemberStatus::kDead;
}

bool Gossiper::IsLeft(const NodeId& id) const {
  return Has(id) && StatusOf(id) == MemberStatus::kLeft;
}

std::vector<NodeId> Gossiper::Members() const {
  std::vector<NodeId> out;
  out.reserve(endpoints_.size());
  for (const auto& [id, _] : endpoints_) out.push_back(id);
  return out;
}

std::vector<NodeId> Gossiper::AliveMembers() const {
  std::vector<NodeId> out;
  for (const auto& [id, state] : endpoints_) {
    if (state.status == MemberStatus::kAlive) out.push_back(id);
  }
  return out;
}

}  // namespace aster
