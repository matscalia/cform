// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matteo Scalia

// Minimal ordered JSON value used by cform.
//
// simdjson is used to parse (schema, `(json)` prompt answers); values are
// copied into this model so references can be resolved, `allOf` folded and the
// final answer serialized with a stable key order.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cform {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  bool integer = false;
  std::string string;
  std::vector<Json> array;
  std::vector<std::pair<std::string, Json>> object;

  Json() = default;

  static Json null();
  static Json boolean_value(bool value);
  static Json number_value(double value, bool is_integer);
  static Json string_value(std::string value);
  static Json array_value(std::vector<Json> items = {});
  static Json object_value(
      std::vector<std::pair<std::string, Json>> entries = {});

  bool is_null() const { return type == Type::Null; }
  bool is_bool() const { return type == Type::Bool; }
  bool is_number() const { return type == Type::Number; }
  bool is_string() const { return type == Type::String; }
  bool is_array() const { return type == Type::Array; }
  bool is_object() const { return type == Type::Object; }

  const Json* get(std::string_view key) const;
  Json* find(std::string_view key);
  bool has(std::string_view key) const { return get(key) != nullptr; }
  void set(std::string key, Json value);
  void erase(std::string_view key);
  std::size_t size() const { return object.size(); }

  const Json* at(std::size_t index) const;
  void push(Json value) { array.push_back(std::move(value)); }

  // simdjson-backed parsing; on failure `error` carries the parser message.
  static std::optional<Json> parse(std::string_view text, std::string& error);

  std::string to_pretty(int indent = 2) const;
  std::string to_compact() const;

 private:
  void write(std::string& out, int indent, int depth) const;
  void write_compact(std::string& out) const;
};

std::string json_escape(std::string_view text);
std::string number_to_string(double value, bool integer);

}  // namespace cform
