// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matteo Scalia

#include "walker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

namespace cform {

namespace {

bool json_equals(const Json& a, const Json& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case Json::Type::Null:
      return true;
    case Json::Type::Bool:
      return a.boolean == b.boolean;
    case Json::Type::Number:
      return a.number == b.number;
    case Json::Type::String:
      return a.string == b.string;
    case Json::Type::Array:
      if (a.array.size() != b.array.size()) return false;
      for (std::size_t index = 0; index < a.array.size(); index++) {
        if (!json_equals(a.array[index], b.array[index])) return false;
      }
      return true;
    case Json::Type::Object:
      if (a.object.size() != b.object.size()) return false;
      for (const auto& [key, value] : a.object) {
        const Json* other = b.get(key);
        if (!other || !json_equals(value, *other)) return false;
      }
      return true;
  }
  return false;
}

std::string stringify_for_label(const Json& value) {
  if (value.is_string()) return value.string;
  if (value.is_number()) return number_to_string(value.number, value.integer);
  if (value.is_bool()) return value.boolean ? "true" : "false";
  if (value.is_null()) return "null";
  return value.to_compact();
}

bool parse_number(const std::string& raw, Json& out) {
  const std::string text = trim(raw);
  char* end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (text.empty() || end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) {
    return false;
  }
  const bool integer = std::floor(parsed) == parsed && std::fabs(parsed) <= 9.007199254740991e15;
  out = Json::number_value(parsed, integer);
  return true;
}

std::string join_types(const SchemaTypes& types) {
  std::string out;
  for (std::size_t index = 0; index < types.size(); index++) {
    if (index > 0) out += " | ";
    out += types[index];
  }
  return out;
}

long prefill_variant(const std::vector<Json>& variants,
                     const std::optional<Json>& prefill, const Json& root) {
  if (prefill && !prefill->is_null()) {
    for (std::size_t index = 0; index < variants.size(); index++) {
      const std::string type = primary_type(variants[index], root);
      const bool matches =
          (type == "array" && prefill->is_array()) ||
          (type == "object" && prefill->is_object()) ||
          (type == "string" && prefill->is_string()) ||
          ((type == "number" || type == "integer") && prefill->is_number()) ||
          (type == "boolean" && prefill->is_bool());
      if (matches) return static_cast<long>(index);
    }
  }
  return 0;
}

std::optional<Json> copy_prefill(const std::optional<Json>& prefill) {
  return prefill;
}

}  // namespace

Path Path::with(const std::string& name) const {
  Path next = *this;
  Segment segment;
  segment.name = name;
  next.segments.push_back(std::move(segment));
  return next;
}

Path Path::with(long index) const {
  Path next = *this;
  Segment segment;
  segment.numeric = true;
  segment.index = index;
  next.segments.push_back(std::move(segment));
  return next;
}

std::string path_label(const Path& path) {
  std::string label;
  for (const Segment& segment : path.segments) {
    if (segment.numeric) {
      label += "[" + std::to_string(segment.index) + "]";
    } else {
      if (!label.empty()) label += '.';
      label += segment.name;
    }
  }
  return label.empty() ? "<root>" : label;
}

Outcome Outcome::of(Json value) {
  Outcome outcome;
  outcome.kind = Kind::Value;
  outcome.value = std::move(value);
  return outcome;
}

Outcome Outcome::back() {
  Outcome outcome;
  outcome.kind = Kind::Back;
  return outcome;
}

Outcome Outcome::skip() {
  Outcome outcome;
  outcome.kind = Kind::Skip;
  return outcome;
}

Walker::Walker(const Json& root, Prompter& prompter)
    : root_(root), prompter_(prompter) {}

std::vector<std::string> Walker::history_values(const Json& raw_schema) const {
  const Json* ref = raw_schema.get("$ref");
  if (!ref || !ref->is_string()) return {};
  const auto entry = history_.find(ref->string);
  if (entry == history_.end()) return {};
  std::vector<std::pair<std::string, long>> counts = entry->second;
  std::stable_sort(counts.begin(), counts.end(),
                   [](const auto& a, const auto& b) { return a.second > b.second; });
  std::vector<std::string> values;
  values.reserve(counts.size());
  for (const auto& [value, count] : counts) {
    (void)count;
    values.push_back(value);
  }
  return values;
}

