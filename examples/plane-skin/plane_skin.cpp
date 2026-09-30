// plane_skin.cpp — заменяет модель бумажного самолёта (PaperPlane) на model.glb
// Всё в одном файле: мини-JSON, загрузчик .glb, доступ к il2cpp и сама замена.
//
// Экспорты для загрузчика Bearite (см. раздел 6 внизу):
//   bearite_on_load(api)   — вызывается загрузчиком один раз
//   bearite_on_update(dt)  — каждый кадр (с главного потока Unity)
//   bearite_on_unload()    — мод выключен, вернуть оригинальную модель
// Старые экспорты (для ручного вызова):
//   sba_plane_init(mod_dir), sba_plane_scan(), sba_plane_apply(plane)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <string>
#include <vector>

#include "../../api/bearite.hpp"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SbaPlane", __VA_ARGS__)
#else
#define LOGI(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

// Log into bearite.log through the loader API.
__attribute__((format(printf, 2, 3)))
static void say(int level, const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (bearite::api()) bearite::log(level, "plane-skin", "%s", buf);
  else LOGI("%s", buf);
}

// ============================================================ 1. mini JSON ==

struct J {
  enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<J> a;
  std::vector<std::pair<std::string, J>> o;
  const J* get(const char* k) const {
    if (t != Obj) return nullptr;
    for (const auto& kv : o) if (kv.first == k) return &kv.second;
    return nullptr;
  }
};

static void append_utf8(std::string& out, unsigned cp) {
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

static bool parse_json(const std::string& text, J& out) {
  JParser p(text);
  return p.value(out);
}

static std::string read_file(const std::string& path) {
  std::string out;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
  }
  return out;
}

// =============================================================== 2. .glb ====
// Читает бинарный glTF в плоские массивы в системе координат Unity
// (X отзеркален, порядок вершин треугольников перевёрнут, V у UV перевёрнут).
// Все меши сцены склеиваются в один. Анимации и кости не поддерживаются.

struct GlbModel {
  std::vector<float> pos, nrm, uv;
  std::vector<uint32_t> idx;
  std::string image;  // байты PNG/JPG базовой текстуры, "" если нет
  float color[4] = {1, 1, 1, 1};
  std::string error;
  size_t vertex_count() const { return pos.size() / 3; }
};

namespace glb {

typedef std::array<float, 16> Mat;  // column-major

static Mat identity() { Mat m{}; m[0] = m[5] = m[10] = m[15] = 1; return m; }

static Mat mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = s;
    }
  return r;
}

static double num(const J* j, double def) { return (j && j->t == J::Num) ? j->n : def; }

static Mat node_matrix(const J& n) {
  if (const J* m = n.get("matrix")) {
    if (m->t == J::Arr && m->a.size() == 16) {
      Mat r;
      for (size_t i = 0; i < 16; ++i) r[i] = static_cast<float>(m->a[i].n);
      return r;
    }
  }
  float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
  auto read = [&](const char* key, float* dst, size_t cnt) {
    const J* a = n.get(key);
    if (a && a->t == J::Arr && a->a.size() == cnt)
      for (size_t i = 0; i < cnt; ++i) dst[i] = static_cast<float>(a->a[i].n);
  };
  read("translation", t, 3);
  read("rotation", q, 4);
  read("scale", s, 3);
  float x = q[0], y = q[1], z = q[2], w = q[3];
  float len = std::sqrt(x * x + y * y + z * z + w * w);
  if (len > 0) { x /= len; y /= len; z /= len; w /= len; }
  float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
  float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
  float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
  Mat m{};
  m[0] = r00 * s[0]; m[1] = r10 * s[0]; m[2] = r20 * s[0];
  m[4] = r01 * s[1]; m[5] = r11 * s[1]; m[6] = r21 * s[1];
  m[8] = r02 * s[2]; m[9] = r12 * s[2]; m[10] = r22 * s[2];
  m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
  return m;
}

struct Acc {
  const uint8_t* base = nullptr;
  size_t count = 0, comps = 0, csize = 0, stride = 0;
  int ctype = 0;
  bool normalized = false, ok = false;
};

static Acc accessor(const J& root, const std::vector<uint8_t>& bin, int index) {
  Acc a;
  const J* accs = root.get("accessors");
  const J* views = root.get("bufferViews");
  if (!accs || accs->t != J::Arr || !views || views->t != J::Arr) return a;
  if (index < 0 || static_cast<size_t>(index) >= accs->a.size()) return a;
  const J& ac = accs->a[static_cast<size_t>(index)];
  if (ac.get("sparse")) return a;
  const J* bv = ac.get("bufferView");
  if (!bv || bv->t != J::Num) return a;
  int vi = static_cast<int>(bv->n);
  if (vi < 0 || static_cast<size_t>(vi) >= views->a.size()) return a;
  const J& view = views->a[static_cast<size_t>(vi)];
  a.count = static_cast<size_t>(num(ac.get("count"), 0));
  a.ctype = static_cast<int>(num(ac.get("componentType"), 0));
  const J* type = ac.get("type");
  std::string ty = (type && type->t == J::Str) ? type->s : "";
  a.comps = ty == "SCALAR" ? 1 : ty == "VEC2" ? 2 : ty == "VEC3" ? 3 : ty == "VEC4" ? 4 : 0;
  switch (a.ctype) {
    case 5120: case 5121: a.csize = 1; break;
    case 5122: case 5123: a.csize = 2; break;
    case 5125: case 5126: a.csize = 4; break;
    default: a.csize = 0;
  }
  if (!a.comps || !a.csize || !a.count) return a;
  const J* nm = ac.get("normalized");
  a.normalized = nm && nm->t == J::Bool && nm->b;
  size_t off = static_cast<size_t>(num(view.get("byteOffset"), 0)) + static_cast<size_t>(num(ac.get("byteOffset"), 0));
  a.stride = static_cast<size_t>(num(view.get("byteStride"), 0));
  if (a.stride == 0) a.stride = a.comps * a.csize;
  if (off + (a.count - 1) * a.stride + a.comps * a.csize > bin.size()) return a;
  a.base = bin.data() + off;
  a.ok = true;
  return a;
}

