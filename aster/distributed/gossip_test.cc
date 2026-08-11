#include <gtest/gtest.h>

#include <string>

#include "aster/distributed/gossip.h"

namespace aster {
namespace {

PhiAccrualConfig TestFdConfig() {
  PhiAccrualConfig cfg;
  // With ~100ms heartbeats, phi ≈ (silence/mean)*0.434. Threshold 5 ⇒ convict
  // after roughly silence ≥ 5/0.434 * mean ≈ 11.5 intervals (~1.2s).
  cfg.threshold = 5.0;
  cfg.window_size = 20;
  cfg.min_std_deviation_ms = 50;
  cfg.acceptable_heartbeat_pause_ms = 0;
  cfg.first_heartbeat_estimate_ms = 100;
  return cfg;
}

TEST(PhiAccrual, PhiGrowsWithSilence) {
  ManualGossipClock clock;
  PhiAccrualFailureDetector fd(TestFdConfig());

  for (int i = 0; i < 10; ++i) {
    clock.Advance(100);
    fd.Report("n1", clock.NowMs());
  }
  const double phi_fresh = fd.Phi("n1", clock.NowMs());
  EXPECT_LT(phi_fresh, 1.0);

  clock.Advance(500);
  const double phi_mid = fd.Phi("n1", clock.NowMs());
  EXPECT_GT(phi_mid, phi_fresh);

  clock.Advance(2000);
  const double phi_late = fd.Phi("n1", clock.NowMs());
  EXPECT_GT(phi_late, phi_mid);
  EXPECT_GE(phi_late, fd.config().threshold);
  EXPECT_FALSE(fd.IsAvailable("n1", clock.NowMs()));
}

TEST(PhiAccrual, NeverSeenStaysAvailable) {
  ManualGossipClock clock;
  PhiAccrualFailureDetector fd(TestFdConfig());
  clock.Advance(10'000);
  EXPECT_TRUE(fd.IsAvailable("ghost", clock.NowMs()));
  EXPECT_DOUBLE_EQ(fd.Phi("ghost", clock.NowMs()), 0.0);
}

TEST(Gossip, JoinPropagatesMembership) {
  ManualGossipClock clock;
  Gossiper a("a", &clock, TestFdConfig());
  Gossiper b("b", &clock, TestFdConfig());
  a.Start();
  b.Start();

  EXPECT_TRUE(a.IsAlive("a"));
  EXPECT_FALSE(a.Has("b"));

  a.GossipWith(b);

  EXPECT_TRUE(a.IsAlive("b"));
  EXPECT_TRUE(b.IsAlive("a"));
  EXPECT_EQ(a.AliveMembers().size(), 2u);
  EXPECT_EQ(b.AliveMembers().size(), 2u);
}

TEST(Gossip, LeavePropagates) {
  ManualGossipClock clock;
  Gossiper a("a", &clock, TestFdConfig());
  Gossiper b("b", &clock, TestFdConfig());
  Gossiper c("c", &clock, TestFdConfig());
  a.Start();
  b.Start();
  c.Start();

  a.GossipWith(b);
  b.GossipWith(c);
  a.GossipWith(c);

  b.Leave();
  a.GossipWith(b);  // a learns Left
  a.GossipWith(c);  // c learns via a

  EXPECT_TRUE(a.IsLeft("b"));
  EXPECT_TRUE(c.IsLeft("b"));
  EXPECT_FALSE(a.IsAlive("b"));
  EXPECT_TRUE(a.IsAlive("a"));
  EXPECT_TRUE(a.IsAlive("c"));
}

TEST(Gossip, DeadMarkedWithinDetectorWindow) {
  ManualGossipClock clock;
  const PhiAccrualConfig cfg = TestFdConfig();
  Gossiper a("a", &clock, cfg);
  Gossiper b("b", &clock, cfg);
  a.Start();
  b.Start();
  a.GossipWith(b);

  // Establish a stable inter-arrival history (~100ms).
  for (int i = 0; i < 15; ++i) {
    clock.Advance(100);
    a.Heartbeat();
    b.Heartbeat();
    a.GossipWith(b);
    a.Tick();
    b.Tick();
    EXPECT_TRUE(a.IsAlive("b")) << "i=" << i;
  }

  // b goes silent: a keeps heartbeating / ticking but no longer gossips with b.
  const int64_t silence_start = clock.NowMs();
  bool marked_dead = false;
  int64_t marked_at = 0;
  for (int i = 0; i < 50; ++i) {
    clock.Advance(100);
    a.Heartbeat();
    a.Tick();
    if (a.IsDead("b")) {
      marked_dead = true;
      marked_at = clock.NowMs();
      break;
    }
  }

  ASSERT_TRUE(marked_dead);
  const int64_t silence_ms = marked_at - silence_start;
  // Detector window: with mean≈100ms and threshold 5, expect conviction on
  // the order of ~1–2s, well under a generous upper bound.
  EXPECT_GE(silence_ms, 500);
  EXPECT_LE(silence_ms, 5'000);
  EXPECT_FALSE(a.IsAlive("b"));
}

TEST(Gossip, DeadPeerResurrectsOnNewerHeartbeat) {
  ManualGossipClock clock;
  Gossiper a("a", &clock, TestFdConfig());
  Gossiper b("b", &clock, TestFdConfig());
  a.Start();
  b.Start();

  for (int i = 0; i < 12; ++i) {
    clock.Advance(100);
    a.Heartbeat();
    b.Heartbeat();
    a.GossipWith(b);
  }

  for (int i = 0; i < 50 && !a.IsDead("b"); ++i) {
    clock.Advance(100);
    a.Heartbeat();
    a.Tick();
  }
  ASSERT_TRUE(a.IsDead("b"));

  clock.Advance(100);
  b.Heartbeat();
  a.GossipWith(b);
  a.Tick();
  EXPECT_TRUE(a.IsAlive("b"));
}

TEST(Gossip, ThreeNodeConvergence) {
  ManualGossipClock clock;
  Gossiper a("a", &clock, TestFdConfig());
  Gossiper b("b", &clock, TestFdConfig());
  Gossiper c("c", &clock, TestFdConfig());
  a.Start();
  b.Start();
  c.Start();

  // Line topology: a↔b, then b↔c, then another a↔b so a learns c.
  a.GossipWith(b);
  b.GossipWith(c);
  a.GossipWith(b);

  EXPECT_TRUE(a.Has("c"));
  EXPECT_TRUE(a.IsAlive("c"));
  EXPECT_TRUE(c.Has("a"));
}

TEST(ManualGossipClock, IgnoresBackwardTime) {
  ManualGossipClock clock(100);
  clock.Advance(-50);
  EXPECT_EQ(clock.NowMs(), 100);
  clock.Set(50);
  EXPECT_EQ(clock.NowMs(), 100);
  clock.Set(200);
  EXPECT_EQ(clock.NowMs(), 200);
}

}  // namespace
}  // namespace aster
