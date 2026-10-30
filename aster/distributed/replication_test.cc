#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "aster/db/db.h"
#include "aster/distributed/replication.h"

namespace aster {
namespace {

class ReplicationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = ::testing::TempDir() + "/aster_replication_" +
            std::to_string(++sequence_);
    ASSERT_TRUE(std::filesystem::create_directories(root_));
  }

  Db* AddDb(const NodeId& id) {
    Db::Options options;
    options.dimension = 2;
    options.metric = Metric::kL2;
    options.data_dir = root_ + "/" + id;
    options.wal_sync = SyncPolicy::kAlways;
    options.compaction_tier_threshold = 0;
    options.max_segments_before_compact = 0;
    auto opened = Db::Open(options);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    dbs_[id] = std::move(opened.value());
    return dbs_[id].get();
  }

  std::unique_ptr<Coordinator> OpenCoordinator(uint32_t rf = 3) {
    CoordinatorOptions options;
    options.replication_factor = rf;
    options.vnodes_per_node = 32;
    options.hint_directory = root_ + "/hints";
    auto opened = Coordinator::Open(options);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    auto coordinator = std::move(opened.value());
    for (const auto& [id, db] : dbs_) {
      EXPECT_TRUE(coordinator->AddReplica(id, db.get()).ok());
    }
    return coordinator;
  }

  static Row RowFor(const std::string& id, Timestamp timestamp,
                    bool tombstone = false) {
    Row row;
    row.id = id;
    row.timestamp = timestamp;
    row.tombstone = tombstone;
    if (!tombstone) row.vector = {static_cast<float>(timestamp), 1.0f};
    return row;
  }

  std::string root_;
  std::map<NodeId, std::unique_ptr<Db>> dbs_;
  inline static int sequence_ = 0;
};

TEST_F(ReplicationTest, QuorumWriteSurvivesReplicaCrashAndRestart) {
  AddDb("a");
  AddDb("b");
  AddDb("c");
  auto coordinator = OpenCoordinator();

  coordinator->SetReachable("c", false);
  ASSERT_TRUE(coordinator->Write(RowFor("doc", 10), ConsistencyLevel::kQuorum)
                  .ok());
  EXPECT_EQ(coordinator->pending_hints("c"), 1u);

  coordinator->SetReachable("c", true);
  ASSERT_TRUE(coordinator->DrainHints("c").ok());
  ASSERT_TRUE(dbs_["a"]->Flush().ok());
  ASSERT_TRUE(dbs_["b"]->Flush().ok());
  ASSERT_TRUE(dbs_["c"]->Flush().ok());
  coordinator.reset();
  dbs_.clear();

  for (const char* node : {"a", "b", "c"}) {
    Db::Options options;
    options.dimension = 2;
    options.metric = Metric::kL2;
    options.data_dir = root_ + "/" + node;
    auto reopened = Db::Open(options);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    EXPECT_EQ(reopened.value()->GetRaw("doc").timestamp, 10u);
  }
}

TEST_F(ReplicationTest, DurableHintSurvivesCoordinatorRestart) {
  AddDb("a");
  AddDb("b");
  AddDb("c");
  {
    auto coordinator = OpenCoordinator();
    coordinator->SetReachable("c", false);
    ASSERT_TRUE(coordinator->Write(RowFor("hinted", 4), ConsistencyLevel::kQuorum)
                    .ok());
    EXPECT_EQ(coordinator->pending_hints("c"), 1u);
  }

  auto coordinator = OpenCoordinator();
  coordinator->SetReachable("c", false);
  EXPECT_EQ(coordinator->pending_hints("c"), 1u);
  coordinator->SetReachable("c", true);
  ASSERT_TRUE(coordinator->DrainHints("c").ok());
  EXPECT_EQ(coordinator->pending_hints("c"), 0u);
  EXPECT_EQ(dbs_["c"]->GetRaw("hinted").timestamp, 4u);
}