static float read_comp(const Acc& a, size_t i, size_t c) {
  const uint8_t* p = a.base + i * a.stride + c * a.csize;
  switch (a.ctype) {
    case 5126: { float f; memcpy(&f, p, 4); return f; }
    case 5125: { uint32_t u; memcpy(&u, p, 4); return static_cast<float>(u); }
    case 5123: { uint16_t u; memcpy(&u, p, 2); return a.normalized ? u / 65535.0f : static_cast<float>(u); }
    case 5122: { int16_t u; memcpy(&u, p, 2); return a.normalized ? std::max(u / 32767.0f, -1.0f) : static_cast<float>(u); }
    case 5121: return a.normalized ? p[0] / 255.0f : static_cast<float>(p[0]);
    case 5120: { int8_t u = static_cast<int8_t>(p[0]); return a.normalized ? std::max(u / 127.0f, -1.0f) : static_cast<float>(u); }
  }
  return 0;
}

static uint32_t read_index(const Acc& a, size_t i) {
  const uint8_t* p = a.base + i * a.stride;
  switch (a.ctype) {
    case 5125: { uint32_t u; memcpy(&u, p, 4); return u; }
    case 5123: { uint16_t u; memcpy(&u, p, 2); return u; }
    case 5121: return p[0];
  }
  return 0;
}

struct Builder {
  const J& root;
  const std::vector<uint8_t>& bin;
  GlbModel& out;
  bool all_normals = true, all_uv = true, got_material = false;
  int visits = 0;
  std::string err;
  Builder(const J& r, const std::vector<uint8_t>& b, GlbModel& o) : root(r), bin(b), out(o) {}

  void take_material(int mi) {
    if (got_material) return;
    const J* mats = root.get("materials");
    if (!mats || mats->t != J::Arr || mi < 0 || static_cast<size_t>(mi) >= mats->a.size()) return;
    const J* pbr = mats->a[static_cast<size_t>(mi)].get("pbrMetallicRoughness");
    if (!pbr) return;
    got_material = true;
    if (const J* cf = pbr->get("baseColorFactor"))
      if (cf->t == J::Arr && cf->a.size() == 4)
        for (size_t i = 0; i < 4; ++i) out.color[i] = static_cast<float>(cf->a[i].n);
    const J* bct = pbr->get("baseColorTexture");
    const J* texs = root.get("textures");
    const J* imgs = root.get("images");
    const J* views = root.get("bufferViews");
    if (!bct || !texs || !imgs || !views) return;
    int ti = static_cast<int>(num(bct->get("index"), -1));
    if (ti < 0 || static_cast<size_t>(ti) >= texs->a.size()) return;
    int si = static_cast<int>(num(texs->a[static_cast<size_t>(ti)].get("source"), -1));
    if (si < 0 || static_cast<size_t>(si) >= imgs->a.size()) return;
    int vi = static_cast<int>(num(imgs->a[static_cast<size_t>(si)].get("bufferView"), -1));
    if (vi < 0 || static_cast<size_t>(vi) >= views->a.size()) return;
    const J& view = views->a[static_cast<size_t>(vi)];
    size_t off = static_cast<size_t>(num(view.get("byteOffset"), 0));
    size_t len = static_cast<size_t>(num(view.get("byteLength"), 0));
    if (off + len <= bin.size() && len > 0) out.image.assign(reinterpret_cast<const char*>(bin.data()) + off, len);
  }

