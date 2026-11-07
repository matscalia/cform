// cform: compile a JSON Schema read from stdin into a JSON value through the
// same schema-driven prompts as `orchestra prompt`.
//
// Prompts are drawn on the controlling terminal (/dev/tty) so stdout stays
// reserved for the resulting JSON.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <unistd.h>

#include "json.hpp"
#include "schema.hpp"
#include "tree.hpp"
#include "tui.hpp"
#include "walker.hpp"

#ifdef CFORM_HAVE_CHAFA
#include "artwork_chafa.hpp"
#endif

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
         "  --base-dir <dir>  directory used for relative path completion\n"
         "  --prompt          start by prompting the root instead of the tree;\n"
         "                    ctrl+f returns to the tree\n"
         "  --image <file>    show an image on the right when it fits (chafa)\n"
         "  --use-terminal-theme\n"
         "                    render image colors with the terminal's 16-color\n"
         "                    palette instead of truecolor\n"
         "  --image-min-width <n>, --image-min-height <n>\n"
         "                    hide the image below this cell size (default 8x4)\n"
         "  --image-min-content-width <n>\n"
         "                    cells reserved for the UI (default 40)\n"
         "  --image-gap <n>   blank cells between the UI and the image (default 2)\n"
         "  --image-margin <n>\n"
         "                    blank cells to the right of the image (default 0)\n"
         "  --image-placement <right|above>\n"
         "                    side panel or right-aligned block above (default right)\n"
         "  --image-max-width <n>, --image-max-height <n>\n"
         "                    optional caps; 0 = no cap (default 0x0)\n"
         "  --image-caption <text>\n"
         "                    plain-text caption drawn over the image\n"
         "  --image-caption-position <top|center|middle|bottom|none|0..1>\n"
         "                    caption row (default bottom)\n"
         "  --image-caption-align <left|center|right>\n"
         "                    caption alignment (default center)\n"
         "  --image-caption-padding <n>\n"
         "                    cells between caption and image edge (default 1)\n"
         "  --image-caption-color <rrggbb|none|transparent>,\n"
         "  --image-caption-bg <rrggbb|none|transparent|terminal>\n"
         "                    caption colors (default ffffff on 000000;\n"
         "                    none/transparent keep the image color; terminal\n"
         "                    uses the terminal's background)\n"
         "  --image-caption-bold, --image-caption-italic\n"
         "                    caption font style (combine for both)\n";
}

