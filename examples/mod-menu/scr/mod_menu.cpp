// mod-menu: adds a "Mods" button to the pause menu and a window with the list
// of installed mods. Works like a normal mod: everything goes through the
// Bearite API, nothing here is part of the loader.
//
// How it works:
//   1. Hook PauseMenuManager.Start. After the original runs, clone the
//      "Achievements" button and the "About" window (both already exist in the
//      pause menu prefab), retitle the clone to "Mods".
//   2. Hook UnityEngine.UI.Button.OnPointerClick / OnSubmit. If the clicked
//      button is our clone, open our window instead of calling the original
//      handler (the clone still carries the Achievements listener from the
//      prefab, so the original must not run). Any button inside our window
//      closes it.
//   3. IL2CPP calls (Instantiate, SetActive, set_text, ...) go through
//      il2cpp_runtime_invoke, resolved from libil2cpp.so with dlsym.
#include "bearite.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char* TAG = "mod-menu";

// Field offsets from dump.cs (PauseMenuManager : SbaBehaviour).
constexpr size_t OFF_ABOUT_WINDOW = 0x28;
constexpr size_t OFF_ACHIEVEMENTS_BUTTON = 0x98;

// ---------------------------------------------------------------- il2cpp ----

struct Il2 {
  void* (*domain_get)();
  void* (*domain_assembly_open)(void*, const char*);
  void* (*assembly_get_image)(void*);
  void* (*class_from_name)(void*, const char*, const char*);
  void* (*class_get_methods)(void*, void**);
  const char* (*method_get_name)(void*);
  uint32_t (*method_get_param_count)(void*);
  void* (*method_get_param)(void*, uint32_t);
  char* (*type_get_name)(void*);
  void (*il_free)(void*);
  void* (*runtime_invoke)(void*, void*, void**, void**);
  void* (*string_new)(const char*);
  const uint16_t* (*string_chars)(void*);
  int32_t (*string_length)(void*);
  void* (*class_get_type)(void*);
  void* (*type_get_object)(void*);
  void* (*object_unbox)(void*);
  uint32_t (*gchandle_new)(void*, int);
  void (*gchandle_free)(uint32_t);
  size_t (*array_length)(void*);
  void* (*exception_message)(void*);  // not exported everywhere, optional
  bool ok = false;
};

Il2 g_il;

bool load_il2cpp() {
  if (g_il.ok) return true;
  void* h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
  if (!h) h = RTLD_DEFAULT;
  bool all = true;
  auto sym = [&](const char* name, auto& out, bool required = true) {
    out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(dlsym(h, name));
    if (!out && required) {
      bearite::log(BEARITE_LOG_ERROR, TAG, "missing export %s", name);
      all = false;
    }
  };
  sym("il2cpp_domain_get", g_il.domain_get);
  sym("il2cpp_domain_assembly_open", g_il.domain_assembly_open);
  sym("il2cpp_assembly_get_image", g_il.assembly_get_image);
  sym("il2cpp_class_from_name", g_il.class_from_name);
  sym("il2cpp_class_get_methods", g_il.class_get_methods);
  sym("il2cpp_method_get_name", g_il.method_get_name);
  sym("il2cpp_method_get_param_count", g_il.method_get_param_count);
  sym("il2cpp_method_get_param", g_il.method_get_param);
  sym("il2cpp_type_get_name", g_il.type_get_name);
  sym("il2cpp_free", g_il.il_free);
  sym("il2cpp_runtime_invoke", g_il.runtime_invoke);
  sym("il2cpp_string_new", g_il.string_new);
  sym("il2cpp_string_chars", g_il.string_chars);
  sym("il2cpp_string_length", g_il.string_length);
  sym("il2cpp_class_get_type", g_il.class_get_type);
  sym("il2cpp_type_get_object", g_il.type_get_object);
  sym("il2cpp_object_unbox", g_il.object_unbox);
  sym("il2cpp_gchandle_new", g_il.gchandle_new);
  sym("il2cpp_gchandle_free", g_il.gchandle_free);
  sym("il2cpp_array_length", g_il.array_length);
  g_il.ok = all;
  return all;
}