  void add_primitive(const J& prim, const Mat& w) {
    if (static_cast<int>(num(prim.get("mode"), 4)) != 4) return;
    const J* attrs = prim.get("attributes");
    if (!attrs) return;
    Acc pa = accessor(root, bin, static_cast<int>(num(attrs->get("POSITION"), -1)));
    if (!pa.ok || pa.comps != 3) return;
    Acc na = accessor(root, bin, static_cast<int>(num(attrs->get("NORMAL"), -1)));
    Acc ua = accessor(root, bin, static_cast<int>(num(attrs->get("TEXCOORD_0"), -1)));
    bool has_n = na.ok && na.comps == 3, has_u = ua.ok && ua.comps == 2;
    if (!has_n) all_normals = false;
    if (!has_u) all_uv = false;
    if (out.vertex_count() + pa.count > 4000000) { err = "model is too big (over 4M vertices)"; return; }
    uint32_t base = static_cast<uint32_t>(out.vertex_count());
    for (size_t i = 0; i < pa.count; ++i) {
      float x = read_comp(pa, i, 0), y = read_comp(pa, i, 1), z = read_comp(pa, i, 2);
      out.pos.push_back(-(w[0] * x + w[4] * y + w[8] * z + w[12]));  // glTF -> Unity: зеркалим X
      out.pos.push_back(w[1] * x + w[5] * y + w[9] * z + w[13]);
      out.pos.push_back(w[2] * x + w[6] * y + w[10] * z + w[14]);
      float nx = 0, ny = 0, nz = 0;
      if (has_n) {
        float ax = read_comp(na, i, 0), ay = read_comp(na, i, 1), az = read_comp(na, i, 2);
        nx = w[0] * ax + w[4] * ay + w[8] * az;
        ny = w[1] * ax + w[5] * ay + w[9] * az;
        nz = w[2] * ax + w[6] * ay + w[10] * az;
        float l = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (l > 0) { nx /= l; ny /= l; nz /= l; }
      }
      out.nrm.push_back(-nx);
      out.nrm.push_back(ny);
      out.nrm.push_back(nz);
      out.uv.push_back(has_u ? read_comp(ua, i, 0) : 0.0f);
      out.uv.push_back(has_u ? 1.0f - read_comp(ua, i, 1) : 0.0f);
    }
    Acc ia = accessor(root, bin, static_cast<int>(num(prim.get("indices"), -1)));
    size_t n = ia.ok ? ia.count : pa.count;
    for (size_t i = 0; i + 2 < n; i += 3) {
      uint32_t a = ia.ok ? read_index(ia, i) : static_cast<uint32_t>(i);
      uint32_t b = ia.ok ? read_index(ia, i + 1) : static_cast<uint32_t>(i + 1);
      uint32_t c = ia.ok ? read_index(ia, i + 2) : static_cast<uint32_t>(i + 2);
      if (a >= pa.count || b >= pa.count || c >= pa.count) continue;
      out.idx.push_back(base + a);
      out.idx.push_back(base + c);
      out.idx.push_back(base + b);
    }
    take_material(static_cast<int>(num(prim.get("material"), -1)));
  }

  void add_node(int ni, const Mat& parent, int depth) {
    const J* nodes = root.get("nodes");
    if (!nodes || nodes->t != J::Arr || ni < 0 || static_cast<size_t>(ni) >= nodes->a.size()) return;
    if (depth > 32 || ++visits > 100000 || !err.empty()) return;
    const J& node = nodes->a[static_cast<size_t>(ni)];
    Mat w = mul(parent, node_matrix(node));
    int mi = static_cast<int>(num(node.get("mesh"), -1));
    const J* meshes = root.get("meshes");
    if (mi >= 0 && meshes && meshes->t == J::Arr && static_cast<size_t>(mi) < meshes->a.size()) {
      if (const J* prims = meshes->a[static_cast<size_t>(mi)].get("primitives"))
        if (prims->t == J::Arr) for (const J& p : prims->a) add_primitive(p, w);
    }
    if (const J* ch = node.get("children"))
      if (ch->t == J::Arr) for (const J& c : ch->a) add_node(static_cast<int>(c.n), w, depth + 1);
  }
};

static bool fail(GlbModel& out, const char* msg) { out.error = msg; return false; }

static bool parse(const std::string& file, GlbModel& out) {
  out = GlbModel();
  if (file.size() < 20) return fail(out, "file is empty or too small");
  auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, file.data() + o, 4); return v; };
  if (u32(0) != 0x46546C67u) return fail(out, "not a .glb file (export as glTF Binary)");
  if (u32(4) != 2) return fail(out, "only glTF 2.0 is supported");
  std::string json_text;
  std::vector<uint8_t> bin;
  bool has_bin = false;
  size_t pos = 12;
  while (pos + 8 <= file.size()) {
    uint32_t len = u32(pos), type = u32(pos + 4);
    pos += 8;
    if (len > file.size() - pos) return fail(out, "damaged .glb (chunk is cut off)");
    if (type == 0x4E4F534Au) json_text.assign(file.data() + pos, len);
    else if (type == 0x004E4942u && !has_bin) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(file.data()) + pos;
      bin.assign(p, p + len);
      has_bin = true;
    }
    pos += (static_cast<size_t>(len) + 3) & ~static_cast<size_t>(3);
  }
  if (json_text.empty()) return fail(out, "no JSON chunk in the .glb");
  if (!has_bin) return fail(out, "no binary chunk in the .glb");
  J root;
  if (!parse_json(json_text, root) || root.t != J::Obj) return fail(out, "can't read the JSON part of the .glb");
  Builder b(root, bin, out);
  Mat id = identity();
  const J* nodes = root.get("nodes");
  const J* scenes = root.get("scenes");
  if (scenes && scenes->t == J::Arr && !scenes->a.empty()) {
    size_t si = static_cast<size_t>(num(root.get("scene"), 0));
    if (si >= scenes->a.size()) si = 0;
    if (const J* list = scenes->a[si].get("nodes"))
      if (list->t == J::Arr) for (const J& n : list->a) b.add_node(static_cast<int>(n.n), id, 0);
  } else if (nodes && nodes->t == J::Arr) {
    std::vector<bool> is_child(nodes->a.size(), false);
    for (const J& n : nodes->a)
      if (const J* ch = n.get("children"))
        if (ch->t == J::Arr) for (const J& c : ch->a) {
          size_t ci = static_cast<size_t>(c.n);
          if (ci < is_child.size()) is_child[ci] = true;
        }
    for (size_t i = 0; i < is_child.size(); ++i) if (!is_child[i]) b.add_node(static_cast<int>(i), id, 0);
  }
  if (!b.err.empty()) return fail(out, b.err.c_str());
  if (out.idx.empty()) return fail(out, "no triangles found in the model");
  if (!b.all_normals) out.nrm.clear();
  if (!b.all_uv) out.uv.clear();
  return true;
}

}  // namespace glb

