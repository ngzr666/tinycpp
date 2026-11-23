#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdio>

namespace tc {

struct SourceFile {
  std::string path;
  std::string text;
};

struct SourceLoc {
  int file = -1;
  int line = 1;
  int col = 1;
};

class SourceManager {
public:
  int addFile(std::string path, std::string text) {
    files_.push_back({std::move(path), std::move(text)});
    return static_cast<int>(files_.size()) - 1;
  }
  const SourceFile& file(int id) const { return files_.at(static_cast<size_t>(id)); }
  std::string locString(SourceLoc loc) const {
    if (loc.file < 0 || loc.file >= static_cast<int>(files_.size()))
      return "<unknown>";
    return files_[static_cast<size_t>(loc.file)].path + ":" +
           std::to_string(loc.line) + ":" + std::to_string(loc.col);
  }
private:
  std::vector<SourceFile> files_;
};

struct CompileError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Diag {
public:
  void setSourceManager(SourceManager* sm) { sm_ = sm; }

  [[noreturn]] void error(SourceLoc loc, const std::string& msg) {
    if (sm_) std::fprintf(stderr, "%s: error: %s\n", sm_->locString(loc).c_str(), msg.c_str());
    else std::fprintf(stderr, "error: %s\n", msg.c_str());
    throw CompileError(msg);
  }

  void warning(SourceLoc loc, const std::string& msg) {
    if (sm_) std::fprintf(stderr, "%s: warning: %s\n", sm_->locString(loc).c_str(), msg.c_str());
    else std::fprintf(stderr, "warning: %s\n", msg.c_str());
  }

private:
  SourceManager* sm_ = nullptr;
};

} // namespace tc