// Layout of an IL2CPP managed array on 64-bit: header(16) + bounds(8) + max_length(8) + items.
inline void** array_items(void* arr) { return reinterpret_cast<void**>(static_cast<char*>(arr) + 32); }

void* find_class(const char* asm_name, const char* ns, const char* name) {
  static std::map<std::string, void*> cache;
  std::string key = std::string(asm_name) + "|" + ns + "|" + name;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  void* c = nullptr;
  void* a = g_il.domain_assembly_open(g_il.domain_get(), asm_name);
  if (a) {
    void* img = g_il.assembly_get_image(a);
    if (img) c = g_il.class_from_name(img, ns, name);
  }
  if (!c) bearite::log(BEARITE_LOG_ERROR, TAG, "class not found: %s %s.%s", asm_name, ns, name);
  cache[key] = c;
  return c;
}

// Finds a method by name and exact parameter type names (so overloads such as
// Object.Instantiate(Object, Transform) resolve unambiguously).
void* find_method(void* klass, const char* name, std::initializer_list<const char*> params) {
  if (!klass) return nullptr;
  static std::map<std::string, void*> cache;
  std::string key = std::to_string(reinterpret_cast<uintptr_t>(klass)) + "|" + name;
  for (const char* p : params) key += std::string(",") + p;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;

  void* found = nullptr;
  void* iter = nullptr;
  while (void* m = g_il.class_get_methods(klass, &iter)) {
    if (strcmp(g_il.method_get_name(m), name) != 0) continue;
    if (g_il.method_get_param_count(m) != params.size()) continue;
    bool match = true;
    uint32_t i = 0;
    for (const char* want : params) {
      char* tn = g_il.type_get_name(g_il.method_get_param(m, i++));
      if (!tn || strcmp(tn, want) != 0) match = false;
      if (tn) g_il.il_free(tn);
      if (!match) break;
    }
    if (match) { found = m; break; }
  }
  if (!found) bearite::log(BEARITE_LOG_ERROR, TAG, "method not found: %s (%zu args)", name, params.size());
  cache[key] = found;
  return found;
}

std::string utf8_from_il2cpp(void* s) {
  std::string out;
  if (!s) return out;
  const uint16_t* c = g_il.string_chars(s);
  int32_t n = g_il.string_length(s);
  for (int32_t i = 0; i < n; ++i) {
    uint32_t cp = c[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) cp = 0x10000 + ((cp - 0xD800) << 10) + (c[++i] - 0xDC00);
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
  }
  return out;
}

// Calls a method. `self` is the object (nullptr for static). Reference-type args
// are passed as the object pointer, value-type args as a pointer to the value.
// Returns the boxed result (value types) or the object (reference types).
void* call(void* method, void* self, std::initializer_list<void*> args, bool* ok = nullptr) {
  if (ok) *ok = false;
  if (!method) return nullptr;
  void* a[8] = {};
  size_t i = 0;
  for (void* v : args) a[i++] = v;
  void* exc = nullptr;
  void* r = g_il.runtime_invoke(method, self, a, &exc);
  if (exc) {
    bearite::log(BEARITE_LOG_WARN, TAG, "managed exception in %s", g_il.method_get_name(method));
    return nullptr;
  }
  if (ok) *ok = true;
  return r;
}

bool unbox_bool(void* boxed) { return boxed && *static_cast<bool*>(g_il.object_unbox(boxed)); }
int unbox_int(void* boxed) { return boxed ? *static_cast<int*>(g_il.object_unbox(boxed)) : 0; }

// ------------------------------------------------------------ unity glue ----

constexpr const char* ASM_CORE = "UnityEngine.CoreModule";
constexpr const char* ASM_UI = "UnityEngine.UI";
constexpr const char* ASM_TMP = "Unity.TextMeshPro";
constexpr const char* ASM_GAME = "Assembly-CSharp";

