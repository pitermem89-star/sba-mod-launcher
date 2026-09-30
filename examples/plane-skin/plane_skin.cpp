// plane_skin.cpp — заменяет модель бумажного самолёта (PaperPlane) на model.glb
// Всё в одном файле: мини-JSON, загрузчик .glb, доступ к il2cpp и сама замена.
//
// Экспортируемые функции (см. раздел 6 внизу):
//   sba_plane_init(mod_dir)  — один раз при загрузке мода
//   sba_plane_scan()         — найти все самолёты на сцене и заменить модель
//   sba_plane_apply(plane)   — заменить модель у одного PaperPlane

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SbaPlane", __VA_ARGS__)
#else
#define LOGI(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

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
  void* (*class_from_name)(void*, const char*, const char*);
  void* (*class_get_methods)(void*, void**);
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

template <class T> static T sym(const char* name) { return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name)); }

static bool init() {
  if (A.ok) return true;
  A.domain_get = sym<decltype(A.domain_get)>("il2cpp_domain_get");
  A.domain_get_assemblies = sym<decltype(A.domain_get_assemblies)>("il2cpp_domain_get_assemblies");
  A.assembly_get_image = sym<decltype(A.assembly_get_image)>("il2cpp_assembly_get_image");
  A.image_get_name = sym<decltype(A.image_get_name)>("il2cpp_image_get_name");
  A.class_from_name = sym<decltype(A.class_from_name)>("il2cpp_class_from_name");
  A.class_get_methods = sym<decltype(A.class_get_methods)>("il2cpp_class_get_methods");
  A.method_get_name = sym<decltype(A.method_get_name)>("il2cpp_method_get_name");
  A.method_get_param_count = sym<decltype(A.method_get_param_count)>("il2cpp_method_get_param_count");
  A.method_get_param = sym<decltype(A.method_get_param)>("il2cpp_method_get_param");
  A.type_get_name = sym<decltype(A.type_get_name)>("il2cpp_type_get_name");
  A.runtime_invoke = sym<decltype(A.runtime_invoke)>("il2cpp_runtime_invoke");
  A.string_new = sym<decltype(A.string_new)>("il2cpp_string_new");
  A.object_new = sym<decltype(A.object_new)>("il2cpp_object_new");
  A.object_get_class = sym<decltype(A.object_get_class)>("il2cpp_object_get_class");
  A.class_get_name = sym<decltype(A.class_get_name)>("il2cpp_class_get_name");
  A.array_new = sym<decltype(A.array_new)>("il2cpp_array_new");
  A.class_get_field_from_name = sym<decltype(A.class_get_field_from_name)>("il2cpp_class_get_field_from_name");
  A.field_get_offset = sym<decltype(A.field_get_offset)>("il2cpp_field_get_offset");
  A.class_get_type = sym<decltype(A.class_get_type)>("il2cpp_class_get_type");
  A.type_get_object = sym<decltype(A.type_get_object)>("il2cpp_type_get_object");
  A.object_unbox = sym<decltype(A.object_unbox)>("il2cpp_object_unbox");
  A.gchandle_new = sym<decltype(A.gchandle_new)>("il2cpp_gchandle_new");
  A.free_mem = sym<decltype(A.free_mem)>("il2cpp_free");
  A.string_chars = sym<decltype(A.string_chars)>("il2cpp_string_chars");
  A.string_length = sym<decltype(A.string_length)>("il2cpp_string_length");
  A.ok = A.domain_get && A.domain_get_assemblies && A.assembly_get_image && A.image_get_name && A.class_from_name &&
         A.class_get_methods && A.method_get_name && A.method_get_param_count && A.method_get_param && A.type_get_name &&
         A.runtime_invoke && A.string_new && A.object_new && A.object_get_class && A.class_get_name && A.array_new &&
         A.class_get_field_from_name && A.field_get_offset && A.class_get_type && A.type_get_object && A.object_unbox &&
         A.gchandle_new && A.string_chars && A.string_length;
  if (!A.ok) LOGI("il2cpp exports not found");
  return A.ok;
}

