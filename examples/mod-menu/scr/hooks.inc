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
