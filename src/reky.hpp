
#ifndef __REKY_H__
#define __REKY_H__

#include <vector>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <fstream>

// Snowball classes
#include "compiler/ctx.h"
#include "compiler/utils/utils.h"
#include "compiler/utils/logger.h"

namespace snowball {
namespace reky {

// Instance of a required package,
// not the actual package installed
// e.g. version might not exist
struct RequiredPackage final {
  std::string name;
  std::string version;
  std::string download_url;
};

struct RekyContext final {
  std::string git_cmd;
  std::vector<std::filesystem::path> projects; // List of paths to search deps
  std::vector<RequiredPackage> required_packages;
};

std::unordered_map<std::string, std::string> parse_config(const std::filesystem::path& path);

class RekyManager final {
  RekyContext ctx;
  const Ctx& compiler_ctx;
public:
  RekyManager(const Ctx& compiler_ctx) : compiler_ctx(compiler_ctx) {
    ctx.git_cmd = driver::get_git(compiler_ctx);
  }

  void fetch_dependencies(const std::vector<std::filesystem::path>& allowed_paths) {
    for (auto& path : allowed_paths) {
      auto config = parse_config(path);
    }
  }
};

void fetch_dependencies(const Ctx& ctx, const std::vector<std::filesystem::path>& allowed_paths) {
  RekyManager manager(ctx);
  manager.fetch_dependencies(allowed_paths);
}

void error(const std::string& message, unsigned int line, std::string file) {
  auto efile = std::make_shared<frontend::SourceFile>(file);
  auto err = E(message, frontend::SourceLocation(line, 1, 1, efile));
  err.print();
  exit(1);
}

std::unordered_map<std::string, std::string> parse_config(const std::filesystem::path& path) {
  std::unordered_map<std::string, std::string> config;
  auto reky_config = path / "deps.reky";
  if (!std::filesystem::exists(reky_config)) {
    return config;
  }
  std::ifstream file(reky_config);
  std::string line;
  // Classic requirements.txt like format
  while (std::getline(file, line)) {
    line = utils::strip(line);
    if (utils::sw(line, "#") || line.empty()) {
      continue;
    }
    auto pos = line.find(":");
    if (pos == std::string::npos) {
      error("Invalid package format. Must be 'name:version'", 1, reky_config.string());
    }
    auto name = line.substr(0, pos);
    auto version = line.substr(pos + 1);
    config[name] = version;
    if (version.empty()) {
      error("Invalid version format. Must be 'name:version'", 1, reky_config.string());
    } else if (name.empty()) {
      error("Invalid name format. Must be 'name:version'", 1, reky_config.string());
    }
  }
  return config;
}

}
}

#endif // __REKY_H__