struct U {
  void *Object, *Component, *GameObject, *Transform, *RectTransform;
  void *Button, *LayoutGroup, *TMP_Text, *TranslateUI, *UIWindow;
  void *m_instantiate, *m_destroy_imm, *m_set_name;
  void *m_get_go, *m_get_tr, *m_get_parent, *m_get_sibling, *m_set_sibling, *m_is_child_of;
  void *m_go_set_active, *m_go_get_comps, *m_go_get_comp, *m_go_get_tr;
  void *m_comp_get_comp, *m_comp_get_in_parent;
  void *m_get_rect, *m_get_apos, *m_set_apos;
  void *m_tmp_set_text, *m_tmp_get_text;
  void *m_win_show;
  void *t_TranslateUI, *t_TMP_Text, *t_LayoutGroup, *t_UIWindow, *t_Button;  // System.Type objects
  bool ok = false;
};
U u;

bool init_unity() {
  if (u.ok) return true;
  if (!load_il2cpp()) return false;
  u.Object = find_class(ASM_CORE, "UnityEngine", "Object");
  u.Component = find_class(ASM_CORE, "UnityEngine", "Component");
  u.GameObject = find_class(ASM_CORE, "UnityEngine", "GameObject");
  u.Transform = find_class(ASM_CORE, "UnityEngine", "Transform");
  u.RectTransform = find_class(ASM_CORE, "UnityEngine", "RectTransform");
  u.Button = find_class(ASM_UI, "UnityEngine.UI", "Button");
  u.LayoutGroup = find_class(ASM_UI, "UnityEngine.UI", "LayoutGroup");
  u.TMP_Text = find_class(ASM_TMP, "TMPro", "TMP_Text");
  u.TranslateUI = find_class(ASM_GAME, "", "TranslateUI");
  u.UIWindow = find_class(ASM_GAME, "", "UIWindow");

  u.m_instantiate = find_method(u.Object, "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform"});
  u.m_destroy_imm = find_method(u.Object, "DestroyImmediate", {"UnityEngine.Object"});
  u.m_set_name = find_method(u.Object, "set_name", {"System.String"});
  u.m_get_go = find_method(u.Component, "get_gameObject", {});
  u.m_get_tr = find_method(u.Component, "get_transform", {});
  u.m_comp_get_comp = find_method(u.Component, "GetComponent", {"System.Type"});
  u.m_comp_get_in_parent = find_method(u.Component, "GetComponentInParent", {"System.Type"});
  u.m_get_parent = find_method(u.Transform, "get_parent", {});
  u.m_get_sibling = find_method(u.Transform, "GetSiblingIndex", {});
  u.m_set_sibling = find_method(u.Transform, "SetSiblingIndex", {"System.Int32"});
  u.m_is_child_of = find_method(u.Transform, "IsChildOf", {"UnityEngine.Transform"});
  u.m_go_set_active = find_method(u.GameObject, "SetActive", {"System.Boolean"});
  u.m_go_get_comps = find_method(u.GameObject, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
  u.m_go_get_comp = find_method(u.GameObject, "GetComponent", {"System.Type"});
  u.m_go_get_tr = find_method(u.GameObject, "get_transform", {});
  u.m_get_rect = find_method(u.RectTransform, "get_rect", {});
  u.m_get_apos = find_method(u.RectTransform, "get_anchoredPosition", {});
  u.m_set_apos = find_method(u.RectTransform, "set_anchoredPosition", {"UnityEngine.Vector2"});
  u.m_tmp_set_text = find_method(u.TMP_Text, "set_text", {"System.String"});
  u.m_tmp_get_text = find_method(u.TMP_Text, "get_text", {});
  u.m_win_show = find_method(u.UIWindow, "ShowWindow", {"System.Boolean"});

  auto type_obj = [](void* k) { return k ? g_il.type_get_object(g_il.class_get_type(k)) : nullptr; };
  u.t_TranslateUI = type_obj(u.TranslateUI);
  u.t_TMP_Text = type_obj(u.TMP_Text);
  u.t_LayoutGroup = type_obj(u.LayoutGroup);
  u.t_UIWindow = type_obj(u.UIWindow);
  u.t_Button = type_obj(u.Button);

  void* required[] = {u.m_instantiate, u.m_destroy_imm, u.m_set_name, u.m_get_go, u.m_get_tr, u.m_get_parent,
                      u.m_set_sibling, u.m_get_sibling, u.m_is_child_of, u.m_go_set_active, u.m_go_get_comps,
                      u.m_go_get_comp, u.m_go_get_tr, u.m_tmp_set_text, u.m_win_show, u.t_TMP_Text, u.t_UIWindow};
  for (void* r : required) if (!r) { bearite::log(BEARITE_LOG_ERROR, TAG, "unity glue incomplete, menu disabled"); return false; }
  u.ok = true;
  return true;
}

