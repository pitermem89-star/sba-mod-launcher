// Per-mod settings stored in <mod_dir>/settings.json. Header-only.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace bearite {

class Settings {
 public:
  explicit Settings(std::filesystem::path file) : file_(std::move(file)) { load(); }

  bool get(const std::string& key, std::string& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    if (it == values_.end()) return false;
    out = it->second;
    return true;
  }

  bool set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key] = value;
    return save();
  }

 private:
  // Numbers and booleans in the file are read as text ("5", "true").
  void load() {
    try {
      std::ifstream in(file_);
      if (!in) return;
      nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
      if (!j.is_object()) return;
      for (auto it = j.begin(); it != j.end(); ++it)
        values_[it.key()] =
            it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
    } catch (...) {
    }
  }

  bool save() {  // mutex is held by the caller
    try {
      nlohmann::json j = nlohmann::json::object();
      for (const auto& kv : values_) j[kv.first] = kv.second;
      std::filesystem::path tmp = file_;
      tmp += ".tmp";
      {
        std::ofstream out(tmp);
        if (!out) return false;
        out << j.dump(2) << '\n';
      }
      std::error_code ec;
      std::filesystem::rename(tmp, file_, ec);
      return !ec;
    } catch (...) {
      return false;
    }
  }

  std::filesystem::path file_;
  std::mutex mutex_;
  std::map<std::string, std::string> values_;
};

}  // namespace bearite