// ============================================================ 3. il2cpp =====
// Берём экспортируемые функции il2cpp напрямую из libil2cpp.so через dlsym.

namespace il {

struct Api {
  void* (*domain_get)();
  void** (*domain_get_assemblies)(void*, size_t*);
  void* (*assembly_get_image)(void*);
  const char* (*image_get_name)(void*);
  size_t (*image_get_class_count)(void*);
  void* (*image_get_class)(void*, size_t);
  void* (*class_from_name)(void*, const char*, const char*);
  void* (*class_get_methods)(void*, void**);
  void* (*class_get_parent)(void*);
  const char* (*method_get_name)(void*);
  uint32_t (*method_get_param_count)(void*);
  void* (*method_get_param)(void*, uint32_t);
  char* (*type_get_name)(void*);
  void* (*runtime_invoke)(void*, void*, void**, void**);
  void* (*string_new)(const char*);
  void* (*object_new)(void*);
  void* (*object_get_class)(void*);
  const char* (*class_get_name)(void*);
  void* (*array_new)(void*, uintptr_t);
  void* (*class_get_field_from_name)(void*, const char*);
  size_t (*field_get_offset)(void*);
  void* (*class_get_type)(void*);
  void* (*type_get_object)(void*);
  void* (*object_unbox)(void*);
  uint32_t (*gchandle_new)(void*, int);
  void (*free_mem)(void*);
  const uint16_t* (*string_chars)(void*);
  int32_t (*string_length)(void*);
  bool ok = false;
};
static Api A;
static void* g_lib = nullptr;

// libil2cpp.so is usually loaded with RTLD_LOCAL, so look in its own handle first.
template <class T> static T sym(const char* name) {
  void* p = g_lib ? dlsym(g_lib, name) : nullptr;
  if (!p) p = dlsym(RTLD_DEFAULT, name);
  return reinterpret_cast<T>(p);
}

static bool init() {
  if (A.ok) return true;
  g_lib = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
#define L(field, name) A.field = sym<decltype(A.field)>(name)
  L(domain_get, "il2cpp_domain_get");
  L(domain_get_assemblies, "il2cpp_domain_get_assemblies");
  L(assembly_get_image, "il2cpp_assembly_get_image");
  L(image_get_name, "il2cpp_image_get_name");
  L(image_get_class_count, "il2cpp_image_get_class_count");
  L(image_get_class, "il2cpp_image_get_class");
  L(class_from_name, "il2cpp_class_from_name");
  L(class_get_methods, "il2cpp_class_get_methods");
  L(class_get_parent, "il2cpp_class_get_parent");
  L(method_get_name, "il2cpp_method_get_name");
  L(method_get_param_count, "il2cpp_method_get_param_count");
  L(method_get_param, "il2cpp_method_get_param");
  L(type_get_name, "il2cpp_type_get_name");
  L(runtime_invoke, "il2cpp_runtime_invoke");
  L(string_new, "il2cpp_string_new");
  L(object_new, "il2cpp_object_new");
  L(object_get_class, "il2cpp_object_get_class");
  L(class_get_name, "il2cpp_class_get_name");
  L(array_new, "il2cpp_array_new");
  L(class_get_field_from_name, "il2cpp_class_get_field_from_name");
  L(field_get_offset, "il2cpp_field_get_offset");
  L(class_get_type, "il2cpp_class_get_type");
  L(type_get_object, "il2cpp_type_get_object");
  L(object_unbox, "il2cpp_object_unbox");
  L(gchandle_new, "il2cpp_gchandle_new");
  L(free_mem, "il2cpp_free");
  L(string_chars, "il2cpp_string_chars");
  L(string_length, "il2cpp_string_length");
#undef L
  A.ok = A.domain_get && A.domain_get_assemblies && A.assembly_get_image && A.image_get_name &&
         A.class_from_name && A.class_get_methods && A.class_get_parent && A.method_get_name &&
         A.method_get_param_count && A.method_get_param && A.type_get_name && A.runtime_invoke &&
         A.object_new && A.array_new && A.class_get_type && A.type_get_object && A.object_unbox &&
         A.gchandle_new;
  if (!A.ok) say(BEARITE_LOG_ERROR, "il2cpp API is incomplete (libil2cpp handle: %s)", g_lib ? "yes" : "no");
  return A.ok;
}

// "UnityEngine.CoreModule.dll" matches "UnityEngine.CoreModule".
static bool name_match(const char* have, const char* want) {
  size_t hl = strlen(have);
  if (hl > 4 && strcmp(have + hl - 4, ".dll") == 0) hl -= 4;
  return strlen(want) == hl && strncmp(have, want, hl) == 0;
}

static void* find_image(const char* asm_name) {
  size_t cnt = 0;
  void** list = A.domain_get_assemblies(A.domain_get(), &cnt);
  if (!list) return nullptr;
  for (size_t i = 0; i < cnt; ++i) {
    void* img = A.assembly_get_image(list[i]);
    const char* nm = img ? A.image_get_name(img) : nullptr;
    if (nm && name_match(nm, asm_name)) return img;
  }
  return nullptr;
}

static void* find_class(const char* asm_name, const char* ns, const char* name) {
  void* img = find_image(asm_name);
  return img ? A.class_from_name(img, ns, name) : nullptr;
}

// Same, but ignores the namespace (used if the game class is not in the global one).
static void* find_class_any_ns(const char* asm_name, const char* name) {
  void* img = find_image(asm_name);
  if (!img || !A.image_get_class_count || !A.image_get_class || !A.class_get_name) return nullptr;
  size_t cnt = A.image_get_class_count(img);
  for (size_t i = 0; i < cnt; ++i) {
    void* k = A.image_get_class(img, i);
    if (k && strcmp(A.class_get_name(k), name) == 0) return k;
  }
  return nullptr;
}

// Finds a method by name and argument count, looking through base classes.
// p0 (optional) is the type name of the first parameter, e.g. "System.Type":
// it tells apart overloads such as GetComponent(Type) and GetComponent(string).
static void* find_method(void* klass, const char* name, int argc, const char* p0 = nullptr) {
  for (void* k = klass; k; k = A.class_get_parent(k)) {
    void* it = nullptr;
    while (void* m = A.class_get_methods(k, &it)) {
      if (strcmp(A.method_get_name(m), name) != 0) continue;
      if (static_cast<int>(A.method_get_param_count(m)) != argc) continue;
      if (p0 && argc > 0) {
        char* tn = A.type_get_name(A.method_get_param(m, 0));
        bool same = tn && strcmp(tn, p0) == 0;
        if (tn && A.free_mem) A.free_mem(tn);
        if (!same) continue;
      }
      return m;
    }
  }
  return nullptr;
}

static bool g_exc = false;  // true if the last call() threw a managed exception

static void* call(void* method, void* obj, std::initializer_list<void*> args = {}) {
  void* buf[8];
  size_t n = 0;
  for (void* p : args) if (n < 8) buf[n++] = p;
  void* exc = nullptr;
  void* r = A.runtime_invoke(method, obj, n ? buf : nullptr, &exc);
  g_exc = exc != nullptr;
  return g_exc ? nullptr : r;
}

static int unbox_int(void* boxed) {
  return boxed ? *static_cast<int*>(A.object_unbox(boxed)) : 0;
}

// Keeps a managed object alive forever (the GC would collect it otherwise).
static void* keep(void* obj) {
  if (obj) A.gchandle_new(obj, 0);
  return obj;
}

static void* type_object(void* klass) { return keep(A.type_get_object(A.class_get_type(klass))); }

// Managed array layout: object header (2 pointers), bounds pointer, length, then the data.
static uint8_t* arr_data(void* arr) { return static_cast<uint8_t*>(arr) + 4 * sizeof(void*); }
static size_t arr_len(void* arr) { return arr ? *reinterpret_cast<size_t*>(static_cast<uint8_t*>(arr) + 3 * sizeof(void*)) : 0; }

}  // namespace il