void* il_str(const std::string& s) { return g_il.string_new(s.c_str()); }
void* game_object(void* comp) { return call(u.m_get_go, comp, {}); }
void* transform(void* comp) { return call(u.m_get_tr, comp, {}); }
void set_active(void* go, bool on) { call(u.m_go_set_active, go, {&on}); }

// All components of type `type_obj` under `go` (including inactive), as a vector.
std::vector<void*> components(void* go, void* type_obj) {
  std::vector<void*> out;
  if (!type_obj) return out;
  bool inactive = true;
  void* arr = call(u.m_go_get_comps, go, {type_obj, &inactive});
  if (!arr) return out;
  size_t n = g_il.array_length(arr);
  for (size_t i = 0; i < n; ++i) if (array_items(arr)[i]) out.push_back(array_items(arr)[i]);
  return out;
}

void set_text(void* tmp, const std::string& s) { call(u.m_tmp_set_text, tmp, {il_str(s)}); }

// Instantiate `src_go` as a child of the same parent as `src_comp`.
void* clone_next_to(void* src_go, void* parent_tr) {
  void* c = call(u.m_instantiate, nullptr, {src_go, parent_tr});
  return c;
}

// The prefab's text components get their string from TranslateUI (a localisation
// key). Remove those on the clone so our own text is not overwritten.
void strip_translate(void* go) {
  for (void* t : components(go, u.t_TranslateUI)) call(u.m_destroy_imm, nullptr, {t});
}

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

std::string mod_list_text() {
  std::string dir = mods_dir();
  std::vector<std::string> lines;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string json = read_file(dir + "/" + e->d_name + "/mod.json");
      if (json.empty()) continue;
      std::string name = json_str(json, "name");
      if (name.empty()) name = json_str(json, "id");
      if (name.empty()) name = e->d_name;
      std::string ver = json_str(json, "version");
      std::string author = json_str(json, "author");
      std::string line = "• " + name;
      if (!ver.empty()) line += "  v" + ver;
      if (!author.empty()) line += "  —  " + author;
      lines.push_back(line);
    }
    closedir(d);
  }
  std::sort(lines.begin(), lines.end());
  if (lines.empty()) return "Моды не найдены";
  std::string out;
  for (auto& l : lines) out += l + "\n";
  return out;
}

// ------------------------------------------------------------- state -------

struct State {
  void* btn = nullptr;      // cloned "Mods" button (Button component)
  void* win_go = nullptr;   // cloned window GameObject
  void* win = nullptr;      // its UIWindow component
  void* win_tr = nullptr;   // its Transform
  uint32_t h[4] = {0, 0, 0, 0};
} g;

void release_state() {
  for (uint32_t& h : g.h) if (h) { g_il.gchandle_free(h); h = 0; }
  g.btn = g.win_go = g.win = g.win_tr = nullptr;
}

