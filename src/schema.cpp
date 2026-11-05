#include "schema.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace cform {

namespace {

std::string decode_percent(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); index++) {
    if (text[index] == '%' && index + 2 < text.size() &&
        std::isxdigit(static_cast<unsigned char>(text[index + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[index + 2]))) {
      const auto hex = text.substr(index + 1, 2);
      out += static_cast<char>(std::stoi(std::string(hex), nullptr, 16));
      index += 2;
      continue;
    }
    out += text[index];
  }
  return out;
}

std::string unescape_ref(std::string_view name) {
  std::string out = decode_percent(name);
  std::string result;
  result.reserve(out.size());
  for (std::size_t index = 0; index < out.size(); index++) {
    if (out[index] == '~' && index + 1 < out.size()) {
      if (out[index + 1] == '1') {
        result += '/';
        index++;
        continue;
      }
      if (out[index + 1] == '0') {
        result += '~';
        index++;
        continue;
      }
    }
    result += out[index];
  }
  return result;
}

const Json* lookup_ref(const std::string& ref, const Json& root) {
  std::string name;
  if (ref.rfind("#/$defs/", 0) == 0) {
    name = ref.substr(8);
  } else if (ref.rfind("#/definitions/", 0) == 0) {
    name = ref.substr(14);
  } else {
    return nullptr;
  }
  name = unescape_ref(name);
  const Json* defs = root.get("$defs");
  if (!defs) defs = root.get("definitions");
  if (!defs || !defs->is_object()) return nullptr;
  return defs->get(name);
}

bool truthy(const Json* value) {
  if (!value || value->is_null()) return false;
  if (value->is_bool()) return value->boolean;
  return true;
}

std::string string_or(const Json* value, const std::string& fallback = "") {
  if (value && value->is_string()) return value->string;
  return fallback;
}

bool has_number(const Json& node, const char* key, double& out) {
  const Json* value = node.get(key);
  if (!value || !value->is_number()) return false;
  out = value->number;
  return true;
}

bool json_equal(const Json& a, const Json& b) {
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
        if (!json_equal(a.array[index], b.array[index])) return false;
      }
      return true;
    case Json::Type::Object:
      if (a.object.size() != b.object.size()) return false;
      for (const auto& [key, value] : a.object) {
        const Json* other = b.get(key);
        if (!other || !json_equal(value, *other)) return false;
      }
      return true;
  }
  return false;
}

bool type_matches(const std::string& type, const Json& value) {
  if (type == "string") return value.is_string();
  if (type == "number") return value.is_number();
  if (type == "integer") {
    return value.is_number() && std::floor(value.number) == value.number;
  }
  if (type == "boolean") return value.is_bool();
  if (type == "null") return value.is_null();
  if (type == "array") return value.is_array();
  if (type == "object") return value.is_object();
  return true;
}

std::size_t utf8_length(const std::string& text) {
  std::size_t count = 0;
  for (unsigned char ch : text) {
    if ((ch & 0xC0) != 0x80) count++;
  }
  return count;
}

}  // namespace

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
    begin++;
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])))
    end--;
  return std::string(text.substr(begin, end - begin));
}

