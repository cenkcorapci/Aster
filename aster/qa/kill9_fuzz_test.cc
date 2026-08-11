// M1-T14: kill -9 / crash-recovery fuzz harness.
//
// Random Upsert / Flush / interrupt cycles. Interrupts are either:
//   - in-process close-without-flush (destroy Db while WAL has acked rows)
//   - fork + SIGKILL of a child writer (true kill -9)
//
// After each interrupt, Open() must recover every acked Upsert
// (SyncPolicy::kAlways ⇒ Upsert OK means WAL durable).
//
// Duration:
//   ASTER_FUZZ_SECONDS   default 15 (CI); set 3600 for the 1h exit criterion
//   ASTER_FUZZ_ROUNDS    optional hard cap on crash rounds (overrides time if set)

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aster/db/db.h"

namespace aster {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kDim = 4;
constexpr int kDefaultFuzzSeconds = 15;

int EnvInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return fallback;
  char* end = nullptr;
  const long n = std::strtol(v, &end, 10);
  if (end == v || n < 0) return fallback;
  return static_cast<int>(n);
}

Row MakeRow(const std::string& id, Timestamp ts) {
  Row row;
  row.id = id;
  row.vector.assign(kDim, 0.0f);
  row.vector[0] = static_cast<float>(ts % 1000) * 0.001f;
  row.timestamp = ts;
  return row;
}

Db::Options FuzzOptions(const std::string& dir) {
  Db::Options opt;
  opt.dimension = kDim;
  opt.metric = Metric::kL2;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kAlways;  // Upsert OK ⇒ durable across kill -9
  opt.memtable_flush_bytes = 4 << 10;  // flush often under load
  opt.memtable_flush_ms = 0;           // avoid timer races with SIGKILL child
  opt.compaction_tier_threshold = 3;
  opt.compaction_bucket_ratio = 4;
  opt.max_segments_before_compact = 6;
  return opt;
}

std::string MakeDir(const std::string& suffix) {
  static std::atomic<uint64_t> seq{0};
  const std::string dir =
      std::string(::testing::TempDir()) + "/aster_kill9_" + suffix + "_" +
      std::to_string(::getpid()) + "_" + std::to_string(seq.fetch_add(1));
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return {};
  }
  return dir;
}

// Durable ack log: after each successful Upsert, append id + fsync.
// Parent only trusts IDs present here (under-approximation is safe).
class AckLog {
 public:
  explicit AckLog(std::string path) : path_(std::move(path)) {}

  bool OpenWrite() {
    fd_ = ::open(path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    return fd_ >= 0;
  }

  bool OpenRead() {
    fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    return fd_ >= 0;
  }

  ~AckLog() {
    if (fd_ >= 0) ::close(fd_);
  }

  bool Append(const std::string& id) {
    if (fd_ < 0) return false;
    std::string line = id + "\n";
    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
      const ssize_t n = ::write(fd_, p, left);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      p += n;
      left -= static_cast<size_t>(n);
    }
    return ::fsync(fd_) == 0;
  }

  std::set<std::string> ReadAll() {
    std::set<std::string> ids;
    if (fd_ < 0) return ids;
    std::string buf;
    char tmp[4096];
    for (;;) {
      const ssize_t n = ::read(fd_, tmp, sizeof(tmp));
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) break;
      buf.append(tmp, static_cast<size_t>(n));
    }
    size_t start = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
      if (buf[i] == '\n') {
        if (i > start) ids.insert(buf.substr(start, i - start));
        start = i + 1;
      }
    }
    return ids;
  }

 private:
  std::string path_;
  int fd_ = -1;
};

void AssertRecovered(const std::string& dir,
                     const std::set<std::string>& acked) {
  auto opened = Db::Open(FuzzOptions(dir));
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  auto& db = *opened.value();
  for (const auto& id : acked) {
    ASSERT_TRUE(db.Get(id).has_value())
        << "acked write missing after recovery: " << id
        << " (acked=" << acked.size() << " segs=" << db.segment_count()
        << " mem=" << db.memtable_rows() << ")";
  }
}

