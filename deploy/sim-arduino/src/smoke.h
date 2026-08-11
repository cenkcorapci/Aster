#pragma once

// Shared Tiny-profile smoke for ESP32 firmware and native host build.
// Prints ASTER_OK / ASTER_FAIL:... on the given stream.

#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "aster/embedded/db.h"

namespace aster {
namespace sim {

inline Row MakeRow(const std::string& id, std::vector<float> vec, Timestamp ts,
                   std::set<std::string> tags = {}) {
  Row row;
  row.id = id;
  row.vector = std::move(vec);
  row.timestamp = ts;
  row.tags = std::move(tags);
  return row;
}

template <typename Printer>
bool RunSmoke(Printer&& print) {
  using embedded::Db;

  Db::Options opt;
  opt.dimension = 8;
  opt.metric = Metric::kL2;
  opt.memtable_flush_rows = 4;
  opt.max_segments_before_compact = 4;
  Db db(opt);

  auto fail = [&](const char* msg) {
    print("ASTER_FAIL:");
    print(msg);
    print("\n");
    return false;
  };

  if (db.Upsert(MakeRow("bad", {1.0f}, 1)).ok()) {
    return fail("dimension_guard");
  }

  for (int i = 0; i < 8; ++i) {
    std::vector<float> v(8, 0.0f);
    v[0] = static_cast<float>(i);
    std::set<std::string> tags;
    if (i % 2 == 0) tags.insert("even");
    if (!db.Upsert(MakeRow("r" + std::to_string(i), std::move(v),
                           static_cast<Timestamp>(i + 1), std::move(tags)))
             .ok()) {
      return fail("upsert");
    }
  }
  if (db.segment_count() < 1) return fail("auto_flush");

  if (!db.Flush().ok()) return fail("flush");

  SearchRequest req;
  req.vector.assign(8, 0.0f);
  req.top_k = 2;
  req.tags = {"even"};
  auto hits = db.Search(req);
  if (hits.empty() || hits[0].id != "r0") return fail("search");

  if (!db.Compact().ok()) return fail("compact");
  if (db.segment_count() != 1) return fail("compact_segments");
  if (!db.Get("r3").has_value()) return fail("get_after_compact");

  print("ASTER_OK\n");
  return true;
}

}  // namespace sim
}  // namespace aster
