#include "aster/distributed/replication.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace aster {
namespace {

constexpr char kHintMagic[] = "ASTER_HINTS_V1";

void PutU32(std::ostream& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.put(static_cast<char>(value >> (i * 8)));
}

void PutU64(std::ostream& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.put(static_cast<char>(value >> (i * 8)));
}

bool GetU32(std::istream& in, uint32_t* value) {
  *value = 0;
  for (int i = 0; i < 4; ++i) {
    const int c = in.get();
    if (c == EOF) return false;
    *value |= static_cast<uint32_t>(static_cast<unsigned char>(c)) << (i * 8);
  }
  return true;
}

bool GetU64(std::istream& in, uint64_t* value) {
  *value = 0;
  for (int i = 0; i < 8; ++i) {
    const int c = in.get();
    if (c == EOF) return false;
    *value |= static_cast<uint64_t>(static_cast<unsigned char>(c)) << (i * 8);
  }
  return true;
}

bool PutString(std::ostream& out, const std::string& value) {
  if (value.size() > UINT32_MAX) return false;
  PutU32(out, static_cast<uint32_t>(value.size()));
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
  return out.good();
}

bool GetString(std::istream& in, std::string* value) {
  uint32_t size = 0;
  if (!GetU32(in, &size)) return false;
  value->resize(size);
  in.read(value->data(), static_cast<std::streamsize>(size));
  return in.good();
}

bool WriteRow(std::ostream& out, const Row& row) {
  if (!PutString(out, row.id)) return false;
  PutU64(out, row.timestamp);
  PutU64(out, row.version);
  out.put(row.tombstone ? 1 : 0);
  if (row.vector.size() > UINT32_MAX) return false;
  PutU32(out, static_cast<uint32_t>(row.vector.size()));
  for (float value : row.vector) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(out, bits);
  }
  if (!PutString(out, row.metadata)) return false;
  if (row.tags.size() > UINT32_MAX) return false;
  PutU32(out, static_cast<uint32_t>(row.tags.size()));
  for (const std::string& tag : row.tags) {
    if (!PutString(out, tag)) return false;
  }
  return out.good();
}