// ========================================================= 4. замена модели ==

namespace sk {

using namespace il;

struct Config {
  bool enabled = true;
  float scale = 1, yaw = 0, pitch = 0, roll = 0, offset_y = 0;
  bool operator==(const Config& o) const {
    return enabled == o.enabled && scale == o.scale && yaw == o.yaw && pitch == o.pitch &&
           roll == o.roll && offset_y == o.offset_y;
  }
};

struct Entry {          // one MeshFilter we touched
  int plane_id = 0;
  void* filter = nullptr;
  void* orig_mesh = nullptr;
  bool primary = false;  // the filter that gets our mesh; the others are hidden
  void* mat = nullptr;   // material of the primary filter
  void* orig_tex = nullptr;
  float orig_col[4] = {1, 1, 1, 1};
  bool dead = false;
};

static std::string g_dir;
static GlbModel g_model;
static Config g_cfg, g_applied;
static bool g_ready = false, g_failed = false, g_have_cfg = false;
static float g_t_cfg = 0.5f, g_t_scan = 1.0f;

static void *k_plane, *k_obj, *k_mf, *k_mesh, *k_tex, *k_v3, *k_v2, *k_i32, *k_u8;
static void *t_plane, *t_mf, *t_mr;
static void *m_find, *m_gcic, *m_gc, *m_instid, *m_get_shared, *m_set_shared, *m_get_mat,
    *m_get_tex, *m_set_tex, *m_get_col, *m_set_col, *m_set_verts, *m_set_norms, *m_set_uv,
    *m_set_tris, *m_set_ifmt, *m_recalc_b, *m_recalc_n;
static bool m_find_by = false;  // FindObjectsByType(Type, sortMode) instead of FindObjectsOfType(Type)
static void* g_mesh = nullptr;
static void* g_tex = nullptr;
static std::vector<Entry> g_entries;
static std::vector<int> g_seen;

static std::string settings_path() { return g_dir + "/settings.json"; }

// Reads one setting: settings.json in the mod folder first (always fresh),
// then the loader API.
static bool get_raw(const J* file, const char* key, double& num_out) {
  if (file) {
    if (const J* v = file->get(key)) {
      if (v->t == J::Num) { num_out = v->n; return true; }
      if (v->t == J::Bool) { num_out = v->b ? 1 : 0; return true; }
      if (v->t == J::Str && !v->s.empty()) {
        if (v->s == "true") { num_out = 1; return true; }
        if (v->s == "false") { num_out = 0; return true; }
        num_out = strtod(v->s.c_str(), nullptr);
        return true;
      }
    }
  }
  std::string s = bearite::setting(key, "");
  if (s.empty()) return false;
  if (s == "true") { num_out = 1; return true; }
  if (s == "false") { num_out = 0; return true; }
  num_out = strtod(s.c_str(), nullptr);
  return true;
}

static Config read_config() {
  Config c;
  J file;
  std::string text = read_file(settings_path());
  const J* f = (!text.empty() && parse_json(text, file) && file.t == J::Obj) ? &file : nullptr;
  double v;
  if (get_raw(f, "enabled", v)) c.enabled = v != 0;
  if (get_raw(f, "scale", v)) c.scale = static_cast<float>(v);
  if (get_raw(f, "yaw", v)) c.yaw = static_cast<float>(v);
  if (get_raw(f, "pitch", v)) c.pitch = static_cast<float>(v);
  if (get_raw(f, "roll", v)) c.roll = static_cast<float>(v);
  if (get_raw(f, "offset_y", v)) c.offset_y = static_cast<float>(v);
  c.scale = std::min(10.0f, std::max(0.05f, c.scale));
  return c;
}

// ---- mesh ----

static void* new_array(void* elem_class, size_t n) { return A.array_new(elem_class, n); }

// Pushes the model into the Unity mesh. full = also indices and UVs (first time only).
static void upload_mesh(bool full) {
  size_t n = g_model.vertex_count();
  const float d2r = 3.14159265f / 180.0f;
  float cy = std::cos(g_cfg.yaw * d2r), sy = std::sin(g_cfg.yaw * d2r);
  float cx = std::cos(g_cfg.pitch * d2r), sx = std::sin(g_cfg.pitch * d2r);
  float cz = std::cos(g_cfg.roll * d2r), sz = std::sin(g_cfg.roll * d2r);
  // Unity order: roll (Z), then pitch (X), then yaw (Y).  R = Ry * Rx * Rz
  float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
  float rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
  float rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
  float t[9], r[9];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      t[i * 3 + j] = rx[i * 3] * rz[j] + rx[i * 3 + 1] * rz[3 + j] + rx[i * 3 + 2] * rz[6 + j];
    }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      r[i * 3 + j] = ry[i * 3] * t[j] + ry[i * 3 + 1] * t[3 + j] + ry[i * 3 + 2] * t[6 + j];
    }
  void* va = new_array(k_v3, n);
  float* vp = reinterpret_cast<float*>(arr_data(va));
  for (size_t i = 0; i < n; ++i) {
    const float* p = &g_model.pos[i * 3];
    vp[i * 3] = (r[0] * p[0] + r[1] * p[1] + r[2] * p[2]) * g_cfg.scale;
    vp[i * 3 + 1] = (r[3] * p[0] + r[4] * p[1] + r[5] * p[2]) * g_cfg.scale + g_cfg.offset_y;
    vp[i * 3 + 2] = (r[6] * p[0] + r[7] * p[1] + r[8] * p[2]) * g_cfg.scale;
  }
  if (full && n > 65535 && m_set_ifmt) {
    int fmt = 1;  // IndexFormat.UInt32
    call(m_set_ifmt, g_mesh, {&fmt});
  }
  call(m_set_verts, g_mesh, {va});
  if (full) {
    void* ta = new_array(k_i32, g_model.idx.size());
    memcpy(arr_data(ta), g_model.idx.data(), g_model.idx.size() * sizeof(uint32_t));
    call(m_set_tris, g_mesh, {ta});
    if (!g_model.uv.empty() && m_set_uv) {
      void* ua = new_array(k_v2, n);
      memcpy(arr_data(ua), g_model.uv.data(), n * 2 * sizeof(float));
      call(m_set_uv, g_mesh, {ua});
    }
  }
  if (!g_model.nrm.empty() && m_set_norms) {
    void* na = new_array(k_v3, n);
    float* np = reinterpret_cast<float*>(arr_data(na));
    for (size_t i = 0; i < n; ++i) {
      const float* p = &g_model.nrm[i * 3];
      np[i * 3] = r[0] * p[0] + r[1] * p[1] + r[2] * p[2];
      np[i * 3 + 1] = r[3] * p[0] + r[4] * p[1] + r[5] * p[2];
      np[i * 3 + 2] = r[6] * p[0] + r[7] * p[1] + r[8] * p[2];
    }
    call(m_set_norms, g_mesh, {na});
  } else if (m_recalc_n) {
    call(m_recalc_n, g_mesh);
  }
  if (m_recalc_b) call(m_recalc_b, g_mesh);
}

