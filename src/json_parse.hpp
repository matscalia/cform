// Dependency-free JSON parser used by builds without simdjson (the wasm
// module). It produces the same ordered cform::Json model as the simdjson path.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "json.hpp"

namespace cform {

std::optional<Json> parse_json_fallback(std::string_view text,
                                        std::string& error);

}  // namespace cform
