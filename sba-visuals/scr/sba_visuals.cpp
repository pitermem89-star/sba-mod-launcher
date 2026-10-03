// SBA Visuals: screen effects for Super Bear Adventure, as a normal Bearite mod.
// Everything is drawn with plain Unity UI on its own overlay canvas, so it does
// not depend on any game class:
//   - colour tint  (preset + strength)
//   - vignette     (soft dark/coloured edges)
//   - glow particles that float upwards and twinkle
// Settings come from <mod dir>/settings.json (edited in the Mods window) and are
// re-read once per second, so changes apply live.
// A per-frame tick is obtained by hooking EventSystem.Update.
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
#include <type_traits>
#include <utility>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char* TAG = "sba-visuals";



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
  v.Mask = find_class(ASM_UI, "UnityEngine.UI", "Mask");
  v.t_Mask = type_obj(v.Mask);
  v.m_mask_show = find_method_n(v.Mask, "set_showMaskGraphic", 1);
  v.m_obj_name = find_method_n(u.Object, "get_name", 0);

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


// Unity "fake null": a destroyed UnityEngine.Object is still a non-null pointer
// in native code, so ask Unity itself (Object.op_Implicit).
bool alive(void* obj) {
  if (!obj) return false;
  static void* m = nullptr;
  static bool tried = false;
  if (!tried) { tried = true; m = find_method(u.Object, "op_Implicit", {"UnityEngine.Object"}); }
  if (!m) return true;  // cannot check: assume alive
  bool ok = false;
  void* r = call(m, nullptr, {obj}, &ok);
  return ok ? unbox_bool(r) : false;
}


std::vector<uint32_t> g_keep;  // GC handles that live as long as the game (textures / sprites)

// Textures / sprites made in code have no scene owner, so Unity deletes them in
// Resources.UnloadUnusedAssets, which runs on every scene load (entering a level).
// A GC handle does not count as a reference for Unity. Mark them as permanent.
void keep_asset(void* obj) {
  if (!obj || !u.ok) return;
  static void* m_flags = find_method(u.Object, "set_hideFlags", {"UnityEngine.HideFlags"});
  static void* m_ddol = find_method(u.Object, "DontDestroyOnLoad", {"UnityEngine.Object"});
  int dont_unload = 32;  // HideFlags.DontUnloadUnusedAsset
  call(m_flags, obj, {&dont_unload});
  call(m_ddol, nullptr, {obj});
}

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
  keep_asset(tex);
  g_keep.push_back(g_il.gchandle_new(tex, 0));

  float r[4] = {0, 0, static_cast<float>(w), static_cast<float>(h)};
  float pivot[2] = {0.5f, 0.5f};
  float ppu = 100.0f;
  uint32_t extrude = 0;
  int mesh = 0;  // SpriteMeshType.FullRect
  float b[4] = {border, border, border, border};
  void* sprite = call(v.m_sprite_create7, nullptr, {tex, r, pivot, &ppu, &extrude, &mesh, b}, &ok);
  if (sprite) { keep_asset(sprite); g_keep.push_back(g_il.gchandle_new(sprite, 0)); }
  return sprite;
}


// ============================================================== SBA Visuals ==

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

std::string own_dir() {
  const BeariteApi* a = bearite::api();
  std::string d = (a && a->mod_dir) ? a->mod_dir : "";
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  return d;
}

// Raw value of "key": ... in a flat JSON object (strings are unquoted).
std::string raw_value(const std::string& j, const char* key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = j.find(k);
  if (p == std::string::npos) return "";
  p = j.find(':', p + k.size());
  if (p == std::string::npos) return "";
  ++p;
  while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) ++p;
  std::string out;
  if (p < j.size() && j[p] == '"') {
    for (++p; p < j.size() && j[p] != '"'; ++p) {
      if (j[p] == '\\' && p + 1 < j.size()) ++p;
      out += j[p];
    }
  } else {
    while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != '\n' && j[p] != '\r' && j[p] != ' ') out += j[p++];
  }
  return out;
}

struct Cfg {
  bool enabled = true;
  std::string tint = "warm";
  float tint_strength = 0.15f;
  float vignette = 0.4f;
  bool particles = true;
  int count = 25;
  std::string pcolor = "gold";
  bool operator==(const Cfg& o) const {
    return enabled == o.enabled && tint == o.tint && tint_strength == o.tint_strength && vignette == o.vignette &&
           particles == o.particles && count == o.count && pcolor == o.pcolor;
  }
};

float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