std::string collapse_whitespace(std::string_view text) {
  std::string out;
  bool pending_space = false;
  for (std::size_t index = 0; index < text.size(); index++) {
    const unsigned char ch = static_cast<unsigned char>(text[index]);
    if (std::isspace(ch)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out += ' ';
    pending_space = false;
    out += static_cast<char>(ch);
  }
  return out;
}

Json resolve_ref(const Json& schema, const Json& root,
                 std::set<std::string> seen) {
  Json node = schema;
  if (node.is_object()) {
    const Json* ref = node.get("$ref");
    if (ref && ref->is_string() && !seen.count(ref->string)) {
      if (const Json* target = lookup_ref(ref->string, root)) {
        auto next_seen = seen;
        next_seen.insert(ref->string);
        Json resolved = resolve_ref(*target, root, std::move(next_seen));
        for (const auto& [key, value] : node.object) {
          if (key == "$ref") continue;
          resolved.set(key, value);
        }
        node = std::move(resolved);
      }
    }
  }
  return merge_all_of(node, root);
}

Json merge_all_of(const Json& node, const Json& root) {
  if (!node.is_object()) return node;
  const Json* all_of = node.get("allOf");
  if (!all_of || !all_of->is_array()) return node;

  Json merged = node;
  merged.erase("allOf");

  for (const Json& raw_part : all_of->array) {
    const Json part = resolve_ref(raw_part, root);

    const Json* part_props = part.get("properties");
    if (part_props && part_props->is_object()) {
      Json props = Json::object_value();
      for (const auto& entry : part_props->object) props.set(entry.first, entry.second);
      if (const Json* own = merged.get("properties"); own && own->is_object()) {
        for (const auto& entry : own->object) props.set(entry.first, entry.second);
      }
      merged.set("properties", props);
    }

    const Json* part_patterns = part.get("patternProperties");
    if (part_patterns && part_patterns->is_object()) {
      Json patterns = Json::object_value();
      for (const auto& entry : part_patterns->object)
        patterns.set(entry.first, entry.second);
      if (const Json* own = merged.get("patternProperties");
          own && own->is_object()) {
        for (const auto& entry : own->object)
          patterns.set(entry.first, entry.second);
      }
      merged.set("patternProperties", patterns);
    }

    const Json* part_required = part.get("required");
    if (part_required && part_required->is_array()) {
      std::vector<Json> required;
      if (const Json* own = merged.get("required"); own && own->is_array()) {
        for (const auto& item : own->array) required.push_back(item);
      }
      for (const auto& item : part_required->array) required.push_back(item);
      merged.set("required", Json::array_value(std::move(required)));
    }

    static const char* keywords[] = {
        "type",           "items",       "additionalProperties",
        "propertyNames",  "minItems",    "maxItems",
        "minLength",      "maxLength",   "minimum",
        "maximum",        "exclusiveMinimum", "exclusiveMaximum",
        "pattern",        "enum",        "const",
        "description",    "title",       "default",
    };
    for (const char* key : keywords) {
      if (!merged.has(key) && part.has(key)) merged.set(key, *part.get(key));
    }
  }
  return merged;
}

SchemaTypes schema_types(const Json& schema, const Json& root) {
  const Json node = resolve_ref(schema, root);
  const Json* type = node.get("type");
  if (type) {
    if (type->is_array()) {
      SchemaTypes types;
      for (const auto& item : type->array) {
        if (item.is_string()) types.push_back(item.string);
      }
      return types;
    }
    if (type->is_string()) return {type->string};
  }
  if (node.has("anyOf") || node.has("oneOf")) return {"union"};
  if (node.has("enum") || node.has("const")) return {"enum"};
  if (node.has("properties") || node.has("patternProperties") ||
      truthy(node.get("additionalProperties"))) {
    return {"object"};
  }
  if (node.has("items")) return {"array"};
  return {"unknown"};
}

std::string primary_type(const Json& schema, const Json& root) {
  const SchemaTypes types = schema_types(schema, root);
  return types.empty() ? "unknown" : types.front();
}

bool is_null_variant(const Json& schema, const Json& root) {
  const SchemaTypes types = schema_types(schema, root);
  return std::find(types.begin(), types.end(), "null") != types.end();
}

std::optional<std::vector<Json>> union_variants(const Json& schema,
                                                const Json& root) {
  const Json node = resolve_ref(schema, root);
  const Json* variants = node.get("anyOf");
  if (!variants) variants = node.get("oneOf");
  if (variants && variants->is_array()) return variants->array;
  const Json* type = node.get("type");
  if (type && type->is_array() && type->array.size() > 1) {
    std::vector<Json> result;
    for (const auto& item : type->array) {
      if (!item.is_string()) continue;
      Json variant = node;
      variant.set("type", Json::string_value(item.string));
      result.push_back(std::move(variant));
    }
    return result;
  }
  return std::nullopt;
}

std::optional<std::string> full_hint(const Json& node) {
  const Json* description = node.get("description");
  if (!description || !description->is_string()) return std::nullopt;
  const std::string text = trim(description->string);
  if (text.empty()) return std::nullopt;
  return text;
}

std::optional<std::string> with_default_hint(
    const std::optional<std::string>& hint, const Json& raw_property,
    const Json& root, bool show) {
  if (!show) return hint;
  const Json node = resolve_ref(raw_property, root);
  const Json* value = raw_property.get("default");
  if (!value) value = node.get("default");
  if (!value) return hint;
  const std::string suffix = "default: " + value->to_compact();
  if (!hint) return suffix;
  return hint.value() + " (" + suffix + ")";
}

std::string variant_label(const Json& schema, const Json& root) {
  const Json node = resolve_ref(schema, root);
  if (const Json* constant = node.get("const")) return constant->to_compact();
  if (const Json* enum_values = node.get("enum"); enum_values && enum_values->is_array()) {
    std::string out;
    for (std::size_t index = 0; index < enum_values->array.size(); index++) {
      if (index > 0) out += " | ";
      out += enum_values->array[index].to_compact();
    }
    return out;
  }
  const std::string type = primary_type(node, root);
  if (type == "string") {
    double min_length = 0;
    if (has_number(node, "minLength", min_length)) {
      return "string (min " + number_to_string(min_length, true) + ")";
    }
    return "string";
  }
  if (type == "array") {
    const Json* items = node.get("items");
    if (items) {
      if (items->is_array()) {
        if (items->array.empty()) return "list";
        return "list of " + variant_label(items->array.front(), root);
      }
      return "list of " + variant_label(*items, root);
    }
    return "list";
  }
  if (type == "object") {
    const Json* title = node.get("title");
    if (title && title->is_string()) return "object " + title->string;
    return "object";
  }
  if (type == "null") return "none (null)";
  return type;
}

std::optional<std::string> default_string(const Json& raw_schema,
                                          const Json& node) {
  const Json* value = raw_schema.get("default");
  if (!value) value = node.get("default");
  if (!value || value->is_null()) return std::nullopt;
  if (value->is_string()) return value->string;
  if (value->is_number()) return number_to_string(value->number, value->integer);
  if (value->is_bool()) return value->boolean ? "true" : "false";
  return value->to_compact();
}

std::optional<std::string> prefill_string(const Json& prefill) {
  if (prefill.is_string()) return prefill.string;
  if (prefill.is_number()) return number_to_string(prefill.number, prefill.integer);
  if (prefill.is_bool()) return prefill.boolean ? "true" : "false";
  return prefill.to_compact();
}

Validator validate_string(const Json& node, const Json& root) {
  (void)root;
  std::optional<std::regex> pattern;
  if (const Json* raw_pattern = node.get("pattern");
      raw_pattern && raw_pattern->is_string()) {
    try {
      pattern.emplace(raw_pattern->string);
    } catch (const std::regex_error&) {
      pattern.reset();
    }
  }
  double min_length = 0, max_length = 0;
  const bool has_min = has_number(node, "minLength", min_length);
  const bool has_max = has_number(node, "maxLength", max_length);
  const std::string pattern_text =
      string_or(node.get("pattern"));
  return [=](const std::string& value) -> std::optional<std::string> {
    const std::size_t length = utf8_length(value);
    if (has_min && length < static_cast<std::size_t>(min_length)) {
      return "must be at least " + number_to_string(min_length, true) +
             " character(s)";
    }
    if (has_max && length > static_cast<std::size_t>(max_length)) {
      return "must be at most " + number_to_string(max_length, true) +
             " character(s)";
    }
    if (pattern && !std::regex_search(value, *pattern)) {
      return "must match " + pattern_text;
    }
    return std::nullopt;
  };
}

Validator validate_number(const Json& node, const Json& root) {
  (void)root;
  const bool wants_integer = string_or(node.get("type")) == "integer";
  double minimum = 0, maximum = 0, exclusive_minimum = 0, exclusive_maximum = 0;
  const bool has_minimum = has_number(node, "minimum", minimum);
  const bool has_maximum = has_number(node, "maximum", maximum);
  const bool has_exclusive_minimum =
      has_number(node, "exclusiveMinimum", exclusive_minimum);
  const bool has_exclusive_maximum =
      has_number(node, "exclusiveMaximum", exclusive_maximum);
  return [=](const std::string& raw) -> std::optional<std::string> {
    const std::string text = trim(raw);
    if (text.empty()) return "expected a number";
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) {
      return "\"" + text + "\" is not a number";
    }
    if (wants_integer && std::floor(parsed) != parsed) {
      return "expected an integer";
    }
    if (has_minimum && parsed < minimum) {
      return "must be >= " + number_to_string(minimum, minimum == std::floor(minimum));
    }
    if (has_maximum && parsed > maximum) {
      return "must be <= " + number_to_string(maximum, maximum == std::floor(maximum));
    }
    if (has_exclusive_minimum && parsed <= exclusive_minimum) {
      return "must be > " +
             number_to_string(exclusive_minimum,
                              exclusive_minimum == std::floor(exclusive_minimum));
    }
    if (has_exclusive_maximum && parsed >= exclusive_maximum) {
      return "must be < " +
             number_to_string(exclusive_maximum,
                              exclusive_maximum == std::floor(exclusive_maximum));
    }
    return std::nullopt;
  };
}

