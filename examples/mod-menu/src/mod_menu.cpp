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
//   3. Mods that declare "settings" in their mod.json get a "Настроить" button;
//      the values are stored in <mod dir>/settings.json (see settings.inc).
//   4. IL2CPP calls (Instantiate, SetActive, set_text, ...) go through
//      il2cpp_runtime_invoke, resolved from libil2cpp.so with dlsym.
#include "bearite.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char* TAG = "mod-menu";

// Field offsets from dump.cs (PauseMenuManager : SbaBehaviour).
constexpr size_t OFF_ABOUT_WINDOW = 0x28;
constexpr size_t OFF_ACHIEVEMENTS_BUTTON = 0x98;
constexpr size_t OFF_CLOSE_ABOUT_BUTTON = 0xA0;


// The code is split into several files (each one is included right here, in
// order) so that every file stays small.
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

   // dlsym'ed il2cpp API + Unity helpers
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

     // finds installed mods and reads mod.json
// ---- extra Unity glue for building the list UI from code

void* find_method_n(void* klass, const char* name, uint32_t argc) {
  if (!klass) return nullptr;
  void* iter = nullptr;
  while (void* m = g_il.class_get_methods(klass, &iter))
    if (strcmp(g_il.method_get_name(m), name) == 0 && g_il.method_get_param_count(m) == argc) return m;
  return nullptr;
}

struct V {
  void *Image, *Graphic, *Sprite, *Texture, *Texture2D, *ImageConversion, *SystemType, *SystemByte;
  void *m_go_ctor, *m_go_add_comp, *m_set_parent, *m_destroy;
  void *m_amin, *m_amax, *m_omin, *m_omax, *m_pivot, *m_size;
  void *m_set_color, *m_set_raycast, *m_set_sprite;
  void *m_tmp_align, *m_tmp_size;
  void *m_tex_ctor, *m_tex_w, *m_tex_h, *m_load_image, *m_sprite_create;
  void *m_tex_ctor4, *m_tex_raw, *m_tex_apply, *m_sprite_create7, *m_image_type;
  void *t_RectTransform, *t_Image, *t_Button;
  void *Mask, *t_Mask, *m_mask_show, *m_obj_name;
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