Cfg load_cfg() {
  Cfg c;
  std::string j = read_file(own_dir() + "/settings.json");
  if (j.empty()) return c;
  std::string r;
  if (!(r = raw_value(j, "enabled")).empty()) c.enabled = (r == "true");
  if (!(r = raw_value(j, "tint")).empty()) c.tint = r;
  if (!(r = raw_value(j, "tint_strength")).empty()) c.tint_strength = clampf(static_cast<float>(atof(r.c_str())), 0.0f, 0.8f);
  if (!(r = raw_value(j, "vignette")).empty()) c.vignette = clampf(static_cast<float>(atof(r.c_str())), 0.0f, 1.0f);
  if (!(r = raw_value(j, "particles")).empty()) c.particles = (r == "true");
  if (!(r = raw_value(j, "particle_count")).empty()) c.count = static_cast<int>(clampf(static_cast<float>(atof(r.c_str())), 0.0f, 80.0f));
  if (!(r = raw_value(j, "particle_color")).empty()) c.pcolor = r;
  return c;
}

Color tint_rgb(const std::string& name) {
  if (name == "warm") return {1.00f, 0.60f, 0.25f, 1};
  if (name == "cool") return {0.30f, 0.60f, 1.00f, 1};
  if (name == "sunset") return {1.00f, 0.35f, 0.50f, 1};
  if (name == "night") return {0.10f, 0.15f, 0.55f, 1};
  if (name == "mint") return {0.30f, 1.00f, 0.70f, 1};
  return {0, 0, 0, 0};  // "off"
}

Color hsv(float h, float s, float val) {
  h = h - std::floor(h);
  float r = 0, g = 0, b = 0;
  float f = h * 6.0f;
  int i = static_cast<int>(f);
  float ff = f - static_cast<float>(i), p = val * (1 - s), q = val * (1 - s * ff), t = val * (1 - s * (1 - ff));
  switch (i % 6) {
    case 0: r = val; g = t; b = p; break;
    case 1: r = q; g = val; b = p; break;
    case 2: r = p; g = val; b = t; break;
    case 3: r = p; g = q; b = val; break;
    case 4: r = t; g = p; b = val; break;
    default: r = val; g = p; b = q; break;
  }
  return {r, g, b, 1};
}

Color particle_rgb(const std::string& name, int idx, int n) {
  if (name == "white") return {1, 1, 1, 1};
  if (name == "pink") return {1.00f, 0.50f, 0.80f, 1};
  if (name == "cyan") return {0.40f, 0.95f, 1.00f, 1};
  if (name == "rainbow") return hsv(static_cast<float>(idx) / static_cast<float>(n > 0 ? n : 1), 0.55f, 1.0f);
  return {1.00f, 0.85f, 0.30f, 1};  // gold
}

float frand() {
  static uint32_t s = 2463534242u;
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return static_cast<float>(s & 0xFFFFFF) / 16777216.0f;
}

// ---- sprites (drawn in code)

void* glow_sprite() {
  static void* s = nullptr;
  if (s && alive(s)) return s;
  const int N = 64;
  std::vector<unsigned char> px(N * N * 4, 255);
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      float fx = x + 0.5f - N / 2.0f, fy = y + 0.5f - N / 2.0f;
      float d = clampf(std::sqrt(fx * fx + fy * fy) / (N / 2.0f), 0.0f, 1.0f);
      float a = (1.0f - d) * (1.0f - d);
      px[(y * N + x) * 4 + 3] = static_cast<unsigned char>(a * 255.0f);
    }
  s = sprite_from_rgba(px, N, N, 0);
  if (!s) bearite::log(BEARITE_LOG_WARN, TAG, "glow sprite unavailable");
  return s;
}

void* vignette_sprite() {
  static void* s = nullptr;
  if (s && alive(s)) return s;
  const int N = 128;
  std::vector<unsigned char> px(N * N * 4, 255);
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      float fx = (x + 0.5f - N / 2.0f) / (N / 2.0f), fy = (y + 0.5f - N / 2.0f) / (N / 2.0f);
      float d = std::sqrt(fx * fx + fy * fy) / 1.4142f;
      float a = clampf((d - 0.45f) / 0.55f, 0.0f, 1.0f);
      a *= a;
      px[(y * N + x) * 4 + 3] = static_cast<unsigned char>(a * 255.0f);
    }
  s = sprite_from_rgba(px, N, N, 0);
  if (!s) bearite::log(BEARITE_LOG_WARN, TAG, "vignette sprite unavailable");
  return s;
}

// ---- overlay

struct Particle {
  void *tr, *img;
  float x, y, vy, phase;
  Color col;
};