bool ReadRow(std::istream& in, Row* row) {
  if (!GetString(in, &row->id) || !GetU64(in, &row->timestamp) ||
      !GetU64(in, &row->version)) {
    return false;
  }
  const int tombstone = in.get();
  if (tombstone == EOF) return false;
  row->tombstone = tombstone != 0;
  uint32_t dimensions = 0;
  if (!GetU32(in, &dimensions)) return false;
  row->vector.resize(dimensions);
  for (float& value : row->vector) {
    uint32_t bits = 0;
    if (!GetU32(in, &bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
  }
  if (!GetString(in, &row->metadata)) return false;
  uint32_t tags = 0;
  if (!GetU32(in, &tags)) return false;
  row->tags.clear();
  for (uint32_t i = 0; i < tags; ++i) {
    std::string tag;
    if (!GetString(in, &tag)) return false;
    row->tags.insert(std::move(tag));
  }
  return true;
}

Status SyncFile(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return Status::IoError("open hint journal failed: " +
                                     std::string(std::strerror(errno)));
  const int result = ::fsync(fd);
  const int saved_errno = errno;
  ::close(fd);
  if (result != 0) {
    return Status::IoError("fsync hint journal failed: " +
                           std::string(std::strerror(saved_errno)));
  }
  return Status::Ok();
}

}  // namespace

struct Coordinator::HintJournal {
  using HintsByKey = std::map<RowId, Row>;
  using HintsByNode = std::map<NodeId, HintsByKey>;

  explicit HintJournal(std::string directory) : directory_(std::move(directory)) {}

  Status Open() {
    if (directory_.empty()) return Status::Ok();
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) return Status::IoError("create hint directory failed: " +
                                      error.message());

    std::ifstream in(Path(), std::ios::binary);
    if (!in) return Status::Ok();
    std::string magic(sizeof(kHintMagic) - 1, '\0');
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != kHintMagic) {
      return Status::Corruption("invalid hint journal header");
    }

    uint32_t count = 0;
    if (!GetU32(in, &count)) return Status::Corruption("truncated hint journal");
    for (uint32_t i = 0; i < count; ++i) {
      NodeId node;
      Row row;
      if (!GetString(in, &node) || !ReadRow(in, &row)) {
        return Status::Corruption("truncated hint journal record");
      }
      Put(std::move(node), std::move(row));
    }
    return Status::Ok();
  }

  void Put(NodeId node, Row row) {
    auto& existing = hints_[std::move(node)][row.id];
    if (existing.id.empty() || NewerThan(row, existing)) existing = std::move(row);
  }

  const HintsByKey* For(const NodeId& node) const {
    const auto it = hints_.find(node);
    return it == hints_.end() ? nullptr : &it->second;
  }

  size_t Count(const NodeId& node) const {
    const HintsByKey* hints = For(node);
    return hints ? hints->size() : 0;
  }

  void Erase(const NodeId& node, const RowId& id) {
    const auto node_it = hints_.find(node);
    if (node_it == hints_.end()) return;
    node_it->second.erase(id);
    if (node_it->second.empty()) hints_.erase(node_it);
  }

  Status Save() const {
    if (directory_.empty()) return Status::Ok();
    const std::string temp = Path() + ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return Status::IoError("open hint journal temp failed");
    out.write(kHintMagic, sizeof(kHintMagic) - 1);
    size_t count = 0;
    for (const auto& [_, rows] : hints_) count += rows.size();
    if (count > UINT32_MAX) return Status::ResourceExhausted("too many hints");
    PutU32(out, static_cast<uint32_t>(count));
    for (const auto& [node, rows] : hints_) {
      for (const auto& [_, row] : rows) {
        if (!PutString(out, node) || !WriteRow(out, row)) {
          return Status::IoError("write hint journal failed");
        }
      }
    }
    out.close();
    if (!out) return Status::IoError("close hint journal failed");
    if (auto status = SyncFile(temp); !status.ok()) return status;
    if (::rename(temp.c_str(), Path().c_str()) != 0) {
      return Status::IoError("rename hint journal failed: " +
                             std::string(std::strerror(errno)));
    }
    return SyncFile(directory_);
  }

 private:
  std::string Path() const { return directory_ + "/HINTS"; }

  std::string directory_;
  HintsByNode hints_;
};

Coordinator::Coordinator(CoordinatorOptions options)
    : options_(std::move(options)),
      ring_(options_.vnodes_per_node),
      hints_(std::make_unique<HintJournal>(options_.hint_directory)) {}

Coordinator::~Coordinator() = default;

Result<std::unique_ptr<Coordinator>> Coordinator::Open(
    CoordinatorOptions options) {
  if (options.replication_factor == 0) {
    return Status::InvalidArgument("replication factor must be positive");
  }
  auto coordinator =
      std::unique_ptr<Coordinator>(new Coordinator(std::move(options)));
  if (auto status = coordinator->hints_->Open(); !status.ok()) return status;
  return coordinator;
}

Status Coordinator::AddReplica(NodeId id, Db* db) {
  if (id.empty() || db == nullptr) {
    return Status::InvalidArgument("replica id and db are required");
  }
  if (replicas_.count(id) != 0) {
    return Status::InvalidArgument("duplicate replica: " + id);
  }
  ring_.AddNode(id);
  reachable_.emplace(id, true);
  replicas_.emplace(std::move(id), db);
  return Status::Ok();
}

void Coordinator::SetReachable(const NodeId& id, bool reachable) {
  if (reachable_.count(id) != 0) reachable_[id] = reachable;
}

bool Coordinator::IsReachable(const NodeId& id) const {
  const auto it = reachable_.find(id);
  return it != reachable_.end() && it->second;
}

size_t Coordinator::RequiredAcks(ConsistencyLevel consistency,
                                 size_t replica_count) const {
  switch (consistency) {
    case ConsistencyLevel::kOne:
      return 1;
    case ConsistencyLevel::kQuorum:
      return replica_count / 2 + 1;
    case ConsistencyLevel::kAll:
      return replica_count;
  }
  return replica_count;
}