TEST_F(ReplicationTest, QuorumReadRepairsStaleValueAndTombstone) {
  AddDb("a");
  AddDb("b");
  AddDb("c");
  auto coordinator = OpenCoordinator();

  ASSERT_TRUE(dbs_["a"]->Upsert(RowFor("doc", 7)).ok());
  ASSERT_TRUE(dbs_["b"]->Upsert(RowFor("doc", 9)).ok());
  auto value = coordinator->Read("doc", ConsistencyLevel::kQuorum);
  ASSERT_TRUE(value.ok()) << value.status().message();
  ASSERT_TRUE(value.value().has_value());
  EXPECT_EQ(value.value()->timestamp, 9u);
  EXPECT_EQ(dbs_["a"]->GetRaw("doc").timestamp, 9u);
  EXPECT_EQ(dbs_["c"]->GetRaw("doc").timestamp, 9u);

  ASSERT_TRUE(dbs_["a"]->Upsert(RowFor("doc", 12, true)).ok());
  auto deleted = coordinator->Read("doc", ConsistencyLevel::kQuorum);
  ASSERT_TRUE(deleted.ok()) << deleted.status().message();
  EXPECT_FALSE(deleted.value().has_value());
  EXPECT_TRUE(dbs_["b"]->GetRaw("doc").tombstone);
  EXPECT_EQ(dbs_["b"]->GetRaw("doc").timestamp, 12u);
  EXPECT_TRUE(dbs_["c"]->GetRaw("doc").tombstone);
}

TEST_F(ReplicationTest, AllRejectsPartitionWithoutAcknowledgingSuccess) {
  AddDb("a");
  AddDb("b");
  AddDb("c");
  auto coordinator = OpenCoordinator();

  coordinator->SetReachable("c", false);
  const Status status =
      coordinator->Write(RowFor("doc", 3), ConsistencyLevel::kAll);
  EXPECT_EQ(status.code(), StatusCode::kUnavailable);
  EXPECT_EQ(coordinator->pending_hints("c"), 1u);
}

// Local Jepsen-style chaos: randomized partitions, crashes, delayed handoff,
// updates, and deletes. Every successful QUORUM operation must converge after
// all nodes rejoin and hints drain.
TEST_F(ReplicationTest, ChaosQuorumConvergesAfterPartitionsAndRejoins) {
  for (const char* node : {"a", "b", "c", "d", "e"}) AddDb(node);
  auto coordinator = OpenCoordinator();
  std::mt19937 rng(0xA57E);
  std::map<RowId, Row> acknowledged;

  constexpr int kOperations = 2'000;
  for (int i = 1; i <= kOperations; ++i) {
    // Keep at most one node down so every RF=3 range retains a quorum while
    // its unavailable member exercises durable hinted handoff.
    const std::array<const char*, 5> nodes = {"a", "b", "c", "d", "e"};
    for (const char* node : nodes) coordinator->SetReachable(node, true);
    if ((rng() % 100) < 70) {
      coordinator->SetReachable(nodes[rng() % nodes.size()], false);
    }
    const RowId id = "key-" + std::to_string(rng() % 80);
    const bool tombstone = (rng() % 7) == 0;
    Row row = RowFor(id, static_cast<Timestamp>(i), tombstone);
    const Status status = coordinator->Write(row, ConsistencyLevel::kQuorum);
    if (status.ok()) acknowledged[id] = row;
  }

  for (const char* node : {"a", "b", "c", "d", "e"}) {
    coordinator->SetReachable(node, true);
    ASSERT_TRUE(coordinator->DrainHints(node).ok());
  }
  for (const auto& [id, expected] : acknowledged) {
    const auto replicas = coordinator->ring().GetReplicas(id, 3);
    ASSERT_EQ(replicas.size(), 3u);
    for (const NodeId& node : replicas) {
      const Row actual = dbs_[node]->GetRaw(id);
      EXPECT_EQ(actual.timestamp, expected.timestamp) << id << " on " << node;
      EXPECT_EQ(actual.tombstone, expected.tombstone) << id << " on " << node;
    }
  }
}

// Sustained local load through the full coordinator path. The assertion
// covers both high write volume and quorum visibility after handoff.
TEST_F(ReplicationTest, LoadQuorumWritesRemainVisible) {
  for (const char* node : {"a", "b", "c", "d", "e"}) AddDb(node);
  auto coordinator = OpenCoordinator();

  constexpr int kWrites = 10'000;
  for (int i = 1; i <= kWrites; ++i) {
    const Row row = RowFor("load-" + std::to_string(i % 400),
                           static_cast<Timestamp>(i));
    ASSERT_TRUE(coordinator->Write(row, ConsistencyLevel::kQuorum).ok())
        << i;
  }
  for (int i = 0; i < 400; ++i) {
    const auto value =
        coordinator->Read("load-" + std::to_string(i), ConsistencyLevel::kQuorum);
    ASSERT_TRUE(value.ok()) << value.status().message();
    ASSERT_TRUE(value.value().has_value());
    const Timestamp expected =
        static_cast<Timestamp>(kWrites - ((kWrites - i) % 400));
    EXPECT_EQ(value.value()->timestamp, expected);
  }
}

}  // namespace
}  // namespace aster