// Имя сборки сравниваем без ".dll".
static void* klass(const char* image_name, const char* ns, const char* name) {
  size_t n = 0;
  void** asms = A.domain_get_assemblies(A.domain_get(), &n);
  for (size_t i = 0; i < n; ++i) {
    void* img = A.assembly_get_image(asms[i]);
    std::string nm = A.image_get_name(img);
    if (nm.size() > 4 && nm.compare(nm.size() - 4, 4, ".dll") == 0) nm.resize(nm.size() - 4);
    if (nm == image_name) return A.class_from_name(img, ns, name);
  }
  return nullptr;
}

// Метод по имени и полным именам типов параметров (без путаницы с перегрузками).
static void* method(void* k, const char* name, std::initializer_list<const char*> params) {
  if (!k) return nullptr;
  void* iter = nullptr;
  while (void* m = A.class_get_methods(k, &iter)) {
    if (strcmp(A.method_get_name(m), name) != 0) continue;
    if (A.method_get_param_count(m) != params.size()) continue;
    bool same = true;
    uint32_t i = 0;
    for (const char* want : params) {
      char* got = A.type_get_name(A.method_get_param(m, i++));
      if (!got || strcmp(got, want) != 0) same = false;
      if (got && A.free_mem) A.free_mem(got);
      if (!same) break;
    }
    if (same) return m;
  }
  return nullptr;
}

static void* invoke(void* m, void* self, std::initializer_list<void*> args) {
  if (!m) return nullptr;
  void* p[8] = {nullptr};
  size_t i = 0;
  for (void* a : args) p[i++] = a;
  void* exc = nullptr;
  void* r = A.runtime_invoke(m, self, p, &exc);
  if (exc) { LOGI("managed exception in %s", A.method_get_name(m)); return nullptr; }
  return r;
}

static void* str(const char* s) { return A.string_new(s); }
static void* type_obj(void* k) { return A.type_get_object(A.class_get_type(k)); }
static void* new_obj(void* k) { return A.object_new(k); }
static void pin(void* o) { if (o) A.gchandle_new(o, 0); }

static void* array_data(void* arr) { return static_cast<char*>(arr) + 32; }  // 64-bit
static size_t array_len(void* arr) { return arr ? *reinterpret_cast<uintptr_t*>(static_cast<char*>(arr) + 24) : 0; }

static void* field_ptr(void* obj, void* k, const char* name) {
  void* f = A.class_get_field_from_name(k, name);
  if (!f) return nullptr;
  return *reinterpret_cast<void**>(static_cast<char*>(obj) + A.field_get_offset(f));
}

static bool unbox_bool(void* boxed) { return boxed && *static_cast<bool*>(A.object_unbox(boxed)); }

}  // namespace il

// ============================================================ 4. Unity ======

struct V3 { float x, y, z; };
struct Quat { float x, y, z, w; };

static Quat quat_euler(float pitch, float yaw, float roll) {  // градусы, порядок Unity: Z, X, Y
  const float k = 3.14159265f / 360.0f;
  float sx = std::sin(pitch * k), cx = std::cos(pitch * k);
  float sy = std::sin(yaw * k), cy = std::cos(yaw * k);
  float sz = std::sin(roll * k), cz = std::cos(roll * k);
  auto mulq = [](Quat a, Quat b) {
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
  };
  return mulq(mulq(Quat{0, sy, 0, cy}, Quat{sx, 0, 0, cx}), Quat{0, 0, sz, cz});
}

struct Unity {
  void *Mesh, *Texture2D, *Texture, *Material, *GameObject, *Transform, *Component, *Renderer, *MeshFilter,
      *MeshRenderer, *UObject, *Vector3, *Vector2, *Byte, *Int32, *ImageConversion, *PaperPlane;
  void *mesh_ctor, *mesh_indexFormat, *mesh_vertices, *mesh_normals, *mesh_uv, *mesh_triangles, *mesh_bounds, *mesh_recalc_normals;
  void *tex_ctor, *tex_load, *tex_filter, *tex_wrap;
  void *mat_copy, *mat_maintex, *mat_color;
  void *go_ctor, *go_addcomp, *go_transform, *go_get_layer, *go_set_layer;
  void *comp_go, *comp_children, *obj_name;
  void *tr_setparent, *tr_pos, *tr_rot, *tr_scale, *tr_find;
  void *mf_mesh, *rend_material, *rend_get_material, *rend_enabled;
  void *find_objects;
  bool ok = false;
};
static Unity U;

