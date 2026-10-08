#include "api/memory/Hook.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

class OreUIConfig {
public:
  void *mUnknown1;
  void *mUnknown2;
  std::function<bool()> mUnknown3;
  std::function<bool()> mUnknown4;
};

class OreUi {
public:
  std::unordered_map<std::string, OreUIConfig> mConfigs;
};

// clang-format off
#define OREUI_PATTERN                                                                                                    \
     std::initializer_list<const char *>({                                                                               \
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 44 88 8D ? ? ? ? 44 88 85 ? ? ? ? 48 89 D7", \
  })
// clang-format on

namespace {

using Json = nlohmann::json;

constexpr char kConfigFileName[] = "config.json";
constexpr char kModDirName[] = "ForceCloseOreUI";

std::string getMinecraftModsPath() {
  const char *appDataPath = std::getenv("APPDATA");
  if (!appDataPath || !*appDataPath) {
    printf("Failed to get APPDATA path.\n");
    return "";
  }

  return (fs::path(appDataPath) / "Minecraft Bedrock" / "mods").string();
}

std::string getUWPModsDir() {
  std::string appDataPath = getMinecraftModsPath();
  std::string uwpMods = appDataPath + "\\ForceCloseOreUI\\";
  return uwpMods;
}

void logConfigInfo(const std::string &message) {
  std::printf("[ForceCloseOreUI] %s\n", message.c_str());
}

void logConfigError(const std::string &message) {
  std::printf("[ForceCloseOreUI] %s\n", message.c_str());
}

std::string normalizedPathText(const fs::path &path) {
  return path.lexically_normal().generic_string();
}

void appendUniquePath(std::vector<fs::path> &paths, fs::path path) {
  if (path.empty())
    return;

  const std::string normalized = normalizedPathText(path);
  auto duplicated =
      std::any_of(paths.begin(), paths.end(), [&](const fs::path &item) {
        return normalizedPathText(item) == normalized;
      });
  if (!duplicated)
    paths.emplace_back(std::move(path));
}

bool pathExists(const fs::path &path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

bool isDirectoryWritable(const fs::path &dir) {
  if (dir.empty())
    return false;

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec)
    return false;

  const fs::path test_file = dir / "._perm_test";
  std::ofstream ofs(test_file, std::ios::binary | std::ios::trunc);
  if (!ofs.is_open())
    return false;

  ofs.close();
  fs::remove(test_file, ec);
  return true;
}

std::vector<fs::path> getConfigDirCandidates() {
  std::vector<fs::path> paths;
  appendUniquePath(paths, getUWPModsDir());
  appendUniquePath(paths, fs::path("mods") / kModDirName);
  return paths;
}

fs::path selectWritableConfigDir(const std::vector<fs::path> &candidates) {
  for (const auto &dir : candidates) {
    if (isDirectoryWritable(dir))
      return dir;
  }

  if (!candidates.empty())
    return candidates.front();
  return fs::path("mods") / kModDirName;
}

struct ConfigFiles {
  fs::path source;
  fs::path target;
};

ConfigFiles resolveConfigFiles() {
  std::vector<fs::path> candidates = getConfigDirCandidates();
  fs::path target_dir = selectWritableConfigDir(candidates);
  fs::path target = target_dir / kConfigFileName;

  if (pathExists(target))
    return {target, target};

  for (const auto &dir : candidates) {
    fs::path candidate = dir / kConfigFileName;
    if (pathExists(candidate))
      return {candidate, target};
  }

  return {{}, target};
}

std::string trimAscii(std::string value) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), is_space));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(),
              value.end());
  return value;
}

std::optional<bool> parseBoolString(std::string value) {
  value = trimAscii(std::move(value));
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (value == "true" || value == "1" || value == "yes" || value == "on" ||
      value == "enabled")
    return true;
  if (value == "false" || value == "0" || value == "no" || value == "off" ||
      value == "disabled")
    return false;
  return std::nullopt;
}

std::optional<bool> readBoolLike(const Json &value) {
  try {
    if (value.is_boolean())
      return value.get<bool>();
    if (value.is_number_integer())
      return value.get<long long>() != 0;
    if (value.is_number_unsigned())
      return value.get<unsigned long long>() != 0;
    if (value.is_number_float())
      return value.get<double>() != 0.0;
    if (value.is_string())
      return parseBoolString(value.get<std::string>());
    if (value.is_object()) {
      for (std::string_view key :
           {"value", "enabled", "enable", "state", "default"}) {
        auto it = value.find(std::string(key));
        if (it == value.end())
          continue;
        if (auto parsed = readBoolLike(*it))
          return parsed;
      }

      auto disabled = value.find("disabled");
      if (disabled != value.end()) {
        if (auto parsed = readBoolLike(*disabled))
          return !*parsed;
      }
    }
  } catch (...) {
  }

  return std::nullopt;
}

