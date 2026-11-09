// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matteo Scalia

// Schema-driven prompt walker: turns a JSON Schema into a sequence of prompts
// and builds the value. Same traversal, validation and back-navigation rules as
// the TypeScript implementation.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"
#include "schema.hpp"
#include "tui.hpp"

namespace cform {

struct Segment {
  bool numeric = false;
  long index = 0;
  std::string name;
};

struct Path {
  std::vector<Segment> segments;
  Path with(const std::string& name) const;
  Path with(long index) const;
};

std::string path_label(const Path& path);

struct ValueOptions {
  std::optional<Json> prefill;
  bool resume = false;
  bool required = false;
  /// seed objects/records from `prefill` (and preserve unknown keys), while
  /// still asking every known field from the first one
  bool seed = false;
};

struct PropertyOptions {
  bool ask = true;
  std::optional<Json> prefill;
  bool resume = false;
  bool seed = false;
};

struct Outcome {
  enum class Kind { Value, Back, Skip };
  Kind kind = Kind::Value;
  Json value;

  static Outcome of(Json value);
  static Outcome back();
  static Outcome skip();
};

class Walker {
 public:
  Walker(const Json& root, Prompter& prompter);

  Outcome value(const Json& schema, const Path& path,
                const ValueOptions& options = {});

  /// called with every value that is finalized during a walk; lets the tree
  /// persist answers as they are given (so `ctrl+f` keeps them)
  using ProgressCallback = std::function<void(const Path&, const Json&)>;
  void set_progress(ProgressCallback callback) {
    progress_ = std::move(callback);
  }

 private:
  Outcome dispatch(const Json& schema, const Path& path,
                   const ValueOptions& options);
  Outcome object(const Json& schema, const Path& path,
                 const ValueOptions& options);
  Outcome array(const Json& schema, const Path& path,
                const ValueOptions& options);
  Outcome record(const Json& schema, const Path& path,
                 const ValueOptions& options);
  Outcome union_value(const std::vector<Json>& variants, const Path& path,
                      const ValueOptions& options);
  Outcome leaf(const Json& schema, const Path& path,
               const ValueOptions& options);
  Outcome property(const Json& parent, const std::string& key,
                   const Path& path, bool required, PropertyOptions options);

  // Schema-less editing: when the schema does not describe an object (or the
  // value at all), the members are prompted by inferring their types from the
  // document instead of asking for raw JSON.
  Outcome inferred(const Path& path, const std::optional<Json>& prefill);
  Outcome inferred_object(const Path& path, const Json& prefill);
  Outcome inferred_array(const Path& path, const Json& prefill);
  Outcome json_prompt(const Path& path, const std::optional<Json>& prefill);
  Json infer_schema(const Json& value) const;

  std::vector<std::string> history_values(const Json& raw_schema) const;
  void remember_value(const Json& raw_schema, const Json& value);

  const Json& root_;
  Prompter& prompter_;
  ProgressCallback progress_;
  std::map<std::string, std::vector<std::pair<std::string, long>>> history_;
};

}  // namespace cform
