// cform: compile a JSON Schema read from stdin into a JSON value through the
// same schema-driven prompts as `orchestra prompt`.
//
// Prompts are drawn on the controlling terminal (/dev/tty) so stdout stays
// reserved for the resulting JSON.
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <unistd.h>

#include "json.hpp"
#include "schema.hpp"
#include "tree.hpp"
#include "tui.hpp"
#include "walker.hpp"

namespace {

void usage(std::ostream& out) {
  out << "usage: cform [options] [document]\n"
         "\n"
         "Reads a JSON Schema from stdin, prompts for a value and prints it on\n"
         "stdout. Prompts are drawn on the controlling terminal.\n"
         "\n"
         "  document          existing JSON file to start from (optional); the\n"
         "                    tree is always shown with schema diagnostics,\n"
         "                    `enter` fills a node and `ctrl+f` returns to it\n"
         "  --lib-dir <dir>   root used to complete `{lib:...}` references\n"
         "  --base-dir <dir>  directory used for relative path completion\n";
}

cform::Json empty_document(const cform::Json& root_schema) {
  const cform::Json root = cform::resolve_ref(root_schema, root_schema);
  const std::string type = cform::primary_type(root, root_schema);
  if (type == "array") return cform::Json::array_value();
  if (type == "object") return cform::Json::object_value();
  return cform::Json::null();
}

std::optional<cform::Json> read_document(const std::string& path) {
  std::ifstream stream(path);
  if (!stream) {
    std::cerr << "cform: cannot read " << path << "\n";
    return std::nullopt;
  }
  std::string content((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
  std::string error;
  const auto parsed = cform::Json::parse(content, error);
  if (!parsed) {
    std::cerr << "cform: " << path << " is not valid JSON: " << error << "\n";
    return std::nullopt;
  }
  return *parsed;
}

}  // namespace

int main(int argc, char** argv) {
  std::string lib_dir;
  std::string base_dir;
  std::string document_path;
  for (int index = 1; index < argc; index++) {
    const std::string argument = argv[index];
    if ((argument == "--lib-dir" || argument == "--base-dir") && index + 1 < argc) {
      (argument == "--lib-dir" ? lib_dir : base_dir) = argv[++index];
      continue;
    }
    if (argument == "-h" || argument == "--help") {
      usage(std::cout);
      return 0;
    }
    if (!argument.empty() && argument[0] == '-') {
      std::cerr << "cform: unknown argument: " << argument << "\n";
      return 2;
    }
    if (!document_path.empty()) {
      std::cerr << "cform: unexpected argument: " << argument << "\n";
      return 2;
    }
    document_path = argument;
  }

  if (isatty(STDIN_FILENO)) {
    std::cerr << "cform: reading the JSON Schema from stdin, finish with ctrl-d\n";
  }
  std::string input((std::istreambuf_iterator<char>(std::cin)),
                    std::istreambuf_iterator<char>());
  const std::string text = cform::trim(input);
  if (text.empty()) {
    std::cerr << "cform: no JSON Schema on stdin\n";
    return 1;
  }

  std::string error;
  const auto parsed = cform::Json::parse(text, error);
  if (!parsed) {
    std::cerr << "cform: stdin is not valid JSON: " << error << "\n";
    return 1;
  }
  if (!parsed->is_object()) {
    std::cerr << "cform: expected a JSON Schema object on stdin\n";
    return 1;
  }

  std::optional<cform::Json> document;
  if (!document_path.empty()) {
    document = read_document(document_path);
    if (!document) return 1;
  }

  cform::PromptUI* ui = nullptr;
  try {
    cform::PromptUI::Options options;
    if (!lib_dir.empty()) options.lib_dir = lib_dir;
    if (!base_dir.empty()) options.base_dir = base_dir;
    auto prompt_ui = std::make_unique<cform::PromptUI>(options);
    ui = prompt_ui.get();
    prompt_ui->set_tree_mode(true);

    std::string title = "cform";
    if (const cform::Json* schema_title = parsed->get("title");
        schema_title && schema_title->is_string()) {
      title += " — " + schema_title->string;
    }
    prompt_ui->banner(title);
    if (const cform::Json* description = parsed->get("description");
        description && description->is_string()) {
      prompt_ui->note(description->string, std::nullopt);
    }

    cform::Walker walker(*parsed, *prompt_ui);
    cform::Json value =
        document ? *document : empty_document(*parsed);
    cform::run_tree(*prompt_ui, *parsed, value, walker,
                    document_path.empty() ? "<new document>" : document_path);
    prompt_ui->close();
    std::cout << value.to_pretty(2) << "\n";
    return 0;
  } catch (const cform::PromptCancelled&) {
    if (ui) ui->cancel("prompt cancelled");
    return 1;
  } catch (const std::exception& exception) {
    std::cerr << "cform: " << exception.what() << "\n";
    return 1;
  }
}