void pin(void* obj, int slot) { g.h[slot] = g_il.gchandle_new(obj, 0); }

void log_texts(const char* what, const std::vector<void*>& texts) {
  int i = 0;
  for (void* t : texts) {
    std::string s = utf8_from_il2cpp(call(u.m_tmp_get_text, t, {}));
    if (s.size() > 40) s.resize(40);
    bearite::log(BEARITE_LOG_DEBUG, TAG, "%s text[%d] = \"%s\"", what, i++, s.c_str());
  }
}

bool in_button(void* comp) {
  return u.m_comp_get_in_parent && u.t_Button && call(u.m_comp_get_in_parent, comp, {u.t_Button}) != nullptr;
}

// Fills the cloned About window: shortest non-button text becomes the title,
// the longest one the body, the rest are cleared. Buttons keep their labels.
void fill_window() {
  std::vector<void*> texts = components(g.win_go, u.t_TMP_Text);
  std::vector<void*> plain;
  for (void* t : texts) if (!in_button(t)) plain.push_back(t);
  if (plain.empty()) { bearite::log(BEARITE_LOG_WARN, TAG, "window clone has no plain text"); return; }

  auto len = [](void* t) { return utf8_from_il2cpp(call(u.m_tmp_get_text, t, {})).size(); };
  void* body = plain[0];
  for (void* t : plain) if (len(t) > len(body)) body = t;
  void* title = nullptr;
  for (void* t : plain) if (t != body) { title = t; break; }

  for (void* t : plain) if (t != body && t != title) set_text(t, "");
  if (title) { set_text(title, "Моды"); set_text(body, mod_list_text()); }
  else set_text(body, "Моды\n\n" + mod_list_text());
}

void open_window() {
  if (!g.win || !u.ok) return;
  fill_window();
  bool on = true;
  call(u.m_win_show, g.win, {&on});
}

void close_window() {
  if (!g.win) return;
  bool off = false;
  call(u.m_win_show, g.win, {&off});
}

bool inside_window(void* button) {
  if (!g.win_tr || !button) return false;
  void* tr = transform(button);
  return tr && unbox_bool(call(u.m_is_child_of, tr, {g.win_tr}));
}

// ----------------------------------------------------------- building ------