std::optional<bool> readNamedConfigValue(const Json &root,
                                         const std::string &name) {
  if (root.is_object()) {
    auto direct = root.find(name);
    if (direct != root.end()) {
      if (auto parsed = readBoolLike(*direct))
        return parsed;
    }

    for (std::string_view key :
         {"configs", "settings", "toggles", "values", "oreui", "OreUI"}) {
      auto section = root.find(std::string(key));
      if (section == root.end() || !section->is_object())
        continue;

      auto nested = section->find(name);
      if (nested != section->end()) {
        if (auto parsed = readBoolLike(*nested))
          return parsed;
      }
    }
  }

  if (root.is_array()) {
    for (const Json &item : root) {
      if (!item.is_object())
        continue;

      bool name_matched = false;
      for (std::string_view key : {"name", "key", "id"}) {
        auto name_node = item.find(std::string(key));
        if (name_node != item.end() && name_node->is_string() &&
            name_node->get<std::string>() == name) {
          name_matched = true;
          break;
        }
      }

      if (name_matched) {
        if (auto parsed = readBoolLike(item))
          return parsed;
      }
    }
  }

  return std::nullopt;
}

struct ConfigDocument {
  Json raw = Json::object();
  Json canonical = Json::object();
  fs::path source;
  fs::path target;
  bool dirty = false;
};

ConfigDocument loadConfigDocument() {
  ConfigFiles files = resolveConfigFiles();
  ConfigDocument document;
  document.source = files.source;
  document.target = files.target;
  document.dirty =
      document.source.empty() || normalizedPathText(document.source) !=
                                     normalizedPathText(document.target);

  if (document.source.empty())
    return document;

  try {
    std::ifstream input(document.source, std::ios::binary);
    if (!input.is_open()) {
      document.dirty = true;
      logConfigError("Failed to open config: " + document.source.string());
      return document;
    }

    Json parsed = Json::parse(input, nullptr, false, true);
    if (parsed.is_discarded()) {
      document.dirty = true;
      logConfigError("Invalid config json, rebuilding: " +
                     document.source.string());
      return document;
    }

    document.raw = parsed;
    if (parsed.is_object()) {
      document.canonical = std::move(parsed);
    } else {
      document.dirty = true;
      document.canonical = Json::object();
      logConfigInfo("Legacy non-object config detected, migrating: " +
                    document.source.string());
    }
  } catch (const std::exception &e) {
    document.dirty = true;
    logConfigError(std::string("Failed to read config: ") + e.what());
  } catch (...) {
    document.dirty = true;
    logConfigError("Failed to read config: unknown error");
  }

  return document;
}

bool saveConfigDocument(const fs::path &path, const Json &config) {
  if (path.empty())
    return false;

  try {
    std::error_code ec;
    fs::path parent = path.parent_path();
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        logConfigError("Failed to create config directory: " + parent.string());
        return false;
      }
    }

    fs::path temp_path = path;
    temp_path += ".tmp";

    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        logConfigError("Failed to write config temp file: " +
                       temp_path.string());
        return false;
      }

      std::string payload = config.dump(4);
      output.write(payload.data(),
                   static_cast<std::streamsize>(payload.size()));
      output.put('\n');
      output.close();
      if (!output) {
        logConfigError("Failed to flush config temp file: " +
                       temp_path.string());
        return false;
      }
    }

    fs::rename(temp_path, path, ec);
    if (!ec)
      return true;

    ec.clear();
    fs::copy_file(temp_path, path, fs::copy_options::overwrite_existing, ec);
    std::error_code remove_ec;
    fs::remove(temp_path, remove_ec);
    if (ec) {
      logConfigError("Failed to replace config: " + path.string());
      return false;
    }

    return true;
  } catch (const std::exception &e) {
    logConfigError(std::string("Failed to save config: ") + e.what());
  } catch (...) {
    logConfigError("Failed to save config: unknown error");
  }

  return false;
}

void setOreUiConfigValue(OreUIConfig &config, bool value) {
  config.mUnknown3 = [value]() { return value; };
  config.mUnknown4 = [value]() { return value; };
}

void applyConfig(OreUi &ore_ui) {
  ConfigDocument document = loadConfigDocument();
  bool dirty = document.dirty || !pathExists(document.target);

  if (!document.canonical.is_object()) {
    document.canonical = Json::object();
    dirty = true;
  }

  for (auto &[name, config] : ore_ui.mConfigs) {
    bool value = false;
    bool rewrite_value = false;

    auto existing = document.canonical.find(name);
    if (existing != document.canonical.end()) {
      if (auto parsed = readBoolLike(*existing)) {
        value = *parsed;
        rewrite_value = !existing->is_boolean();
      } else {
        if (auto legacy = readNamedConfigValue(document.raw, name))
          value = *legacy;
        rewrite_value = true;
      }
    } else {
      if (auto legacy = readNamedConfigValue(document.raw, name))
        value = *legacy;
      rewrite_value = true;
    }

    if (rewrite_value) {
      document.canonical[name] = value;
      dirty = true;
    }

    setOreUiConfigValue(config, value);
  }

  if (dirty && !saveConfigDocument(document.target, document.canonical)) {
    logConfigError("Config was applied in memory but could not be saved.");
  }
}

SKY_AUTO_STATIC_HOOK(Hook2, memory::HookPriority::Normal, OREUI_PATTERN, void,
                     OreUi &a1, void *a2, void *a3, void *a4, void *a5,
                     void *a6) {
  origin(a1, a2, a3, a4, a5, a6);

  try {
    applyConfig(a1);
  } catch (const std::exception &e) {
    logConfigError(std::string("Failed to apply OreUI config: ") + e.what());
  } catch (...) {
    logConfigError("Failed to apply OreUI config: unknown error");
  }
}

} // namespace
