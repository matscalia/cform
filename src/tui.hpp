// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matteo Scalia

// Terminal UI of cform: the same prompt kinds and rendering as the TypeScript
// implementation, without any third-party TUI library.
#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "artwork.hpp"

namespace cform {

struct Option {
  std::string label;
  std::optional<std::string> hint;
};

struct TextPrompt {
  std::string message;
  std::optional<std::string> hint;
  std::optional<std::string> placeholder;
  std::optional<std::string> default_value;
  std::optional<std::string> initial_value;
  std::vector<std::string> suggest;
  bool complete = true;
  bool multiline = false;
  std::function<std::optional<std::string>(const std::string&)> validate;
};

struct SelectPrompt {
  std::string message;
  std::optional<std::string> hint;
  std::vector<Option> options;
  std::optional<int> initial_index;
};

struct MultiSelectPrompt {
  std::string message;
  std::optional<std::string> hint;
  std::vector<Option> options;
  std::vector<int> initial_indices;
  bool required = false;
};

struct ConfirmPrompt {
  std::string message;
  std::optional<std::string> hint;
  std::optional<bool> initial;
};

template <class T>
struct Reply {
  T value{};
  bool back = false;
};

class PromptCancelled : public std::runtime_error {
 public:
  PromptCancelled() : std::runtime_error("cancelled") {}
};

/// thrown when ctrl+f asks the prompt walker to go back to the tree screen
class TreeJump : public std::exception {
 public:
  const char* what() const noexcept override { return "tree jump"; }
};

/// Overlay box drawn near a content line, used by the tree screen.
struct Popup {
  std::string text;
  /// 0-based index in the content passed to render_lines().
  int anchor_line = 0;
};

/// one decoded key press
struct Key {
  std::string name;
  bool ctrl = false;
  std::string str;
  bool has_str = false;
};

class Prompter {
 public:
  virtual ~Prompter() = default;
  virtual Reply<std::string> text(const TextPrompt& prompt) = 0;
  virtual Reply<int> select(const SelectPrompt& prompt) = 0;
  virtual Reply<std::vector<int>> multiselect(const MultiSelectPrompt& prompt) = 0;
  virtual Reply<bool> confirm(const ConfirmPrompt& prompt) = 0;
  virtual void note(const std::string& message,
                    const std::optional<std::string>& title) {
    (void)message;
    (void)title;
  }
  virtual void error(const std::string& message) { (void)message; }
};

class PromptUI : public Prompter {
 public:
  struct Options {
    std::optional<std::string> lib_dir;
    std::optional<std::string> base_dir;
    std::optional<int> columns;
  };

  explicit PromptUI(Options options = {});
  ~PromptUI() override;

  PromptUI(const PromptUI&) = delete;
  PromptUI& operator=(const PromptUI&) = delete;

  Reply<std::string> text(const TextPrompt& prompt) override;
  Reply<int> select(const SelectPrompt& prompt) override;
  Reply<std::vector<int>> multiselect(const MultiSelectPrompt& prompt) override;
  Reply<bool> confirm(const ConfirmPrompt& prompt) override;
  void note(const std::string& message,
            const std::optional<std::string>& title) override;
  void error(const std::string& message) override;

  void banner(const std::string& title);
  void outro(const std::string& message);
  void cancel(const std::string& message);
  void close();

  // --- primitives for custom screens (the tree view) ------------------------
  Key read_key();
  void render_lines(const std::vector<std::string>& lines,
                    bool pin_last_line = false,
                    bool show_artwork = true,
                    const Popup* popup = nullptr);
  void clear_lines();
  int terminal_rows() const;
  int terminal_columns() const;
  /// when enabled, ctrl+f aborts a prompt with `TreeJump`
  void set_tree_mode(bool enabled);

  /// right-aligned artwork drawn next to/under the frame when it fits.
  void set_artwork(std::optional<Artwork> artwork,
                   ArtworkLayout layout = ArtworkLayout{});

  /// Provider used to (re)render the artwork when the terminal is resized.
  using ArtworkProvider =
      std::function<std::optional<Artwork>(int max_width, int max_height)>;
  void set_artwork_provider(ArtworkProvider provider,
                            ArtworkLayout layout = ArtworkLayout{});
  /// Renders the artwork with the provider at the current terminal size.
  bool refresh_artwork();

  /// Start/stop a wizard prompt session. While active, prompt history flows
  /// next to a right-side image and scrolls within that column.
  void begin_prompts();
  void end_prompts();
  bool can_resume_prompt() const;
  /// Re-run the last prompt session from its recorded key logs. Returns false
  /// when there is no prompt to resume.
  bool begin_prompt_replay();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace cform
