// mod.json parsing, version checks and dependency ordering. Header-only.
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace bearite {

namespace fs = std::filesystem;

struct Dependency {
  std::string id;
  std::string min_version = "0";
  bool optional = false;
};

struct Mod {
  std::string id, name, version, author, library;
  std::vector<std::string> game_versions;  // empty = any version
  std::vector<Dependency> deps;
  fs::path dir;
  bool enabled = true;
  std::string reason;  // why it is disabled
};

// "1.2.3-beta" -> {1,2,3}. Anything after the first non-digit/non-dot is ignored.
inline std::vector<long> split_version(const std::string& v) {
  std::vector<long> parts;
  long cur = 0;
  bool has = false;
  for (char c : v) {
    if (c >= '0' && c <= '9') {
      cur = cur * 10 + (c - '0');
      has = true;
    } else if (c == '.') {
      parts.push_back(cur);
      cur = 0;
      has = false;
    } else {
      break;
    }
  }
  if (has || !parts.empty()) parts.push_back(cur);
  return parts;
}

// Returns -1, 0 or 1.
inline int compare_versions(const std::string& a, const std::string& b) {
  auto x = split_version(a), y = split_version(b);
  size_t n = std::max(x.size(), y.size());
  for (size_t i = 0; i < n; ++i) {
    long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}

inline bool valid_id(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-' || c == '.';
    if (!ok) return false;
  }
  return s.find("..") == std::string::npos;
}

inline bool valid_library(const std::string& s) {
  if (s.size() < 4 || s.find('/') != std::string::npos || s[0] == '.') return false;
  return s.compare(s.size() - 3, 3, ".so") == 0;
}

// Reads <dir>/mod.json. On failure returns false and sets `error`.
inline bool parse_mod(const fs::path& dir, Mod& mod, std::string& error) {
  try {
    std::ifstream in(dir / "mod.json");
    if (!in) {
      error = "mod.json not found";
      return false;
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
    if (!j.is_object()) {
      error = "mod.json must be a JSON object";
      return false;
    }
    mod.dir = dir;
    mod.id = j.value("id", "");
    mod.version = j.value("version", "");
    mod.name = j.value("name", mod.id);
    mod.author = j.value("author", "");
    mod.library = j.value("library", "");

    if (!valid_id(mod.id)) {
      error = "missing or invalid 'id' (use a-z 0-9 . _ -)";
      return false;
    }
    if (mod.version.empty()) {
      error = "missing 'version'";
      return false;
    }
    if (!valid_library(mod.library)) {
      error = "missing or invalid 'library' (file name ending in .so)";
      return false;
    }
    if (j.contains("game_versions")) {
      if (!j["game_versions"].is_array()) {
        error = "'game_versions' must be an array";
        return false;
      }
      for (const auto& v : j["game_versions"]) mod.game_versions.push_back(v.get<std::string>());
    }
    if (j.contains("dependencies")) {
      if (!j["dependencies"].is_array()) {
        error = "'dependencies' must be an array";
        return false;
      }
      for (const auto& d : j["dependencies"]) {
        Dependency dep;
        dep.id = d.value("id", "");
        dep.min_version = d.value("version", "0");
        dep.optional = d.value("optional", false);
        if (!valid_id(dep.id)) {
          error = "dependency with invalid 'id'";
          return false;
        }
        mod.deps.push_back(dep);
      }
    }
    return true;
  } catch (const std::exception& e) {
    error = std::string("bad mod.json: ") + e.what();
    return false;
  }
}

inline bool supports_game_version(const Mod& m, const std::string& game) {
  if (m.game_versions.empty() || game.empty()) return true;
  for (const auto& v : m.game_versions)
    if (v == "*" || v == game) return true;
  return false;
}

// Returns mods in load order (dependencies first). Mods that cannot be loaded
// get enabled=false and a reason. Do not resize `mods` while using the result.
inline std::vector<Mod*> resolve_load_order(std::vector<Mod>& mods) {
  std::vector<Mod*> order;
  std::vector<int> state(mods.size(), 0);  // 0 new, 1 visiting, 2 done

  auto find = [&](const std::string& id) -> int {
    for (size_t i = 0; i < mods.size(); ++i)
      if (mods[i].id == id) return static_cast<int>(i);
    return -1;
  };
  auto fail = [&](Mod& m, const std::string& why) {
    if (m.enabled) {
      m.enabled = false;
      m.reason = why;
    }
  };

  std::function<bool(int)> visit = [&](int i) -> bool {
    Mod& m = mods[i];
    if (state[i] == 2) return m.enabled;
    if (state[i] == 1) return false;
    state[i] = 1;
    for (const Dependency& dep : m.deps) {
      if (!m.enabled) break;
      int j = find(dep.id);
      if (j < 0) {
        if (!dep.optional) fail(m, "missing dependency '" + dep.id + "'");
        continue;
      }
      if (state[j] == 1) {
        fail(m, "dependency cycle with '" + dep.id + "'");
        break;
      }
      if (!visit(j)) {
        if (!dep.optional) fail(m, "dependency '" + dep.id + "' is disabled");
        continue;
      }
      if (compare_versions(mods[j].version, dep.min_version) < 0) {
        fail(m, "needs '" + dep.id + "' >= " + dep.min_version + " (found " +
                    mods[j].version + ")");
      }
    }
    state[i] = 2;
    if (m.enabled) order.push_back(&m);
    return m.enabled;
  };

  for (size_t i = 0; i < mods.size(); ++i) visit(static_cast<int>(i));
  return order;
}

}  // namespace bearite