// In-process: Upsert/Flush randomly, then destroy without Flush.
bool RunCloseWithoutFlushRound(std::mt19937& rng, int* ops_out) {
  const std::string dir = MakeDir("close");
  if (dir.empty()) return false;
  const std::string ack_path = dir + "/ACKED";
  AckLog ack(ack_path);
  if (!ack.OpenWrite()) return false;

  auto opened = Db::Open(FuzzOptions(dir));
  if (!opened.ok()) return false;
  auto db = std::move(opened.value());

  std::uniform_int_distribution<int> op_dist(0, 99);
  std::uniform_int_distribution<int> burst_dist(8, 64);
  const int burst = burst_dist(rng);
  int ops = 0;
  for (int i = 0; i < burst; ++i) {
    const std::string id = "c" + std::to_string(i);
    if (!db->Upsert(MakeRow(id, static_cast<Timestamp>(i + 1))).ok()) {
      return false;
    }
    if (!ack.Append(id)) return false;
    ++ops;
    if (op_dist(rng) < 15) {
      if (!db->Flush().ok()) return false;
    }
  }

  // Crash simulation: drop Db without Flush (WAL still has unflushed acks).
  db.reset();

  AckLog reader(ack_path);
  if (!reader.OpenRead()) return false;
  const auto acked = reader.ReadAll();
  AssertRecovered(dir, acked);
  if (ops_out) *ops_out = ops;
  return !::testing::Test::HasFailure();
}

// Child writes until killed; parent SIGKILLs after a short random window.
bool RunSigkillRound(std::mt19937& rng, int* ops_out) {
  const std::string dir = MakeDir("kill");
  if (dir.empty()) return false;
  const std::string ack_path = dir + "/ACKED";

  const pid_t pid = ::fork();
  if (pid < 0) return false;

  if (pid == 0) {
    AckLog ack(ack_path);
    if (!ack.OpenWrite()) _exit(11);
    auto opened = Db::Open(FuzzOptions(dir));
    if (!opened.ok()) _exit(12);
    auto& db = *opened.value();
    std::mt19937 child_rng(rng());
    std::uniform_int_distribution<int> op_dist(0, 99);
    for (uint64_t i = 0;; ++i) {
      const std::string id = "k" + std::to_string(i);
      if (!db.Upsert(MakeRow(id, i + 1)).ok()) _exit(13);
      if (!ack.Append(id)) _exit(14);
      if (op_dist(child_rng) < 10) {
        if (!db.Flush().ok()) _exit(15);
      }
      if ((i & 15u) == 0u) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
  }

  std::uniform_int_distribution<int> wait_us(500, 25000);
  std::this_thread::sleep_for(std::chrono::microseconds(wait_us(rng)));
  (void)::kill(pid, SIGKILL);
  int status = 0;
  const pid_t w = ::waitpid(pid, &status, 0);
  if (w != pid) return false;
  EXPECT_TRUE(WIFSIGNALED(status) || WIFEXITED(status));
  if (WIFSIGNALED(status)) {
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
  }

  AckLog reader(ack_path);
  if (!reader.OpenRead()) {
    if (ops_out) *ops_out = 0;
    return true;
  }
  const auto acked = reader.ReadAll();
  if (ops_out) *ops_out = static_cast<int>(acked.size());
  if (!acked.empty()) {
    AssertRecovered(dir, acked);
  }
  return !::testing::Test::HasFailure();
}

TEST(Kill9Fuzz, CrashRecovery) {
  const int seconds = EnvInt("ASTER_FUZZ_SECONDS", kDefaultFuzzSeconds);
  const int max_rounds = EnvInt("ASTER_FUZZ_ROUNDS", 0);
  ASSERT_GT(seconds, 0);

  std::mt19937 rng(static_cast<uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count() ^
      static_cast<uint64_t>(::getpid())));
  std::uniform_int_distribution<int> mode_dist(0, 1);

  const auto deadline = Clock::now() + std::chrono::seconds(seconds);
  int rounds = 0;
  int close_rounds = 0;
  int kill_rounds = 0;
  int64_t total_ops = 0;

  while (Clock::now() < deadline) {
    if (max_rounds > 0 && rounds >= max_rounds) break;
    int ops = 0;
    const bool use_kill = mode_dist(rng) == 1;
    bool ok = false;
    if (use_kill) {
      ok = RunSigkillRound(rng, &ops);
      if (ok) ++kill_rounds;
    } else {
      ok = RunCloseWithoutFlushRound(rng, &ops);
      if (ok) ++close_rounds;
    }
    ASSERT_TRUE(ok) << "fuzz round failed mode="
                    << (use_kill ? "sigkill" : "close")
                    << " round=" << rounds;
    total_ops += ops;
    ++rounds;
  }

  ASSERT_GE(rounds, 1);
  std::printf(
      "kill9_fuzz seconds=%d rounds=%d close=%d sigkill=%d ops=%lld\n",
      seconds, rounds, close_rounds, kill_rounds,
      static_cast<long long>(total_ops));
}

}  // namespace
}  // namespace aster