// ---- setup ----

static bool need(void* p, const char* what) {
  if (!p) { say(BEARITE_LOG_ERROR, "setup failed: %s not found", what); g_failed = true; return false; }
  return true;
}

static bool setup() {
  if (!il::init()) { g_failed = true; return false; }
  const char* core = "UnityEngine.CoreModule";
  k_plane = find_class("Assembly-CSharp", "", "PaperPlane");
  if (!k_plane) k_plane = find_class_any_ns("Assembly-CSharp", "PaperPlane");
  k_obj = find_class(core, "UnityEngine", "Object");
  void* k_comp = find_class(core, "UnityEngine", "Component");
  k_mf = find_class(core, "UnityEngine", "MeshFilter");
  void* k_mr = find_class(core, "UnityEngine", "MeshRenderer");
  k_mesh = find_class(core, "UnityEngine", "Mesh");
  k_tex = find_class(core, "UnityEngine", "Texture2D");
  void* k_mat = find_class(core, "UnityEngine", "Material");
  k_v3 = find_class(core, "UnityEngine", "Vector3");
  k_v2 = find_class(core, "UnityEngine", "Vector2");
  k_i32 = find_class("mscorlib", "System", "Int32");
  k_u8 = find_class("mscorlib", "System", "Byte");
  if (!need(k_plane, "class PaperPlane (Assembly-CSharp)") || !need(k_obj, "UnityEngine.Object") ||
      !need(k_comp, "UnityEngine.Component") || !need(k_mf, "UnityEngine.MeshFilter") ||
      !need(k_mr, "UnityEngine.MeshRenderer") || !need(k_mesh, "UnityEngine.Mesh") ||
      !need(k_mat, "UnityEngine.Material") || !need(k_v3, "Vector3") || !need(k_v2, "Vector2") ||
      !need(k_i32, "System.Int32") || !need(k_u8, "System.Byte"))
    return false;

  t_plane = type_object(k_plane);
  t_mf = type_object(k_mf);
  t_mr = type_object(k_mr);

  m_find = find_method(k_obj, "FindObjectsOfType", 1, "System.Type");
  if (!m_find) { m_find = find_method(k_obj, "FindObjectsByType", 2, "System.Type"); m_find_by = m_find != nullptr; }
  m_gcic = find_method(k_comp, "GetComponentsInChildren", 2, "System.Type");
  m_gc = find_method(k_comp, "GetComponent", 1, "System.Type");
  m_instid = find_method(k_obj, "GetInstanceID", 0);
  m_get_shared = find_method(k_mf, "get_sharedMesh", 0);
  m_set_shared = find_method(k_mf, "set_sharedMesh", 1);
  m_get_mat = find_method(k_mr, "get_material", 0);
  m_get_tex = find_method(k_mat, "get_mainTexture", 0);
  m_set_tex = find_method(k_mat, "set_mainTexture", 1);
  m_get_col = find_method(k_mat, "get_color", 0);
  m_set_col = find_method(k_mat, "set_color", 1);
  m_set_verts = find_method(k_mesh, "set_vertices", 1);
  m_set_norms = find_method(k_mesh, "set_normals", 1);
  m_set_uv = find_method(k_mesh, "set_uv", 1);
  m_set_tris = find_method(k_mesh, "set_triangles", 1);
  m_set_ifmt = find_method(k_mesh, "set_indexFormat", 1);
  m_recalc_b = find_method(k_mesh, "RecalculateBounds", 0);
  m_recalc_n = find_method(k_mesh, "RecalculateNormals", 0);
  if (!need(m_find, "Object.FindObjectsOfType") || !need(m_gcic, "Component.GetComponentsInChildren(Type,bool)") ||
      !need(m_gc, "Component.GetComponent(Type)") || !need(m_instid, "Object.GetInstanceID") ||
      !need(m_get_shared, "MeshFilter.get_sharedMesh") || !need(m_set_shared, "MeshFilter.set_sharedMesh") ||
      !need(m_get_mat, "Renderer.get_material") || !need(m_set_verts, "Mesh.set_vertices") ||
      !need(m_set_tris, "Mesh.set_triangles"))
    return false;

  // model.glb
  std::string file = read_file(g_dir + "/model.glb");
  if (file.empty()) {
    say(BEARITE_LOG_ERROR, "model.glb not found in %s", g_dir.c_str());
    g_failed = true;
    return false;
  }
  if (!glb::parse(file, g_model)) {
    say(BEARITE_LOG_ERROR, "model.glb: %s", g_model.error.c_str());
    g_failed = true;
    return false;
  }
  say(BEARITE_LOG_INFO, "model.glb loaded: %zu vertices, %zu triangles, texture: %s", g_model.vertex_count(),
      g_model.idx.size() / 3, g_model.image.empty() ? "no" : "yes");

  // texture
  if (!g_model.image.empty() && k_tex) {
    void* ctor = find_method(k_tex, ".ctor", 2, "System.Int32");
    void* tex = ctor ? A.object_new(k_tex) : nullptr;
    if (tex) {
      int w = 2, h = 2;
      call(ctor, tex, {&w, &h});
      void* bytes = new_array(k_u8, g_model.image.size());
      memcpy(arr_data(bytes), g_model.image.data(), g_model.image.size());
      bool loaded = false;
      void* k_ic = find_class("UnityEngine.ImageConversionModule", "UnityEngine", "ImageConversion");
      void* m_load = k_ic ? find_method(k_ic, "LoadImage", 2, "UnityEngine.Texture2D") : nullptr;
      if (m_load) {
        void* r = call(m_load, nullptr, {tex, bytes});
        loaded = r && unbox_int(r) != 0;
      } else if (void* m_inst = find_method(k_tex, "LoadImage", 1, "System.Byte[]")) {
        void* r = call(m_inst, tex, {bytes});
        loaded = r && unbox_int(r) != 0;
      }
      if (loaded) g_tex = keep(tex);
      else say(BEARITE_LOG_WARN, "texture could not be decoded, using the color only");
    }
  }

  // mesh
  void* mctor = find_method(k_mesh, ".ctor", 0);
  g_mesh = mctor ? A.object_new(k_mesh) : nullptr;
  if (!need(g_mesh, "Mesh constructor")) return false;
  call(mctor, g_mesh);
  keep(g_mesh);
  g_cfg = read_config();
  g_applied = g_cfg;
  g_have_cfg = true;
  upload_mesh(true);
  say(BEARITE_LOG_INFO, "setup done (PaperPlane found, mesh built)");
  g_ready = true;
  return true;
}

