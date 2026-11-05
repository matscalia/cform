#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

#ifdef CFORM_USE_SIMDJSON
#include <simdjson.h>
#else
#include "json_parse.hpp"
#endif

namespace cform {

Json Json::null() { return Json(); }

Json Json::boolean_value(bool value) {
  Json json;
  json.type = Type::Bool;
  json.boolean = value;
  return json;
}

Json Json::number_value(double value, bool is_integer) {
  Json json;
  json.type = Type::Number;
  json.number = value;
  json.integer = is_integer;
  return json;
}

Json Json::string_value(std::string value) {
  Json json;
  json.type = Type::String;
  json.string = std::move(value);
  return json;
}

Json Json::array_value(std::vector<Json> items) {
  Json json;
  json.type = Type::Array;
  json.array = std::move(items);
  return json;
}

Json Json::object_value(std::vector<std::pair<std::string, Json>> entries) {
  Json json;
  json.type = Type::Object;
  json.object = std::move(entries);
  return json;
}

const Json* Json::get(std::string_view key) const {
  for (const auto& [name, value] : object) {
    if (name == key) return &value;
  }
  return nullptr;
}

Json* Json::find(std::string_view key) {
  for (auto& [name, value] : object) {
    if (name == key) return &value;
  }
  return nullptr;
}

void Json::set(std::string key, Json value) {
  if (auto* existing = find(key)) {
    *existing = std::move(value);
    return;
  }
  object.emplace_back(std::move(key), std::move(value));
}

void Json::erase(std::string_view key) {
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (it->first == key) {
      object.erase(it);
      return;
    }
  }
}

const Json* Json::at(std::size_t index) const {
  return index < array.size() ? &array[index] : nullptr;
}

#ifdef CFORM_USE_SIMDJSON
namespace {

Json from_simdjson(const simdjson::dom::element& element) {
  using simdjson::dom::element_type;
  switch (element.type()) {
    case element_type::NULL_VALUE:
      return Json::null();
    case element_type::BOOL: {
      bool value = false;
      const auto error = element.get_bool().get(value);
      return error == simdjson::SUCCESS ? Json::boolean_value(value)
                                        : Json::null();
    }
    case element_type::INT64: {
      int64_t value = 0;
      const auto error = element.get_int64().get(value);
      return error == simdjson::SUCCESS
                 ? Json::number_value(static_cast<double>(value), true)
                 : Json::null();
    }
    case element_type::UINT64: {
      uint64_t value = 0;
      const auto error = element.get_uint64().get(value);
      return error == simdjson::SUCCESS
                 ? Json::number_value(static_cast<double>(value), true)
                 : Json::null();
    }
    case element_type::BIGINT: {
      std::string_view value;
      const auto error = element.get_bigint().get(value);
      return error == simdjson::SUCCESS ? Json::string_value(std::string(value))
                                        : Json::null();
    }
    case element_type::DOUBLE: {
      double value = 0;
      const auto error = element.get_double().get(value);
      return error == simdjson::SUCCESS ? Json::number_value(value, false)
                                        : Json::null();
    }
    case element_type::STRING: {
      std::string_view value;
      const auto error = element.get_string().get(value);
      return error == simdjson::SUCCESS ? Json::string_value(std::string(value))
                                        : Json::null();
    }
    case element_type::ARRAY: {
      std::vector<Json> items;
      simdjson::dom::array array;
      if (element.get_array().get(array) == simdjson::SUCCESS) {
        for (const auto& item : array) items.push_back(from_simdjson(item));
      }
      return Json::array_value(std::move(items));
    }
    case element_type::OBJECT: {
      std::vector<std::pair<std::string, Json>> entries;
      simdjson::dom::object object;
      if (element.get_object().get(object) == simdjson::SUCCESS) {
        for (const auto& field : object) {
          entries.emplace_back(std::string(field.key),
                               from_simdjson(field.value));
        }
      }
      return Json::object_value(std::move(entries));
    }
  }
  return Json::null();
}

}  // namespace

std::optional<Json> Json::parse(std::string_view text, std::string& error) {
  simdjson::dom::parser parser;
  simdjson::padded_string padded(text);
  auto result = parser.parse(padded);
  if (result.error() != simdjson::SUCCESS) {
    error = simdjson::error_message(result.error());
    return std::nullopt;
  }
  return from_simdjson(result.value());
}

#else

std::optional<Json> Json::parse(std::string_view text, std::string& error) {
  return parse_json_fallback(text, error);
}

#endif

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (unsigned char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          out += buffer;
        } else {
          out += static_cast<char>(ch);
        }
    }
  }
  return out;
}

std::string number_to_string(double value, bool integer) {
  if (integer) return std::to_string(static_cast<long long>(value));
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc()) return "0";
  return std::string(buffer, result.ptr);
}

void Json::write(std::string& out, int indent, int depth) const {
  const std::string pad(static_cast<std::size_t>(indent * depth), ' ');
  const std::string child_pad(static_cast<std::size_t>(indent * (depth + 1)), ' ');
  switch (type) {
    case Type::Null:
      out += "null";
      return;
    case Type::Bool:
      out += boolean ? "true" : "false";
      return;
    case Type::Number:
      out += number_to_string(number, integer);
      return;
    case Type::String:
      out += '"';
      out += json_escape(string);
      out += '"';
      return;
    case Type::Array:
      if (array.empty()) {
        out += "[]";
        return;
      }
      out += "[\n";
      for (std::size_t index = 0; index < array.size(); index++) {
        out += child_pad;
        array[index].write(out, indent, depth + 1);
        if (index + 1 < array.size()) out += ',';
        out += '\n';
      }
      out += pad;
      out += ']';
      return;
    case Type::Object:
      if (object.empty()) {
        out += "{}";
        return;
      }
      out += "{\n";
      for (std::size_t index = 0; index < object.size(); index++) {
        out += child_pad;
        out += '"';
        out += json_escape(object[index].first);
        out += "\": ";
        object[index].second.write(out, indent, depth + 1);
        if (index + 1 < object.size()) out += ',';
        out += '\n';
      }
      out += pad;
      out += '}';
      return;
  }
}

void Json::write_compact(std::string& out) const {
  switch (type) {
    case Type::Null:
      out += "null";
      return;
    case Type::Bool:
      out += boolean ? "true" : "false";
      return;
    case Type::Number:
      out += number_to_string(number, integer);
      return;
    case Type::String:
      out += '"';
      out += json_escape(string);
      out += '"';
      return;
    case Type::Array:
      out += '[';
      for (std::size_t index = 0; index < array.size(); index++) {
        if (index > 0) out += ',';
        array[index].write_compact(out);
      }
      out += ']';
      return;
    case Type::Object:
      out += '{';
      for (std::size_t index = 0; index < object.size(); index++) {
        if (index > 0) out += ',';
        out += '"';
        out += json_escape(object[index].first);
        out += "\":";
        object[index].second.write_compact(out);
      }
      out += '}';
      return;
  }
}

std::string Json::to_pretty(int indent) const {
  std::string out;
  write(out, indent, 0);
  return out;
}

std::string Json::to_compact() const {
  std::string out;
  write_compact(out);
  return out;
}

}  // namespace cform
