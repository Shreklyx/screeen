#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ss {

static const std::string kEmpty;
static const Json kNull;
static const Json::Array kNoItems;
static const Json::Object kNoFields;

const std::string& Json::str() const { return type_ == Type::String ? str_ : kEmpty; }
const Json::Array& Json::items() const { return type_ == Type::Array ? *arr_ : kNoItems; }
const Json::Object& Json::fields() const { return type_ == Type::Object ? *obj_ : kNoFields; }

const Json& Json::operator[](const std::string& key) const {
  if (type_ != Type::Object) return kNull;
  auto it = obj_->find(key);
  return it == obj_->end() ? kNull : it->second;
}

bool Json::has(const std::string& key) const { return type_ == Type::Object && obj_->count(key) > 0; }

Json& Json::set(const std::string& key, Json value) {
  if (type_ != Type::Object) *this = object();
  else if (obj_.use_count() > 1) obj_ = std::make_shared<Object>(*obj_); // copy on write
  (*obj_)[key] = std::move(value);
  return *this;
}

Json& Json::push(Json value) {
  if (type_ != Type::Array) *this = array();
  else if (arr_.use_count() > 1) arr_ = std::make_shared<Array>(*arr_);
  arr_->push_back(std::move(value));
  return *this;
}

size_t Json::size() const {
  if (type_ == Type::Array) return arr_->size();
  if (type_ == Type::Object) return obj_->size();
  return 0;
}

/* ------------------------------------------------ writing */

static void dumpString(const std::string& s, std::string& out) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  out += '"';
}

void Json::dumpTo(std::string& out) const {
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
      if (!std::isfinite(num_)) { out += "null"; break; }
      char buf[32];
      if (num_ == std::floor(num_) && std::fabs(num_) < 9007199254740992.0) std::snprintf(buf, sizeof buf, "%.0f", num_);
      else std::snprintf(buf, sizeof buf, "%.17g", num_);
      out += buf;
      break;
    }
    case Type::String: dumpString(str_, out); break;
    case Type::Array: {
      out += '[';
      bool first = true;
      for (const Json& v : *arr_) {
        if (!first) out += ',';
        first = false;
        v.dumpTo(out);
      }
      out += ']';
      break;
    }
    case Type::Object: {
      out += '{';
      bool first = true;
      for (const auto& kv : *obj_) {
        if (!first) out += ',';
        first = false;
        dumpString(kv.first, out);
        out += ':';
        kv.second.dumpTo(out);
      }
      out += '}';
      break;
    }
  }
}

std::string Json::dump() const {
  std::string out;
  dumpTo(out);
  return out;
}

/* ------------------------------------------------ reading */

namespace {
struct Parser {
  const std::string& s;
  size_t i = 0;
  int depth = 0;
  bool ok = true;

  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
  bool lit(const char* w) {
    size_t n = std::char_traits<char>::length(w);
    if (s.compare(i, n, w) != 0) return false;
    i += n;
    return true;
  }
  static void utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | cp >> 6); out += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { out += char(0xE0 | cp >> 12); out += char(0x80 | (cp >> 6 & 63)); out += char(0x80 | (cp & 63)); }
    else { out += char(0xF0 | cp >> 18); out += char(0x80 | (cp >> 12 & 63)); out += char(0x80 | (cp >> 6 & 63)); out += char(0x80 | (cp & 63)); }
  }
  bool hex4(uint32_t& v) {
    if (i + 4 > s.size()) return false;
    v = 0;
    for (int k = 0; k < 4; k++) {
      char c = s[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool string(std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    i++;
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') return true;
      if (c != '\\') { out += c; continue; }
      if (i >= s.size()) return false;
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            i += 2;
            uint32_t lo;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  Json value() {
    if (++depth > 64) { ok = false; return Json(); }
    ws();
    Json out;
    if (i >= s.size()) ok = false;
    else if (s[i] == '{') {
      i++;
      out = Json::object();
      ws();
      if (i < s.size() && s[i] == '}') i++;
      else {
        for (;;) {
          ws();
          std::string key;
          if (!string(key)) { ok = false; break; }
          ws();
          if (i >= s.size() || s[i] != ':') { ok = false; break; }
          i++;
          Json v = value();
          if (!ok) break;
          out.set(key, std::move(v));
          ws();
          if (i < s.size() && s[i] == ',') { i++; continue; }
          if (i < s.size() && s[i] == '}') { i++; break; }
          ok = false;
          break;
        }
      }
    } else if (s[i] == '[') {
      i++;
      out = Json::array();
      ws();
      if (i < s.size() && s[i] == ']') i++;
      else {
        for (;;) {
          Json v = value();
          if (!ok) break;
          out.push(std::move(v));
          ws();
          if (i < s.size() && s[i] == ',') { i++; continue; }
          if (i < s.size() && s[i] == ']') { i++; break; }
          ok = false;
          break;
        }
      }
    } else if (s[i] == '"') {
      std::string str;
      if (string(str)) out = Json(std::move(str));
      else ok = false;
    } else if (lit("true")) out = Json(true);
    else if (lit("false")) out = Json(false);
    else if (lit("null")) out = Json();
    else {
      const char* start = s.c_str() + i;
      char* end = nullptr;
      double d = std::strtod(start, &end);
      if (end == start) ok = false;
      else { i += size_t(end - start); out = Json(d); }
    }
    depth--;
    return out;
  }
};
}  // namespace

Json Json::parse(const std::string& text) {
  Parser p{text};
  Json v = p.value();
  p.ws();
  if (!p.ok || p.i != text.size()) return Json();
  return v;
}

}  // namespace ss