std::vector<std::string> validate_value(const Json& schema, const Json& value,
                                        const Json& root) {
  const Json node = resolve_ref(schema, root);
  std::vector<std::string> errors;

  const Json* variants = node.get("anyOf");
  if (!variants) variants = node.get("oneOf");
  if (variants && variants->is_array()) {
    for (const Json& variant : variants->array) {
      if (validate_value(variant, value, root).empty()) return {};
    }
    errors.push_back("does not match any of the allowed variants");
    return errors;
  }

  if (const Json* constant = node.get("const")) {
    if (!json_equal(*constant, value)) errors.push_back("is not the required value");
  }
  if (const Json* enum_values = node.get("enum");
      enum_values && enum_values->is_array()) {
    bool found = false;
    for (const Json& candidate : enum_values->array) {
      if (json_equal(candidate, value)) {
        found = true;
        break;
      }
    }
    if (!found) errors.push_back("is not one of the allowed values");
  }

  if (const Json* type = node.get("type")) {
    bool matched = true;
    if (type->is_string()) {
      matched = type_matches(type->string, value);
    } else if (type->is_array()) {
      matched = false;
      for (const Json& candidate : type->array) {
        if (candidate.is_string() && type_matches(candidate.string, value)) {
          matched = true;
          break;
        }
      }
    }
    if (!matched) {
      errors.push_back("has the wrong type");
      return errors;
    }
  }

  if (value.is_string()) {
    if (const auto error = validate_string(node, root)(value.string)) {
      errors.push_back(*error);
    }
  }
  if (value.is_number()) {
    if (const auto error = validate_number(node, root)(
            number_to_string(value.number, value.integer))) {
      errors.push_back(*error);
    }
  }
  if (value.is_array()) {
    double min_items = 0, max_items = 0;
    if (has_number(node, "minItems", min_items) &&
        value.array.size() < static_cast<std::size_t>(min_items)) {
      errors.push_back("has too few items");
    }
    if (has_number(node, "maxItems", max_items) &&
        value.array.size() > static_cast<std::size_t>(max_items)) {
      errors.push_back("has too many items");
    }
    const Json* items = node.get("items");
    if (items) {
      if (items->is_array()) {
        for (std::size_t index = 0; index < value.array.size(); index++) {
          if (index >= items->array.size()) break;
          for (const std::string& error :
               validate_value(items->array[index], value.array[index], root)) {
            errors.push_back("item " + std::to_string(index) + ": " + error);
          }
        }
      } else {
        for (std::size_t index = 0; index < value.array.size(); index++) {
          for (const std::string& error :
               validate_value(*items, value.array[index], root)) {
            errors.push_back("item " + std::to_string(index) + ": " + error);
          }
        }
      }
    }
  }
  if (value.is_object()) {
    if (const Json* required = node.get("required");
        required && required->is_array()) {
      for (const Json& name : required->array) {
        if (!name.is_string() || !value.has(name.string)) {
          errors.push_back("is missing required property \"" +
                           (name.is_string() ? name.string : std::string("?")) +
                           "\"");
        }
      }
    }
    const Json* properties = node.get("properties");
    if (properties && properties->is_object()) {
      for (const auto& [name, property_schema] : properties->object) {
        if (const Json* property_value = value.get(name)) {
          for (const std::string& error :
               validate_value(property_schema, *property_value, root)) {
            errors.push_back(name + ": " + error);
          }
        }
      }
    }
    const Json* additional = node.get("additionalProperties");
    const bool additional_allowed = !additional || truthy(additional);
    const Json* patterns = node.get("patternProperties");
    for (const auto& [name, entry_value] : value.object) {
      if (properties && properties->has(name)) continue;
      bool matched = false;
      if (patterns && patterns->is_object()) {
        for (const auto& [pattern, pattern_schema] : patterns->object) {
          bool matches = false;
          try {
            matches = std::regex_search(name, std::regex(pattern));
          } catch (const std::regex_error&) {
            matches = false;
          }
          if (!matches) continue;
          matched = true;
          for (const std::string& error :
               validate_value(pattern_schema, entry_value, root)) {
            errors.push_back(name + ": " + error);
          }
        }
      }
      if (matched) continue;
      if (!additional_allowed) {
        errors.push_back("has an unknown property \"" + name + "\"");
      } else if (additional && additional->is_object()) {
        for (const std::string& error :
             validate_value(*additional, entry_value, root)) {
          errors.push_back(name + ": " + error);
        }
      }
    }
  }
  return errors;
}

