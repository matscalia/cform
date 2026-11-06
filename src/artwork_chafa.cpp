#include "artwork_chafa.hpp"

#include <chafa.h>

// stb_image contains static helpers that are not used by every translation
// unit; silence only that warning while including it.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace cform {

std::optional<Artwork> render_artwork_file(const std::string& path,
                                           int max_width, int max_height) {
  if (max_width <= 0 || max_height <= 0) return std::nullopt;

  int image_width = 0;
  int image_height = 0;
  int channels = 0;
  unsigned char* pixels =
      stbi_load(path.c_str(), &image_width, &image_height, &channels, 4);
  if (!pixels) return std::nullopt;

  // 0.5 is the typical terminal cell width/height ratio. calc_canvas_geometry
  // picks the largest canvas with the source image's aspect ratio; without it
  // the image would be stretched to the requested box.
  constexpr gfloat kFontRatio = 0.5f;
  gint canvas_width = max_width;
  gint canvas_height = max_height;
  chafa_calc_canvas_geometry(image_width, image_height, &canvas_width,
                             &canvas_height, kFontRatio, TRUE, FALSE);
  if (canvas_width < 1) canvas_width = 1;
  if (canvas_height < 1) canvas_height = 1;

  ChafaCanvasConfig* config = chafa_canvas_config_new();
  chafa_canvas_config_set_cell_geometry(config, 8, 16);
  chafa_canvas_config_set_geometry(config, canvas_width, canvas_height);
  chafa_canvas_config_set_canvas_mode(config, CHAFA_CANVAS_MODE_TRUECOLOR);
  chafa_canvas_config_set_dither_mode(config, CHAFA_DITHER_MODE_NONE);

  ChafaCanvas* canvas = chafa_canvas_new(config);
  std::optional<Artwork> artwork;
  if (canvas) {
    chafa_canvas_draw_all_pixels(canvas, CHAFA_PIXEL_RGBA8_UNASSOCIATED, pixels,
                                 image_width, image_height, image_width * 4);
    // A null ChafaTermInfo selects the same fallback terminfo that the
    // deprecated chafa_canvas_build_ansi() used, so the ANSI bytes are
    // unchanged while avoiding the deprecated entry point.
    GString* ansi = chafa_canvas_print(canvas, nullptr);
    if (ansi) {
      artwork = make_artwork(std::string_view(ansi->str, ansi->len));
      g_string_free(ansi, TRUE);
    }
    chafa_canvas_unref(canvas);
  }
  chafa_canvas_config_unref(config);
  stbi_image_free(pixels);
  return artwork;
}

}  // namespace cform