bool parse_int(const std::string& text, int& value) {
  try {
    std::size_t used = 0;
    const int parsed = std::stoi(text, &used);
    if (used != text.size() || parsed < 0) return false;
    value = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_color(const std::string& text, int& value) {
  std::string hex = text;
  if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
  if (hex.size() != 6) return false;
  try {
    std::size_t used = 0;
    const unsigned long parsed = std::stoul(hex, &used, 16);
    if (used != hex.size()) return false;
    value = static_cast<int>(parsed & 0xFFFFFFul);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_caption_position(const std::string& text, float& value) {
  if (text == "none") {
    value = -1.0f;
    return true;
  }
  if (text == "top") {
    value = 0.0f;
    return true;
  }
  if (text == "center" || text == "middle") {
    value = 0.5f;
    return true;
  }
  if (text == "bottom") {
    value = 1.0f;
    return true;
  }
  try {
    std::size_t used = 0;
    const float parsed = std::stof(text, &used);
    if (used != text.size() || !std::isfinite(parsed) || parsed < 0.0f ||
        parsed > 1.0f)
      return false;
    value = parsed;
    return true;
  } catch (...) {
    return false;
  }
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
  auto parsed = cform::Json::parse(content, error);
  if (!parsed) {
    std::cerr << "cform: " << path << " is not valid JSON: " << error << "\n";
    return std::nullopt;
  }
  return std::move(*parsed);
}

}  // namespace

int main(int argc, char** argv) {
  std::string lib_dir;
  std::string base_dir;
  std::string document_path;
  std::string image_path;
  int image_min_width = 8;
  int image_min_height = 4;
  int image_min_content_width = 40;
  int image_gap = 2;
  int image_margin = 0;
  int image_max_width = 0;
  int image_max_height = 0;
  bool image_above = false;
  bool use_terminal_theme = false;
  bool prompt_root = false;
  std::string image_caption;
  float image_caption_position = 1.0f;
  std::string image_caption_align = "center";
  int image_caption_padding = 1;
  int image_caption_fg = 0xFFFFFF;
  int image_caption_bg = 0x000000;
  bool image_caption_bold = false;
  bool image_caption_italic = false;
  for (int index = 1; index < argc; index++) {
    const std::string argument = argv[index];
    const bool takes_value =
        argument == "--lib-dir" || argument == "--base-dir" ||
        argument == "--image" || argument == "--image-min-width" ||
        argument == "--image-min-height" || argument == "--image-gap" ||
        argument == "--image-margin" || argument == "--image-max-width" ||
        argument == "--image-max-height" ||
        argument == "--image-min-content-width" ||
        argument == "--image-placement" || argument == "--image-caption" ||
        argument == "--image-caption-position" ||
        argument == "--image-caption-align" ||
        argument == "--image-caption-padding" ||
        argument == "--image-caption-color" ||
        argument == "--image-caption-bg";
    if (takes_value) {
      if (index + 1 >= argc) {
        std::cerr << "cform: missing value for " << argument << "\n";
        return 2;
      }
      const std::string value = argv[++index];
      if (argument == "--lib-dir") lib_dir = value;
      else if (argument == "--base-dir") base_dir = value;
      else if (argument == "--image") image_path = value;
      else if (argument == "--image-placement") {
        if (value == "right") image_above = false;
        else if (value == "above") image_above = true;
        else {
          std::cerr << "cform: --image-placement must be right or above\n";
          return 2;
        }
      } else if (argument == "--image-caption") {
        image_caption = value;
      } else if (argument == "--image-caption-position") {
        if (!parse_caption_position(value, image_caption_position)) {
          std::cerr << "cform: --image-caption-position must be top, center, "
                       "middle, bottom, none or a number in 0..1\n";
          return 2;
        }
      } else if (argument == "--image-caption-align") {
        if (value != "left" && value != "center" && value != "right") {
          std::cerr << "cform: --image-caption-align must be left, center or right\n";
          return 2;
        }
        image_caption_align = value;
      } else if (argument == "--image-caption-color") {
        if (value == "none" || value == "transparent") {
          image_caption_fg = -1;
        } else if (!parse_color(value, image_caption_fg)) {
          std::cerr << "cform: invalid color for --image-caption-color: " << value << "\n";
          return 2;
        }
      } else if (argument == "--image-caption-bg") {
        if (value == "none" || value == "transparent") {
          image_caption_bg = cform::kCaptionBackgroundKeep;
        } else if (value == "terminal") {
          image_caption_bg = cform::kCaptionBackgroundTerminal;
        } else if (!parse_color(value, image_caption_bg)) {
          std::cerr << "cform: invalid color for --image-caption-bg: " << value << "\n";
          return 2;
        }
      } else {
        int* target = nullptr;
        if (argument == "--image-min-width") target = &image_min_width;
        else if (argument == "--image-min-height") target = &image_min_height;
        else if (argument == "--image-min-content-width") target = &image_min_content_width;
        else if (argument == "--image-gap") target = &image_gap;
        else if (argument == "--image-margin") target = &image_margin;
        else if (argument == "--image-max-width") target = &image_max_width;
        else if (argument == "--image-max-height") target = &image_max_height;
        else if (argument == "--image-caption-padding") target = &image_caption_padding;
        if (!target || !parse_int(value, *target)) {
          std::cerr << "cform: invalid value for " << argument << ": " << value << "\n";
          return 2;
        }
      }
      continue;
    }
    if (argument == "--use-terminal-theme") {
      use_terminal_theme = true;
      continue;
    }
    if (argument == "--prompt") {
      prompt_root = true;
      continue;
    }
    if (argument == "--image-caption-bold") {
      image_caption_bold = true;
      continue;
    }
    if (argument == "--image-caption-italic") {
      image_caption_italic = true;
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

  std::unique_ptr<cform::PromptUI> prompt_ui;
  cform::PromptUI* ui = nullptr;
  try {
    cform::PromptUI::Options options;
    if (!lib_dir.empty()) options.lib_dir = lib_dir;
    if (!base_dir.empty()) options.base_dir = base_dir;
    prompt_ui = std::make_unique<cform::PromptUI>(options);
    ui = prompt_ui.get();
    prompt_ui->set_tree_mode(true);

#ifdef CFORM_HAVE_CHAFA
    if (!image_path.empty()) {
      cform::ArtworkLayout layout;
      layout.placement = image_above ? cform::ArtworkPlacement::Above
                                     : cform::ArtworkPlacement::Right;
      layout.min_width = image_min_width;
      layout.min_height = image_min_height;
      layout.min_content_width = image_min_content_width;
      layout.gap = image_gap;
      layout.margin = image_margin;
      layout.max_width = image_max_width;
      layout.max_height = image_max_height;
      layout.caption = image_caption;
      layout.caption_position = image_caption_position;
      layout.caption_align = image_caption_align == "left"
                                 ? cform::ArtworkAlign::Left
                                 : image_caption_align == "right"
                                       ? cform::ArtworkAlign::Right
                                       : cform::ArtworkAlign::Center;
      layout.caption_padding = image_caption_padding;
      layout.caption_fg = image_caption_fg;
      layout.caption_bg = image_caption_bg;
      layout.caption_bold = image_caption_bold;
      layout.caption_italic = image_caption_italic;

      prompt_ui->set_artwork_provider(
          [image_path, use_terminal_theme](int max_width, int max_height) {
            return cform::render_artwork_file(image_path, max_width, max_height,
                                              use_terminal_theme);
          },
          layout);
      if (!prompt_ui->refresh_artwork()) {
        std::cerr << "cform: cannot render image " << image_path << "\n";
      }
    }
#else
    if (!image_path.empty()) {
      std::cerr << "cform: built without chafa support; ignoring --image\n";
    }
#endif

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
        document ? *document : cform::empty_document(*parsed);
    cform::run_tree(*prompt_ui, *parsed, value, walker,
                    document_path.empty() ? "<new document>" : document_path,
                    prompt_root);
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
