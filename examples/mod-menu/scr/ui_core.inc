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
