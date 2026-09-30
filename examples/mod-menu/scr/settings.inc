// settings.inc — per-mod settings for mod-menu.
//
// A mod declares its settings in its own mod.json:
//
//   "settings": [
//     { "key": "greeting", "label": "Приветствие", "type": "choice",
//       "choices": ["hello", "hi", "привет"], "default": "hello" },
//     { "key": "enabled",  "label": "Включено",    "type": "bool",  "default": true },
//     { "key": "speed",    "label": "Скорость",    "type": "int",
//       "min": 1, "max": 10, "step": 1, "default": 5 },
//     { "key": "scale",    "label": "Масштаб",     "type": "float",
//       "min": 0.5, "max": 2, "step": 0.1, "default": 1 }
//   ]
//
// Types: bool, int, float, choice (cycles through "choices"). Values are saved
// to <mod dir>/settings.json, the same file bearite::setting() reads.
// A key that already exists keeps its JSON type (string / number / bool);
// a new key is written as a string.

// ------------------------------------------------------------ tiny JSON ----

struct J {
  enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
  bool b = false;
  double n = 0;
  std::string s;  // string value, or the raw text of a number
  std::vector<J> a;
  std::vector<std::pair<std::string, J>> o;

  const J* get(const char* k) const {
    if (t != Obj) return nullptr;
    for (const auto& kv : o) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  J* get_mut(const char* k) {
    return const_cast<J*>(static_cast<const J*>(this)->get(k));
  }
  // Scalar as text ("" for null / containers).
  std::string text() const {
    switch (t) {
      case Bool: return b ? "true" : "false";
      case Num:
      case Str: return s;
      default: return "";
    }
  }
};

void append_utf8(std::string& out, unsigned cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
  else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

struct JParser {
  const std::string& src;
  size_t i = 0;
  int depth = 0;
  explicit JParser(const std::string& s) : src(s) {}

  void ws() { while (i < src.size() && (src[i] == ' ' || src[i] == '\n' || src[i] == '\r' || src[i] == '\t')) ++i; }
  bool lit(const char* w) {
    size_t n = strlen(w);
    if (src.compare(i, n, w) != 0) return false;
    i += n;
    return true;
  }

  bool str(std::string& out) {
    if (i >= src.size() || src[i] != '"') return false;
    ++i;
    out.clear();
    while (i < src.size() && src[i] != '"') {
      char c = src[i++];
      if (c != '\\' || i >= src.size()) { out += c; continue; }
      char e = src[i++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          if (i + 4 > src.size()) return false;
          unsigned cp = static_cast<unsigned>(strtoul(src.substr(i, 4).c_str(), nullptr, 16));
          i += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= src.size() && src.compare(i, 2, "\\u") == 0) {
            unsigned lo = static_cast<unsigned>(strtoul(src.substr(i + 2, 4).c_str(), nullptr, 16));
            i += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          append_utf8(out, cp);
          break;
        }
        default: out += e;
      }
    }
    if (i >= src.size()) return false;
    ++i;
    return true;
  }

  bool value(J& v) {
    struct Depth { int& d; explicit Depth(int& x) : d(x) { ++d; } ~Depth() { --d; } } guard(depth);
    if (depth > 16) return false;
    ws();
    if (i >= src.size()) return false;
    char c = src[i];
    if (c == '{') {
      v.t = J::Obj;
      ++i; ws();
      if (i < src.size() && src[i] == '}') { ++i; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (i >= src.size() || src[i] != ':') return false;
        ++i;
        J child;
        if (!value(child)) return false;
        v.o.emplace_back(k, std::move(child));
        ws();
        if (i < src.size() && src[i] == ',') { ++i; continue; }
        if (i < src.size() && src[i] == '}') { ++i; return true; }
        return false;
      }
    }
    if (c == '[') {
      v.t = J::Arr;
      ++i; ws();
      if (i < src.size() && src[i] == ']') { ++i; return true; }
      for (;;) {
        J child;
        if (!value(child)) return false;
        v.a.push_back(std::move(child));
        ws();
        if (i < src.size() && src[i] == ',') { ++i; continue; }
        if (i < src.size() && src[i] == ']') { ++i; return true; }
        return false;
      }
    }
    if (c == '"') { v.t = J::Str; return str(v.s); }
    if (lit("true")) { v.t = J::Bool; v.b = true; return true; }
    if (lit("false")) { v.t = J::Bool; v.b = false; return true; }
    if (lit("null")) { v.t = J::Null; return true; }
    size_t st = i;
    while (i < src.size() && strchr("+-0123456789.eE", src[i])) ++i;
    if (i == st) return false;
    v.t = J::Num;
    v.s = src.substr(st, i - st);
    v.n = strtod(v.s.c_str(), nullptr);
    return true;
  }
};

bool parse_json(const std::string& text, J& out) {
  JParser p(text);
  return p.value(out);
}

std::string json_escape(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o + "\"";
}

void json_dump(const J& j, std::string& out, int indent) {
  std::string pad(static_cast<size_t>(indent) * 2, ' ');
  switch (j.t) {
    case J::Null: out += "null"; break;
    case J::Bool: out += j.b ? "true" : "false"; break;
    case J::Num: out += j.s.empty() ? "0" : j.s; break;
    case J::Str: out += json_escape(j.s); break;
    case J::Arr:
      out += "[";
      for (size_t k = 0; k < j.a.size(); ++k) { if (k) out += ", "; json_dump(j.a[k], out, indent + 1); }
      out += "]";
      break;
    case J::Obj:
      if (j.o.empty()) { out += "{}"; break; }
      out += "{\n";
      for (size_t k = 0; k < j.o.size(); ++k) {
        out += pad + "  " + json_escape(j.o[k].first) + ": ";
        json_dump(j.o[k].second, out, indent + 1);
        out += (k + 1 < j.o.size()) ? ",\n" : "\n";
      }
      out += pad + "}";
      break;
  }
}

// ------------------------------------------------------------ schema -------

struct SettingDef {
  std::string key, label, type, def;  // type: bool | int | float | choice | string
  std::vector<std::string> choices;
  double min = 0, max = 0, step = 1;
  bool has_min = false, has_max = false;
};

std::vector<SettingDef> parse_defs(const std::string& mod_json) {
  std::vector<SettingDef> out;
  J root;
  if (!parse_json(mod_json, root)) return out;
  const J* arr = root.get("settings");
  if (!arr || arr->t != J::Arr) return out;
  for (const J& e : arr->a) {
    if (e.t != J::Obj) continue;
    const J* key = e.get("key");
    if (!key || key->t != J::Str || key->s.empty()) continue;
    SettingDef d;
    d.key = key->s;
    const J* label = e.get("label");
    d.label = (label && label->t == J::Str && !label->s.empty()) ? label->s : d.key;
    const J* dv = e.get("default");
    if (dv) d.def = dv->text();
    if (const J* ch = e.get("choices")) {
      if (ch->t == J::Arr) for (const J& c : ch->a) if (c.t != J::Null) d.choices.push_back(c.text());
    }
    const J* type = e.get("type");
    if (type && type->t == J::Str) d.type = type->s;
    if (d.type.empty()) {
      if (!d.choices.empty()) d.type = "choice";
      else if (dv && dv->t == J::Bool) d.type = "bool";
      else if (dv && dv->t == J::Num) d.type = dv->s.find('.') == std::string::npos ? "int" : "float";
      else d.type = "string";
    }
    if (d.type == "choice" && d.choices.empty()) d.type = "string";
    if (const J* x = e.get("min")) { d.min = x->n; d.has_min = true; }
    if (const J* x = e.get("max")) { d.max = x->n; d.has_max = true; }
    if (const J* x = e.get("step")) { if (x->n > 0) d.step = x->n; }
    else d.step = d.type == "float" ? 0.1 : 1;
    if (d.def.empty()) d.def = d.type == "bool" ? "false" : d.type == "choice" ? d.choices[0] : d.type == "string" ? "" : "0";
    out.push_back(d);
  }
  return out;
}

// ------------------------------------------------------------ values -------

J read_settings(const std::string& dir) {
  J j;
  std::string text = read_file(dir + "/settings.json");
  if (text.empty() || !parse_json(text, j) || j.t != J::Obj) j = J();
  j.t = J::Obj;
  return j;
}

bool write_settings(const std::string& dir, const J& j) {
  std::string out;
  json_dump(j, out, 0);
  out += "\n";
  std::string tmp = dir + "/settings.json.tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  ok = (fclose(f) == 0) && ok;
  return ok && rename(tmp.c_str(), (dir + "/settings.json").c_str()) == 0;
}

std::string current_value(const J& settings, const SettingDef& d) {
  const J* v = settings.get(d.key.c_str());
  if (!v || v->t == J::Null || v->t == J::Arr || v->t == J::Obj) return d.def;
  return v->text();
}

void store_value(J& settings, const std::string& key, const std::string& val) {
  if (J* old = settings.get_mut(key.c_str())) {
    if (old->t == J::Bool) { old->b = (val == "true" || val == "1"); return; }
    if (old->t == J::Num) { old->s = val; old->n = strtod(val.c_str(), nullptr); return; }
    old->t = J::Str;
    old->s = val;
    return;
  }
  J n;
  n.t = J::Str;
  n.s = val;
  settings.o.emplace_back(key, std::move(n));
}

std::string fmt_number(double x, bool integer) {
  char b[32];
  if (integer) snprintf(b, sizeof(b), "%ld", static_cast<long>(x < 0 ? x - 0.5 : x + 0.5));
  else snprintf(b, sizeof(b), "%.3g", x);
  return b;
}

std::string shown_value(const SettingDef& d, const std::string& val) {
  if (d.type == "bool") return val == "true" || val == "1" ? "Вкл" : "Выкл";
  return val;
}

// Applies TOGGLE / DEC / INC / CYCLE on setting number `a.arg` of the mod being
// configured. Returns true if something was written.
bool change_setting(const Action& a) {
  if (g.cfg_mod < 0 || g.cfg_mod >= static_cast<int>(g.mods.size())) return false;
  const ModInfo& m = g.mods[static_cast<size_t>(g.cfg_mod)];
  std::vector<SettingDef> defs = parse_defs(m.json);
  if (a.arg < 0 || a.arg >= static_cast<int>(defs.size())) return false;
  const SettingDef& d = defs[static_cast<size_t>(a.arg)];
  J settings = read_settings(m.dir);
  std::string cur = current_value(settings, d);
  std::string next = cur;

  if (a.kind == ACT_TOGGLE) {
    next = (cur == "true" || cur == "1") ? "false" : "true";
  } else if (a.kind == ACT_CYCLE && !d.choices.empty()) {
    size_t idx = 0;
    for (size_t k = 0; k < d.choices.size(); ++k) if (d.choices[k] == cur) idx = k + 1;
    next = d.choices[idx % d.choices.size()];
  } else if (a.kind == ACT_DEC || a.kind == ACT_INC) {
    double x = strtod(cur.c_str(), nullptr) + (a.kind == ACT_INC ? d.step : -d.step);
    if (d.has_min && x < d.min) x = d.min;
    if (d.has_max && x > d.max) x = d.max;
    next = fmt_number(x, d.type == "int");
  } else {
    return false;
  }
  store_value(settings, d.key, next);
  bool ok = write_settings(m.dir, settings);
  bearite::log(ok ? BEARITE_LOG_INFO : BEARITE_LOG_ERROR, TAG, "%s: %s = %s (%s)", m.name.c_str(), d.key.c_str(),
               next.c_str(), ok ? "saved" : "write failed");
  return ok;
}

// ------------------------------------------------------------ rows -------

const Color COL_BTN = {0.36f, 0.28f, 0.75f, 1};
const Color COL_ON = {0.25f, 0.65f, 0.40f, 1};
const Color COL_OFF = {0.45f, 0.40f, 0.60f, 1};
const Color COL_SETTING_ROW = {0.17f, 0.12f, 0.42f, 0.95f};

// One setting inside the mod's cell: name on the left, control on the right.
// (y0, y1) are the row's anchors inside the cell.
void build_setting_row(void* cell_tr, const SettingDef& d, const std::string& val, int idx, float y0, float y1) {
  void* row = new_ui("SettingRow", cell_tr);
  if (!row) return;
  void* row_tr = call(u.m_go_get_tr, row, {});
  rect(row_tr, {0.03f, y0}, {0.97f, y1});
  add_image(row, COL_SETTING_ROW, false);

  void* label = new_label(row_tr, d.label, 513, 30);
  if (label) rect(call(u.m_go_get_tr, label, {}), {0, 0}, {0.55f, 1}, {22, 2}, {0, -2});

  if (d.type == "bool") {
    bool on = val == "true" || val == "1";
    make_button(row_tr, "Toggle", shown_value(d, val), {0.66f, 0.14f}, {0.98f, 0.86f}, on ? COL_ON : COL_OFF, {ACT_TOGGLE, idx}, 28);
  } else if (d.type == "int" || d.type == "float") {
    make_button(row_tr, "Dec", "-", {0.60f, 0.14f}, {0.69f, 0.86f}, COL_BTN, {ACT_DEC, idx}, 40);
    void* v_lbl = new_label(row_tr, val, 514, 30);
    if (v_lbl) rect(call(u.m_go_get_tr, v_lbl, {}), {0.70f, 0}, {0.89f, 1});
    make_button(row_tr, "Inc", "+", {0.90f, 0.14f}, {0.99f, 0.86f}, COL_BTN, {ACT_INC, idx}, 40);
  } else if (d.type == "choice") {
    make_button(row_tr, "Cycle", val, {0.60f, 0.14f}, {0.98f, 0.86f}, COL_BTN, {ACT_CYCLE, idx}, 26);
  } else {
    // free text has no keyboard input yet: show the value, edit it in settings.json
    void* v_lbl = new_label(row_tr, val.empty() ? "—" : val, 514, 24);
    if (v_lbl) rect(call(u.m_go_get_tr, v_lbl, {}), {0.60f, 0}, {0.98f, 1});
  }
}
