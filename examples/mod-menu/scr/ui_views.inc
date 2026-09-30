// One list cell: header (icon, name, version / author, gear button) and, when
// the gear is toggled, the mod's settings right below inside the same cell.
constexpr float SETTING_ROW_UNITS = 0.62f;  // height of a setting row relative to a header

void build_cell(void* list_tr, const ModInfo& m, int index, float top, float bottom,
                const std::vector<SettingDef>* defs, const J* values) {
  void* cell = new_ui("ModCell", list_tr);
  if (!cell) return;
  void* cell_tr = call(u.m_go_get_tr, cell, {});
  rect(cell_tr, {0, bottom}, {1, top});
  add_image(cell, {0.24f, 0.18f, 0.55f, 0.95f}, true);  // rounded cell

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
  void* img = add_image(icon, sprite ? Color{1, 1, 1, 1} : tile_color(m.name), true, sprite == nullptr);  // rounded icon
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
  make_button(root_tr, "TabInstalled", "скачанные", {0.14f, 0.82f}, {0.44f, 0.95f},
              g.tab == 0 ? Color{0.36f, 0.28f, 0.75f, 1} : Color{0.27f, 0.20f, 0.60f, 1}, {ACT_TAB_INSTALLED, 0}, 34);
  make_button(root_tr, "TabAll", "все моды", {0.47f, 0.82f}, {0.77f, 0.95f},
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
