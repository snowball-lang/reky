
#ifndef __REKY_H__
#define __REKY_H__

#include <vector>
#include <string>
#include <filesystem>
#include <unordered_map>

// Snowball classes
#include "compiler/utils/utils.h"
#include "compiler/utils/logger.h"

namespace snowball {
namespace reky {

class Context final {
  std::string git_cmd;
  std::vector<std::filesystem::path> projects; // List of paths to search deps
};

// Instance of a required package,
// not the actual package installed
// e.g. version might not exist
class RequiredPackage final {
  std::string name;
  std::string version;
  std::string download_url;
};

class RekyManager final {
  std::vector<RequiredPackage> required_packages;
};

}
}

#endif // __REKY_H__
