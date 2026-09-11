#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wendoo::test {

/**
 * One parsed JSON value. A scalar carries its own payload; an array carries its
 * elements and an object its members in document order. Every accessor that
 * names a kind requires that kind and throws `std::runtime_error` otherwise.
 */
class JsonValue {
public:
  /** The kinds a parsed JSON value can carry. */
  enum class Kind { Null, Bool, Number, String, Array, Object };

  /** A null value. */
  JsonValue() = default;

  /** Kind of the parsed value. */
  Kind kind() const { return kind_; }

  /** The number payload. Requires kind `Number`. */
  double number() const {
    require(Kind::Number);
    return number_;
  }

  /** The boolean payload. Requires kind `Bool`. */
  bool boolean() const {
    require(Kind::Bool);
    return bool_;
  }

  /** The decoded string payload. Requires kind `String`. */
  const std::string& string() const {
    require(Kind::String);
    return string_;
  }

  /** The elements, in document order. Requires kind `Array`. */
  const std::vector<JsonValue>& elements() const {
    require(Kind::Array);
    return elements_;
  }

  /**
   * The member named `name`. Requires kind `Object`; throws when the object
   * carries no member of that name.
   */
  const JsonValue& member(const std::string& name) const {
    require(Kind::Object);
    for (const std::pair<std::string, JsonValue>& entry : members_) {
      if (entry.first == name) {
        return entry.second;
      }
    }
    throw std::runtime_error("json object has no member '" + name + "'");
  }

  /** A number value carrying `value`. */
  static JsonValue makeNumber(double value) {
    JsonValue out;
    out.kind_ = Kind::Number;
    out.number_ = value;
    return out;
  }

  /** A boolean value carrying `value`. */
  static JsonValue makeBool(bool value) {
    JsonValue out;
    out.kind_ = Kind::Bool;
    out.bool_ = value;
    return out;
  }

  /** A string value carrying the decoded `value`. */
  static JsonValue makeString(std::string value) {
    JsonValue out;
    out.kind_ = Kind::String;
    out.string_ = std::move(value);
    return out;
  }

  /** An array value carrying `elements` in document order. */
  static JsonValue makeArray(std::vector<JsonValue> elements) {
    JsonValue out;
    out.kind_ = Kind::Array;
    out.elements_ = std::move(elements);
    return out;
  }

  /** An object value carrying `members` in document order. */
  static JsonValue makeObject(std::vector<std::pair<std::string, JsonValue>> members) {
    JsonValue out;
    out.kind_ = Kind::Object;
    out.members_ = std::move(members);
    return out;
  }

private:
  void require(Kind kind) const {
    if (kind_ != kind) {
      throw std::runtime_error("json value is not of the requested kind");
    }
  }

  Kind kind_ = Kind::Null;
  bool bool_ = false;
  double number_ = 0;
  std::string string_;
  std::vector<JsonValue> elements_;
  std::vector<std::pair<std::string, JsonValue>> members_;
};

namespace json_detail {

/** Recursive-descent cursor over a JSON document. */
class JsonParser {
public:
  explicit JsonParser(const std::string& text) : text_(text) {}

  JsonValue parseDocument() {
    skipSpace();
    JsonValue value = parseValue();
    skipSpace();
    if (pos_ != text_.size()) {
      fail("trailing content after the top-level value");
    }
    return value;
  }

private:
  [[noreturn]] void fail(const std::string& what) const {
    throw std::runtime_error("malformed json at offset " + std::to_string(pos_) + ": " + what);
  }

  char peek() const {
    if (pos_ >= text_.size()) {
      fail("unexpected end of input");
    }
    return text_[pos_];
  }

  void expect(char c) {
    if (peek() != c) {
      fail(std::string("expected '") + c + "'");
    }
    pos_++;
  }

  void skipSpace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        return;
      }
      pos_++;
    }
  }

  void expectWord(const char* word) {
    for (const char* p = word; *p != '\0'; p++) {
      if (peek() != *p) {
        fail(std::string("expected '") + word + "'");
      }
      pos_++;
    }
  }

  JsonValue parseValue() {
    switch (peek()) {
    case '{':
      return parseObject();
    case '[':
      return parseArray();
    case '"':
      return JsonValue::makeString(parseString());
    case 't':
      expectWord("true");
      return JsonValue::makeBool(true);
    case 'f':
      expectWord("false");
      return JsonValue::makeBool(false);
    case 'n':
      expectWord("null");
      return JsonValue();
    default:
      return JsonValue::makeNumber(parseNumber());
    }
  }

  JsonValue parseObject() {
    expect('{');
    std::vector<std::pair<std::string, JsonValue>> members;
    skipSpace();
    if (peek() == '}') {
      pos_++;
      return JsonValue::makeObject(std::move(members));
    }
    while (true) {
      skipSpace();
      std::string name = parseString();
      skipSpace();
      expect(':');
      skipSpace();
      members.emplace_back(std::move(name), parseValue());
      skipSpace();
      if (peek() == ',') {
        pos_++;
        continue;
      }
      expect('}');
      return JsonValue::makeObject(std::move(members));
    }
  }

  JsonValue parseArray() {
    expect('[');
    std::vector<JsonValue> elements;
    skipSpace();
    if (peek() == ']') {
      pos_++;
      return JsonValue::makeArray(std::move(elements));
    }
    while (true) {
      skipSpace();
      elements.push_back(parseValue());
      skipSpace();
      if (peek() == ',') {
        pos_++;
        continue;
      }
      expect(']');
      return JsonValue::makeArray(std::move(elements));
    }
  }

  std::string parseString() {
    expect('"');
    std::string out;
    while (true) {
      const char c = peek();
      pos_++;
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      const char escape = peek();
      pos_++;
      switch (escape) {
      case '"':
      case '\\':
      case '/':
        out.push_back(escape);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        fail("unsupported string escape");
      }
    }
  }

  double parseNumber() {
    const size_t start = pos_;
    size_t consumed = 0;
    double value = 0;
    try {
      value = std::stod(text_.substr(start), &consumed);
    } catch (const std::exception&) {
      fail("expected a number");
    }
    if (consumed == 0) {
      fail("expected a number");
    }
    pos_ = start + consumed;
    return value;
  }

  const std::string& text_;
  size_t pos_ = 0;
};

} // namespace json_detail

/**
 * Parses `text` as a JSON document and returns its top-level value. Throws
 * `std::runtime_error` describing the offset when the text is not valid JSON.
 *
 * @param text - Complete JSON document.
 */
inline JsonValue parseJson(const std::string& text) {
  return json_detail::JsonParser(text).parseDocument();
}

} // namespace wendoo::test