struct Fx {
  Cfg cfg;
  bool have_cfg = false;
  void* root = nullptr;
  std::vector<Particle> ps;
  std::vector<uint32_t> pins;
  float time = 0, cfg_timer = 0;
  int last_frame = -1;
  int fails = 0;
  // glue
  void *m_dt = nullptr, *m_frame = nullptr, *m_screen_h = nullptr, *m_ddol = nullptr;
  void *m_set_rm = nullptr, *m_set_so = nullptr, *t_canvas = nullptr;
  bool ok = false;
} fx;

void pin(void* obj) { if (obj) fx.pins.push_back(g_il.gchandle_new(obj, 0)); }

void drop_overlay() {
  if (fx.root && alive(fx.root)) call(u.m_destroy_imm, nullptr, {fx.root});
  fx.root = nullptr;
  fx.ps.clear();
  for (uint32_t h : fx.pins) g_il.gchandle_free(h);
  fx.pins.clear();
}

bool init_fx() {
  if (fx.ok) return true;
  if (!init_ui()) return false;
  void* Time = find_class(ASM_CORE, "UnityEngine", "Time");
  void* Screen = find_class(ASM_CORE, "UnityEngine", "Screen");
  void* Canvas = find_class("UnityEngine.UIModule", "UnityEngine", "Canvas");
  fx.m_dt = find_method(Time, "get_deltaTime", {});
  fx.m_frame = find_method(Time, "get_frameCount", {});
  fx.m_screen_h = find_method(Screen, "get_height", {});
  fx.m_ddol = find_method(u.Object, "DontDestroyOnLoad", {"UnityEngine.Object"});
  fx.m_set_rm = find_method(Canvas, "set_renderMode", {"UnityEngine.RenderMode"});
  fx.m_set_so = find_method(Canvas, "set_sortingOrder", {"System.Int32"});
  fx.t_canvas = Canvas ? g_il.type_get_object(g_il.class_get_type(Canvas)) : nullptr;
  if (!fx.m_dt || !fx.m_frame || !fx.m_set_rm || !fx.t_canvas || !v.m_set_sprite) {
    bearite::log(BEARITE_LOG_ERROR, TAG, "glue incomplete, effects disabled");
    return false;
  }
  fx.ok = true;
  return true;
}

void* add_image(void* go, Color c, bool raycast, bool /*rounded*/) {
  void* img = call(v.m_go_add_comp, go, {v.t_Image});
  if (!img) return nullptr;
  call(v.m_set_color, img, {&c});
  if (v.m_set_raycast) call(v.m_set_raycast, img, {&raycast});
  return img;
}

void* make_fill(const char* name, void* parent_tr, Color c, void* sprite) {
  void* go = new_ui(name, parent_tr);
  if (!go) return nullptr;
  void* tr = call(u.m_go_get_tr, go, {});
  rect(tr, {0, 0}, {1, 1});
  void* img = add_image(go, c, false, false);
  if (img && sprite) call(v.m_set_sprite, img, {sprite});
  pin(go);
  pin(img);
  return img;
}

bool build_overlay() {
  void* root = new_ui("SBAVisuals", nullptr);
  if (!root) return false;
  void* canvas = call(v.m_go_add_comp, root, {fx.t_canvas});
  if (!canvas) { call(u.m_destroy_imm, nullptr, {root}); return false; }
  int overlay = 0;  // RenderMode.ScreenSpaceOverlay
  call(fx.m_set_rm, canvas, {&overlay});
  int order = -100;  // below the game's own canvases when they are overlays too
  if (fx.m_set_so) call(fx.m_set_so, canvas, {&order});
  if (fx.m_ddol) call(fx.m_ddol, nullptr, {root});  // survive scene loads
  pin(root);
  fx.root = root;
  void* rtr = call(u.m_go_get_tr, root, {});

  const Cfg& c = fx.cfg;
  Color t = tint_rgb(c.tint);
  t.a *= c.tint_strength;
  if (t.a > 0.001f) make_fill("Tint", rtr, t, nullptr);
  if (c.vignette > 0.001f) make_fill("Vignette", rtr, {0.0f, 0.0f, 0.0f, c.vignette}, vignette_sprite());

  if (c.particles && c.count > 0) {
    float sh = 720.0f;
    if (fx.m_screen_h) {
      float h = static_cast<float>(unbox_int(call(fx.m_screen_h, nullptr, {})));
      if (h > 100.0f) sh = h;
    }
    void* glow = glow_sprite();
    for (int i = 0; i < c.count; ++i) {
      void* go = new_ui("Glow", rtr);
      if (!go) continue;
      void* tr = call(u.m_go_get_tr, go, {});
      Particle p;
      p.x = frand();
      p.y = frand();
      p.vy = 0.02f + 0.05f * frand();
      p.phase = frand() * 6.2831853f;
      p.col = particle_rgb(c.pcolor, i, c.count);
      float half = sh * (0.015f + 0.03f * frand());
      Vec2 a{p.x, p.y};
      rect(tr, a, a, {-half, -half}, {half, half});
      void* img = add_image(go, p.col, false, false);
      if (img && glow) call(v.m_set_sprite, img, {glow});
      p.tr = tr;
      p.img = img;
      pin(go);
      pin(tr);
      pin(img);
      if (img) fx.ps.push_back(p);
    }
  }
  bearite::log(BEARITE_LOG_INFO, TAG, "overlay built: tint=%s vignette=%.2f particles=%zu", c.tint.c_str(), c.vignette, fx.ps.size());
  return true;
}