// --- patternProperties: literal alternations --------------------------------

namespace {

bool is_escaped(const std::string& text, std::size_t index) {
  int backslashes = 0;
  for (std::size_t i = index; i > 0 && text[i - 1] == '\\'; i--) backslashes++;
  return backslashes % 2 == 1;
}

std::optional<std::vector<std::string>> split_top_level(
    const std::string& pattern) {
  std::vector<std::string> parts;
  std::string current;
  int depth = 0;
  for (std::size_t index = 0; index < pattern.size(); index++) {
    const char ch = pattern[index];
    if (ch == '\\') {
      if (index + 1 >= pattern.size()) return std::nullopt;
      current += ch;
      current += pattern[index + 1];
      index++;
      continue;
    }
    if (ch == '(') depth++;
    if (ch == ')') {
      depth--;
      if (depth < 0) return std::nullopt;
    }
    if (ch == '|' && depth == 0) {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current += ch;
  }
  if (depth != 0) return std::nullopt;
  parts.push_back(current);
  return parts;
}

std::string strip_anchors(std::string branch) {
  if (!branch.empty() && branch.front() == '^') branch.erase(branch.begin());
  if (!branch.empty() && branch.back() == '$' &&
      !is_escaped(branch, branch.size() - 1)) {
    branch.pop_back();
  }
  return branch;
}

std::optional<std::size_t> wrapping_group_end(const std::string& text) {
  if (text.empty() || text.front() != '(') return std::nullopt;
  int depth = 0;
  for (std::size_t index = 0; index < text.size(); index++) {
    const char ch = text[index];
    if (ch == '\\') {
      index++;
      continue;
    }
    if (ch == '(') depth++;
    else if (ch == ')') {
      depth--;
      if (depth == 0) return index;
    }
  }
  return std::nullopt;
}

const std::string ESCAPABLE = ".-+*?()[]{}^$|/\\";
const std::string METACHARS = ".*+?^${}()|[]\\";

std::optional<std::string> unescape_literal(const std::string& text) {
  std::string out;
  for (std::size_t index = 0; index < text.size(); index++) {
    const char ch = text[index];
    if (ch == '\\') {
      if (index + 1 >= text.size()) return std::nullopt;
      const char next = text[index + 1];
      if (ESCAPABLE.find(next) == std::string::npos) return std::nullopt;
      out += next;
      index++;
      continue;
    }
    if (METACHARS.find(ch) != std::string::npos) return std::nullopt;
    out += ch;
  }
  return out;
}

bool parse_literal_branch(const std::string& branch,
                          std::vector<std::string>& out) {
  const std::string text = strip_anchors(branch);
  if (const auto group_end = wrapping_group_end(text);
      group_end && *group_end == text.size() - 1) {
    const std::string inner = text.substr(1, text.size() - 2);
    const auto parts = split_top_level(inner);
    if (!parts) return false;
    for (const std::string& part : *parts) {
      if (!parse_literal_branch(part, out)) return false;
    }
    return true;
  }
  const auto literal = unescape_literal(text);
  if (!literal || literal->empty()) return false;
  out.push_back(*literal);
  return true;
}

}  // namespace

std::optional<std::vector<std::string>> literal_alternatives(
    const std::string& pattern) {
  const auto branches = split_top_level(trim(pattern));
  if (!branches || branches->empty()) return std::nullopt;
  std::vector<std::string> literals;
  for (const std::string& branch : *branches) {
    if (!parse_literal_branch(branch, literals)) return std::nullopt;
  }
  std::vector<std::string> unique;
  for (const std::string& literal : literals) {
    if (std::find(unique.begin(), unique.end(), literal) == unique.end())
      unique.push_back(literal);
  }
  if (unique.empty()) return std::nullopt;
  return unique;
}

std::optional<PatternKeys> pattern_keys(const Json& node) {
  const Json* patterns = node.get("patternProperties");
  if (!patterns || !patterns->is_object()) return std::nullopt;
  PatternKeys result;
  for (const auto& [pattern, schema] : patterns->object) {
    const auto literals = literal_alternatives(pattern);
    if (!literals) return std::nullopt;
    for (const std::string& literal : *literals) {
      if (result.schemas.count(literal)) continue;
      result.schemas.emplace(literal, schema);
      result.keys.push_back(literal);
    }
  }
  if (result.keys.empty()) return std::nullopt;
  return result;
}

}  // namespace cform
