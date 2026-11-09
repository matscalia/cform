// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matteo Scalia

// Native artwork renderer: image file -> ANSI via libchafa.
#pragma once

#include <optional>
#include <string>

#include "artwork.hpp"

namespace cform {

/// Renders `path` into at most `width` x `height` terminal cells.
/// When `use_terminal_theme` is true, colors are mapped to the terminal's
/// 16-color ANSI palette instead of truecolor. Returns nullopt when the image
/// cannot be read.
std::optional<Artwork> render_artwork_file(const std::string& path, int width,
                                           int height,
                                           bool use_terminal_theme = false);

}  // namespace cform
