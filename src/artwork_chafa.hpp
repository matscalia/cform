// Native artwork renderer: image file -> ANSI via libchafa.
#pragma once

#include <optional>
#include <string>

#include "artwork.hpp"

namespace cform {

/// Renders `path` into at most `width` x `height` terminal cells.
/// Returns nullopt when the image cannot be read.
std::optional<Artwork> render_artwork_file(const std::string& path, int width,
                                           int height);

}  // namespace cform
