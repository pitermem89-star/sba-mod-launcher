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
  void* (*object_new)(void*);
  void* (*array_new)(void*, size_t);
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
  sym("il2cpp_object_new", g_il.object_new);
  sym("il2cpp_array_new", g_il.array_new);
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

struct ModInfo {
  std::string dir, name, version, author;
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

// ------------------------------------------------------------- state -------

// ---- extra Unity glue for building the list UI from code

struct V {
  void *Image, *Graphic, *Sprite, *Texture, *Texture2D, *ImageConversion, *SystemType, *SystemByte;
  void *m_go_ctor, *m_go_add_comp, *m_set_parent, *m_destroy;
  void *m_amin, *m_amax, *m_omin, *m_omax, *m_pivot, *m_size;
  void *m_set_color, *m_set_raycast, *m_set_sprite;
  void *m_tmp_align, *m_tmp_size;
  void *m_tex_ctor, *m_tex_w, *m_tex_h, *m_load_image, *m_sprite_create;
  void *t_RectTransform, *t_Image, *t_Button;
  bool ok = false;
};
V v;

bool init_ui() {
  if (v.ok) return true;
  if (!init_unity()) return false;
  v.Image = find_class(ASM_UI, "UnityEngine.UI", "Image");
  v.Graphic = find_class(ASM_UI, "UnityEngine.UI", "Graphic");
  v.Sprite = find_class(ASM_CORE, "UnityEngine", "Sprite");
  v.Texture = find_class(ASM_CORE, "UnityEngine", "Texture");
  v.Texture2D = find_class(ASM_CORE, "UnityEngine", "Texture2D");
  v.ImageConversion = find_class("UnityEngine.ImageConversionModule", "UnityEngine", "ImageConversion");
  v.SystemType = find_class("mscorlib", "System", "Type");
  v.SystemByte = find_class("mscorlib", "System", "Byte");

  v.m_go_ctor = find_method(u.GameObject, ".ctor", {"System.String", "System.Type[]"});
  v.m_go_add_comp = find_method(u.GameObject, "AddComponent", {"System.Type"});
  v.m_set_parent = find_method(u.Transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
  v.m_destroy = find_method(u.Object, "Destroy", {"UnityEngine.Object"});
  v.m_amin = find_method(u.RectTransform, "set_anchorMin", {"UnityEngine.Vector2"});
  v.m_amax = find_method(u.RectTransform, "set_anchorMax", {"UnityEngine.Vector2"});
  v.m_omin = find_method(u.RectTransform, "set_offsetMin", {"UnityEngine.Vector2"});
  v.m_omax = find_method(u.RectTransform, "set_offsetMax", {"UnityEngine.Vector2"});
  v.m_pivot = find_method(u.RectTransform, "set_pivot", {"UnityEngine.Vector2"});
  v.m_size = find_method(u.RectTransform, "set_sizeDelta", {"UnityEngine.Vector2"});
  v.m_set_color = find_method(v.Graphic, "set_color", {"UnityEngine.Color"});
  v.m_set_raycast = find_method(v.Graphic, "set_raycastTarget", {"System.Boolean"});
  v.m_set_sprite = find_method(v.Image, "set_sprite", {"UnityEngine.Sprite"});
  v.m_tmp_align = find_method(u.TMP_Text, "set_alignment", {"TMPro.TextAlignmentOptions"});
  v.m_tmp_size = find_method(u.TMP_Text, "set_fontSize", {"System.Single"});

  // Optional: custom mod icons (icon.png). Missing pieces just disable icons.
  if (v.Texture2D) v.m_tex_ctor = find_method(v.Texture2D, ".ctor", {"System.Int32", "System.Int32"});
  if (v.Texture) { v.m_tex_w = find_method(v.Texture, "get_width", {}); v.m_tex_h = find_method(v.Texture, "get_height", {}); }
  if (v.ImageConversion) v.m_load_image = find_method(v.ImageConversion, "LoadImage", {"UnityEngine.Texture2D", "System.Byte[]"});
  if (v.Sprite) v.m_sprite_create = find_method(v.Sprite, "Create", {"UnityEngine.Texture2D", "UnityEngine.Rect", "UnityEngine.Vector2"});

  auto type_obj = [](void* k) { return k ? g_il.type_get_object(g_il.class_get_type(k)) : nullptr; };
  v.t_RectTransform = type_obj(u.RectTransform);
  v.t_Image = type_obj(v.Image);
  v.t_Button = u.t_Button;

  void* required[] = {v.SystemType, v.m_go_ctor, v.m_go_add_comp, v.m_set_parent, v.m_destroy, v.m_amin, v.m_amax,
                      v.m_omin, v.m_omax, v.m_set_color, v.m_tmp_align, v.t_RectTransform, v.t_Image, v.t_Button};
  for (void* r : required) if (!r) { bearite::log(BEARITE_LOG_ERROR, TAG, "UI glue incomplete, fancy list disabled"); return false; }
  v.ok = true;
  return true;
}

struct Vec2 { float x, y; };
struct Color { float r, g, b, a; };

void rect(void* tr, Vec2 amin, Vec2 amax, Vec2 omin = {0, 0}, Vec2 omax = {0, 0}) {
  call(v.m_amin, tr, {&amin});
  call(v.m_amax, tr, {&amax});
  call(v.m_omin, tr, {&omin});
  call(v.m_omax, tr, {&omax});
}

// new GameObject(name, typeof(RectTransform)) as a child of `parent_tr`.
void* new_ui(const char* name, void* parent_tr) {
  void* arr = g_il.array_new(v.SystemType, 1);
  array_items(arr)[0] = v.t_RectTransform;
  void* go = g_il.object_new(u.GameObject);
  bool ok = false;
  call(v.m_go_ctor, go, {il_str(name), arr}, &ok);
  if (!ok) return nullptr;
  void* tr = call(u.m_go_get_tr, go, {});
  bool keep_world = false;
  call(v.m_set_parent, tr, {parent_tr, &keep_world});
  return go;
}

void* add_image(void* go, Color c, bool raycast) {
  void* img = call(v.m_go_add_comp, go, {v.t_Image});
  if (!img) return nullptr;
  call(v.m_set_color, img, {&c});
  if (v.m_set_raycast) call(v.m_set_raycast, img, {&raycast});
  return img;
}

// ---- state

struct State {
  void* btn = nullptr;       // cloned "Mods" button (Button component)
  void* win_go = nullptr;    // cloned window GameObject
  void* win = nullptr;       // its UIWindow component
  void* win_tr = nullptr;    // its Transform
  void* label_tpl = nullptr; // GameObject of a text we clone for our own labels
  void* ui_root = nullptr;   // "ModsUI" container, rebuilt on every open / tab switch
  int tab = 0;               // 0 = installed, 1 = all mods
  std::vector<uint32_t> h;   // GC handles keeping our objects alive
  std::map<void*, int> actions;  // our own buttons -> action id
} g;

void pin(void* obj) { g.h.push_back(g_il.gchandle_new(obj, 0)); }

void release_state() {
  for (uint32_t h : g.h) g_il.gchandle_free(h);
  g.h.clear();
  g.actions.clear();
  g.btn = g.win_go = g.win = g.win_tr = g.label_tpl = g.ui_root = nullptr;
}

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

// A text label cloned from the window's own text, so it keeps the game's font.
void* new_label(void* parent_tr, const std::string& text, int align, float size = 0) {
  if (!g.label_tpl) return nullptr;
  void* go = call(u.m_instantiate, nullptr, {g.label_tpl, parent_tr});
  if (!go) return nullptr;
  strip_translate(go);
  set_active(go, true);
  std::vector<void*> t = components(go, u.t_TMP_Text);
  if (t.empty()) return go;
  set_text(t[0], text);
  call(v.m_tmp_align, t[0], {&align});
  if (size > 0 && v.m_tmp_size) call(v.m_tmp_size, t[0], {&size});
  return go;
}

// Tries <mod dir>/icon.png -> Sprite. Returns nullptr if anything is missing.
void* load_icon(const std::string& dir) {
  if (!v.m_tex_ctor || !v.m_load_image || !v.m_sprite_create || !v.m_tex_w || !v.SystemByte) return nullptr;
  std::string png = read_file(dir + "/icon.png");
  if (png.empty()) return nullptr;
  void* bytes = g_il.array_new(v.SystemByte, png.size());
  memcpy(array_items(bytes), png.data(), png.size());
  void* tex = g_il.object_new(v.Texture2D);
  int two = 2;
  bool ok = false;
  call(v.m_tex_ctor, tex, {&two, &two}, &ok);
  if (!ok) return nullptr;
  if (!unbox_bool(call(v.m_load_image, nullptr, {tex, bytes}))) return nullptr;
  float w = static_cast<float>(unbox_int(call(v.m_tex_w, tex, {})));
  float h = static_cast<float>(unbox_int(call(v.m_tex_h, tex, {})));
  float r[4] = {0, 0, w, h};
  float pivot[2] = {0.5f, 0.5f};
  pin(tex);
  return call(v.m_sprite_create, nullptr, {tex, r, pivot});
}

Color tile_color(const std::string& name) {
  unsigned hsh = 2166136261u;
  for (char c : name) hsh = (hsh ^ static_cast<unsigned char>(c)) * 16777619u;
  static const Color palette[] = {{0.30f, 0.69f, 0.55f, 1}, {0.90f, 0.55f, 0.20f, 1}, {0.85f, 0.35f, 0.45f, 1},
                                  {0.35f, 0.55f, 0.90f, 1}, {0.70f, 0.55f, 0.90f, 1}, {0.90f, 0.78f, 0.25f, 1}};
  return palette[hsh % 6];
}

std::string first_letter(const std::string& s) {
  if (s.empty()) return "?";
  size_t n = 1;
  unsigned char c = static_cast<unsigned char>(s[0]);
  if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
  return s.substr(0, n);
}

// One list cell: [icon] Name / version - author, all on one rounded-looking panel.
void build_cell(void* list_tr, const ModInfo& m, float top, float bottom) {
  void* cell = new_ui("ModCell", list_tr);
  if (!cell) return;
  void* cell_tr = call(u.m_go_get_tr, cell, {});
  rect(cell_tr, {0, bottom}, {1, top});
  add_image(cell, {0.24f, 0.18f, 0.55f, 0.95f}, false);

  // icon: real PNG if the mod ships one, otherwise a coloured tile with the first letter
  void* icon = new_ui("Icon", cell_tr);
  void* icon_tr = call(u.m_go_get_tr, icon, {});
  Vec2 c = {0, 0.5f};
  call(v.m_amin, icon_tr, {&c});
  call(v.m_amax, icon_tr, {&c});
  Vec2 pv = {0, 0.5f}, sz = {72, 72}, pos = {14, 0};
  call(v.m_pivot, icon_tr, {&pv});
  call(v.m_size, icon_tr, {&sz});
  if (u.m_set_apos) call(u.m_set_apos, icon_tr, {&pos});
  void* sprite = load_icon(m.dir);
  void* img = add_image(icon, sprite ? Color{1, 1, 1, 1} : tile_color(m.name), false);
  if (sprite && img && v.m_set_sprite) call(v.m_set_sprite, img, {sprite});
  if (!sprite) new_label(icon_tr, first_letter(m.name), 514, 40);
  if (!sprite) {
    // stretch the letter over the tile
    std::vector<void*> ic = components(icon, u.t_TMP_Text);
    if (!ic.empty()) rect(transform(ic[0]), {0, 0}, {1, 1});
  }

  // text: bold name, smaller second line
  std::string txt = "<b>" + m.name + "</b>";
  std::string sub = m.version.empty() ? "" : "v" + m.version;
  if (!m.author.empty()) sub += (sub.empty() ? "" : "  ·  ") + m.author;
  if (!sub.empty()) txt += "\n<size=65%>" + sub + "</size>";
  void* label = new_label(cell_tr, txt, 513, 34);
  if (label) rect(call(u.m_go_get_tr, label, {}), {0, 0}, {1, 1}, {100, 4}, {-12, -4});
}

void build_list(void* list_tr) {
  if (g.tab == 1) {
    void* l = new_label(list_tr, "Каталог модов появится в следующем обновлении", 514, 30);
    if (l) rect(call(u.m_go_get_tr, l, {}), {0, 0}, {1, 1});
    return;
  }
  std::vector<ModInfo> mods = scan_mods();
  if (mods.empty()) {
    void* l = new_label(list_tr, "Моды не найдены", 514, 30);
    if (l) rect(call(u.m_go_get_tr, l, {}), {0, 0}, {1, 1});
    return;
  }
  float rh = std::min(0.19f, 1.0f / static_cast<float>(mods.size()));
  float gap = rh * 0.08f;
  for (size_t i = 0; i < mods.size(); ++i) {
    float top = 1.0f - static_cast<float>(i) * rh;
    build_cell(list_tr, mods[i], top, top - rh + gap);
  }
}

void* build_tab(void* parent_tr, const char* text, Vec2 amin, Vec2 amax, bool active, int action) {
  void* go = new_ui(action == 1 ? "TabInstalled" : "TabAll", parent_tr);
  if (!go) return nullptr;
  void* tr = call(u.m_go_get_tr, go, {});
  rect(tr, amin, amax);
  add_image(go, active ? Color{0.36f, 0.28f, 0.75f, 1} : Color{0.27f, 0.20f, 0.60f, 1}, true);
  void* btn = call(v.m_go_add_comp, go, {v.t_Button});
  if (btn) { g.actions[btn] = action; pin(btn); }
  void* label = new_label(tr, text, 514, 34);
  if (label) rect(call(u.m_go_get_tr, label, {}), {0, 0}, {1, 1});
  return go;
}

// (Re)builds tabs + list inside the cloned window.
void rebuild_ui() {
  if (!g.win_go || !g.label_tpl || !init_ui()) return;
  if (g.ui_root) call(v.m_destroy, nullptr, {g.ui_root});
  g.actions.clear();
  g.ui_root = new_ui("ModsUI", g.win_tr);
  if (!g.ui_root) return;
  void* root_tr = call(u.m_go_get_tr, g.ui_root, {});
  rect(root_tr, {0, 0}, {1, 1});

  build_tab(root_tr, "скачанные", {0.14f, 0.82f}, {0.37f, 0.94f}, g.tab == 0, 1);
  build_tab(root_tr, "все моды", {0.40f, 0.82f}, {0.68f, 0.94f}, g.tab == 1, 2);

  void* list = new_ui("ModsList", root_tr);
  if (!list) return;
  void* list_tr = call(u.m_go_get_tr, list, {});
  rect(list_tr, {0.03f, 0.04f}, {0.97f, 0.78f});
  build_list(list_tr);
}

// Blanks the About window's own texts and remembers one of them as the label template.
void prepare_window() {
  std::vector<void*> texts = components(g.win_go, u.t_TMP_Text);
  auto len = [](void* t) { return utf8_from_il2cpp(call(u.m_tmp_get_text, t, {})).size(); };
  void* best = nullptr;
  for (void* t : texts) {
    if (in_button(t)) continue;
    if (!best || len(t) > len(best)) best = t;
  }
  if (!best && !texts.empty()) best = texts[0];
  if (best) g.label_tpl = game_object(best);
  for (void* t : texts) if (!in_button(t) && t != best) set_text(t, "");
  if (best) set_text(best, "");
  if (g.label_tpl) pin(g.label_tpl);
}

void open_window() {
  if (!g.win || !u.ok) return;
  g.tab = 0;
  rebuild_ui();
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
  pin(btn_comp);
  pin(win_go);
  pin(win);
  pin(win_tr);
  prepare_window();
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
  auto act = g.actions.find(button);
  if (act != g.actions.end()) {
    int tab = act->second == 1 ? 0 : 1;
    if (tab != g.tab) { g.tab = tab; rebuild_ui(); }
    return true;
  }
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
