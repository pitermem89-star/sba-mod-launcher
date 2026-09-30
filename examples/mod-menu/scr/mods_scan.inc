// ------------------------------------------------------------ mod list -----

std::string read_file(const std::string& path) {
  std::string out;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
  }
  return out;
}

// Minimal "key": "value" extractor, enough for mod.json string fields.
std::string json_str(const std::string& j, const char* key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = j.find(k);
  if (p == std::string::npos) return "";
  p = j.find(':', p + k.size());
  if (p == std::string::npos) return "";
  p = j.find('"', p);
  if (p == std::string::npos) return "";
  std::string v;
  for (++p; p < j.size() && j[p] != '"'; ++p) {
    if (j[p] == '\\' && p + 1 < j.size()) ++p;
    v += j[p];
  }
  return v;
}

std::string mods_dir() {
  const BeariteApi* a = bearite::api();
  std::string d = (a && a->mod_dir) ? a->mod_dir : "";
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  size_t s = d.find_last_of('/');
  return s == std::string::npos ? d : d.substr(0, s);
}

struct ModInfo {
  std::string dir, name, version, author;
  std::string json;          // raw mod.json
  bool has_settings = false; // mod.json has a non-empty "settings" array
};

std::vector<ModInfo> scan_mods() {
  std::string root = mods_dir();
  std::vector<ModInfo> out;
  if (DIR* d = opendir(root.c_str())) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string dir = root + "/" + e->d_name;
      std::string json = read_file(dir + "/mod.json");
      if (json.empty()) continue;
      ModInfo m;
      m.dir = dir;
      m.json = json;
      m.has_settings = json.find("\"settings\"") != std::string::npos;
      m.name = json_str(json, "name");
      if (m.name.empty()) m.name = json_str(json, "id");
      if (m.name.empty()) m.name = e->d_name;
      m.version = json_str(json, "version");
      m.author = json_str(json, "author");
      out.push_back(m);
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end(), [](const ModInfo& x, const ModInfo& y) { return x.name < y.name; });
  return out;
}
