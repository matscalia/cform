#include "artwork.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cform {

int artwork_cell_width(std::string_view line) {
  int width = 0;
  for (std::size_t index = 0; index < line.size();) {
    const unsigned char ch = static_cast<unsigned char>(line[index]);
    if (ch == '\x1b' && index + 1 < line.size() && line[index + 1] == '[') {
      index += 2;
      while (index < line.size()) {
        const unsigned char code = static_cast<unsigned char>(line[index]);
        if (code >= '@' && code <= '~') {
          index++;
          break;
        }
        index++;
      }
      continue;
    }
    if ((ch & 0xC0) != 0x80) width++;  // UTF-8 lead byte (or ASCII)
    index++;
  }
  return width;
}

std::optional<Artwork> make_artwork(std::string_view ansi, int width) {
  Artwork artwork;
  std::string current;
  auto flush = [&]() {
    if (!current.empty() && current.back() == '\r') current.pop_back();
    artwork.lines.push_back(current);
    current.clear();
  };
  for (char ch : ansi) {
    if (ch == '\n') flush();
    else current.push_back(ch);
  }
  if (!current.empty()) flush();
  // Drop trailing empty lines produced by a final newline.
  while (!artwork.lines.empty() && artwork.lines.back().empty())
    artwork.lines.pop_back();
  if (artwork.lines.empty()) return std::nullopt;

  int measured = 0;
  for (const std::string& line : artwork.lines)
    measured = std::max(measured, artwork_cell_width(line));
  const int resolved = width > 0 ? std::max(width, measured) : measured;
  if (resolved <= 0) return std::nullopt;

  for (std::string& line : artwork.lines) {
    const int cells = artwork_cell_width(line);
    if (cells < resolved) line.append(static_cast<std::size_t>(resolved - cells), ' ');
  }
  artwork.width = resolved;
  artwork.height = static_cast<int>(artwork.lines.size());
  return artwork;
}

namespace {

std::vector<std::string> caption_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::string current;
  for (char ch : text) {
    if (ch == '\n') {
      if (!current.empty() && current.back() == '\r') current.pop_back();
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    if (current.back() == '\r') current.pop_back();
    lines.push_back(current);
  }
  return lines;
}

std::string clip_caption(std::string_view text, int max_cells) {
  std::string out;
  int cells = 0;
  for (std::size_t index = 0; index < text.size();) {
    const unsigned char ch = static_cast<unsigned char>(text[index]);
    if (ch == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
      index += 2;
      while (index < text.size()) {
        const unsigned char code = static_cast<unsigned char>(text[index]);
        index++;
        if (code >= '@' && code <= '~') break;
      }
      continue;  // captions are plain text; drop any styling
    }
    std::size_t length = 1;
    if ((ch & 0xE0) == 0xC0) length = 2;
    else if ((ch & 0xF0) == 0xE0) length = 3;
    else if ((ch & 0xF8) == 0xF0) length = 4;
    if (index + length > text.size()) length = 1;
    if (cells + 1 > max_cells) break;
    out.append(text, index, length);
    cells++;
    index += length;
  }
  return out;
}

std::string sgr_color(int rgb, bool background) {
  const unsigned value = static_cast<unsigned>(rgb) & 0xFFFFFFu;
  const unsigned red = (value >> 16) & 0xFF;
  const unsigned green = (value >> 8) & 0xFF;
  const unsigned blue = value & 0xFF;
  return "\x1b[" + std::to_string(background ? 48 : 38) + ";2;" +
         std::to_string(red) + ";" + std::to_string(green) + ";" +
         std::to_string(blue) + "m";
}

/// Replaces glyphs without touching any of the original SGR sequences, so the
/// image's background (and, with `foreground < 0`, its foreground) is kept.
void overlay_preserving_background(std::string& line, std::string_view text,
                                   int offset, int foreground, bool bold,
                                   bool italic) {
  if (offset < 0 || text.empty()) return;
  std::string out;
  out.reserve(line.size() + text.size() + 32);
  int cell = 0;
  std::size_t text_index = 0;
  for (std::size_t index = 0; index < line.size();) {
    const unsigned char ch = static_cast<unsigned char>(line[index]);
    if (ch == '\x1b' && index + 1 < line.size() && line[index + 1] == '[') {
      const std::size_t start = index;
      index += 2;
      while (index < line.size()) {
        const unsigned char code = static_cast<unsigned char>(line[index]);
        index++;
        if (code >= '@' && code <= '~') break;
      }
      out.append(line, start, index - start);
      continue;
    }
    std::size_t length = 1;
    if ((ch & 0xE0) == 0xC0) length = 2;
    else if ((ch & 0xF0) == 0xE0) length = 3;
    else if ((ch & 0xF8) == 0xF0) length = 4;
    if (index + length > line.size()) length = 1;

    if (cell >= offset && text_index < text.size()) {
      if (foreground >= 0) out += sgr_color(foreground, false);
      if (bold) out += "\x1b[1m";
      if (italic) out += "\x1b[3m";
      const unsigned char tc = static_cast<unsigned char>(text[text_index]);
      std::size_t text_length = 1;
      if ((tc & 0xE0) == 0xC0) text_length = 2;
      else if ((tc & 0xF0) == 0xE0) text_length = 3;
      else if ((tc & 0xF8) == 0xF0) text_length = 4;
      if (text_index + text_length > text.size()) text_length = 1;
      out.append(text, text_index, text_length);
      text_index += text_length;
      if (text_index == text.size()) {
        if (foreground >= 0) out += "\x1b[39m";
        if (bold) out += "\x1b[22m";
        if (italic) out += "\x1b[23m";
      }
    } else {
      out.append(line, index, length);
    }
    cell++;
    index += length;
  }
  line = std::move(out);
}

}  // namespace

