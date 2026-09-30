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
