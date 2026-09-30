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
