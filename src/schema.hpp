// JSON Schema helpers: `$ref` resolution, `allOf` folding, type inspection,
// validation of prompt answers, and the `patternProperties` literal parser.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace cform {

using SchemaTypes = std::vector<std::string>;

Json resolve_ref(const Json& schema, const Json& root,
                 std::set<std::string> seen = {});
Json merge_all_of(const Json& node, const Json& root);

/// Applies `if`/`then`/`else` to `node` for the given instance value. The
/// returned schema has the `if`/`then`/`else` keywords removed and the selected
/// branch's properties/required merged in.
Json apply_conditionals(const Json& schema, const Json& value, const Json& root);

/// Returns every property that can be prompted at this object level (base and
/// all conditional branches), topologically ordered so `if` dependencies are
/// asked before the fields they control.
std::vector<std::string> conditional_property_order(const Json& schema,
                                                    const Json& root);

SchemaTypes schema_types(const Json& schema, const Json& root);
std::string primary_type(const Json& schema, const Json& root);
/// Empty value matching the root schema's primary type.
Json empty_document(const Json& root_schema);
bool is_null_variant(const Json& schema, const Json& root);
std::optional<std::vector<Json>> union_variants(const Json& schema,
                                                const Json& root);

std::string trim(std::string_view text);
std::string collapse_whitespace(std::string_view text);
std::optional<std::string> full_hint(const Json& node);
std::optional<std::string> with_default_hint(
    const std::optional<std::string>& hint, const Json& raw_property,
    const Json& root, bool show = true);

std::string variant_label(const Json& schema, const Json& root);

std::optional<std::string> default_string(const Json& raw_schema,
                                          const Json& node);
std::optional<std::string> prefill_string(const Json& prefill);

using Validator = std::function<std::optional<std::string>(const std::string&)>;
Validator validate_string(const Json& node, const Json& root);
Validator validate_number(const Json& node, const Json& root);

// Validates a complete value against a schema (used to decide whether an
// already-filled document field is "full"). Returns human-readable problems.
std::vector<std::string> validate_value(const Json& schema, const Json& value,
                                        const Json& root);

// `patternProperties` made of plain literal alternations, e.g. `^(a|b)$`.
std::optional<std::vector<std::string>> literal_alternatives(
    const std::string& pattern);

struct PatternKeys {
  std::vector<std::string> keys;
  std::map<std::string, Json> schemas;
};
std::optional<PatternKeys> pattern_keys(const Json& node);

}  // namespace cform
