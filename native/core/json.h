// Screen Share native core – a small JSON value for the signaling messages and the UI events.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ss {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : type_(Type::Bool), bool_(b) {}
  Json(int n) : type_(Type::Number), num_(n) {}
  Json(unsigned n) : type_(Type::Number), num_(n) {}
  Json(long n) : type_(Type::Number), num_(double(n)) {}
  Json(long long n) : type_(Type::Number), num_(double(n)) {}
  Json(unsigned long n) : type_(Type::Number), num_(double(n)) {}
  Json(unsigned long long n) : type_(Type::Number), num_(double(n)) {}
  Json(double n) : type_(Type::Number), num_(n) {}
  Json(const char* s) : type_(Type::String), str_(s) {}
  Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
  Json(Array a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
  Json(Object o) : type_(Type::Object), obj_(std::make_shared<Object>(std::move(o))) {}

  static Json array() { return Json(Array{}); }
  static Json object() { return Json(Object{}); }
  /** null on invalid input */
  static Json parse(const std::string& text);

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  bool b(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
  double num(double fallback = 0) const { return type_ == Type::Number ? num_ : fallback; }
  const std::string& str() const;
  std::string str(const std::string& fallback) const { return type_ == Type::String ? str_ : fallback; }
  const Array& items() const;
  const Object& fields() const;

  /** Object member (null when missing or not an object). */
  const Json& operator[](const std::string& key) const;
  /** Sets an object member (makes this an object). */
  Json& set(const std::string& key, Json value);
  /** Appends to an array (makes this an array). */
  Json& push(Json value);
  size_t size() const;
  bool has(const std::string& key) const;

  std::string dump() const;

 private:
  void dumpTo(std::string& out) const;
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::shared_ptr<Array> arr_;
  std::shared_ptr<Object> obj_;
};

}  // namespace ss