std::vector<NodeId> Coordinator::ReplicasFor(const RowId& id) const {
  return ring_.GetReplicas(id, options_.replication_factor);
}

Status Coordinator::PersistHint(const NodeId& target, const Row& row) {
  hints_->Put(target, row);
  return hints_->Save();
}

Status Coordinator::Write(const Row& row, ConsistencyLevel consistency) {
  if (row.id.empty()) return Status::InvalidArgument("row id is required");
  const std::vector<NodeId> targets = ReplicasFor(row.id);
  const size_t required = RequiredAcks(consistency, targets.size());
  if (targets.size() < options_.replication_factor) {
    return Status::Unavailable("not enough replicas for replication factor");
  }

  size_t acknowledgements = 0;
  Status last_error = Status::Unavailable("no replica acknowledgements");
  for (const NodeId& id : targets) {
    const auto replica = replicas_.find(id);
    if (replica == replicas_.end()) continue;
    if (!IsReachable(id)) {
      if (auto status = PersistHint(id, row); !status.ok()) return status;
      continue;
    }
    Status status = replica->second->Upsert(row);
    if (status.ok()) {
      ++acknowledgements;
      continue;
    }
    last_error = status;
    if (auto hint_status = PersistHint(id, row); !hint_status.ok()) {
      return hint_status;
    }
  }
  if (acknowledgements >= required) return Status::Ok();
  return Status::Unavailable("consistency level not satisfied: " +
                             last_error.message());
}

Status Coordinator::Delete(const RowId& id, Timestamp timestamp,
                           ConsistencyLevel consistency) {
  Row tombstone;
  tombstone.id = id;
  tombstone.timestamp = timestamp;
  tombstone.tombstone = true;
  return Write(tombstone, consistency);
}

Result<std::optional<Row>> Coordinator::Read(
    const RowId& id, ConsistencyLevel consistency) {
  const std::vector<NodeId> targets = ReplicasFor(id);
  const size_t required = RequiredAcks(consistency, targets.size());
  if (targets.size() < options_.replication_factor) {
    return Status::Unavailable("not enough replicas for replication factor");
  }

  std::vector<std::pair<Db*, Row>> replies;
  for (const NodeId& node : targets) {
    const auto replica = replicas_.find(node);
    if (replica != replicas_.end() && IsReachable(node)) {
      replies.emplace_back(replica->second, replica->second->GetRaw(id));
    }
  }
  if (replies.size() < required) {
    return Status::Unavailable("consistency level not satisfied for read");
  }

  Row newest = replies.front().second;
  for (const auto& [_, row] : replies) {
    if (NewerThan(row, newest)) newest = row;
  }
  if (newest.timestamp != 0) {
    for (const auto& [db, row] : replies) {
      if (NewerThan(newest, row)) {
        if (auto status = db->Upsert(newest); !status.ok()) return status;
      }
    }
  }
  if (newest.tombstone) return std::optional<Row>{};
  return std::optional<Row>{std::move(newest)};
}

Status Coordinator::DrainHints(const NodeId& id) {
  if (!IsReachable(id)) return Status::Unavailable("replica is unreachable");
  const auto replica = replicas_.find(id);
  if (replica == replicas_.end()) return Status::NotFound("unknown replica");

  const HintJournal::HintsByKey* pending = hints_->For(id);
  if (pending == nullptr) return Status::Ok();
  std::vector<Row> rows;
  rows.reserve(pending->size());
  for (const auto& [_, row] : *pending) rows.push_back(row);
  for (const Row& row : rows) {
    if (auto status = replica->second->Upsert(row); !status.ok()) return status;
    hints_->Erase(id, row.id);
    if (auto status = hints_->Save(); !status.ok()) return status;
  }
  return Status::Ok();
}

size_t Coordinator::pending_hints(const NodeId& id) const {
  return hints_->Count(id);
}

}  // namespace aster