// ---- applying ----

static void set_state(Entry& e, bool on) {
  if (!e.filter) return;
  if (e.primary) {
    call(m_set_shared, e.filter, {on ? g_mesh : e.orig_mesh});
    if (g_exc) { e.dead = true; return; }
    if (e.mat && m_set_tex && m_set_col) {
      if (on) {
        call(m_set_tex, e.mat, {g_tex});
        call(m_set_col, e.mat, {g_model.color});
      } else {
        call(m_set_tex, e.mat, {e.orig_tex});
        call(m_set_col, e.mat, {e.orig_col});
      }
    }
  } else {
    void* m = on ? nullptr : e.orig_mesh;
    call(m_set_shared, e.filter, {m});
    if (g_exc) e.dead = true;
  }
}

static void apply_plane(void* plane) {
  if (!g_ready || !plane) return;
  int id = unbox_int(call(m_instid, plane));
  if (std::find(g_seen.begin(), g_seen.end(), id) != g_seen.end()) return;
  if (g_seen.size() > 512) { g_seen.clear(); g_entries.clear(); }
  g_seen.push_back(id);
  bool include_inactive = true;
  void* arr = call(m_gcic, plane, {t_mf, &include_inactive});
  size_t n = arr_len(arr);
  say(BEARITE_LOG_INFO, "PaperPlane %d: %zu MeshFilter(s)", id, n);
  bool primary_taken = false;
  for (size_t i = 0; i < n; ++i) {
    void* f = reinterpret_cast<void**>(arr_data(arr))[i];
    if (!f) continue;
    Entry e;
    e.plane_id = id;
    e.filter = keep(f);
    e.orig_mesh = keep(call(m_get_shared, f));
    if (!primary_taken) {
      primary_taken = true;
      e.primary = true;
      void* mr = call(m_gc, f, {t_mr});
      if (mr) {
        e.mat = keep(call(m_get_mat, mr));
        if (e.mat && m_get_tex && m_get_col) {
          e.orig_tex = keep(call(m_get_tex, e.mat));
          void* c = call(m_get_col, e.mat);
          if (c) memcpy(e.orig_col, A.object_unbox(c), sizeof(e.orig_col));
        }
      } else {
        say(BEARITE_LOG_WARN, "PaperPlane %d: no MeshRenderer on the main MeshFilter", id);
      }
    }
    g_entries.push_back(e);
    set_state(g_entries.back(), g_cfg.enabled);
  }
}