void Walker::remember_value(const Json& raw_schema, const Json& value) {
  const Json* ref = raw_schema.get("$ref");
  if (!ref || !ref->is_string()) return;
  if (!value.is_string() && !value.is_number()) return;
  const std::string text =
      value.is_string() ? value.string
                        : number_to_string(value.number, value.integer);
  if (trim(text).empty()) return;
  auto& entries = history_[ref->string];
  for (auto& [existing, count] : entries) {
    if (existing == text) {
      count++;
      return;
    }
  }
  entries.emplace_back(text, 1);
}

Outcome Walker::leaf(const Json& schema, const Path& path,
                     const ValueOptions& options) {
  const Json node = resolve_ref(schema, root_);
  const std::string label = path_label(path);
  const auto hint = full_hint(node);
  const auto schema_default = default_string(schema, node);
  const std::vector<std::string> known = history_values(schema);
  const std::vector<std::string>& suggest = known;
  // a value already present in the document overrides the autocomplete default
  const std::optional<std::string> prefilled =
      options.prefill ? prefill_string(*options.prefill) : std::nullopt;
  const std::optional<std::string> default_value =
      prefilled ? prefilled
                : (known.empty() ? schema_default
                                 : std::optional<std::string>(known.front()));
  const std::optional<std::string>& initial = prefilled;

  const Json* format = node.get("format");
  const bool multiline =
      primary_type(node, root_) == "string" && format && format->is_string() &&
      (format->string == "multiline" || format->string == "textarea");
  if (multiline) {
    TextPrompt prompt;
    prompt.message = label;
    prompt.hint = hint;
    prompt.multiline = true;
    prompt.placeholder = schema_default;
    prompt.default_value = schema_default;
    prompt.initial_value = initial;
    prompt.validate = validate_string(node, root_);
    const auto reply = prompter_.text(prompt);
    if (reply.back) return Outcome::back();
    return Outcome::of(Json::string_value(reply.value));
  }

  if (const Json* constant = node.get("const")) return Outcome::of(*constant);

  if (const Json* enum_values = node.get("enum");
      enum_values && enum_values->is_array()) {
    if (enum_values->array.empty()) return Outcome::skip();
    if (enum_values->array.size() == 1) {
      return Outcome::of(enum_values->array.front());
    }
    std::size_t initial_index = 0;
    bool found = false;
    if (options.prefill && !found) {
      for (std::size_t index = 0; index < enum_values->array.size(); index++) {
        if (json_equals(*options.prefill, enum_values->array[index])) {
          initial_index = index;
          found = true;
          break;
        }
      }
    }
    if (!found && !known.empty()) {
      for (std::size_t index = 0; index < enum_values->array.size(); index++) {
        if (stringify_for_label(enum_values->array[index]) == known.front()) {
          initial_index = index;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      const Json* fallback = node.get("default");
      if (fallback) {
        for (std::size_t index = 0; index < enum_values->array.size(); index++) {
          if (json_equals(*fallback, enum_values->array[index])) {
            initial_index = index;
            break;
          }
        }
      }
    }
    SelectPrompt prompt;
    prompt.message = "select " + label;
    prompt.hint = hint;
    for (const Json& value : enum_values->array) {
      Option option;
      option.label = stringify_for_label(value);
      prompt.options.push_back(std::move(option));
    }
    prompt.initial_index = static_cast<int>(initial_index);
    const auto reply = prompter_.select(prompt);
    if (reply.back) return Outcome::back();
    const Json& chosen = enum_values->array[static_cast<std::size_t>(reply.value)];
    remember_value(schema, chosen);
    return Outcome::of(chosen);
  }

  auto required_string = [&](const std::string& value) -> std::optional<std::string> {
    if (options.required && trim(value).empty()) return "a value is required";
    return std::nullopt;
  };

  const std::string type = primary_type(node, root_);
  if (type == "boolean") {
    bool initial_value = false;
    if (options.prefill && options.prefill->is_bool()) {
      initial_value = options.prefill->boolean;
    } else if (const Json* fallback = node.get("default");
               fallback && fallback->is_bool()) {
      initial_value = fallback->boolean;
    }
    ConfirmPrompt prompt;
    prompt.message = label;
    prompt.hint = hint;
    prompt.initial = initial_value;
    const auto reply = prompter_.confirm(prompt);
    if (reply.back) return Outcome::back();
    return Outcome::of(Json::boolean_value(reply.value));
  }

  if (type == "integer" || type == "number") {
    const Validator validate = validate_number(node, root_);
    TextPrompt prompt;
    prompt.message = label;
    prompt.hint = hint;
    prompt.placeholder = default_value;
    prompt.default_value = default_value;
    prompt.suggest = suggest;
    prompt.initial_value = initial;
    prompt.complete = false;
    prompt.validate = [required_string, validate](const std::string& value) {
      if (const auto error = required_string(value)) return error;
      return validate(value);
    };
    const auto reply = prompter_.text(prompt);
    if (reply.back) return Outcome::back();
    Json out;
    parse_number(reply.value, out);
    remember_value(schema, out);
    return Outcome::of(out);
  }

  if (type == "null") return Outcome::of(Json::null());

  if (type == "string") {
    const Validator validate = validate_string(node, root_);
    TextPrompt prompt;
    prompt.message = label;
    prompt.hint = hint;
    prompt.placeholder = default_value;
    prompt.default_value = default_value;
    prompt.suggest = suggest;
    prompt.initial_value = initial;
    prompt.complete = true;
    prompt.validate = [required_string, validate](const std::string& value) {
      if (const auto error = required_string(value)) return error;
      return validate(value);
    };
    const auto reply = prompter_.text(prompt);
    if (reply.back) return Outcome::back();
    const Json out = Json::string_value(reply.value);
    remember_value(schema, out);
    return Outcome::of(out);
  }

  // unknown shape: accept JSON
  TextPrompt prompt;
  prompt.message = label + " (json)";
  prompt.hint = hint;
  prompt.placeholder = schema_default;
  prompt.default_value = schema_default;
  prompt.initial_value = initial;
  prompt.complete = false;
  prompt.validate = [](const std::string& value) -> std::optional<std::string> {
    std::string error;
    const auto parsed = Json::parse(value, error);
    if (!parsed) return error;
    return std::nullopt;
  };
  const auto reply = prompter_.text(prompt);
  if (reply.back) return Outcome::back();
  std::string error;
  const auto parsed = Json::parse(reply.value, error);
  if (parsed) return Outcome::of(*parsed);
  return Outcome::of(Json::string_value(reply.value));
}

Outcome Walker::property(const Json& parent, const std::string& key,
                         const Path& path, bool required,
                         PropertyOptions options) {
  const Json node = resolve_ref(parent, root_);
  const Json* properties = node.get("properties");
  const Json* property_schema = properties ? properties->get(key) : nullptr;
  if (!property_schema) return Outcome::skip();

  if (!required && options.ask) {
    const Json property_node = resolve_ref(*property_schema, root_);
    bool initial_value = options.prefill.has_value();
    if (const Json* fallback = property_node.get("default");
        fallback && !fallback->is_null()) {
      initial_value = true;
    }
    ConfirmPrompt prompt;
    prompt.message = "set " + path_label(path) + "?";
    prompt.hint = with_default_hint(
        full_hint(resolve_ref(*property_schema, root_)), *property_schema,
        root_);
    prompt.initial = initial_value;
    const auto reply = prompter_.confirm(prompt);
    if (reply.back) return Outcome::back();
    if (!reply.value) return Outcome::skip();
  }

  std::optional<Json> prefill = options.prefill;
  bool resume = options.resume;
  for (;;) {
    const Outcome outcome = value(*property_schema, path,
                                  {copy_prefill(prefill), resume, required,
                                   options.seed});
    if (outcome.kind != Outcome::Kind::Value) return outcome;
    std::string error;
    if (required && outcome.value.is_string() && trim(outcome.value.string).empty()) {
      error = "a value is required";
    }
    if (error.empty()) return outcome;
    prompter_.error(path_label(path) + ": " + error);
    prefill = outcome.value;
    resume = false;
  }
}

Outcome Walker::object(const Json& schema, const Path& path,
                       const ValueOptions& options) {
  const Json base = resolve_ref(schema, root_);
  const Json prefill = options.prefill && options.prefill->is_object()
                           ? *options.prefill
                           : Json::object_value();
  Json result = options.resume || options.seed ? prefill : Json::object_value();

  Json node = apply_conditionals(base, result, root_);

  if (path.segments.size() >= 1 && path.segments.size() <= 2) {
    if (const auto description = full_hint(node)) {
      prompter_.note(*description, path_label(path));
    }
  }

  const std::vector<std::string> property_order =
      conditional_property_order(base, root_);
  std::size_t index = 0;
  if (options.resume) {
    const Json* active = node.get("properties");
    for (std::size_t candidate = property_order.size(); candidate > 0;
         candidate--) {
      if (active && active->is_object() &&
          active->has(property_order[candidate - 1])) {
        index = candidate - 1;
        break;
      }
    }
  }
  bool resume = options.resume;
  while (index < property_order.size()) {
    node = apply_conditionals(base, result, root_);
    const std::string& key = property_order[index];
    const Json* current_properties = node.get("properties");
    const Json* current_schema =
        current_properties ? current_properties->get(key) : nullptr;
    if (!current_schema) {
      // A conditional branch changed and this property is no longer part of
      // the applicable schema.
      index++;
      continue;
    }

    std::set<std::string> required;
    if (const Json* required_list = node.get("required");
        required_list && required_list->is_array()) {
      for (const Json& item : required_list->array) {
        if (item.is_string()) required.insert(item.string);
      }
    }

    std::optional<Json> previous;
    if (const Json* existing = result.get(key)) previous = *existing;
    else if (const Json* seeded = prefill.get(key)) previous = *seeded;
    const Outcome outcome =
        property(node, key, path.with(key), required.count(key) > 0,
                 {true, previous, resume, options.seed});
    resume = false;
    if (outcome.kind == Outcome::Kind::Back) {
      if (index == 0) return Outcome::back();
      index--;
      resume = true;
      continue;
    }
    if (outcome.kind == Outcome::Kind::Skip) {
      result.erase(key);
    } else {
      result.set(key, outcome.value);
    }
    index++;
  }

  node = apply_conditionals(base, result, root_);
  const auto pattern = pattern_keys(node);
  if (!pattern) return Outcome::of(result);

  std::set<std::string> final_required;
  if (const Json* required_list = node.get("required");
      required_list && required_list->is_array()) {
    for (const Json& item : required_list->array) {
      if (item.is_string()) final_required.insert(item.string);
    }
  }
  std::vector<std::string> required_pattern;
  std::vector<std::string> optional_pattern;
  for (const std::string& key : pattern->keys) {
    if (final_required.count(key)) required_pattern.push_back(key);
    else optional_pattern.push_back(key);
  }
  for (const std::string& key : required_pattern) {
    if (result.has(key)) continue;
    const auto seeded = prefill.get(key);
    const Outcome outcome =
        value(pattern->schemas.at(key), path.with(key),
              {seeded ? std::optional<Json>(*seeded) : std::nullopt, false, true,
               options.seed});
    if (outcome.kind == Outcome::Kind::Back) return Outcome::back();
    if (outcome.kind == Outcome::Kind::Value) result.set(key, outcome.value);
  }

  std::vector<std::string> remaining;
  for (const std::string& key : optional_pattern) {
    if (!result.has(key)) remaining.push_back(key);
  }
  const std::string label = path_label(path);
  while (!remaining.empty()) {
    SelectPrompt prompt;
    prompt.message = "key for " + label;
    prompt.hint = "pick one of the keys the schema allows";
    for (const std::string& key : remaining) {
      Option option;
      option.label = key;
      const Json value_schema = pattern->schemas.at(key);
      if (const auto hint = full_hint(resolve_ref(value_schema, root_))) {
        option.hint = hint;
      } else {
        option.hint = join_types(schema_types(value_schema, root_));
      }
      prompt.options.push_back(std::move(option));
    }
    Option done;
    done.label = "(done)";
    prompt.options.push_back(done);
    prompt.initial_index = 0;
    const auto reply = prompter_.select(prompt);
    if (reply.back) return Outcome::back();
    if (reply.value == static_cast<int>(prompt.options.size()) - 1) break;
    const std::string chosen = remaining[static_cast<std::size_t>(reply.value)];
    const Outcome outcome = value(pattern->schemas.at(chosen),
                                 path.with(chosen), {std::nullopt, false, false,
                                                     options.seed});
    if (outcome.kind == Outcome::Kind::Back) continue;
    if (outcome.kind == Outcome::Kind::Value) result.set(chosen, outcome.value);
    remaining.erase(remaining.begin() + reply.value);
  }
  return Outcome::of(result);
}

Outcome Walker::array(const Json& schema, const Path& path,
                      const ValueOptions& options) {
  const Json base = resolve_ref(schema, root_);
  const Json seed_value = options.prefill && options.prefill->is_array()
                              ? *options.prefill
                              : Json::array_value();
  const Json node = apply_conditionals(base, seed_value, root_);
  const Json* items_ptr = node.get("items");
  Json item;
  if (!items_ptr) return Outcome::of(Json::array_value());
  if (items_ptr->is_array()) {
    if (items_ptr->array.empty()) return Outcome::of(Json::array_value());
    item = items_ptr->array.front();
  } else {
    item = *items_ptr;
  }
  long minimum = 0, maximum = -1;
  if (const Json* value = node.get("minItems"); value && value->is_number()) {
    minimum = static_cast<long>(value->number);
  }
  if (const Json* value = node.get("maxItems"); value && value->is_number()) {
    maximum = static_cast<long>(value->number);
  }
  const std::string label = path_label(path);
  const auto hint = full_hint(resolve_ref(item, root_));

  std::vector<Json> values;
  if (options.prefill && options.prefill->is_array()) values = options.prefill->array;

  bool resume = options.resume;
  std::size_t index = resume && !values.empty() ? values.size() - 1 : 0;
  while (index < values.size()) {
    const Outcome outcome =
        value(item, path.with(static_cast<long>(index)),
              {values[index], resume, false, options.seed});
    resume = false;
    if (outcome.kind == Outcome::Kind::Back) {
      if (index == 0) return Outcome::back();
      index--;
      resume = true;
      continue;
    }
    values[index] = outcome.value;
    index++;
  }

  auto reopen_last = [&]() -> Outcome {
    long last = static_cast<long>(values.size()) - 1;
    bool resumed = true;
    for (;;) {
      const Outcome outcome =
          value(item, path.with(last),
                {values[static_cast<std::size_t>(last)], resumed, false,
                 options.seed});
      resumed = false;
      if (outcome.kind == Outcome::Kind::Back) {
        if (last == 0) return Outcome::back();
        last--;
        resumed = true;
        continue;
      }
      values[static_cast<std::size_t>(last)] = outcome.value;
      return Outcome::of(Json::null());
    }
  };

  for (;;) {
    if (maximum >= 0 && static_cast<long>(values.size()) >= maximum) break;
    bool add = true;
    if (static_cast<long>(values.size()) >= minimum) {
      ConfirmPrompt prompt;
      prompt.message = values.empty() ? "add an item to " + label + "?"
                                      : "add another item to " + label + "?";
      prompt.hint = hint;
      prompt.initial = false;
      const auto reply = prompter_.confirm(prompt);
      if (reply.back) {
        if (values.empty()) return Outcome::back();
        const Outcome reopened = reopen_last();
        if (reopened.kind == Outcome::Kind::Back) return Outcome::back();
        continue;
      }
      add = reply.value;
    }
    if (!add) break;
    const Outcome outcome =
        value(item, path.with(static_cast<long>(values.size())), {});
    if (outcome.kind == Outcome::Kind::Back) {
      if (values.empty()) return Outcome::back();
      const Outcome reopened = reopen_last();
      if (reopened.kind == Outcome::Kind::Back) return Outcome::back();
      continue;
    }
    values.push_back(outcome.value);
  }
  return Outcome::of(Json::array_value(values));
}

Outcome Walker::record(const Json& schema, const Path& path,
                       const ValueOptions& options) {
  const Json node = resolve_ref(schema, root_);
  const Json* additional = node.get("additionalProperties");
  const bool has_value_schema = additional && additional->is_object();
  const std::string label = path_label(path);
  const Json prefill = options.prefill && options.prefill->is_object()
                           ? *options.prefill
                           : Json::object_value();
  Json result = options.resume || options.seed ? prefill : Json::object_value();
  std::vector<std::string> names;
  for (const auto& [key, value] : prefill.object) {
    (void)value;
    names.push_back(key);
  }

  bool resume = options.resume;
  std::size_t index = resume && !names.empty() ? names.size() - 1 : 0;
  while (index < names.size()) {
    const std::string& name = names[index];
    const auto seeded = prefill.get(name);
    Outcome outcome = has_value_schema
                          ? value(*additional, path.with(name),
                                  {seeded ? std::optional<Json>(*seeded) : std::nullopt, resume, false})
                          : Outcome::of(Json::string_value(""));
    resume = false;
    if (outcome.kind == Outcome::Kind::Back) {
      if (index == 0) return Outcome::back();
      index--;
      resume = true;
      continue;
    }
    result.set(name, outcome.value);
    index++;
  }

  for (;;) {
    TextPrompt key_prompt;
    key_prompt.message = "key for " + label;
    if (const auto hint = full_hint(node)) key_prompt.hint = hint;
    key_prompt.validate = [&](const std::string& value) -> std::optional<std::string> {
      const std::string text = trim(value);
      if (text.empty()) return "a key is required";
      if (result.has(text)) return "\"" + text + "\" is already set";
      if (const Json* property_names = node.get("propertyNames")) {
        const Json resolved_names = resolve_ref(*property_names, root_);
        const Validator validate = validate_string(resolved_names, root_);
        if (const auto error = validate(text)) return error;
      }
      return std::nullopt;
    };
    const auto key_reply = prompter_.text(key_prompt);
    if (key_reply.back) {
      if (result.object.empty()) return Outcome::back();
      const std::string last = result.object.back().first;
      const auto previous = result.get(last);
      Outcome outcome = has_value_schema
                            ? value(*additional, path.with(last),
                                    {previous ? std::optional<Json>(*previous) : std::nullopt, true, false})
                            : Outcome::of(Json::string_value(""));
      if (outcome.kind == Outcome::Kind::Back) return Outcome::back();
      result.set(last, outcome.value);
      continue;
    }
    const std::string name = trim(key_reply.value);
    Outcome outcome = has_value_schema
                          ? value(*additional, path.with(name), {})
                          : Outcome::of(Json::string_value(""));
    if (outcome.kind == Outcome::Kind::Back) continue;
    result.set(name, outcome.value);

    ConfirmPrompt more_prompt;
    more_prompt.message = "add another entry to " + label + "?";
    more_prompt.initial = false;
    const auto more = prompter_.confirm(more_prompt);
    if (more.back) return Outcome::back();
    if (!more.value) break;
  }
  return Outcome::of(result);
}

Outcome Walker::union_value(const std::vector<Json>& variants, const Path& path,
                            const ValueOptions& options) {
  const std::string label = path_label(path);
  bool nullable = false;
  std::vector<Json> values;
  for (const Json& variant : variants) {
    if (is_null_variant(variant, root_)) nullable = true;
    else values.push_back(variant);
  }

  if (nullable) {
    ConfirmPrompt prompt;
    prompt.message = "set " + label + "?";
    prompt.hint = "answer no to leave it null";
    prompt.initial = options.prefill ? !options.prefill->is_null() : false;
    const auto reply = prompter_.confirm(prompt);
    if (reply.back) return Outcome::back();
    if (!reply.value) return Outcome::of(Json::null());
  }

  if (values.empty()) return Outcome::of(Json::null());
  if (values.size() == 1) return value(values.front(), path, options);

  SelectPrompt prompt;
  prompt.message = "how should " + label + " be provided?";
  for (const Json& variant : values) {
    Option option;
    option.label = variant_label(variant, root_);
    option.hint = full_hint(resolve_ref(variant, root_));
    prompt.options.push_back(std::move(option));
  }
  prompt.initial_index =
      static_cast<int>(prefill_variant(values, options.prefill, root_));
  const auto reply = prompter_.select(prompt);
  if (reply.back) return Outcome::back();
  return value(values[static_cast<std::size_t>(reply.value)], path, options);
}

Json Walker::infer_schema(const Json& value) const {
  if (value.is_string()) {
    Json schema = Json::object_value();
    schema.set("type", Json::string_value("string"));
    return schema;
  }
  if (value.is_number()) {
    Json schema = Json::object_value();
    schema.set("type", Json::string_value("number"));
    return schema;
  }
  if (value.is_bool()) {
    Json schema = Json::object_value();
    schema.set("type", Json::string_value("boolean"));
    return schema;
  }
  Json schema = Json::object_value();
  if (value.is_array()) {
    schema.set("type", Json::string_value("array"));
    if (!value.array.empty()) schema.set("items", infer_schema(value.array.front()));
  }
  return schema;
}

Outcome Walker::json_prompt(const Path& path, const std::optional<Json>& prefill) {
  TextPrompt prompt;
  prompt.message = path_label(path) + " (json)";
  if (prefill) {
    const std::string initial = prefill->to_compact();
    prompt.placeholder = initial;
    prompt.default_value = initial;
    prompt.initial_value = initial;
  }
  prompt.complete = false;
  prompt.validate = [](const std::string& value) -> std::optional<std::string> {
    std::string error;
    const auto parsed = Json::parse(value, error);
    if (!parsed) return error;
    return std::nullopt;
  };
  const auto reply = prompter_.text(prompt);
  if (reply.back) return Outcome::back();
  std::string error;
  const auto parsed = Json::parse(reply.value, error);
  if (parsed) return Outcome::of(*parsed);
  return Outcome::of(Json::string_value(reply.value));
}

Outcome Walker::inferred_array(const Path& path, const Json& prefill) {
  std::vector<Json> values = prefill.array;
  for (std::size_t index = 0; index < values.size(); index++) {
    const Outcome outcome = inferred(
        path.with(static_cast<long>(index)), values[index]);
    if (outcome.kind == Outcome::Kind::Back) return Outcome::back();
    if (outcome.kind == Outcome::Kind::Value) values[index] = outcome.value;
  }
  for (;;) {
    ConfirmPrompt prompt;
    prompt.message = "add an item to " + path_label(path) + "?";
    prompt.initial = false;
    const auto reply = prompter_.confirm(prompt);
    if (reply.back || !reply.value) break;
    const Outcome outcome = inferred(path.with(static_cast<long>(values.size())),
                                     std::nullopt);
    if (outcome.kind == Outcome::Kind::Back) continue;
    if (outcome.kind == Outcome::Kind::Value) values.push_back(outcome.value);
  }
  return Outcome::of(Json::array_value(values));
}

Outcome Walker::inferred_object(const Path& path, const Json& prefill) {
  Json result = prefill;
  for (const auto& [key, value] : prefill.object) {
    const Outcome outcome = inferred(path.with(key), value);
    if (outcome.kind == Outcome::Kind::Back) return Outcome::back();
    if (outcome.kind == Outcome::Kind::Value) result.set(key, outcome.value);
  }
  for (;;) {
    ConfirmPrompt add;
    add.message = "add a member to " + path_label(path) + "?";
    add.initial = false;
    const auto more = prompter_.confirm(add);
    if (more.back || !more.value) break;
    TextPrompt key_prompt;
    key_prompt.message = "key for " + path_label(path);
    key_prompt.validate = [&](const std::string& value) -> std::optional<std::string> {
      const std::string text = trim(value);
      if (text.empty()) return "a key is required";
      if (result.has(text)) return "\"" + text + "\" is already set";
      return std::nullopt;
    };
    const auto key_reply = prompter_.text(key_prompt);
    if (key_reply.back) break;
    const std::string name = trim(key_reply.value);
    const Outcome outcome = inferred(path.with(name), std::nullopt);
    if (outcome.kind == Outcome::Kind::Back) continue;
    if (outcome.kind == Outcome::Kind::Value) result.set(name, outcome.value);
  }
  return Outcome::of(result);
}

Outcome Walker::inferred(const Path& path, const std::optional<Json>& prefill) {
  if (prefill) {
    if (prefill->is_object()) return inferred_object(path, *prefill);
    if (prefill->is_array()) return inferred_array(path, *prefill);
    if (!prefill->is_null()) {
      return value(infer_schema(*prefill), path, {prefill, false, false, true});
    }
  }
  SelectPrompt pick;
  pick.message = "type of " + path_label(path);
  pick.hint = "the schema does not describe this value";
  const char* labels[] = {"string", "number", "boolean", "object",
                          "array",  "null",   "JSON"};
  for (const char* label : labels) {
    Option option;
    option.label = label;
    pick.options.push_back(std::move(option));
  }
  int initial = 0;
  if (prefill && prefill->is_null()) initial = 5;
  pick.initial_index = initial;
  const auto reply = prompter_.select(pick);
  if (reply.back) return Outcome::back();
  switch (reply.value) {
    case 0:
      return value(infer_schema(Json::string_value("")), path, {});
    case 1:
      return value(infer_schema(Json::number_value(0, false)), path, {});
    case 2:
      return value(infer_schema(Json::boolean_value(false)), path, {});
    case 3:
      return inferred_object(path, Json::object_value());
    case 4:
      return inferred_array(path, Json::array_value());
    case 5:
      return Outcome::of(Json::null());
    default:
      return json_prompt(path, prefill);
  }
}

Outcome Walker::value(const Json& schema, const Path& path,
                      const ValueOptions& options) {
  const Outcome outcome = dispatch(schema, path, options);
  if (outcome.kind == Outcome::Kind::Value && progress_) {
    progress_(path, outcome.value);
  }
  return outcome;
}

Outcome Walker::dispatch(const Json& schema, const Path& path,
                         const ValueOptions& options) {
  const Json base = resolve_ref(schema, root_);
  const Json representative = options.prefill.value_or(Json::null());
  const Json node = apply_conditionals(base, representative, root_);
  if (const auto variants = union_variants(node, root_)) {
    return union_value(*variants, path, options);
  }
  const std::string type = primary_type(node, root_);
  const bool conditional = base.has("if") || base.has("then") ||
                           base.has("else");
  if (type == "object") {
    if (node.has("properties") || node.has("patternProperties") ||
        conditional) {
      return object(schema, path, options);
    }
    const Json* additional = node.get("additionalProperties");
    const bool constrained_additional =
        additional && additional->is_object() && !additional->object.empty();
    if (constrained_additional) return record(node, path, options);
    // opaque object: members are inferred from the document (or a type picker)
    if (additional && additional->is_bool() && !additional->boolean) {
      if (options.prefill) return inferred(path, options.prefill);
      return Outcome::of(Json::object_value());
    }
    if (options.prefill) return inferred(path, options.prefill);
    return inferred_object(path, Json::object_value());
  }
  if (type == "array") {
    if (!node.has("items") && !conditional) {
      if (options.prefill) return inferred(path, options.prefill);
      return inferred_array(path, Json::array_value());
    }
    return array(schema, path, options);
  }
  if (type == "unknown") {
    return inferred(path, options.prefill);
  }
  return leaf(schema, path, options);
}

}  // namespace cform
