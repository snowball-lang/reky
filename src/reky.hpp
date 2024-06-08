
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
#include "compiler/utils/hash.h"
#include "compiler/utils/utils.h"
#include "compiler/utils/logger.h"
#include "compiler/backend/drivers.h"

#ifndef REKY_PACKAGE_INDEX 
#define REKY_PACKAGE_INDEX "https://github.com/snowball-lang/packages.git"
#endif

#ifndef REKY_CACHE_FILE
#define REKY_CACHE_FILE ".reky_cache"
#endif

#ifndef REKY_DEFAULT_FILE
#define REKY_DEFAULT_FILE "sn.reky"
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
  bool first_run = true;
  bool index_fetched = false;
};

struct ReckyCache final {
  std::unordered_map<std::string, std::string> cache;
  bool has_changed = false;

  void save(std::ostream& file) {
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
      file << " ==  " << value << std::endl;
    }
  }

  void save_cache(const std::filesystem::path& root) {
    std::ofstream file(root / REKY_CACHE_FILE);
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

[[noreturn]] void error(const std::string& message, unsigned int line, std::string file) {
  auto efile = std::make_shared<frontend::SourceFile>(file);
  auto err = E(message, frontend::SourceLocation(line, 1, 1, efile));
  err.print();
}

[[noreturn]] void error(const std::string& message) {
  auto ef = std::make_shared<frontend::SourceFile>();
  auto err = E(message, frontend::SourceLocation(0,0,0, ef));
  err.print();
  exit(1);
}

std::unordered_map<std::string, std::string> parse_config(const std::filesystem::path& path, bool for_cache = false) {
  std::unordered_map<std::string, std::string> config;
  auto reky_config = path / (!for_cache ? REKY_DEFAULT_FILE : REKY_CACHE_FILE);
  if (!std::filesystem::exists(reky_config)) {
    return config;
  }
  std::ifstream file(reky_config);
  std::string line;
  unsigned int line_number = 1;
  bool has_error = false;
  // Classic requirements.txt like format
  while (std::getline(file, line)) {
    utils::strip(line);
    if (utils::sw(line, "#") || line.empty()) {
      continue;
    }
    auto pos = line.find("==");
    if (pos == std::string::npos) {
      error("Invalid package format. Must be 'name==version'", line_number, reky_config.string());
      has_error = true;
      continue;
    }
    auto name = line.substr(0, pos);
    auto version = line.substr(pos + 2);
    config[name] = version;
    if (version.empty()) {
      error("Invalid version format. Must be 'name==version'", line_number, reky_config.string());
      has_error = true;
      continue;
    } else if (name.empty()) {
      error("Invalid name format. Must be 'name==version'", line_number, reky_config.string());
      has_error = true;
      continue;
    }
    line_number++;
  }
  if (has_error) {
    exit(1);
  }
  return config;
}

struct DepsGraph final {
  std::map<std::string, std::vector<std::string>> graph;
};

class RekyManager final {
  RekyContext ctx;
  ReckyCache cache;
  DepsGraph graph;
  const Ctx& compiler_ctx;
public:
  RekyManager(const Ctx& compiler_ctx) : compiler_ctx(compiler_ctx) {
    ctx.git_cmd = driver::get_git(compiler_ctx);
  }

  ReckyCache& fetch_dependencies(std::vector<std::filesystem::path>& allowed_paths) {
    if (ctx.first_run) {
      cache = fetch_cache(allowed_paths);
      ctx.first_run = false;
    }
    for (auto& path : allowed_paths) {
      auto path_filename = path.filename().string();
      if (path_filename.empty()) {
        // The main crate path is empty
        path_filename = path.parent_path().filename().string();
      }
      path_filename = get_name_from_hash(path_filename);
      auto config = parse_config(path);
      graph.graph[path_filename] = std::vector<std::string>();
      for (auto& [name, version] : config) {
        graph.graph[path_filename].push_back(name);
        if (!cache.has_package(name)) {
          auto path = driver::get_workspace_path(compiler_ctx, driver::WorkSpaceType::Deps);
          path = std::filesystem::absolute(path);
          allowed_paths.push_back(path / get_dep_folder(name));
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
      installed_if_needed();
      cache.reset_changed();
      return fetch_dependencies(allowed_paths);
    }
    return cache;
  }

  std::string get_name_from_hash(const std::string& hash) {
    auto path = driver::get_workspace_path(compiler_ctx, driver::WorkSpaceType::Deps);
    path /= hash;
    std::ifstream ifs(path.string() + ".name");
    if (ifs.is_open()) {
      std::string content((std::istreambuf_iterator<char>(ifs)),
                          (std::istreambuf_iterator<char>()));
      return content;
    }
    return hash;
  }

  void installed_if_needed() {
    get_package_index();
    for (auto& [name, version] : cache.cache) {
      if (!is_installed(name, version)) {
        install(name, version);
      }
    }
  }

  void get_package_index() {
    if (ctx.index_fetched) {
      return;
    }
    auto index_path = driver::get_snowball_home() / "packages";
    ctx.index_fetched = true;
    if (!std::filesystem::exists(index_path)) {
      utils::Logger::status("Fetching", "Reky package index");
      run_git({"clone", REKY_PACKAGE_INDEX, index_path.string()});
    } else {
      update_package_index(index_path);
    }
  }

  void update_package_index(const std::filesystem::path& index_path) {
    utils::Logger::status("Updating", "Reky package index");
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
  
  static std::string get_dep_folder(const std::string& name) {
    return utils::hash::hashString(name);
  }

  bool is_installed(const std::string& name, const std::string& version) {
    auto deps_path = driver::get_workspace_path(compiler_ctx, driver::WorkSpaceType::Deps);
    return std::filesystem::exists(deps_path / get_dep_folder(name));
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
    auto package_path = deps_path / get_dep_folder(name);
    std::ofstream f(package_path.string() + ".name");
    f << name;
    f.close();
    utils::Logger::status("Download", fmt::format("{}@{}", name, install_version));
    run_git({"clone", "-c", "advice.detachedHead=false", package_data.value()["download_url"], package_path.string(), "--branch", install_version, "--depth", "1"});
  }

  ReckyCache fetch_cache(std::vector<std::filesystem::path>& allowed_paths) {
    auto reky_path = driver::get_workspace_path(compiler_ctx, driver::WorkSpaceType::Reky);
    auto deps_path = driver::get_workspace_path(compiler_ctx, driver::WorkSpaceType::Deps);
    auto config = parse_config(reky_path, true);
    ReckyCache cache;
    for (auto& [name, version] : config) {
      auto dep_path = std::filesystem::absolute(deps_path / get_dep_folder(name));
      allowed_paths.push_back(dep_path);
      cache.add_package(name, version);
    }
    cache.reset_changed();
    return cache;
  }

  void export_dot(std::ofstream& file) {
    file << "digraph G {" << std::endl;
    file << "  label = \"Reky Dependencies\";" << std::endl;
    for (auto& [name, deps] : graph.graph) {
      for (auto& dep : deps) {
        file << fmt::format("  \"{}\" -> \"{}\" [arrowhead = diamond];", name, dep) << std::endl;
      }
    }
    file << "}" << std::endl;
    file.close();
  }
};

RekyManager* fetch_dependencies(const Ctx& ctx, std::vector<std::filesystem::path>& allowed_paths) {
  auto manager = new RekyManager(ctx);
  auto cache = manager->fetch_dependencies(allowed_paths);
  cache.save_cache(driver::get_workspace_path(ctx, driver::WorkSpaceType::Reky));
  return manager;
}

}
}

#endif // __REKY_H__
