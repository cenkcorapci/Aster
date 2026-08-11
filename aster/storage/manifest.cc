#include "aster/storage/manifest.h"

#include <fstream>
#include <sstream>

namespace aster {
namespace {

// Simple line-oriented format (not JSON) to avoid dependencies:
//   ASTMANIFEST1
//   generation=<u64>
//   segment=<id>\t<path>[\t<hnsw_path>]
//   ...
constexpr const char* kMagic = "ASTMANIFEST1";

}  // namespace

Status WriteManifest(const std::string& path, const Manifest& manifest) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return Status::IoError("open failed: " + tmp);
    out << kMagic << "\n";
    out << "generation=" << manifest.generation << "\n";
    for (const auto& e : manifest.segments) {
      out << "segment=" << e.segment_id << "\t" << e.path;
      if (!e.hnsw_path.empty()) {
        out << "\t" << e.hnsw_path;
      }
      out << "\n";
    }
    if (!out) return Status::IoError("write failed: " + tmp);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return Status::IoError("rename failed: " + path);
  }
  return Status::Ok();
}

Result<Manifest> ReadManifest(const std::string& path) {
  std::ifstream in(path);
  if (!in) return Status::IoError("open failed: " + path);
  std::string line;
  if (!std::getline(in, line) || line != kMagic) {
    return Status::Corruption("bad manifest magic");
  }
  Manifest m;
  while (std::getline(in, line)) {
    if (line.rfind("generation=", 0) == 0) {
      m.generation = std::stoull(line.substr(11));
    } else if (line.rfind("segment=", 0) == 0) {
      const auto tab = line.find('\t');
      if (tab == std::string::npos) {
        return Status::Corruption("bad manifest segment line");
      }
      ManifestEntry e;
      e.segment_id = std::stoull(line.substr(8, tab - 8));
      const auto tab2 = line.find('\t', tab + 1);
      if (tab2 == std::string::npos) {
        e.path = line.substr(tab + 1);
      } else {
        e.path = line.substr(tab + 1, tab2 - tab - 1);
        e.hnsw_path = line.substr(tab2 + 1);
      }
      m.segments.push_back(std::move(e));
    }
  }
  return m;
}

}  // namespace aster
