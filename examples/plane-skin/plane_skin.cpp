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
  uint32_t (*gchandle_new)(void*, int
