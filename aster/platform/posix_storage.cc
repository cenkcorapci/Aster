#include "aster/platform/posix_storage.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

namespace aster {
namespace {

Status MkdirParents(const std::string& file_path) {
  const auto slash = file_path.find_last_of('/');
  if (slash == std::string::npos) return Status::Ok();
  std::string dir = file_path.substr(0, slash);
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if (dir[i] == '/' || i + 1 == dir.size()) {
      if (cur.empty() || cur == "/") continue;
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
        return Status::IoError("mkdir failed: " + cur);
      }
    }
  }
  return Status::Ok();
}

}  // namespace

PosixStorage::PosixStorage(std::string root) : root_(std::move(root)) {
  if (!root_.empty() && root_.back() == '/') root_.pop_back();
}

std::string PosixStorage::Resolve(const std::string& path) const {
  if (path.empty()) return root_;
  if (path[0] == '/') return root_ + path;
  return root_ + "/" + path;
}

Status PosixStorage::Put(const std::string& path, const std::string& data) {
  const std::string full = Resolve(path);
  if (auto st = MkdirParents(full); !st.ok()) return st;
  const std::string tmp = full + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return Status::IoError("open failed: " + tmp);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) return Status::IoError("write failed: " + tmp);
  }
  if (std::rename(tmp.c_str(), full.c_str()) != 0) {
    return Status::IoError("rename failed: " + full);
  }
  return Status::Ok();
}

Result<std::string> PosixStorage::Read(const std::string& path) {
  const std::string full = Resolve(path);
  std::ifstream in(full, std::ios::binary);
  if (!in) return Status::NotFound(path);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

Status PosixStorage::Remove(const std::string& path) {
  const std::string full = Resolve(path);
  if (::unlink(full.c_str()) != 0) {
    if (errno == ENOENT) return Status::NotFound(path);
    return Status::IoError("unlink failed: " + full);
  }
  return Status::Ok();
}

Result<std::vector<std::string>> PosixStorage::List(const std::string& prefix) {
  // List files under root whose relative path starts with prefix.
  // Shallow: only direct children of root (enough for M3-T03 tests).
  std::vector<std::string> out;
  DIR* dir = ::opendir(root_.c_str());
  if (!dir) return Status::IoError("opendir failed: " + root_);
  while (dirent* ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    if (name.compare(0, prefix.size(), prefix) == 0) out.push_back(name);
  }
  ::closedir(dir);
  return out;
}

bool PosixStorage::Exists(const std::string& path) {
  const std::string full = Resolve(path);
  struct stat st {};
  return ::stat(full.c_str(), &st) == 0;
}

}  // namespace aster