void build(void* pause_menu) {
  if (!init_unity()) return;
  release_state();

  char* base = static_cast<char*>(pause_menu);
  void* ach = *reinterpret_cast<void**>(base + OFF_ACHIEVEMENTS_BUTTON);
  void* about = *reinterpret_cast<void**>(base + OFF_ABOUT_WINDOW);
  if (!ach || !about) { bearite::log(BEARITE_LOG_WARN, TAG, "achievementsButton/aboutWindow are null, skipping"); return; }

  // ---- button
  void* ach_tr = transform(ach);
  void* parent = call(u.m_get_parent, ach_tr, {});
  void* btn_go = clone_next_to(game_object(ach), parent);
  if (!btn_go) { bearite::log(BEARITE_LOG_ERROR, TAG, "cloning the button failed"); return; }
  call(u.m_set_name, btn_go, {il_str("ModsButton")});
  strip_translate(btn_go);

  std::vector<void*> btn_texts = components(btn_go, u.t_TMP_Text);
  log_texts("button", btn_texts);
  if (!btn_texts.empty()) set_text(btn_texts[0], "Моды");
  for (size_t i = 1; i < btn_texts.size(); ++i) set_text(btn_texts[i], "");
  void* btn_tr = call(u.m_go_get_tr, btn_go, {});
  void* btn_comp = call(u.m_go_get_comp, btn_go, {u.t_Button});
  if (!btn_comp || !btn_tr) { bearite::log(BEARITE_LOG_ERROR, TAG, "cloned button has no Button component"); return; }

  // Put it right after the original in the same column.
  int idx = unbox_int(call(u.m_get_sibling, ach_tr, {}));
  int next = idx + 1;
  call(u.m_set_sibling, btn_tr, {&next});
  set_active(btn_go, true);

  // If the parent has no layout group, the clone sits exactly on top of the
  // original: shift it down by one button height.
  void* layout = (u.m_comp_get_comp && u.t_LayoutGroup) ? call(u.m_comp_get_comp, parent, {u.t_LayoutGroup}) : nullptr;
  if (!layout && u.m_get_rect && u.m_get_apos && u.m_set_apos) {
    void* rect = call(u.m_get_rect, btn_tr, {});
    void* pos = call(u.m_get_apos, btn_tr, {});
    if (rect && pos) {
      float* r = static_cast<float*>(g_il.object_unbox(rect));  // x, y, width, height
      float p[2];
      memcpy(p, g_il.object_unbox(pos), sizeof(p));
      p[1] -= r[3] * 1.15f;
      call(u.m_set_apos, btn_tr, {p});
    }
  }

  // ---- window
  void* about_tr = transform(about);
  void* about_parent = call(u.m_get_parent, about_tr, {});
  void* win_go = clone_next_to(game_object(about), about_parent);
  if (!win_go) { bearite::log(BEARITE_LOG_ERROR, TAG, "cloning the window failed"); return; }
  call(u.m_set_name, win_go, {il_str("ModsWindow")});
  strip_translate(win_go);
  void* win = call(u.m_go_get_comp, win_go, {u.t_UIWindow});
  void* win_tr = call(u.m_go_get_tr, win_go, {});
  if (!win || !win_tr) { bearite::log(BEARITE_LOG_ERROR, TAG, "cloned window has no UIWindow"); return; }
  log_texts("window", components(win_go, u.t_TMP_Text));
  set_active(win_go, false);

  g.btn = btn_comp;
  g.win_go = win_go;
  g.win = win;
  g.win_tr = win_tr;
  pin(btn_comp, 0);
  pin(win_go, 1);
  pin(win, 2);
  pin(win_tr, 3);
  bearite::log(BEARITE_LOG_INFO, TAG, "Mods button and window created");
}

// -------------------------------------------------------------- hooks ------

void (*orig_start)(void*, void*) = nullptr;
void (*orig_click)(void*, void*, void*) = nullptr;
void (*orig_submit)(void*, void*, void*) = nullptr;

void hk_start(void* self, void* mi) {
  orig_start(self, mi);
  build(self);
}

// Returns true if the event was consumed by us.
bool handle_press(void* button) {
  if (!g.btn) return false;
  if (button == g.btn) { open_window(); return true; }
  if (inside_window(button)) { close_window(); return true; }
  return false;
}

void hk_click(void* self, void* ev, void* mi) {
  if (handle_press(self)) return;
  orig_click(self, ev, mi);
}

void hk_submit(void* self, void* ev, void* mi) {
  if (handle_press(self)) return;
  orig_submit(self, ev, mi);
}

}  // namespace

// ------------------------------------------------------- mod entry points --

extern "C" {

BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  bool a = bearite::hook(ASM_GAME, "", "PauseMenuManager", "Start", 0,
                         reinterpret_cast<void*>(&hk_start), reinterpret_cast<void**>(&orig_start));
  bool b = bearite::hook(ASM_UI, "UnityEngine.UI", "Button", "OnPointerClick", 1,
                         reinterpret_cast<void*>(&hk_click), reinterpret_cast<void**>(&orig_click));
  bool c = bearite::hook(ASM_UI, "UnityEngine.UI", "Button", "OnSubmit", 1,
                         reinterpret_cast<void*>(&hk_submit), reinterpret_cast<void**>(&orig_submit));
  bearite::log(BEARITE_LOG_INFO, TAG, "loaded, hooks: Start=%d OnPointerClick=%d OnSubmit=%d", a, b, c);
  return (a && b && c) ? 0 : 1;
}

BEARITE_EXPORT void bearite_on_unload(void) { g.btn = nullptr; }

}  // extern "C"