static void scan() {
  if (!g_ready) return;
  int sort_mode = 0;
  void* arr = m_find_by ? call(m_find, nullptr, {t_plane, &sort_mode}) : call(m_find, nullptr, {t_plane});
  size_t n = arr_len(arr);
  for (size_t i = 0; i < n; ++i) apply_plane(reinterpret_cast<void**>(arr_data(arr))[i]);
}

static void apply_all(bool on) {
  for (Entry& e : g_entries) set_state(e, on);
  g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [](const Entry& e) { return e.dead; }),
                  g_entries.end());
}

static void update(float dt) {
  if (g_failed) return;
  const BeariteApi* api = bearite::api();
  if (!api || !BEARITE_API_HAS(api, il2cpp_ready) || !api->il2cpp_ready(api)) return;
  if (!g_ready && !setup()) return;
  g_t_cfg += dt;
  g_t_scan += dt;
  if (g_t_cfg >= 0.5f) {
    g_t_cfg = 0;
    Config c = read_config();
    if (!(c == g_applied)) {
      bool toggled = c.enabled != g_applied.enabled;
      bool moved = c.scale != g_applied.scale || c.yaw != g_applied.yaw || c.pitch != g_applied.pitch ||
                   c.roll != g_applied.roll || c.offset_y != g_applied.offset_y;
      g_cfg = c;
      g_applied = c;
      if (moved) upload_mesh(false);
      if (toggled) apply_all(c.enabled);
    }
  }
  if (g_t_scan >= 1.0f) {
    g_t_scan = 0;
    if (g_cfg.enabled) scan();
  }
}

static void unload() {
  if (g_ready) apply_all(false);
}

}  // namespace sk

// ============================================================ 6. exports =====

extern "C" {

BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  if (api && api->mod_dir) sk::g_dir = api->mod_dir;
  say(BEARITE_LOG_INFO, "Plane Skin loaded, mod dir: %s", sk::g_dir.c_str());
  return 0;
}

BEARITE_EXPORT void bearite_on_update(float dt) { sk::update(dt); }

BEARITE_EXPORT void bearite_on_unload(void) { sk::unload(); }

// Manual entry points (same code path as the loader calls).
BEARITE_EXPORT void sba_plane_init(const char* mod_dir) {
  if (mod_dir) sk::g_dir = mod_dir;
}

BEARITE_EXPORT void sba_plane_scan(void) { sk::scan(); }

BEARITE_EXPORT void sba_plane_apply(void* plane) { sk::apply_plane(plane); }

}  // extern "C"