  // Optional: procedural rounded / gear sprites (shapes.inc)
  if (v.Texture2D) {
    v.m_tex_ctor4 = find_method(v.Texture2D, ".ctor", {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"});
    v.m_tex_raw = find_method(v.Texture2D, "LoadRawTextureData", {"System.Byte[]"});
    v.m_tex_apply = find_method(v.Texture2D, "Apply", {"System.Boolean", "System.Boolean"});
  }
  if (v.Sprite) v.m_sprite_create7 = find_method_n(v.Sprite, "Create", 7);
  v.m_image_type = find_method_n(v.Image, "set_type", 1);

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

void* round_sprite();
void* gear_sprite();

// Adds an Image. Rounded corners by default (sliced rounded-rectangle sprite).
void* add_image(void* go, Color c, bool raycast, bool rounded = true) {
  void* img = call(v.m_go_add_comp, go, {v.t_Image});
  if (!img) return nullptr;
  call(v.m_set_color, img, {&c});
  if (v.m_set_raycast) call(v.m_set_raycast, img, {&raycast});
  if (rounded && v.m_set_sprite && v.m_image_type) {
    if (void* sp = round_sprite()) {
      call(v.m_set_sprite, img, {sp});
      int sliced = 1;
      call(v.m_image_type, img, {&sliced});
    }
  }
  return img;
}

// ---- actions attached to our own buttons

enum ActionKind { ACT_NONE = 0, ACT_TAB_INSTALLED, ACT_TAB_ALL, ACT_SETTINGS, ACT_CLOSE, ACT_TOGGLE, ACT_DEC, ACT_INC, ACT_CYCLE };
struct Action {
  int kind = ACT_NONE;
  int arg = 0;
};

// ---- state

struct State {
  void* btn = nullptr;       // cloned "Mods" button (Button component)
  void* win_go = nullptr;    // cloned window GameObject
  void* win = nullptr;       // its UIWindow component
  void* win_tr = nullptr;    // its Transform
  void* label_tpl = nullptr; // GameObject of a text we clone for our own labels
  void* ui_root = nullptr;   // "ModsUI" container, rebuilt on every open / tab switch
  int tab = 0;               // 0 = installed, 1 = all mods
  std::vector<ModInfo> mods; // snapshot shown in the list
  int cfg_mod = -1;          // index into `mods` of the cell whose settings are expanded
  std::vector<uint32_t> h;   // GC handles keeping our objects alive
  std::map<void*, Action> actions;  // our own buttons -> what they do
} g;

void pin(void* obj) { g.h.push_back(g_il.gchandle_new(obj, 0)); }

void release_state() {
  for (uint32_t h : g.h) g_il.gchandle_free(h);
  g.h.clear();
  g.actions.clear();
  g.btn = g.win_go = g.win = g.win_tr = g.label_tpl = g.ui_root = nullptr;
  g.mods.clear();
  g.cfg_mod = -1;
  g.tab = 0;
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

// A clickable panel with a centered text. Clicks are routed by hk_click via `act`.
void* make_button(void* parent_tr, const char* name, const std::string& text, Vec2 amin, Vec2 amax,
                  Color bg, Action act, float size = 30) {
  void* go = new_ui(name, parent_tr);
  if (!go) return nullptr;
  void* tr = call(u.m_go_get_tr, go, {});
  rect(tr, amin, amax);
  add_image(go, bg, true);
  void* btn = call(v.m_go_add_comp, go, {v.t_Button});
  if (btn) { g.actions[btn] = act; pin(btn); }
  void* label = new_label(tr, text, 514, size);
  if (label) rect(call(u.m_go_get_tr, label, {}), {0, 0}, {1, 1});
  return go;
}

// Plain centered text stretched over `parent_tr`.
void* fill_label(void* parent_tr, const std::string& text, int align, float size) {
  void* l = new_label(parent_tr, text, align, size);
  if (l) rect(call(u.m_go_get_tr, l, {}), {0, 0}, {1, 1});
  return l;
}
       // state, UI building blocks (buttons, labels, ...)
// shapes.inc — sprites drawn in code, so the menu needs no image files:
// a 9-sliced rounded rectangle (rounded corners for every panel / button) and a gear.

std::vector<uint32_t> g_keep;  // GC handles that live as long as the game (textures / sprites)

void* sprite_from_rgba(const std::vector<unsigned char>& px, int w, int h, float border) {
  if (!v.Texture2D || !v.m_tex_ctor4 || !v.m_tex_raw || !v.m_tex_apply || !v.m_sprite_create7 || !v.SystemByte) return nullptr;
  void* tex = g_il.object_new(v.Texture2D);
  int iw = w, ih = h, fmt = 4;  // TextureFormat.RGBA32
  bool mip = false, ok = false;
  call(v.m_tex_ctor4, tex, {&iw, &ih, &fmt, &mip}, &ok);
  if (!ok) return nullptr;
  void* bytes = g_il.array_new(v.SystemByte, px.size());
  memcpy(array_items(bytes), px.data(), px.size());
  call(v.m_tex_raw, tex, {bytes}, &ok);
  if (!ok) return nullptr;
  bool no = false;
  call(v.m_tex_apply, tex, {&no, &no});
  g_keep.push_back(g_il.gchandle_new(tex, 0));

  float r[4] = {0, 0, static_cast<float>(w), static_cast<float>(h)};
  float pivot[2] = {0.5f, 0.5f};
  float ppu = 100.0f;
  uint32_t extrude = 0;
  int mesh = 0;  // SpriteMeshType.FullRect
  float b[4] = {border, border, border, border};
  void* sprite = call(v.m_sprite_create7, nullptr, {tex, r, pivot, &ppu, &extrude, &mesh, b}, &ok);
  if (sprite) g_keep.push_back(g_il.gchandle_new(sprite, 0));
  return sprite;
}

// White rounded rectangle, 64x64, corner radius 22 px, used as a sliced Image.
void* round_sprite() {
  static void* s = nullptr;
  static bool tried = false;
  if (tried) return s;
  tried = true;
  const int N = 64;
  const float R = 22.0f;
  std::vector<unsigned char> px(N * N * 4, 255);
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      float dx = std::max(std::fabs(x + 0.5f - N / 2.0f) - (N / 2.0f - R), 0.0f);
      float dy = std::max(std::fabs(y + 0.5f - N / 2.0f) - (N / 2.0f - R), 0.0f);
      float d = std::sqrt(dx * dx + dy * dy) - R;
      float a = std::min(std::max(0.5f - d, 0.0f), 1.0f);
      px[(y * N + x) * 4 + 3] = static_cast<unsigned char>(a * 255.0f);
    }
  s = sprite_from_rgba(px, N, N, R);
  if (!s) bearite::log(BEARITE_LOG_WARN, TAG, "rounded sprite unavailable, using square panels");
  return s;
}

// White gear (8 teeth, hole in the middle), 128x128.
void* gear_sprite() {
  static void* s = nullptr;
  static bool tried = false;
  if (tried) return s;
  tried = true;
  const int N = 128;
  const float C = N / 2.0f;
  const float PI2 = 6.2831853f;
  std::vector<unsigned char> px(N * N * 4, 255);
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      float fx = x + 0.5f - C, fy = y + 0.5f - C;
      float r = std::sqrt(fx * fx + fy * fy);
      float th = std::atan2(fy, fx);
      float t = std::fmod(th / PI2 * 8.0f + 8.0f, 1.0f);
      float tooth = (t < 0.5f) ? 1.0f : 0.0f;               // square teeth
      float edge = 41.0f + 15.0f * tooth;                    // body radius 41, teeth up to 56
      float a = std::min(std::max(0.5f - (r - edge), 0.0f), 1.0f);
      a *= std::min(std::max(r - 17.0f + 0.5f, 0.0f), 1.0f);  // center hole
      px[(y * N + x) * 4 + 3] = static_cast<unsigned char>(a * 255.0f);
    }
  s = sprite_from_rgba(px, N, N, 0);
  if (!s) bearite::log(BEARITE_LOG_WARN, TAG, "gear sprite unavailable");
  return s;
}
        // procedurally drawn sprites (rounded rectangle, gear)
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
      // per-mod settings: mod.json schema, settings.json, settings screen
// One list cell: header (icon, name, version / author, gear button) and, when
// the gear is toggled, the mod's settings right below inside the same cell.
constexpr float SETTING_ROW_UNITS = 0.62f;  // height of a setting row relative to a header

void build_cell(void* list_tr, const ModInfo& m, int index, float top, float bottom,
                const std::vector<SettingDef>* defs, const J* values) {
  void* cell = new_ui("ModCell", list_tr);
  if (!cell) return;
  void* cell_tr = call(u.m_go_get_tr, cell, {});
  rect(cell_tr, {0, bottom}, {1, top});
  add_image(cell, {0.24f, 0.18f, 0.55f, 0.95f}, false);

  size_t n = defs ? defs->size() : 0;
  float hfrac = 1.0f / (1.0f + SETTING_ROW_UNITS * static_cast<float>(n));  // header share of the cell
  float rfrac = SETTING_ROW_UNITS * hfrac;                                   // one setting row's share

  void* hdr = new_ui("Header", cell_tr);
  if (!hdr) return;
  void* hdr_tr = call(u.m_go_get_tr, hdr, {});
  rect(hdr_tr, {0, 1.0f - hfrac}, {1, 1});

  // icon: real PNG if the mod ships one, otherwise a coloured tile with the first letter
  void* icon = new_ui("Icon", hdr_tr);
  void* icon_tr = call(u.m_go_get_tr, icon, {});
  Vec2 c = {0, 0.5f};
  call(v.m_amin, icon_tr, {&c});
  call(v.m_amax, icon_tr, {&c});
  Vec2 pv = {0, 0.5f}, sz = {84, 84}, pos = {16, 0};
  call(v.m_pivot, icon_tr, {&pv});
  call(v.m_size, icon_tr, {&sz});
  if (u.m_set_apos) call(u.m_set_apos, icon_tr, {&pos});
  void* sprite = load_icon(m.dir);
  void* img = add_image(icon, sprite ? Color{1, 1, 1, 1} : tile_color(m.name), false, sprite == nullptr);
  if (sprite && img && v.m_set_sprite) call(v.m_set_sprite, img, {sprite});
  if (!sprite) {
    new_label(icon_tr, first_letter(m.name), 514, 46);
    std::vector<void*> ic = components(icon, u.t_TMP_Text);
    if (!ic.empty()) rect(transform(ic[0]), {0, 0}, {1, 1});
  }

  // text: bold name, smaller second line
  std::string txt = "<b>" + m.name + "</b>";
  std::string sub = m.version.empty() ? "" : "v" + m.version;
  if (!m.author.empty()) sub += (sub.empty() ? "" : "  ·  ") + m.author;
  if (!sub.empty()) txt += "\n<size=65%>" + sub + "</size>";
  void* label = new_label(hdr_tr, txt, 513, 40);
  float right = m.has_settings ? 0.86f : 1.0f;
  if (label) rect(call(u.m_go_get_tr, label, {}), {0, 0}, {right, 1}, {118, 4}, {m.has_settings ? 0.0f : -12.0f, -4});

  // gear button (only for mods that declare "settings" in their mod.json)
  if (m.has_settings) {
    bool open = defs != nullptr;
    void* gear_btn = make_button(hdr_tr, "Gear", "", {0.885f, 0.18f}, {0.975f, 0.82f},
                                 open ? Color{0.25f, 0.65f, 0.40f, 1} : Color{0.36f, 0.28f, 0.75f, 1}, {ACT_SETTINGS, index});
    if (gear_btn) {
      void* gtr = call(u.m_go_get_tr, gear_btn, {});
      void* gi = new_ui("GearIcon", gtr);
      if (gi) {
        rect(call(u.m_go_get_tr, gi, {}), {0.16f, 0.16f}, {0.84f, 0.84f});
        void* gimg = add_image(gi, {1, 1, 1, 1}, false, false);
        if (void* gs = gear_sprite()) { if (gimg && v.m_set_sprite) call(v.m_set_sprite, gimg, {gs}); }
      }
    }
  }

  // settings inside the same cell
  if (defs && values) {
    float y = 1.0f - hfrac;
    for (size_t j = 0; j < defs->size(); ++j) {
      float y1 = y - rfrac * 0.06f;
      float y0 = y - rfrac * 0.96f;
      build_setting_row(cell_tr, (*defs)[j], current_value(*values, (*defs)[j]), static_cast<int>(j), y0, y1);
      y -= rfrac;
    }
  }
}

void build_list(void* list_tr) {
  if (g.tab == 1) { fill_label(list_tr, "Каталог модов появится в следующем обновлении", 514, 30); return; }
  g.mods = scan_mods();
  if (g.mods.empty()) { fill_label(list_tr, "Моды не найдены", 514, 30); return; }
  if (g.cfg_mod >= static_cast<int>(g.mods.size())) g.cfg_mod = -1;

  // settings of the expanded mod (the list may have changed since it was opened)
  std::vector<SettingDef> defs;
  J values;
  if (g.cfg_mod >= 0) {
    const ModInfo& e = g.mods[static_cast<size_t>(g.cfg_mod)];
    defs = parse_defs(e.json);
    values = read_settings(e.dir);
    if (defs.empty()) g.cfg_mod = -1;
  }

  // heights in "header units": a collapsed cell is 1, an expanded one is 1 + rows
  float total = 0;
  std::vector<float> h(g.mods.size(), 1.0f);
  if (g.cfg_mod >= 0) h[static_cast<size_t>(g.cfg_mod)] += SETTING_ROW_UNITS * static_cast<float>(defs.size());
  for (float x : h) total += x;
  float unit = std::min(0.24f, 1.0f / total);
  float gap = unit * 0.07f;

  float y = 1.0f;
  for (size_t i = 0; i < g.mods.size(); ++i) {
    float top = y;
    float bottom = y - h[i] * unit + gap;
    bool open = static_cast<int>(i) == g.cfg_mod;
    build_cell(list_tr, g.mods[i], static_cast<int>(i), top, bottom, open ? &defs : nullptr, open ? &values : nullptr);
    y -= h[i] * unit;
  }
}

// (Re)builds the panel (background, close button, tabs, list) inside the cloned window.
void rebuild_ui() {
  if (!g.win_go || !g.label_tpl || !init_ui()) return;
  if (g.ui_root) call(v.m_destroy, nullptr, {g.ui_root});
  g.actions.clear();
  g.ui_root = new_ui("ModsUI", g.win_tr);
  if (!g.ui_root) return;
  void* root_tr = call(u.m_go_get_tr, g.ui_root, {});
  // Slightly inset, opaque rounded panel: it covers everything the About window
  // draws (logo, its buttons) and swallows clicks meant for them.
  rect(root_tr, {0.008f, 0.012f}, {0.992f, 0.988f});
  add_image(g.ui_root, {0.40f, 0.34f, 0.78f, 0.98f}, true);

  make_button(root_tr, "Close", "X", {0.025f, 0.80f}, {0.115f, 0.97f}, {0.86f, 0.22f, 0.22f, 1}, {ACT_CLOSE, 0}, 40);
  make_button(root_tr, "TabInstalled", "Установлено", {0.14f, 0.82f}, {0.44f, 0.95f},
              g.tab == 0 ? Color{0.36f, 0.28f, 0.75f, 1} : Color{0.27f, 0.20f, 0.60f, 1}, {ACT_TAB_INSTALLED, 0}, 34);
  make_button(root_tr, "TabAll", "Все моды", {0.47f, 0.82f}, {0.77f, 0.95f},
              g.tab == 1 ? Color{0.36f, 0.28f, 0.75f, 1} : Color{0.27f, 0.20f, 0.60f, 1}, {ACT_TAB_ALL, 0}, 34);

  void* list = new_ui("ModsList", root_tr);
  if (!list) return;
  void* list_tr = call(u.m_go_get_tr, list, {});
  rect(list_tr, {0.03f, 0.04f}, {0.97f, 0.78f});
  build_list(list_tr);
}

// Blanks the About window's own texts and remembers one of them as the label
// template (its font / material are what our labels are cloned from).
void prepare_window() {
  if (!init_ui()) return;
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
  g.cfg_mod = -1;
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

// What our own buttons do.
void on_action(const Action& a) {
  switch (a.kind) {
    case ACT_TAB_INSTALLED: g.tab = 0; rebuild_ui(); break;
    case ACT_TAB_ALL: g.tab = 1; rebuild_ui(); break;
    case ACT_CLOSE: close_window(); break;
    case ACT_SETTINGS:  // gear: expand / collapse this mod's settings inside its cell
      g.cfg_mod = (g.cfg_mod == a.arg) ? -1 : a.arg;
      rebuild_ui();
      break;
    default:
      if (change_setting(a)) rebuild_ui();
  }
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
      // window contents, building the menu, actions
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
    Action a = act->second;  // copy: rebuilding clears the map
    on_action(a);
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
         // game hooks

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
  bearite::log(BEARITE_LOG_INFO, TAG, "BUILD 3 loaded, hooks: Start=%d OnPointerClick=%d OnSubmit=%d", a, b, c);
  return (a && b && c) ? 0 : 1;
}

BEARITE_EXPORT void bearite_on_unload(void) { g.btn = nullptr; }

}  // extern "C"
