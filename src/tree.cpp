#include "tree.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <string>

#include "schema.hpp"

namespace cform {

namespace {

constexpr const char *RESET = "\x1b[0m";
constexpr const char *DIM = "\x1b[2m";
constexpr const char *RED = "\x1b[31m";
constexpr const char *GREEN = "\x1b[32m";
constexpr const char *CYAN = "\x1b[36m";
constexpr const char *REVERSE = "\x1b[7m";

std::string dim(const std::string &text) {
  return std::string(DIM) + text + RESET;
}
std::string red(const std::string &text) {
  return std::string(RED) + text + RESET;
}
std::string green(const std::string &text) {
  return std::string(GREEN) + text + RESET;
}
std::string cyan(const std::string &text) {
  return std::string(CYAN) + text + RESET;
}

std::string short_value(const Json &value, std::size_t max = 32) {
  std::string text;
  if (value.is_string()) {
    text = "\"" + value.string + "\"";
  } else if (value.is_number()) {
    text = number_to_string(value.number, value.integer);
  } else if (value.is_bool()) {
    text = value.boolean ? "true" : "false";
  } else if (value.is_null()) {
    text = "null";
  } else if (value.is_object()) {
    text = "{" + std::to_string(value.object.size()) + "}";
  } else if (value.is_array()) {
    text = "[" + std::to_string(value.array.size()) + "]";
  }
  if (text.size() > max)
    text = text.substr(0, max - 1) + "…";
  return text;
}

const Json *match_pattern_schema(const Json &node, const std::string &name) {
  const Json *patterns = node.get("patternProperties");
  if (!patterns || !patterns->is_object())
    return nullptr;
  for (const auto &[pattern, schema] : patterns->object) {
    try {
      if (std::regex_search(name, std::regex(pattern)))
        return &schema;
    } catch (const std::regex_error &) {
      continue;
    }
  }
  return nullptr;
}

struct Child {
  std::string name;
  Json schema;
  bool required = false;
  bool extra_forbidden = false;
  bool known = true;
};

std::vector<Child> object_children(const Json &node, const Json &value) {
  std::vector<Child> children;
  std::set<std::string> seen;
  const Json *properties = node.get("properties");
  if (properties && properties->is_object()) {
    for (const auto &[name, schema] : properties->object) {
      seen.insert(name);
      children.push_back({name, schema, false, false, true});
    }
  }
  const auto patterns = pattern_keys(node);
  std::set<std::string> required;
  if (const Json *list = node.get("required"); list && list->is_array()) {
    for (const Json &name : list->array) {
      if (name.is_string())
        required.insert(name.string);
    }
  }
  if (patterns) {
    for (const std::string &name : patterns->keys) {
      if (seen.count(name))
        continue;
      seen.insert(name);
      children.push_back(
          {name, patterns->schemas.at(name), false, false, true});
    }
  }
  if (value.is_object()) {
    const Json *additional = node.get("additionalProperties");
    const bool additional_allowed =
        !additional || (additional->is_bool() ? additional->boolean : true);
    for (const auto &[name, entry] : value.object) {
      if (seen.count(name))
        continue;
      seen.insert(name);
      Child child;
      child.name = name;
      child.known = false;
      if (const Json *pattern_schema = match_pattern_schema(node, name)) {
        child.schema = *pattern_schema;
        child.known = true;
      } else if (additional && additional->is_object()) {
        child.schema = *additional;
      } else {
        child.schema = Json::object_value();
        child.extra_forbidden = !additional_allowed;
      }
      children.push_back(std::move(child));
    }
  }
  for (Child &child : children)
    child.required = required.count(child.name) > 0;
  return children;
}

void build_rows(const Json *value, const Json &schema, const Path &path,
                int depth, const std::vector<bool> &guides, bool is_last,
                bool required, const std::string &label, bool extra_forbidden,
                const Json &root_schema, const std::set<std::string> &expanded,
                std::vector<TreeRow> &out) {
  TreeRow row;
  row.path = path;
  row.schema = schema;
  row.required = required;
  row.depth = depth;
  row.guides = guides;
  row.is_last = is_last;
  row.label = label;

  if (value) {
    row.present = true;
    row.value = *value;
    row.valid = true;
    if (extra_forbidden) {
      row.error = "unknown property";
      row.valid = false;
    } else {
      const std::vector<std::string> errors =
          validate_value(schema, *value, root_schema);
      if (!errors.empty()) {
        row.error = errors.front();
        if (errors.size() > 1) {
          row.error += " (+" + std::to_string(errors.size() - 1) + ")";
        }
        row.valid = false;
      }
    }
  } else {
    row.present = false;
    row.error = required ? "missing required field" : "";
    row.valid = false;
  }

  // children
  std::vector<Child> children;
  const Json node =
      apply_conditionals(schema, value ? *value : Json::null(), root_schema);
  const std::string type = primary_type(node, root_schema);
  if ((value && value->is_object()) ||
      (type == "object" && node.has("properties"))) {
    children = object_children(node, value ? *value : Json::null());
  }
  const bool expandable = !children.empty();
  const std::string key = path_label(path);
  row.expandable = expandable;
  row.folded = expandable && !path.segments.empty() && !expanded.count(key);
  out.push_back(row);
  if (!expandable || row.folded)
    return;

  for (std::size_t index = 0; index < children.size(); index++) {
    const Child &child = children[index];
    const Json *child_value = nullptr;
    if (value && value->is_object())
      child_value = value->get(child.name);
    std::vector<bool> child_guides = guides;
    child_guides.push_back(is_last);
    build_rows(child_value, child.schema, path.with(child.name), depth + 1,
               child_guides, index + 1 == children.size(), child.required,
               child.name, child.extra_forbidden, root_schema, expanded, out);
  }
}

std::string render_row(const TreeRow &row, bool cursor) {
  std::string prefix;
  for (bool last : row.guides)
    prefix += last ? "    " : "│   ";
  if (row.depth > 0)
    prefix += row.is_last ? "└── " : "├── ";
  const std::string marker = row.expandable ? (row.folded ? "▸ " : "▾ ") : "  ";
  std::string preview;
  if (row.present) {
    preview = " " + short_value(row.value);
  }
  std::string body = marker + row.label + preview;
  if (cursor) {
    body = std::string(REVERSE) + " " + body + " " + RESET;
  } else if (!row.present) {
    body = dim(body);
  }
  std::string diagnostics;
  if (!row.error.empty()) {
    diagnostics = " " + red("󰀪 ") + " " + row.error;
  } else if (row.present && row.valid) {
    diagnostics = " " + green("✓");
  }
  return dim(prefix) + body + diagnostics;
}

std::string footer(const std::string &help, const std::string &indicator,
                   int width) {
  const std::size_t available =
      width > static_cast<int>(indicator.size()) + 2
          ? static_cast<std::size_t>(width) - indicator.size() - 1
          : 1;
  std::string line = help.size() > available ? help.substr(0, available) : help;
  line.resize(available, ' ');
  return std::string(REVERSE) + line + RESET + indicator;
}

int find_row(const std::vector<TreeRow> &rows, const Path &path) {
  for (std::size_t index = 0; index < rows.size(); index++) {
    if (path_equal(rows[index].path, path))
      return static_cast<int>(index);
  }
  return -1;
}

} // namespace

bool path_equal(const Path &a, const Path &b) {
  if (a.segments.size() != b.segments.size())
    return false;
  for (std::size_t index = 0; index < a.segments.size(); index++) {
    const Segment &left = a.segments[index];
    const Segment &right = b.segments[index];
    if (left.numeric != right.numeric)
      return false;
    if (left.numeric ? left.index != right.index : left.name != right.name) {
      return false;
    }
  }
  return true;
}

const Json *get_at_path(const Json &document, const Path &path) {
  const Json *current = &document;
  for (const Segment &segment : path.segments) {
    if (!current)
      return nullptr;
    if (segment.numeric) {
      if (!current->is_array() || segment.index < 0 ||
          static_cast<std::size_t>(segment.index) >= current->array.size()) {
        return nullptr;
      }
      current = &current->array[static_cast<std::size_t>(segment.index)];
    } else {
      if (!current->is_object())
        return nullptr;
      current = current->get(segment.name);
    }
  }
  return current;
}

void set_at_path(Json &document, const Path &path, Json value) {
  if (path.segments.empty()) {
    document = std::move(value);
    return;
  }
  Json *current = &document;
  for (std::size_t index = 0; index + 1 < path.segments.size(); index++) {
    const Segment &segment = path.segments[index];
    const Segment &next = path.segments[index + 1];
    if (segment.numeric) {
      if (!current->is_array())
        *current = Json::array_value();
      const std::size_t position = static_cast<std::size_t>(segment.index);
      while (current->array.size() <= position)
        current->array.push_back(Json::null());
      current = &current->array[position];
    } else {
      if (!current->is_object())
        *current = Json::object_value();
      Json *child = current->find(segment.name);
      if (!child) {
        current->set(segment.name,
                     next.numeric ? Json::array_value() : Json::object_value());
        child = current->find(segment.name);
      }
      current = child;
    }
  }
  const Segment &last = path.segments.back();
  if (last.numeric) {
    if (!current->is_array())
      *current = Json::array_value();
    const std::size_t position = static_cast<std::size_t>(last.index);
    while (current->array.size() <= position)
      current->array.push_back(Json::null());
    current->array[position] = std::move(value);
  } else {
    if (!current->is_object())
      *current = Json::object_value();
    current->set(last.name, std::move(value));
  }
}

bool erase_at_path(Json &document, const Path &path) {
  if (path.segments.empty())
    return false;
  Json *parent = &document;
  for (std::size_t index = 0; index + 1 < path.segments.size(); index++) {
    const Segment &segment = path.segments[index];
    if (segment.numeric) {
      if (!parent->is_array() ||
          static_cast<std::size_t>(segment.index) >= parent->array.size()) {
        return false;
      }
      parent = &parent->array[static_cast<std::size_t>(segment.index)];
    } else {
      parent = parent->find(segment.name);
      if (!parent)
        return false;
    }
  }
  const Segment &last = path.segments.back();
  if (last.numeric)
    return false;
  if (!parent->is_object())
    return false;
  parent->erase(last.name);
  return true;
}

std::vector<TreeRow> build_tree_rows(const Json &document,
                                     const Json &root_schema,
                                     const std::set<std::string> &expanded) {
  std::vector<TreeRow> rows;
  build_rows(&document, root_schema, Path{}, 0, {}, true, false, "document",
             false, root_schema, expanded, rows);
  return rows;
}

void run_tree(PromptUI &ui, const Json &root_schema, Json &document,
              Walker &walker, const std::string &document_label,
              bool prompt_root_first) {

  walker.set_progress([&document](const Path &path, const Json &value) {
    if (path.segments.empty())
      return;
    set_at_path(document, path, value);
  });

  struct LastPrompt {
    Json schema;
    Path path;
    ValueOptions options;
    bool expandable = false;
  };

  std::set<std::string> expanded;
  std::vector<TreeRow> rows;
  int cursor = 0;
  int scroll = 0;
  std::optional<Path> restore_to;
  std::optional<LastPrompt> last_prompt;

  auto apply_outcome = [&](const Outcome &outcome, const Path &path,
                           bool expandable) {
    if (outcome.kind == Outcome::Kind::Value) {
      if (path.segments.empty())
        document = outcome.value;
      else
        set_at_path(document, path, outcome.value);
      if (expandable)
        expanded.insert(path_label(path));
    } else if (outcome.kind == Outcome::Kind::Skip) {
      if (!path.segments.empty())
        erase_at_path(document, path);
    }
  };

  auto run_walker = [&](const Json &schema, const Path &path,
                        const ValueOptions &options, bool replay,
                        bool expandable) {
    if (!replay) ui.begin_prompts();
    try {
      const Outcome outcome = walker.value(schema, path, options);
      apply_outcome(outcome, path, expandable);
    } catch (const TreeJump &) {
      // ctrl+f: keep the previous value and go back to the tree
    }
    ui.end_prompts();
  };

  auto open_row = [&](const TreeRow &row) {
    restore_to = row.path;
    std::optional<Json> prefill;
    if (row.present)
      prefill = row.value;
    const ValueOptions options{prefill, false, row.required,
                               prefill.has_value()};
    last_prompt = LastPrompt{row.schema, row.path, options, row.expandable};
    run_walker(row.schema, row.path, options, false, row.expandable);
  };

  if (prompt_root_first) {
    const ValueOptions options{std::optional<Json>(document), false, false, true};
    last_prompt = LastPrompt{root_schema, Path{}, options, false};
    restore_to = Path{};
    run_walker(root_schema, Path{}, options, false, false);
  }

  rows = build_tree_rows(document, root_schema, expanded);

  for (;;) {
    rows = build_tree_rows(document, root_schema, expanded);
    if (restore_to) {
      const int found = find_row(rows, *restore_to);
      if (found >= 0)
        cursor = found;
      restore_to.reset();
    }
    if (rows.empty())
      rows.push_back(TreeRow{});
    cursor = std::max(0, std::min(cursor, static_cast<int>(rows.size()) - 1));

    const int height = std::max(6, ui.terminal_rows());
    const int width = std::max(20, ui.terminal_columns());
    const int visible = std::max(3, height - 3);
    if (cursor < scroll)
      scroll = cursor;
    if (cursor >= scroll + visible)
      scroll = cursor - visible + 1;
    scroll = std::max(
        0,
        std::min(scroll, std::max(0, static_cast<int>(rows.size()) - visible)));

    std::vector<std::string> lines;
    lines.push_back(cyan("fields") + " " + dim(document_label));
    for (int index = scroll;
         index < static_cast<int>(rows.size()) && index < scroll + visible;
         index++) {
      lines.push_back(render_row(rows[index], index == cursor));
    }
    lines.push_back(footer(
        " ↑↓/jk:nav  ←/h:fold  →/l:open  enter:edit  ctrl+o:prompt  ctrl+f:tree  q:done ",
        std::to_string(cursor + 1) + "/" + std::to_string(rows.size()), width));
    ui.render_lines(lines, true, true);

    const Key key = ui.read_key();
    if (key.ctrl && key.name == "c")
      throw PromptCancelled();
    if (key.name == "escape" || (key.has_str && key.str == "q")) {
      ui.clear_lines();
      return;
    }
    if (key.name == "up" || (key.has_str && key.str == "k")) {
      cursor = std::max(0, cursor - 1);
      continue;
    }
    if (key.name == "down" || (key.has_str && key.str == "j")) {
      cursor = std::min(static_cast<int>(rows.size()) - 1, cursor + 1);
      continue;
    }
    if (key.name == "pageup") {
      cursor = std::max(0, cursor - visible);
      continue;
    }
    if (key.name == "pagedown") {
      cursor = std::min(static_cast<int>(rows.size()) - 1, cursor + visible);
      continue;
    }
    if (key.name == "right" || (key.has_str && key.str == "l")) {
      const TreeRow row = rows[cursor];
      if (!row.expandable)
        continue;
      if (row.folded) {
        expanded.insert(path_label(row.path));
        restore_to = row.path;
      } else if (cursor + 1 < static_cast<int>(rows.size()) &&
                 rows[cursor + 1].depth > row.depth) {
        cursor++;
      }
      continue;
    }
    if (key.name == "left" || (key.has_str && key.str == "h")) {
      const TreeRow row = rows[cursor];
      if (row.expandable && !row.folded) {
        expanded.erase(path_label(row.path));
        restore_to = row.path;
        continue;
      }
      Path parent = row.path;
      if (!parent.segments.empty()) {
        parent.segments.pop_back();
        const int found = find_row(rows, parent);
        if (found >= 0)
          cursor = found;
      }
      continue;
    }
    if (key.ctrl && key.name == "o") {
      if (last_prompt && ui.begin_prompt_replay()) {
        restore_to = last_prompt->path;
        run_walker(last_prompt->schema, last_prompt->path,
                   last_prompt->options, true, last_prompt->expandable);
      }
      continue;
    }
    if (key.name == "return" || key.name == "space" ||
        (key.has_str && key.str == " ")) {
      open_row(rows[cursor]);
      continue;
    }
  }
}

} // namespace cform
