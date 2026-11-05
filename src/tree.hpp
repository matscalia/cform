// Tree screen used when cform edits an existing document: navigate the
// document tree, see which nodes respect the schema, and pick what to fill.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "walker.hpp"
#include "tui.hpp"

namespace cform {

struct TreeRow {
  Path path;
  Json schema;
  Json value;
  bool present = false;
  bool required = false;
  bool expandable = false;
  bool folded = false;
  int depth = 0;
  /// per ancestor level: true when that ancestor is the last child (no `│`)
  std::vector<bool> guides;
  bool is_last = true;
  std::string label;
  /// first schema violation for this node, empty when it is valid/unset
  std::string error;
  bool valid = false;
};

bool path_equal(const Path& a, const Path& b);

const Json* get_at_path(const Json& document, const Path& path);
void set_at_path(Json& document, const Path& path, Json value);
bool erase_at_path(Json& document, const Path& path);

/// Builds the visible rows for the document (respecting `expanded`); the root
/// is always expanded. `expanded` holds `path_label` strings.
std::vector<TreeRow> build_tree_rows(const Json& document, const Json& root_schema,
                                     const std::set<std::string>& expanded);

/// Runs the tree screen until the user quits; edits `document` in place.
/// When `prompt_root_first` is true, the root wizard is shown before the tree;
/// ctrl+f returns to the tree at any point.
void run_tree(PromptUI& ui, const Json& root_schema, Json& document,
              Walker& walker, const std::string& document_label,
              bool prompt_root_first = false);

}  // namespace cform