void animate(float dt) {
  float t = fx.time;
  for (Particle& p : fx.ps) {
    p.y += p.vy * dt;
    p.x += std::sin(t * 0.7f + p.phase) * 0.02f * dt;
    if (p.y > 1.05f) { p.y = -0.05f; p.x = frand(); }
    float edge = clampf(std::min(p.y * 8.0f, (1.0f - p.y) * 8.0f), 0.0f, 1.0f);
    float twinkle = 0.5f + 0.5f * std::sin(t * 1.8f + p.phase);
    Color c = p.col;
    c.a = (0.2f + 0.6f * twinkle) * edge;
    Vec2 a{p.x, p.y};
    call(v.m_amin, p.tr, {&a});
    call(v.m_amax, p.tr, {&a});
    call(v.m_set_color, p.img, {&c});
  }
}

void tick() {
  if (!init_fx()) return;
  void* fr = call(fx.m_frame, nullptr, {});
  int frame = unbox_int(fr);
  if (frame == fx.last_frame) return;  // several EventSystems / hooks: once per frame
  fx.last_frame = frame;
  float dt = 0.016f;
  if (void* d = call(fx.m_dt, nullptr, {})) dt = clampf(*static_cast<float*>(g_il.object_unbox(d)), 0.0f, 0.1f);
  fx.time += dt;
  fx.cfg_timer -= dt;

  if (fx.cfg_timer <= 0.0f || !fx.have_cfg) {
    fx.cfg_timer = 1.0f;
    Cfg c = load_cfg();
    if (!fx.have_cfg || !(c == fx.cfg)) {
      fx.cfg = c;
      fx.have_cfg = true;
      fx.fails = 0;
      drop_overlay();  // rebuilt below with the new settings
    }
  }
  if (!fx.cfg.enabled) { if (fx.root) drop_overlay(); return; }
  if (fx.root && !alive(fx.root)) drop_overlay();  // destroyed by Unity
  if (!fx.root) {
    if (fx.fails >= 3) return;
    if (!build_overlay()) {
      ++fx.fails;
      bearite::log(BEARITE_LOG_ERROR, TAG, "building the overlay failed (%d/3)", fx.fails);
      drop_overlay();
      return;
    }
  }
  animate(dt);
}

// ----------------------------------------------------------------- hooks ----

void (*orig_es_update)(void*, void*) = nullptr;
void hk_es_update(void* self, void* mi) {
  orig_es_update(self, mi);
  tick();
}

void (*orig_sim_process)(void*, void*) = nullptr;
void hk_sim_process(void* self, void* mi) {
  orig_sim_process(self, mi);
  tick();
}

}  // namespace

extern "C" {

BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  bool a = bearite::hook(ASM_UI, "UnityEngine.EventSystems", "EventSystem", "Update", 0,
                         reinterpret_cast<void*>(&hk_es_update), reinterpret_cast<void**>(&orig_es_update));
  bool b = false;
  if (!a)  // fallback tick source
    b = bearite::hook(ASM_UI, "UnityEngine.EventSystems", "StandaloneInputModule", "Process", 0,
                      reinterpret_cast<void*>(&hk_sim_process), reinterpret_cast<void**>(&orig_sim_process));
  bearite::log(BEARITE_LOG_INFO, TAG, "SBA-VISUALS 1 loaded, hooks: EventSystem.Update=%d StandaloneInputModule.Process=%d", a, b);
  return (a || b) ? 0 : 1;
}

BEARITE_EXPORT void bearite_on_unload(void) { fx.root = nullptr; }

}  // extern "C"