static bool unity_init() {
  if (U.ok) return true;
  if (!il::init()) return false;
  const char* core = "UnityEngine.CoreModule";
  U.Mesh = il::klass(core, "UnityEngine", "Mesh");
  U.Texture2D = il::klass(core, "UnityEngine", "Texture2D");
  U.Texture = il::klass(core, "UnityEngine", "Texture");
  U.Material = il::klass(core, "UnityEngine", "Material");
  U.GameObject = il::klass(core, "UnityEngine", "GameObject");
  U.Transform = il::klass(core, "UnityEngine", "Transform");
  U.Component = il::klass(core, "UnityEngine", "Component");
  U.Renderer = il::klass(core, "UnityEngine", "Renderer");
  U.MeshFilter = il::klass(core, "UnityEngine", "MeshFilter");
  U.MeshRenderer = il::klass(core, "UnityEngine", "MeshRenderer");
  U.UObject = il::klass(core, "UnityEngine", "Object");
  U.Vector3 = il::klass(core, "UnityEngine", "Vector3");
  U.Vector2 = il::klass(core, "UnityEngine", "Vector2");
  U.Byte = il::klass("mscorlib", "System", "Byte");
  U.Int32 = il::klass("mscorlib", "System", "Int32");
  U.ImageConversion = il::klass("UnityEngine.ImageConversionModule", "UnityEngine", "ImageConversion");
  U.PaperPlane = il::klass("Assembly-CSharp", "", "PaperPlane");

  U.mesh_ctor = il::method(U.Mesh, ".ctor", {});
  U.mesh_indexFormat = il::method(U.Mesh, "set_indexFormat", {"UnityEngine.Rendering.IndexFormat"});
  U.mesh_vertices = il::method(U.Mesh, "set_vertices", {"UnityEngine.Vector3[]"});
  U.mesh_normals = il::method(U.Mesh, "set_normals", {"UnityEngine.Vector3[]"});
  U.mesh_uv = il::method(U.Mesh, "set_uv", {"UnityEngine.Vector2[]"});
  U.mesh_triangles = il::method(U.Mesh, "set_triangles", {"System.Int32[]"});
  U.mesh_bounds = il::method(U.Mesh, "RecalculateBounds", {});
  U.mesh_recalc_normals = il::method(U.Mesh, "RecalculateNormals", {});
  U.tex_ctor = il::method(U.Texture2D, ".ctor", {"System.Int32", "System.Int32"});
  U.tex_load = il::method(U.ImageConversion, "LoadImage", {"UnityEngine.Texture2D", "System.Byte[]"});
  U.tex_filter = il::method(U.Texture, "set_filterMode", {"UnityEngine.FilterMode"});
  U.tex_wrap = il::method(U.Texture, "set_wrapMode", {"UnityEngine.TextureWrapMode"});
  U.mat_copy = il::method(U.Material, ".ctor", {"UnityEngine.Material"});
  U.mat_maintex = il::method(U.Material, "set_mainTexture", {"UnityEngine.Texture"});
  U.mat_color = il::method(U.Material, "set_color", {"UnityEngine.Color"});
  U.go_ctor = il::method(U.GameObject, ".ctor", {"System.String"});
  U.go_addcomp = il::method(U.GameObject, "AddComponent", {"System.Type"});
  U.go_transform = il::method(U.GameObject, "get_transform", {});
  U.go_get_layer = il::method(U.GameObject, "get_layer", {});
  U.go_set_layer = il::method(U.GameObject, "set_layer", {"System.Int32"});
  U.comp_go = il::method(U.Component, "get_gameObject", {});
  U.obj_name = il::method(U.UObject, "get_name", {});
  U.comp_children = il::method(U.Component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
  U.tr_setparent = il::method(U.Transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
  U.tr_pos = il::method(U.Transform, "set_localPosition", {"UnityEngine.Vector3"});
  U.tr_rot = il::method(U.Transform, "set_localRotation", {"UnityEngine.Quaternion"});
  U.tr_scale = il::method(U.Transform, "set_localScale", {"UnityEngine.Vector3"});
  U.tr_find = il::method(U.Transform, "Find", {"System.String"});
  U.mf_mesh = il::method(U.MeshFilter, "set_sharedMesh", {"UnityEngine.Mesh"});
  U.rend_material = il::method(U.Renderer, "set_sharedMaterial", {"UnityEngine.Material"});
  U.rend_get_material = il::method(U.Renderer, "get_sharedMaterial", {});
  U.rend_enabled = il::method(U.Renderer, "set_enabled", {"System.Boolean"});
  U.find_objects = il::method(U.UObject, "FindObjectsOfType", {"System.Type"});

  struct Need { const char* name; void* p; };
  Need need[] = {{"Mesh", U.Mesh}, {"GameObject", U.GameObject}, {"Transform", U.Transform}, {"PaperPlane", U.PaperPlane},
                 {"Vector3", U.Vector3}, {"Int32", U.Int32}, {"mesh ctor", U.mesh_ctor}, {"set_vertices", U.mesh_vertices},
                 {"set_triangles", U.mesh_triangles}, {"go ctor", U.go_ctor}, {"AddComponent", U.go_addcomp},
                 {"get_transform", U.go_transform}, {"SetParent", U.tr_setparent}, {"set_sharedMesh", U.mf_mesh},
                 {"set_sharedMaterial", U.rend_material}, {"Material copy ctor", U.mat_copy}, {"GetComponentsInChildren", U.comp_children}};
  bool all = true;
  for (const Need& n : need) if (!n.p) { LOGI("missing in game: %s", n.name); all = false; }
  U.ok = all;
  return all;
}

// ========================================================= 5. замена модели =

static std::string g_dir;
static GlbModel g_model;
static void* g_mesh = nullptr;
static void* g_tex = nullptr;
static bool g_loaded = false, g_failed = false;

struct Cfg {
  bool enabled = true;
  float scale = 1, yaw = 0, pitch = 0, roll = 0, ox = 0, oy = 0, oz = 0;
};

// Читает <mod_dir>/settings.json (его пишет меню модов). Нет файла — значения по умолчанию.
static Cfg read_cfg() {
  Cfg c;
  J j;
  std::string text = read_file(g_dir + "/settings.json");
  if (text.empty() || !parse_json(text, j) || j.t != J::Obj) return c;
  auto f = [&](const char* k, float& dst) {
    const J* v = j.get(k);
    if (v && v->t == J::Num) dst = static_cast<float>(v->n);
    else if (v && v->t == J::Str) dst = static_cast<float>(strtod(v->s.c_str(), nullptr));
  };
  if (const J* e = j.get("enabled")) c.enabled = e->t == J::Bool ? e->b : (e->s == "true" || e->s == "1");
  f("scale", c.scale); f("yaw", c.yaw); f("pitch", c.pitch); f("roll", c.roll);
  f("offset_x", c.ox); f("offset_y", c.oy); f("offset_z", c.oz);
  if (c.scale <= 0.001f) c.scale = 1;
  return c;
}

static void* make_mesh(const GlbModel& m) {
  void* mesh = il::new_obj(U.Mesh);
  il::invoke(U.mesh_ctor, mesh, {});
  int fmt32 = 1;  // IndexFormat.UInt32
  il::invoke(U.mesh_indexFormat, mesh, {&fmt32});
  size_t n = m.vertex_count();
  void* va = il::A.array_new(U.Vector3, n);
  memcpy(il::array_data(va), m.pos.data(), n * 12);
  il::invoke(U.mesh_vertices, mesh, {va});
  if (m.nrm.size() == m.pos.size() && U.mesh_normals) {
    void* na = il::A.array_new(U.Vector3, n);
    memcpy(il::array_data(na), m.nrm.data(), n * 12);
    il::invoke(U.mesh_normals, mesh, {na});
  }
  if (m.uv.size() == n * 2 && U.mesh_uv && U.Vector2) {
    void* ua = il::A.array_new(U.Vector2, n);
    memcpy(il::array_data(ua), m.uv.data(), n * 8);
    il::invoke(U.mesh_uv, mesh, {ua});
  }
  void* ia = il::A.array_new(U.Int32, m.idx.size());
  memcpy(il::array_data(ia), m.idx.data(), m.idx.size() * 4);
  il::invoke(U.mesh_triangles, mesh, {ia});
  if (m.nrm.size() != m.pos.size()) il::invoke(U.mesh_recalc_normals, mesh, {});
  il::invoke(U.mesh_bounds, mesh, {});
  il::pin(mesh);
  return mesh;
}

static void* make_texture(const std::string& img) {
  if (!U.Texture2D || !U.tex_ctor || !U.tex_load || !U.Byte || img.empty()) return nullptr;
  void* tex = il::new_obj(U.Texture2D);
  int two = 2;
  il::invoke(U.tex_ctor, tex, {&two, &two});
  void* bytes = il::A.array_new(U.Byte, img.size());
  memcpy(il::array_data(bytes), img.data(), img.size());
  if (!il::unbox_bool(il::invoke(U.tex_load, nullptr, {tex, bytes}))) { LOGI("texture decode failed"); return nullptr; }
  int point = 0, clamp = 1;  // FilterMode.Point — чёткие пиксели Blockbench
  il::invoke(U.tex_filter, tex, {&point});
  il::invoke(U.tex_wrap, tex, {&clamp});
  il::pin(tex);
  return tex;
}

static bool ensure_model() {
  if (g_loaded) return true;
  if (g_failed || !unity_init()) return false;
  g_failed = true;  // если что-то пойдёт не так, второй раз не пробуем
  std::string file = read_file(g_dir + "/model.glb");
  if (file.empty()) { LOGI("model.glb not found in %s", g_dir.c_str()); return false; }
  if (!glb::parse(file, g_model)) { LOGI("model.glb: %s", g_model.error.c_str()); return false; }
  g_mesh = make_mesh(g_model);
  g_tex = make_texture(g_model.image);
  if (!g_mesh) return false;
  LOGI("model loaded: %zu vertices, %zu triangles, texture: %s", g_model.vertex_count(), g_model.idx.size() / 3, g_tex ? "yes" : "no");
  g_failed = false;
  g_loaded = true;
  return true;
}

static bool is_mesh_renderer(void* obj) {
  const char* n = il::A.class_get_name(il::A.object_get_class(obj));
  return n && strstr(n, "MeshRenderer") != nullptr;
}

static void place(void* tr, const Cfg& c) {
  V3 pos{c.ox, c.oy, c.oz}, scl{c.scale, c.scale, c.scale};
  Quat rot = quat_euler(c.pitch, c.yaw, c.roll);
  il::invoke(U.tr_pos, tr, {&pos});
  il::invoke(U.tr_rot, tr, {&rot});
  il::invoke(U.tr_scale, tr, {&scl});
}

static const char* OUR_NAME = "SbaCustomPlane";

static bool is_ours(void* go) {
  if (!go || !U.obj_name) return false;
  void* s = il::invoke(U.obj_name, go, {});
  if (!s) return false;
  const uint16_t* ch = il::A.string_chars(s);
  int32_t len = il::A.string_length(s);
  if (len != static_cast<int32_t>(strlen(OUR_NAME))) return false;
  for (int32_t i = 0; i < len; ++i) if (ch[i] != static_cast<uint16_t>(OUR_NAME[i])) return false;
  return true;
}

// Включает/выключает меши: наш объект получает ours_on, родные меши самолёта — обратное.
static void set_renderers(void* plane_mesh_tr, bool ours_on) {
  if (!U.Renderer || !U.rend_enabled) return;
  bool incl = true;
  void* arr = il::invoke(U.comp_children, plane_mesh_tr, {il::type_obj(U.Renderer), &incl});
  size_t n = il::array_len(arr);
  void** items = static_cast<void**>(il::array_data(arr));
  for (size_t i = 0; i < n; ++i) {
    if (!items[i] || !is_mesh_renderer(items[i])) continue;
    bool ours = is_ours(il::invoke(U.comp_go, items[i], {}));
    bool on = ours ? ours_on : !ours_on;
    il::invoke(U.rend_enabled, items[i], {&on});
  }
}

extern "C" void sba_plane_apply(void* plane) {
  if (!plane || !ensure_model()) return;
  Cfg c = read_cfg();
  void* pm = il::field_ptr(plane, U.PaperPlane, "planeMesh");  // Transform с моделью самолёта
  if (!pm) { LOGI("planeMesh is null"); return; }

  void* existing = il::invoke(U.tr_find, pm, {il::str(OUR_NAME)});
  if (existing) {  // уже заменено: обновляем настройки и состояние мешей
    place(existing, c);
    set_renderers(pm, c.enabled);
    return;
  }
  if (!c.enabled) return;

  // Материал: копия материала самолёта, чтобы сохранить его шейдер.
  void* src = il::field_ptr(plane, U.PaperPlane, "planeMaterial");
  if (!src) {
    bool incl = true;
    void* arr = il::invoke(U.comp_children, pm, {il::type_obj(U.Renderer), &incl});
    void** items = static_cast<void**>(il::array_data(arr));
    for (size_t i = 0; i < il::array_len(arr) && !src; ++i)
      if (items[i] && is_mesh_renderer(items[i])) src = il::invoke(U.rend_get_material, items[i], {});
  }
  void* mat = nullptr;
  if (src) {
    mat = il::new_obj(U.Material);
    il::invoke(U.mat_copy, mat, {src});
    if (g_tex && U.mat_maintex) il::invoke(U.mat_maintex, mat, {g_tex});
    else if (U.mat_color) il::invoke(U.mat_color, mat, {g_model.color});
  } else {
    LOGI("no source material found, the model may look wrong");
  }

  set_renderers(pm, true);  // прячем родные меши (нашего объекта ещё нет)

  void* go = il::new_obj(U.GameObject);
  il::invoke(U.go_ctor, go, {il::str(OUR_NAME)});
  void* mf = il::invoke(U.go_addcomp, go, {il::type_obj(U.MeshFilter)});
  void* mr = il::invoke(U.go_addcomp, go, {il::type_obj(U.MeshRenderer)});
  il::invoke(U.mf_mesh, mf, {g_mesh});
  if (mat) il::invoke(U.rend_material, mr, {mat});

  void* tr = il::invoke(U.go_transform, go, {});
  bool keep_world = false;
  il::invoke(U.tr_setparent, tr, {pm, &keep_world});
  place(tr, c);
  if (U.comp_go && U.go_get_layer && U.go_set_layer) {  // тот же слой, что у самолёта
    void* pgo = il::invoke(U.comp_go, pm, {});
    void* layer = il::invoke(U.go_get_layer, pgo, {});
    if (layer) il::invoke(U.go_set_layer, go, {layer ? il::A.object_unbox(layer) : nullptr});
  }
  il::pin(go);
  LOGI("plane model replaced");
}

extern "C" int sba_plane_scan() {
  if (!ensure_model()) return 0;
  void* arr = il::invoke(U.find_objects, nullptr, {il::type_obj(U.PaperPlane)});
  size_t n = il::array_len(arr);
  void** items = static_cast<void**>(il::array_data(arr));
  for (size_t i = 0; i < n; ++i) sba_plane_apply(items[i]);
  LOGI("planes found: %zu", n);
  return static_cast<int>(n);
}

extern "C" void sba_plane_init(const char* mod_dir) {
  g_dir = mod_dir ? mod_dir : "";
  while (g_dir.size() > 1 && g_dir.back() == '/') g_dir.pop_back();
  g_loaded = g_failed = false;
  LOGI("plane_skin ready, dir: %s", g_dir.c_str());
}

// ====================================================== 6. связка с загрузчиком
// Этот раздел зависит от твоего загрузчика (Bearite). Нужно два вызова:
//
//   1) при загрузке мода:            sba_plane_init(mod_dir);
//   2) когда на сцене появляется самолёт — самый простой способ: в момент
//      создания меню паузы (где у тебя build(pause_menu)) вызывать
//                                    sba_plane_scan();
//      Он найдёт все PaperPlane на сцене и заменит модель.
//
// Точнее — повесить хук на PaperPlane.Start (RVA 0x1A89ABC) и после оригинала
// вызвать sba_plane_apply(this). Как регистрировать хук — в твоём hooks.inc.
