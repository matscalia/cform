// Embind entry points for the Node/Wasm build.
//
// `run(schema, document, options)` is blocking: it draws the same tree screen
// and prompts as the native CLI, reading keys through the JS terminal backend.
// It never reads the schema from stdin, so stdin stays free for keystrokes.
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "artwork.hpp"
#include "json.hpp"
#include "schema.hpp"
#include "tree.hpp"
#include "tui.hpp"
#include "walker.hpp"

namespace {

struct RunOptions {
  std::string libDir;
  std::string baseDir;
  int columns = 0;
  bool promptRoot = false;
  // Pre-rendered ANSI artwork (chafa-wasm renders it in the Node wrapper).
  std::string artAnsi;
  int artWidth = 0;
  int artMinWidth = 8;
  int artMinHeight = 4;
  int artMinContentWidth = 40;
  int artGap = 2;
  int artMargin = 0;
  int artMaxWidth = 0;
  int artMaxHeight = 0;
  std::string artPlacement = "right";
  std::string artCaption;
  float artCaptionPosition = -1.0f;
  std::string artCaptionAlign = "center";
  int artCaptionPadding = 1;
  int artCaptionFg = 0xFFFFFF;
  int artCaptionBg = 0x000000;
  bool artCaptionBold = false;
  bool artCaptionItalic = false;
};

struct RunResult {
  bool ok = false;
  bool cancelled = false;
  std::string value;
  std::string error;
};

RunResult run(const std::string& schema_text, const std::string& document_text,
              const RunOptions& options) {
  RunResult result;
  if (cform::trim(schema_text).empty()) {
    result.error = "no JSON Schema provided";
    return result;
  }

  std::string error;
  const auto schema = cform::Json::parse(schema_text, error);
  if (!schema) {
    result.error = "schema is not valid JSON: " + error;
    return result;
  }
  if (!schema->is_object()) {
    result.error = "expected a JSON Schema object";
    return result;
  }

  std::optional<cform::Json> document;
  if (!cform::trim(document_text).empty()) {
    auto parsed = cform::Json::parse(document_text, error);
    if (!parsed) {
      result.error = "document is not valid JSON: " + error;
      return result;
    }
    document = std::move(*parsed);
  }

  std::unique_ptr<cform::PromptUI> prompt_ui;
  cform::PromptUI* ui = nullptr;
  try {
    cform::PromptUI::Options ui_options;
    if (!options.libDir.empty()) ui_options.lib_dir = options.libDir;
    if (!options.baseDir.empty()) ui_options.base_dir = options.baseDir;
    if (options.columns > 0) ui_options.columns = options.columns;

    prompt_ui = std::make_unique<cform::PromptUI>(ui_options);
    ui = prompt_ui.get();
    prompt_ui->set_tree_mode(true);

    cform::ArtworkLayout layout;
    layout.placement = options.artPlacement == "above"
                           ? cform::ArtworkPlacement::Above
                           : cform::ArtworkPlacement::Right;
    layout.min_width = options.artMinWidth;
    layout.min_height = options.artMinHeight;
    layout.min_content_width = options.artMinContentWidth;
    layout.gap = options.artGap;
    layout.margin = options.artMargin;
    layout.max_width = options.artMaxWidth;
    layout.max_height = options.artMaxHeight;
    layout.caption = options.artCaption;
    layout.caption_position = options.artCaptionPosition;
    layout.caption_align = options.artCaptionAlign == "left"
                               ? cform::ArtworkAlign::Left
                               : options.artCaptionAlign == "right"
                                     ? cform::ArtworkAlign::Right
                                     : cform::ArtworkAlign::Center;
    layout.caption_padding = options.artCaptionPadding;
    layout.caption_fg = options.artCaptionFg;
    layout.caption_bg = options.artCaptionBg;
    layout.caption_bold = options.artCaptionBold;
    layout.caption_italic = options.artCaptionItalic;

    if (!options.artAnsi.empty()) {
      if (auto artwork = cform::make_artwork(options.artAnsi, options.artWidth)) {
        prompt_ui->set_artwork(std::move(*artwork), layout);
      }
      emscripten::val render_fn =
          emscripten::val::module_property("cformRenderArtwork");
      if (!render_fn.isUndefined() && !render_fn.isNull()) {
        prompt_ui->set_artwork_provider(
            [render_fn](int max_width, int max_height)
                -> std::optional<cform::Artwork> {
              emscripten::val result = render_fn(max_width, max_height);
              if (result.isUndefined() || result.isNull()) return std::nullopt;
              const std::string ansi = result.as<std::string>();
              if (ansi.empty()) return std::nullopt;
              return cform::make_artwork(ansi);
            },
            layout);
        // The image was decoded at load/prepare time; render it again with the
        // terminal size that exists right now (it may have changed since).
        prompt_ui->refresh_artwork();
      }
    }

    std::string title = "cform";
    if (const cform::Json* schema_title = schema->get("title");
        schema_title && schema_title->is_string()) {
      title += " — " + schema_title->string;
    }
    prompt_ui->banner(title);
    if (const cform::Json* description = schema->get("description");
        description && description->is_string()) {
      prompt_ui->note(description->string, std::nullopt);
    }

    cform::Walker walker(*schema, *prompt_ui);
    cform::Json value = document ? *document : cform::empty_document(*schema);
    cform::run_tree(*prompt_ui, *schema, value, walker,
                    document ? "document" : "<new document>",
                    options.promptRoot);
    prompt_ui->close();
    result.ok = true;
    result.value = value.to_pretty(2);
    return result;
  } catch (const cform::PromptCancelled&) {
    if (ui) ui->cancel("prompt cancelled");
    result.cancelled = true;
    result.error = "prompt cancelled";
    return result;
  } catch (const std::exception& exception) {
    result.error = exception.what();
    return result;
  }
}

}  // namespace

EMSCRIPTEN_BINDINGS(cform_wasm) {
  emscripten::value_object<RunOptions>("RunOptions")
      .field("libDir", &RunOptions::libDir)
      .field("baseDir", &RunOptions::baseDir)
      .field("columns", &RunOptions::columns)
      .field("promptRoot", &RunOptions::promptRoot)
      .field("artAnsi", &RunOptions::artAnsi)
      .field("artWidth", &RunOptions::artWidth)
      .field("artMinWidth", &RunOptions::artMinWidth)
      .field("artMinHeight", &RunOptions::artMinHeight)
      .field("artMinContentWidth", &RunOptions::artMinContentWidth)
      .field("artGap", &RunOptions::artGap)
      .field("artMargin", &RunOptions::artMargin)
      .field("artMaxWidth", &RunOptions::artMaxWidth)
      .field("artMaxHeight", &RunOptions::artMaxHeight)
      .field("artPlacement", &RunOptions::artPlacement)
      .field("artCaption", &RunOptions::artCaption)
      .field("artCaptionPosition", &RunOptions::artCaptionPosition)
      .field("artCaptionAlign", &RunOptions::artCaptionAlign)
      .field("artCaptionPadding", &RunOptions::artCaptionPadding)
      .field("artCaptionFg", &RunOptions::artCaptionFg)
      .field("artCaptionBg", &RunOptions::artCaptionBg)
      .field("artCaptionBold", &RunOptions::artCaptionBold)
      .field("artCaptionItalic", &RunOptions::artCaptionItalic);

  emscripten::value_object<RunResult>("RunResult")
      .field("ok", &RunResult::ok)
      .field("cancelled", &RunResult::cancelled)
      .field("value", &RunResult::value)
      .field("error", &RunResult::error);

  emscripten::function("run", &run);
}
