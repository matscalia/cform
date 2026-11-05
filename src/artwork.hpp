// Pre-rendered ANSI artwork (e.g. from libchafa) that the prompt UI can show
// on the right side of its frame.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cform {

struct Artwork {
  int width = 0;   // terminal cells
  int height = 0;  // rows
  std::vector<std::string> lines;

  bool empty() const { return width <= 0 || height <= 0 || lines.empty(); }
};

enum class ArtworkPlacement {
  /// Right-side panel; the prompt UI is truncated to make room.
  Right,
  /// Right-aligned block above the prompt UI.
  Above,
};

enum class ArtworkAlign { Left, Center, Right };

struct ArtworkLayout {
  ArtworkPlacement placement = ArtworkPlacement::Right;
  // Below this size the artwork is not worth showing.
  int min_width = 8;
  int min_height = 4;
  // Room reserved for the prompt UI (longer lines are truncated); Right only.
  int min_content_width = 40;
  // Blank cells between the prompt and the image, and to the right edge.
  int gap = 2;
  int margin = 0;
  // Optional caps for the initial/re-rendered image (0 = use available space).
  int max_width = 0;
  int max_height = 0;

  // Caption drawn over the artwork (plain text, `\n` for multiple lines).
  std::string caption;
  // < 0 hides the caption; 0 = top, 0.5 = center, 1 = bottom.
  float caption_position = -1.0f;
  ArtworkAlign caption_align = ArtworkAlign::Center;
  int caption_padding = 1;
  int caption_fg = 0xFFFFFF;  // packed 0xRRGGBB; negative = keep image fg
  int caption_bg = 0x000000;  // packed 0xRRGGBB; negative = keep image bg
  bool caption_bold = false;
  bool caption_italic = false;
};

/// Visible cell count of a line containing ANSI CSI escape sequences. Counts
/// UTF-8 lead bytes as one cell (adequate for chafa's half-block output).
int artwork_cell_width(std::string_view line);

/// Splits an ANSI string into lines and builds an Artwork. `width` is used when
/// it is positive (otherwise it is measured); lines shorter than the final
/// width are padded with spaces so the image aligns.
std::optional<Artwork> make_artwork(std::string_view ansi, int width = 0);

/// Replaces the caption rows of `artwork` with styled text. Called by
/// PromptUI::set_artwork; exposed for callers that cache artwork themselves.
void apply_artwork_caption(Artwork& artwork, const ArtworkLayout& layout);

}  // namespace cform