void apply_artwork_caption(Artwork& artwork, const ArtworkLayout& layout) {
  if (layout.caption.empty() || layout.caption_position < 0.0f) return;
  if (artwork.width <= 0 || artwork.lines.empty()) return;
  const std::vector<std::string> lines = caption_lines(layout.caption);
  if (lines.empty()) return;

  const int count = std::min<int>(
      static_cast<int>(lines.size()), static_cast<int>(artwork.lines.size()));
  const int max_first = static_cast<int>(artwork.lines.size()) - count;
  const float position = std::clamp(layout.caption_position, 0.0f, 1.0f);
  const int first =
      static_cast<int>(std::lround(position * static_cast<float>(max_first)));
  const int padding = std::max(0, layout.caption_padding);
  const int available = std::max(0, artwork.width - 2 * padding);
  const int foreground = layout.caption_fg >= 0 ? layout.caption_fg : 0xFFFFFF;
  std::string style = "\x1b[0m";
  if (layout.caption_bold) style += "\x1b[1m";
  if (layout.caption_italic) style += "\x1b[3m";
  style += sgr_color(foreground, false);
  if (layout.caption_bg >= 0) style += sgr_color(layout.caption_bg, true);

  for (int index = 0; index < count; index++) {
    std::string text = clip_caption(lines[index], available);
    const int cells = artwork_cell_width(text);
    int offset = padding;
    if (layout.caption_align == ArtworkAlign::Center) {
      offset = (artwork.width - cells) / 2;
    } else if (layout.caption_align == ArtworkAlign::Right) {
      offset = artwork.width - padding - cells;
    }
    offset = std::max(0, std::min(offset, artwork.width - cells));
    if (layout.caption_bg < 0) {
      overlay_preserving_background(artwork.lines[first + index], text, offset,
                                    layout.caption_fg, layout.caption_bold,
                                    layout.caption_italic);
      continue;
    }
    std::string row = style;
    row.append(static_cast<std::size_t>(offset), ' ');
    row += text;
    row.append(static_cast<std::size_t>(artwork.width - offset - cells), ' ');
    row += "\x1b[0m";
    artwork.lines[first + index] = std::move(row);
  }
}

}  // namespace cform
