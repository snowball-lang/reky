
#ifndef __REKY_H__
#define __REKY_H__

#include <vector>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <fstream>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

// Snowball classes
#include "compiler/ctx.h"
#include "compiler/utils/utils.h"
#include "compiler/utils/logger.h"

#ifndef REQUI_PACKAGE_INDEX 
#define REQUI_PACKAGE_INDEX "https://github.com/snowball-lang/packages.git"
#endif

using json = nlohmann::json;

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

struct ReckyCache final {
  std::unordered_map<std::string, std::string> cache;
  bool has_changed = false;

  void save(std::ofstream& file) {
    // Get the largest key size
    size_t max_key_size = 0;
    for (auto& [key, value] : cache) {
      max_key_size = std::max(max_key_size, key.size());
    }
    // Write the cache to the file
    for (auto& [key, value] : cache) {
      file << key;
      for (size_t i = 0; i < max_key_size - key.size(); i++) {
        file << " ";
      }
      file << " :  " << value << std::endl;
    }
  }

  void save_cache(const std::filesystem::path& root) {
    std::ofstream file(root / ".reky_cache");
    save(file);
  }

  bool has_package(const std::string& name) {
    return cache.find(name) != cache.end();
  }

  void add_package(const std::string& name, const std::string& version) {
    cache[name] = version;
    has_changed = true;
  }

  void reset_changed() {
    has_changed = false;
  }
};

void error(const std::string& message, unsigned int line, std::string file) {
  auto efile = std::make_shared<frontend::SourceFile>(file);
  auto err = E(message, frontend::SourceLocation(line, 1, 1, efile));
  err.print();
  exit(1);
}

void error(const std::string& message) {
  auto ef = std::make_shared<frontend::SourceFile>();
  auto err = E(message, frontend::SourceLocation(0,0,0, ef));
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
    utils::strip(line);
    if (utils::sw(line, "#") || line.empty()) {
      continue;
    }
    auto pos = line.find("==");
    if (pos == std::string::npos) {
      error("Invalid package format. Must be 'name==version'", 1, reky_config.string());
    }
    auto name = line.substr(0, pos);
    auto version = line.substr(pos + 2);
    config[name] = version;
    if (version.empty()) {
      error("Invalid version format. Must be 'name==version'", 1, reky_config.string());
    } else if (name.empty()) {
      error("Invalid name format. Must be 'name==version'", 1, reky_config.string());
    }
  }
  return config;
}

class RekyManager final {
  RekyContext ctx;
  ReckyCache cache;
  const Ctx& compiler_ctx;
public:
  RekyManager(const Ctx& compiler_ctx) : compiler_ctx(compiler_ctx) {
    ctx.git_cmd = driver::get_git(compiler_ctx);
  }

  ReckyCache& fetch_dependencies(std::vector<std::filesystem::path>& allowed_paths) {
    for (auto& path : allowed_paths) {
      auto config = parse_config(path);
      for (auto& [name, version] : config) {
        if (!cache.has_package(name)) {
          auto path = driver::get_workspace_path(compiler_ctx, driver::WorkSpaceType::Deps);
          allowed_paths.push_back(path / name);
          cache.add_package(name, version);
        } else {
          auto installed_version = cache.cache[name];
          if (installed_version != version) {
            error(fmt::format("Package '{}' has conflicting versions '{}' and '{}'", name, installed_version, version));
          }
        }
      }
    }
    if (cache.has_changed) {
      installed_if_needed(allowed_paths);
      cache.reset_changed();
      return fetch_dependencies(allowed_paths);
    }
    return cache;
  }

  void installed_if_needed(const std::vector<std::filesystem::path>& allowed_paths) {
    get_package_index();
    for (auto& [name, version] : cache.cache) {
      if (!is_installed(name, version)) {
        install(name, version);
      }
    }
  }

  void get_package_index() {
    auto index_path = driver::get_snowball_home() / "packages";
    if (!std::filesystem::exists(index_path)) {
      utils::Logger::status("Fetching", "Reky package index from " REQUI_PACKAGE_INDEX);
      run_git({"clone", REQUI_PACKAGE_INDEX, index_path.string()});
    } else {
      update_package_index(index_path);
    }
  }

  void update_package_index(const std::filesystem::path& index_path) {
    utils::Logger::status("Updating", "Reky package index from " REQUI_PACKAGE_INDEX);
    run_git({"-C", index_path.string(), "pull"});
  }

  int run_git(const std::vector<std::string>& args) {
    std::string cmd = ctx.git_cmd;
    for (auto& arg : args) {
      cmd += " " + arg;
    }
    // Silently run the command
    cmd += " -q";
    return std::system(cmd.c_str());
  }

  bool is_installed(const std::string& name, const std::string& version) {
    auto deps_path = driver::get_workspace_path(compiler_ctx, driver::WorkSpaceType::Deps);
    return std::filesystem::exists(deps_path / name);
  }

  std::optional<json> get_package_data(const std::string& name, const std::string& version) {
    auto index_path = driver::get_snowball_home() / "packages" / "pkgs";
    auto package_path = (index_path / name).string() + ".json";
    if (!std::filesystem::exists(package_path)) {
      return std::nullopt;
    }
    std::ifstream f(package_path);
    return json::parse(f);
  }

  void install(const std::string& name, const std::string& version) {
    auto package_data = get_package_data(name, version);
    if (!package_data.has_value()) {
      error(fmt::format("Package '{}' not found in the package index", name));
    }
    std::string install_version;
    for (auto& v : package_data.value()["versions"]) {
      if (v == version) {
        install_version = v;
        break;
      }
    }
    if (install_version.empty()) {
      error(fmt::format("Version '{}' not found for package '{}'", version, name));
    }
    auto deps_path = driver::get_workspace_path(compiler_ctx, driver::WorkSpaceType::Deps);
    auto package_path = deps_path / name;
    utils::Logger::status("Download", fmt::format("{}@{}", name, install_version));
    run_git({"clone", package_data.value()["download_url"], package_path.string(), "--branch", install_version, "--depth", "1"});
  }
};

void fetch_dependencies(const Ctx& ctx, std::vector<std::filesystem::path>& allowed_paths) {
  RekyManager manager(ctx);
  auto cache = manager.fetch_dependencies(allowed_paths);
  cache.save_cache(driver::get_workspace_path(ctx, driver::WorkSpaceType::Deps));
}

}
}

#endif // __REKY_H__
