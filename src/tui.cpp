#include "tui.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "schema.hpp"

namespace cform {

namespace {

constexpr const char* RESET = "\x1b[0m";
constexpr const char* DIM = "\x1b[2m";
constexpr const char* RED = "\x1b[31m";
constexpr const char* CYAN = "\x1b[36m";
constexpr const char* REVERSE = "\x1b[7m";
constexpr const char* HIDE_CURSOR = "\x1b[?25l";
constexpr const char* SHOW_CURSOR = "\x1b[?25h";

constexpr int ESCAPE_TIMEOUT_MS = 50;
constexpr std::size_t MAX_COMPLETIONS = 32;
constexpr std::size_t COMPLETION_MENU_MIN = 2;
constexpr std::size_t COMPLETION_MENU_SIZE = 8;

std::string dim(const std::string& text) { return std::string(DIM) + text + RESET; }
std::string cyan(const std::string& text) { return std::string(CYAN) + text + RESET; }
std::string red(const std::string& text) { return std::string(RED) + text + RESET; }

std::size_t visible_length(const std::string& text) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
      index += 2;
      while (index < text.size() &&
             !std::isalpha(static_cast<unsigned char>(text[index])) &&
             text[index] != '~') {
        index++;
      }
      if (index < text.size()) index++;
      continue;
    }
    count++;
    index++;
  }
  return count;
}

std::string fit_line(const std::string& text, std::size_t columns) {
  if (visible_length(text) <= columns) return text;
  std::string out;
  std::size_t visible = 0;
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
      const std::size_t start = index;
      index += 2;
      while (index < text.size() &&
             !std::isalpha(static_cast<unsigned char>(text[index])) &&
             text[index] != '~') {
        index++;
      }
      if (index < text.size()) index++;
      out.append(text, start, index - start);
      continue;
    }
    if (visible + 1 >= columns) {
      out += RESET;
      break;
    }
    out += text[index];
    visible++;
    index++;
  }
  return out;
}

std::pair<std::string, std::string> first_codepoint(const std::string& text) {
  if (text.empty()) return {"", ""};
  unsigned char ch = static_cast<unsigned char>(text[0]);
  std::size_t length = 1;
  if ((ch & 0x80) == 0) length = 1;
  else if ((ch & 0xE0) == 0xC0) length = 2;
  else if ((ch & 0xF0) == 0xE0) length = 3;
  else if ((ch & 0xF8) == 0xF0) length = 4;
  if (length > text.size()) length = 1;
  return {text.substr(0, length), text.substr(length)};
}

std::vector<std::string> path_completions(const std::string& input,
                                          const std::string& root) {
  const bool tilde = !input.empty() && input[0] == '~';
  const bool absolute_input = !input.empty() && input[0] == '/';
  const char* home = std::getenv("HOME");
  std::string expanded = input;
  if (tilde) expanded = std::string(home ? home : "") + input.substr(1);
  std::string absolute;
  if (!expanded.empty() && expanded[0] == '/') {
    absolute = expanded;
  } else {
    absolute = root;
    if (!absolute.empty() && absolute.back() != '/') absolute += '/';
    absolute += expanded;
  }
  const std::size_t slash = absolute.rfind('/');
  const std::string directory = slash == std::string::npos
                                    ? "/"
                                    : absolute.substr(0, slash + 1);
  const std::string prefix = slash == std::string::npos
                                 ? absolute
                                 : absolute.substr(slash + 1);

  DIR* dir = opendir(directory.empty() ? "/" : directory.c_str());
  if (!dir) return {};
  struct Entry {
    std::string name;
    bool is_directory;
  };
  std::vector<Entry> entries;
  while (dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.empty() || name[0] == '.') continue;
    if (name.compare(0, prefix.size(), prefix) != 0) continue;
    bool is_directory = entry->d_type == DT_DIR;
    if (entry->d_type == DT_UNKNOWN) {
      struct stat info {};
      if (stat((directory + name).c_str(), &info) == 0) {
        is_directory = S_ISDIR(info.st_mode);
      }
    }
    entries.push_back({name, is_directory});
  }
  closedir(dir);

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.is_directory != b.is_directory) return a.is_directory;
    return a.name < b.name;
  });

  std::vector<std::string> result;
  for (const Entry& entry : entries) {
    if (result.size() >= MAX_COMPLETIONS) break;
    std::string full = directory + entry.name + (entry.is_directory ? "/" : "");
    std::string shown;
    if (tilde) {
      const std::string home_prefix = home ? home : "";
      if (!home_prefix.empty() && full.rfind(home_prefix, 0) == 0) {
        shown = "~" + full.substr(home_prefix.size());
      } else {
        shown = full;
      }
    } else if (absolute_input) {
      shown = full;
    } else {
      const std::filesystem::path base(root);
      shown = std::filesystem::path(full).lexically_relative(base).generic_string();
      if (entry.is_directory && (shown.empty() || shown.back() != '/')) shown += '/';
    }
    if (shown == input) continue;
    result.push_back(shown);
  }
  return result;
}

std::vector<std::string> value_suggestions(const std::string& typed,
                                           const std::vector<std::string>& suggest) {
  if (suggest.empty()) return {};
  std::string needle = typed;
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  std::vector<std::string> result;
  for (const std::string& value : suggest) {
    if (value == typed) continue;
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    if (lowered.rfind(needle, 0) == 0) result.push_back(value);
  }
  return result;
}

bool ghost_enabled(const std::string& typed) {
  if (typed.empty()) return false;
  if (typed.find('/') != std::string::npos) return true;
  const char first = typed[0];
  return first == '.' || first == '~' || first == '{';
}

std::vector<std::string> completions_for(
    const std::string& typed, bool complete,
    const std::vector<std::string>& suggest, const std::string& lib_dir,
    const std::string& base_dir) {
  const std::vector<std::string> known = value_suggestions(typed, suggest);
  if (!known.empty()) return known;
  if (!complete || typed.empty()) return {};

  auto wrap = [](const std::string& prefix,
                 const std::vector<std::string>& values) {
    std::vector<std::string> wrapped;
    for (const std::string& value : values) {
      wrapped.push_back(prefix + value + (value.empty() || value.back() == '/' ? "" : "}"));
    }
    return wrapped;
  };

  if (typed.rfind("{file:", 0) == 0) {
    return wrap("{file:", path_completions(typed.substr(6), base_dir));
  }
  if (typed.rfind("{lib:", 0) == 0 && !lib_dir.empty()) {
    return wrap("{lib:", path_completions(typed.substr(5), lib_dir));
  }
  if (typed.rfind("{env:", 0) == 0) {
    const std::string prefix = typed.substr(5);
    std::vector<std::string> result;
    for (char** env = environ; *env; env++) {
      const std::string entry = *env;
      const std::size_t equals = entry.find('=');
      const std::string name = entry.substr(0, equals);
      if (name.rfind(prefix, 0) == 0) result.push_back("{env:" + name + "}");
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  return path_completions(typed, base_dir);
}

struct Window {
  std::vector<std::string> items;
  std::size_t offset = 0;
};

Window window_of(const std::vector<std::string>& items, int index,
                 std::size_t size) {
  Window window;
  if (items.empty()) return window;
  const int clamped =
      std::max(0, std::min(index, static_cast<int>(items.size()) - 1));
  int offset = clamped - static_cast<int>(size / 2);
  offset = std::max(
      0, std::min(offset, static_cast<int>(items.size()) - static_cast<int>(size)));
  window.offset = static_cast<std::size_t>(offset);
  for (int position = offset;
       position < static_cast<int>(items.size()) &&
       position < offset + static_cast<int>(size);
       position++) {
    window.items.push_back(items[static_cast<std::size_t>(position)]);
  }
  return window;
}

std::string render_input(const std::string& value, std::size_t cursor,
                         const std::optional<std::string>& ghost,
                         const std::optional<std::string>& placeholder) {
  if (value.empty()) {
    if (placeholder && !placeholder->empty()) {
      auto [head, tail] = first_codepoint(*placeholder);
      return std::string(REVERSE) + head + RESET + DIM + tail + RESET;
    }
    std::string out = std::string(REVERSE) + " " + RESET;
    if (ghost) out += DIM + *ghost + RESET;
    return out;
  }
  const std::string before = value.substr(0, cursor);
  const std::string tail = cursor < value.size() ? value.substr(cursor) : "";
  auto [at, after] = first_codepoint(tail);
  const std::string caret = at.empty() ? std::string(REVERSE) + " " + RESET
                                       : std::string(REVERSE) + at + RESET;
  std::string out = before + caret + after;
  if (ghost) out += DIM + *ghost + RESET;
  return out;
}

std::optional<std::string> choose_editor() {
  const char* visual = std::getenv("VISUAL");
  if (visual && *visual) return std::string(visual);
  const char* editor = std::getenv("EDITOR");
  if (editor && *editor) return std::string(editor);
  return std::string("vi");
}

}  // namespace

struct PromptUI::Impl {
  int input_fd = -1;
  int output_fd = -1;
  struct termios original {};
  bool raw = false;
  bool closed = false;
  bool tree_mode = false;
  int rows = 0;
  std::string lib_dir;
  std::string base_dir;
  std::optional<int> fixed_columns;

  std::string pending;
  std::vector<Key> key_queue;
  std::size_t key_position = 0;

  Impl(Options options) {
    input_fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    output_fd = open("/dev/tty", O_WRONLY | O_CLOEXEC);
    if (input_fd < 0 || output_fd < 0) {
      if (input_fd >= 0) ::close(input_fd);
      if (output_fd >= 0) ::close(output_fd);
      throw std::runtime_error(
          "no interactive terminal available (" + std::string(std::strerror(errno)) + ")");
    }
    if (tcgetattr(input_fd, &original) != 0) {
      throw std::runtime_error("cannot read the terminal mode");
    }
    set_raw(true);
    if (options.lib_dir) lib_dir = *options.lib_dir;
    if (options.base_dir) base_dir = *options.base_dir;
    else {
      char buffer[4096];
      if (getcwd(buffer, sizeof(buffer))) base_dir = buffer;
    }
    fixed_columns = options.columns;
  }

  ~Impl() { close(); }

  void write_all(const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      ssize_t written = ::write(output_fd, data.data() + offset, data.size() - offset);
      if (written < 0) {
        if (errno == EINTR) continue;
        return;
      }
      offset += static_cast<std::size_t>(written);
    }
  }

  void set_raw(bool enabled) {
    if (enabled) {
      struct termios raw_mode = original;
      cfmakeraw(&raw_mode);
      // keep output post-processing off but write explicit "\r\n"
      tcsetattr(input_fd, TCSAFLUSH, &raw_mode);
      raw = true;
    } else {
      tcsetattr(input_fd, TCSAFLUSH, &original);
      raw = false;
    }
  }

  std::size_t columns() const {
    if (fixed_columns) return static_cast<std::size_t>(std::max(10, *fixed_columns));
    struct winsize size {};
    if (ioctl(output_fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) return size.ws_col;
    return 80;
  }

  int terminal_rows() const {
    struct winsize size {};
    if (ioctl(output_fd, TIOCGWINSZ, &size) == 0 && size.ws_row > 0) return size.ws_row;
    return 24;
  }

  void clear() {
    if (rows > 0) {
      if (rows > 1) write_all("\x1b[" + std::to_string(rows - 1) + "A");
      write_all("\r\x1b[J");
      rows = 0;
    }
  }

  void render(const std::vector<std::string>& lines) {
    clear();
    std::string frame = HIDE_CURSOR;
    const std::size_t width = columns();
    for (std::size_t index = 0; index < lines.size(); index++) {
      if (index > 0) frame += "\r\n";
      frame += fit_line(lines[index], width);
    }
    write_all(frame);
    rows = static_cast<int>(lines.size());
  }

  void commit(const std::string& line) {
    clear();
    write_all(fit_line(line, columns()) + "\r\n");
  }

  void close() {
    if (closed) return;
    closed = true;
    clear();
    write_all(SHOW_CURSOR);
    set_raw(false);
    if (input_fd >= 0) ::close(input_fd);
    if (output_fd >= 0) ::close(output_fd);
    input_fd = -1;
    output_fd = -1;
  }

  void parse_input(const std::string& chunk, std::vector<Key>& keys,
                   std::string& rest) {
    std::size_t index = 0;
    while (index < chunk.size()) {
      const char ch = chunk[index];
      if (ch == '\x1b') {
        if (index + 1 < chunk.size() && chunk[index + 1] == '[') {
          std::size_t cursor = index + 2;
          std::string params;
          while (cursor < chunk.size() &&
                 (std::isdigit(static_cast<unsigned char>(chunk[cursor])) ||
                  chunk[cursor] == ';')) {
            params += chunk[cursor];
            cursor++;
          }
          if (cursor >= chunk.size()) {
            rest = chunk.substr(index);
            return;
          }
          const char final = chunk[cursor];
          if (std::isalpha(static_cast<unsigned char>(final)) || final == '~') {
            std::string name;
            if (final == 'A') name = "up";
            else if (final == 'B') name = "down";
            else if (final == 'C') name = "right";
            else if (final == 'D') name = "left";
            else if (final == 'H') name = "home";
            else if (final == 'F') name = "end";
            else if (final == '~') {
              if (params == "1" || params == "7") name = "home";
              else if (params == "4" || params == "8") name = "end";
              else if (params == "3") name = "delete";
              else if (params == "5") name = "pageup";
              else if (params == "6") name = "pagedown";
            }
            if (!name.empty()) {
              Key event;
              event.name = name;
              keys.push_back(event);
            }
            index = cursor + 1;
            continue;
          }
          Key event;
          event.name = "escape";
          keys.push_back(event);
          index++;
          continue;
        }
        if (index + 1 < chunk.size() && chunk[index + 1] == 'O') {
          if (index + 2 >= chunk.size()) {
            rest = chunk.substr(index);
            return;
          }
          const char final = chunk[index + 2];
          std::string name;
          if (final == 'A') name = "up";
          else if (final == 'B') name = "down";
          else if (final == 'C') name = "right";
          else if (final == 'D') name = "left";
          else if (final == 'H') name = "home";
          else if (final == 'F') name = "end";
          if (!name.empty()) {
            Key event;
            event.name = name;
            keys.push_back(event);
          }
          index += 3;
          continue;
        }
        if (index + 1 >= chunk.size()) {
          rest = chunk.substr(index);
          return;
        }
        Key event;
        event.name = "escape";
        keys.push_back(event);
        index++;
        continue;
      }
      bool handled = true;
      Key event;
      switch (ch) {
        case '\r':
        case '\n':
          event.name = "return";
          break;
        case '\t':
          event.name = "tab";
          break;
        case '\x7f':
        case '\b':
          event.name = "backspace";
          break;
        case '\x03':
          event.name = "c";
          event.ctrl = true;
          break;
        case '\x06':
          event.name = "f";
          event.ctrl = true;
          break;
        case '\x04':
          event.name = "d";
          event.ctrl = true;
          break;
        case '\x01':
          event.name = "home";
          break;
        case '\x05':
          event.name = "end";
          break;
        case '\x0b':
          event.name = "k";
          event.ctrl = true;
          break;
        case '\x15':
          event.name = "u";
          event.ctrl = true;
          break;
        case '\x17':
          event.name = "w";
          event.ctrl = true;
          break;
        default:
          handled = false;
      }
      if (handled) {
        keys.push_back(event);
        index++;
        continue;
      }
      // printable run (typing or a paste)
      std::string run;
      while (index < chunk.size()) {
        const unsigned char next = static_cast<unsigned char>(chunk[index]);
        if (next < 0x20 || next == 0x7f || next == 0x1b) break;
        run += chunk[index];
        index++;
      }
      if (run.empty()) {
        index++;
        continue;
      }
      Key text;
      text.has_str = true;
      text.str = run;
      text.name = run == " " ? "space" : "";
      keys.push_back(text);
    }
  }

  Key next_key() {
    for (;;) {
      if (key_position < key_queue.size()) return key_queue[key_position++];
      if (!pending.empty()) {
        std::vector<Key> parsed;
        std::string rest;
        parse_input(pending, parsed, rest);
        pending = rest;
        if (!parsed.empty()) {
          key_queue = std::move(parsed);
          key_position = 0;
          continue;
        }
      }
      struct pollfd descriptor{input_fd, POLLIN, 0};
      const int timeout = pending.empty() ? -1 : ESCAPE_TIMEOUT_MS;
      const int ready = poll(&descriptor, 1, timeout);
      if (ready == 0) {
        // incomplete escape sequence: treat it as esc + the rest as text
        const std::string current = pending;
        pending.clear();
        Key escape;
        escape.name = "escape";
        key_queue = {escape};
        key_position = 0;
        if (current.size() > 1) {
          std::vector<Key> parsed;
          std::string rest;
          parse_input(current.substr(1), parsed, rest);
          pending = rest;
          key_queue.insert(key_queue.end(), parsed.begin(), parsed.end());
        }
        continue;
      }
      if (ready < 0) {
        if (errno == EINTR) continue;
        throw PromptCancelled();
      }
      char buffer[4096];
      const ssize_t count = read(input_fd, buffer, sizeof(buffer));
      if (count <= 0) throw PromptCancelled();
      pending.append(buffer, static_cast<std::size_t>(count));
    }
  }

  void apply_key_policy(const Key& key) {
    if (key.ctrl && key.name == "c") throw PromptCancelled();
    if (tree_mode && key.ctrl && key.name == "f") throw TreeJump();
  }

  // --- prompts -------------------------------------------------------------

  Reply<std::string> text(const TextPrompt& prompt);
  Reply<int> select(const SelectPrompt& prompt);
  Reply<std::vector<int>> multiselect(const MultiSelectPrompt& prompt);
  Reply<bool> confirm(const ConfirmPrompt& prompt);
  Reply<std::string> editor(const TextPrompt& prompt);

  std::string summary(const std::string& label, const std::string& value) {
    return cyan(label) + ": " + value;
  }
};

Reply<std::string> PromptUI::Impl::text(const TextPrompt& prompt) {
  if (prompt.multiline) return editor(prompt);
  struct State {
    std::string typed;
    std::size_t cursor = 0;
    int pick = -1;
  };
  State state;
  state.typed = prompt.initial_value.value_or("");
  state.cursor = state.typed.size();
  std::optional<std::string> error;

  auto completions = [&]() {
    return completions_for(state.typed, prompt.complete, prompt.suggest,
                           lib_dir, base_dir);
  };
  auto materialize = [&](const std::vector<std::string>& matches) {
    if (state.pick < 0) return;
    if (state.pick < static_cast<int>(matches.size())) state.typed = matches[state.pick];
    state.cursor = state.typed.size();
    state.pick = -1;
  };

  for (;;) {
    const std::vector<std::string> matches = completions();
    std::optional<std::string> picked;
    if (state.pick >= 0 && state.pick < static_cast<int>(matches.size())) {
      picked = matches[state.pick];
    }
    const std::string value = picked.value_or(state.typed);
    const std::size_t cursor = picked ? picked->size() : state.cursor;
    std::optional<std::string> ghost;
    if (!picked && !state.typed.empty() &&
        (ghost_enabled(state.typed) ||
         !value_suggestions(state.typed, prompt.suggest).empty()) &&
        !matches.empty() && matches.front().rfind(state.typed, 0) == 0) {
      ghost = matches.front().substr(state.typed.size());
    }
    std::optional<std::string> placeholder;
    if (state.typed.empty() && !picked && prompt.default_value) {
      placeholder = prompt.default_value;
    }
    std::vector<std::string> lines;
    if (prompt.hint) lines.push_back(dim(*prompt.hint));
    lines.push_back(cyan(prompt.message) + " " + dim(">") + " " +
                    render_input(value, cursor, ghost, placeholder));
    if (error) lines.push_back(red(*error));
    if (matches.size() >= COMPLETION_MENU_MIN) {
      const Window window = window_of(
          matches, state.pick >= 0 ? state.pick : 0, COMPLETION_MENU_SIZE);
      for (std::size_t index = 0; index < window.items.size(); index++) {
        const std::size_t position = window.offset + index;
        if (static_cast<int>(position) == state.pick) {
          lines.push_back(std::string(REVERSE) + "  " + window.items[index] + " " + RESET);
        } else {
          lines.push_back("    " + window.items[index]);
        }
      }
      const std::size_t hidden = matches.size() - window.items.size();
      if (hidden > 0) lines.push_back(dim("  (" + std::to_string(hidden) + " more)"));
    }
    render(lines);

    Key key = next_key();
    apply_key_policy(key);
    if (key.name == "escape") {
      commit(dim("‹ back"));
      return {std::string(), true};
    }
    if (key.name == "return") {
      const std::string typed =
          state.pick >= 0 && state.pick < static_cast<int>(matches.size())
              ? matches[state.pick]
              : state.typed;
      std::string value_to_submit = typed;
      if (typed.empty() && prompt.default_value) value_to_submit = *prompt.default_value;
      if (prompt.validate) {
        if (const auto message = prompt.validate(value_to_submit)) {
          error = message;
          continue;
        }
      }
      commit(summary(prompt.message,
                     value_to_submit.empty() ? dim("(empty)") : value_to_submit));
      return {value_to_submit, false};
    }
    if (key.name == "tab") {
      std::optional<std::string> suggestion;
      if (state.pick >= 0 && state.pick < static_cast<int>(matches.size())) {
        suggestion = matches[state.pick];
      } else if (state.typed.empty() && prompt.default_value) {
        suggestion = prompt.default_value;
      } else if (!matches.empty()) {
        suggestion = matches.front();
      }
      if (suggestion) {
        state.typed = *suggestion;
        state.cursor = state.typed.size();
        state.pick = -1;
        error.reset();
      }
      continue;
    }
    if (key.name == "up") {
      if (matches.empty()) continue;
      state.pick = state.pick < 0 ? static_cast<int>(matches.size()) - 1 : state.pick - 1;
      if (state.pick < 0) state.pick = static_cast<int>(matches.size()) - 1;
      error.reset();
      continue;
    }
    if (key.name == "down") {
      if (matches.empty()) continue;
      state.pick++;
      if (state.pick >= static_cast<int>(matches.size())) state.pick = -1;
      error.reset();
      continue;
    }
    if (key.name == "left" || key.name == "right" || key.name == "home" ||
        key.name == "end") {
      materialize(matches);
      if (key.name == "left") state.cursor = state.cursor > 0 ? state.cursor - 1 : 0;
      if (key.name == "right" && state.cursor < state.typed.size()) state.cursor++;
      if (key.name == "home") state.cursor = 0;
      if (key.name == "end") state.cursor = state.typed.size();
      error.reset();
      continue;
    }
    if (key.name == "backspace") {
      materialize(matches);
      if (state.cursor > 0) {
        state.typed.erase(state.cursor - 1, 1);
        state.cursor--;
      }
      error.reset();
      continue;
    }
    if (key.name == "delete") {
      materialize(matches);
      if (state.cursor < state.typed.size()) state.typed.erase(state.cursor, 1);
      error.reset();
      continue;
    }
    if (key.ctrl && key.name == "u") {
      state.typed.erase(0, state.cursor);
      state.cursor = 0;
      state.pick = -1;
      error.reset();
      continue;
    }
    if (key.ctrl && key.name == "k") {
      state.typed.erase(state.cursor);
      state.pick = -1;
      error.reset();
      continue;
    }
    if (key.ctrl && key.name == "w") {
      materialize(matches);
      std::string head = state.typed.substr(0, state.cursor);
      // drop trailing whitespace then the word
      std::size_t end = head.size();
      while (end > 0 && std::isspace(static_cast<unsigned char>(head[end - 1]))) end--;
      while (end > 0 && !std::isspace(static_cast<unsigned char>(head[end - 1]))) end--;
      state.typed = head.substr(0, end) + state.typed.substr(state.cursor);
      state.cursor = end;
      state.pick = -1;
      error.reset();
      continue;
    }
    if (key.has_str && !key.ctrl) {
      materialize(matches);
      state.typed.insert(state.cursor, key.str);
      state.cursor += key.str.size();
      error.reset();
    }
  }
}

Reply<int> PromptUI::Impl::select(const SelectPrompt& prompt) {
  struct State {
    std::string filter;
    int index = 0;
  };
  State state;
  if (prompt.initial_index) state.index = *prompt.initial_index;
  if (state.index < 0) state.index = 0;
  std::optional<std::string> error;

  auto filtered = [&]() {
    std::vector<int> indices;
    std::string needle = state.filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    for (std::size_t index = 0; index < prompt.options.size(); index++) {
      std::string label = prompt.options[index].label;
      std::transform(label.begin(), label.end(), label.begin(),
                     [](unsigned char ch) { return std::tolower(ch); });
      if (needle.empty() || label.find(needle) != std::string::npos) {
        indices.push_back(static_cast<int>(index));
      }
    }
    return indices;
  };

  for (;;) {
    const std::vector<int> indices = filtered();
    const int active = indices.empty()
                           ? -1
                           : indices[std::min<int>(state.index, indices.size() - 1)];
    std::vector<std::string> lines;
    if (prompt.hint) lines.push_back(dim(*prompt.hint));
    std::string header = cyan(prompt.message) + " " + dim(">");
    if (!state.filter.empty()) {
      header += " " + state.filter + " " +
                dim("(" + std::to_string(indices.size()) + " match)");
    }
    lines.push_back(header);
    const std::vector<std::string> labels = [&]() {
      std::vector<std::string> result;
      for (int index : indices) result.push_back(prompt.options[index].label);
      return result;
    }();
    const Window window = window_of(labels, state.index, COMPLETION_MENU_SIZE);
    for (std::size_t index = 0; index < window.items.size(); index++) {
      const std::size_t position = window.offset + index;
      if (static_cast<int>(position) == state.index) {
        lines.push_back(std::string(REVERSE) + "  " + window.items[index] + " " + RESET);
      } else {
        lines.push_back("    " + window.items[index]);
      }
    }
    if (labels.empty()) lines.push_back(dim("no match"));
    if (active >= 0 && prompt.options[active].hint) {
      lines.push_back(dim(*prompt.options[active].hint));
    }
    if (error) lines.push_back(red(*error));
    render(lines);

    Key key = next_key();
    apply_key_policy(key);
    if (key.name == "escape") {
      commit(dim("‹ back"));
      return {0, true};
    }
    if (key.name == "return") {
      if (indices.empty()) {
        error = "nothing matches the filter";
        continue;
      }
      const int chosen = indices[std::min<int>(state.index, indices.size() - 1)];
      commit(summary(prompt.message, prompt.options[chosen].label));
      return {chosen, false};
    }
    if (key.name == "up" || key.name == "down") {
      if (indices.empty()) continue;
      if (key.name == "up") {
        state.index = (state.index + static_cast<int>(indices.size()) - 1) %
                      static_cast<int>(indices.size());
      } else {
        state.index = (state.index + 1) % static_cast<int>(indices.size());
      }
      error.reset();
      continue;
    }
    if (key.name == "backspace") {
      if (!state.filter.empty()) {
        state.filter.pop_back();
        state.index = 0;
      }
      error.reset();
      continue;
    }
    if (key.has_str) {
      state.filter += key.str;
      state.index = 0;
      error.reset();
    }
  }
}

Reply<std::vector<int>> PromptUI::Impl::multiselect(
    const MultiSelectPrompt& prompt) {
  struct State {
    std::string filter;
    int index = 0;
    std::vector<int> selected;
  };
  State state;
  for (int index : prompt.initial_indices) state.selected.push_back(index);
  std::optional<std::string> error;

  auto filtered = [&]() {
    std::vector<int> indices;
    std::string needle = state.filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    for (std::size_t index = 0; index < prompt.options.size(); index++) {
      std::string label = prompt.options[index].label;
      std::transform(label.begin(), label.end(), label.begin(),
                     [](unsigned char ch) { return std::tolower(ch); });
      if (needle.empty() || label.find(needle) != std::string::npos) {
        indices.push_back(static_cast<int>(index));
      }
    }
    return indices;
  };

  for (;;) {
    const std::vector<int> indices = filtered();
    const int active = indices.empty()
                           ? -1
                           : indices[std::min<int>(state.index, indices.size() - 1)];
    std::vector<std::string> lines;
    if (prompt.hint) lines.push_back(dim(*prompt.hint));
    lines.push_back(cyan(prompt.message) + " " + dim(">") +
                    (state.filter.empty() ? "" : " " + state.filter));
    std::vector<std::string> labels;
    for (std::size_t index = 0; index < indices.size(); index++) {
      const int original = indices[index];
      const bool checked =
          std::find(state.selected.begin(), state.selected.end(), original) !=
          state.selected.end();
      labels.push_back(std::string("[") + (checked ? "x" : " ") + "] " +
                       prompt.options[original].label);
    }
    const Window window = window_of(labels, state.index, COMPLETION_MENU_SIZE);
    for (std::size_t index = 0; index < window.items.size(); index++) {
      const std::size_t position = window.offset + index;
      if (static_cast<int>(position) == state.index) {
        lines.push_back(std::string(REVERSE) + "  " + window.items[index] + " " + RESET);
      } else {
        lines.push_back("    " + window.items[index]);
      }
    }
    if (labels.empty()) lines.push_back(dim("no match"));
    if (active >= 0 && prompt.options[active].hint) {
      lines.push_back(dim(*prompt.options[active].hint));
    }
    if (error) lines.push_back(red(*error));
    render(lines);

    Key key = next_key();
    apply_key_policy(key);
    if (key.name == "escape") {
      commit(dim("‹ back"));
      return {{}, true};
    }
    if (key.name == "return") {
      std::vector<int> selected = state.selected;
      std::sort(selected.begin(), selected.end());
      if (prompt.required && selected.empty()) {
        error = "select at least one option";
        continue;
      }
      std::string text;
      for (std::size_t index = 0; index < selected.size(); index++) {
        if (index > 0) text += ", ";
        text += prompt.options[selected[index]].label;
      }
      commit(summary(prompt.message, text.empty() ? dim("none") : text));
      return {selected, false};
    }
    if (key.name == "up" || key.name == "down") {
      if (indices.empty()) continue;
      if (key.name == "up") {
        state.index = (state.index + static_cast<int>(indices.size()) - 1) %
                      static_cast<int>(indices.size());
      } else {
        state.index = (state.index + 1) % static_cast<int>(indices.size());
      }
      error.reset();
      continue;
    }
    if (key.name == "space" || (key.has_str && key.str == " ")) {
      if (active < 0) continue;
      auto it = std::find(state.selected.begin(), state.selected.end(), active);
      if (it == state.selected.end()) state.selected.push_back(active);
      else state.selected.erase(it);
      error.reset();
      continue;
    }
    if (key.name == "backspace") {
      if (!state.filter.empty()) {
        state.filter.pop_back();
        state.index = 0;
      }
      error.reset();
      continue;
    }
    if (key.has_str) {
      state.filter += key.str;
      state.index = 0;
      error.reset();
    }
  }
}

Reply<bool> PromptUI::Impl::confirm(const ConfirmPrompt& prompt) {
  const bool initial = prompt.initial.value_or(false);
  std::string typed;
  for (;;) {
    std::vector<std::string> lines;
    if (prompt.hint) lines.push_back(dim(*prompt.hint));
    lines.push_back(cyan(prompt.message) + " " + typed + REVERSE + " " + RESET + " " +
                    dim(initial ? "[Y/n]" : "[y/N]"));
    render(lines);

    Key key = next_key();
    apply_key_policy(key);
    if (key.name == "escape") {
      commit(dim("‹ back"));
      return {false, true};
    }
    if (key.name == "return") {
      std::string label = prompt.message;
      if (!label.empty() && label.back() == '?') label.pop_back();
      commit(summary(label, initial ? "yes" : "no"));
      return {initial, false};
    }
    if (key.has_str) {
      const char first = static_cast<char>(
          std::tolower(static_cast<unsigned char>(key.str[0])));
      if (first == 'y') {
        std::string label = prompt.message;
        if (!label.empty() && label.back() == '?') label.pop_back();
        commit(summary(label, "yes"));
        return {true, false};
      }
      if (first == 'n') {
        std::string label = prompt.message;
        if (!label.empty() && label.back() == '?') label.pop_back();
        commit(summary(label, "no"));
        return {false, false};
      }
      typed += key.str;
    }
  }
}

Reply<std::string> PromptUI::Impl::editor(const TextPrompt& prompt) {
  const std::string initial = prompt.initial_value.value_or("");
  const std::size_t line_count =
      initial.empty() ? 0 : 1 + static_cast<std::size_t>(
                                     std::count(initial.begin(), initial.end(), '\n'));
  const std::string editor_command = choose_editor().value_or("vi");
  for (;;) {
    std::vector<std::string> lines;
    if (prompt.hint) lines.push_back(dim(*prompt.hint));
    std::string status = initial.empty()
                             ? "empty"
                             : std::to_string(line_count) + " line" +
                                   (line_count == 1 ? "" : "s");
    lines.push_back(cyan(prompt.message) + " " + dim(">") + " " +
                    dim("[" + status + ", enter to edit in " + editor_command + "]"));
    render(lines);

    Key key = next_key();
    apply_key_policy(key);
    if (key.name == "escape") {
      commit(dim("‹ back"));
      return {std::string(), true};
    }
    if (key.name != "return") continue;

    char directory[] = "/tmp/cform-edit-XXXXXX";
    if (!mkdtemp(directory)) throw std::runtime_error("cannot create a temp dir");
    const std::string file = std::string(directory) + "/value.txt";
    {
      FILE* handle = std::fopen(file.c_str(), "wb");
      if (handle) {
        std::fwrite(initial.data(), 1, initial.size(), handle);
        std::fclose(handle);
      }
    }
    set_raw(false);
    write_all(SHOW_CURSOR);
    std::system((editor_command + " \"" + file + "\"").c_str());
    set_raw(true);
    write_all(HIDE_CURSOR);
    std::string value;
    {
      FILE* handle = std::fopen(file.c_str(), "rb");
      if (handle) {
        char buffer[4096];
        std::size_t count;
        while ((count = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
          value.append(buffer, count);
        }
        std::fclose(handle);
      }
    }
    if (!value.empty() && value.back() == '\n') value.pop_back();
    std::filesystem::remove_all(directory);

    std::vector<std::string> value_lines;
    std::string current;
    for (char ch : value) {
      if (ch == '\n') {
        value_lines.push_back(current);
        current.clear();
      } else {
        current += ch;
      }
    }
    value_lines.push_back(current);
    const std::size_t extra = value_lines.size() - 1;
    std::string text;
    if (value.empty()) {
      text = dim("(empty)");
    } else {
      text = value_lines.front();
      if (extra > 0) text += dim(" (+" + std::to_string(extra) + " lines)");
    }
    commit(summary(prompt.message, text));
    return {value, false};
  }
}

PromptUI::PromptUI(Options options) : impl_(new Impl(std::move(options))) {}

PromptUI::~PromptUI() {
  delete impl_;
  impl_ = nullptr;
}

Reply<std::string> PromptUI::text(const TextPrompt& prompt) {
  return impl_->text(prompt);
}

Reply<int> PromptUI::select(const SelectPrompt& prompt) {
  return impl_->select(prompt);
}

Reply<std::vector<int>> PromptUI::multiselect(const MultiSelectPrompt& prompt) {
  return impl_->multiselect(prompt);
}

Reply<bool> PromptUI::confirm(const ConfirmPrompt& prompt) {
  return impl_->confirm(prompt);
}

void PromptUI::note(const std::string& message,
                    const std::optional<std::string>& title) {
  const std::string body = trim(collapse_whitespace(message));
  if (body.empty() && !title) return;
  std::string line;
  if (title) line = *title + ": ";
  line += body;
  impl_->commit(dim(line));
}

void PromptUI::error(const std::string& message) {
  impl_->commit(red(message));
}

void PromptUI::banner(const std::string& title) {
  std::string legend =
      "enter accept · tab complete · ↑/↓ cycle · esc back · ctrl-c cancel";
  if (impl_->tree_mode) legend += " · ctrl-f tree";
  impl_->write_all(cyan(title) + "\r\n" + dim(legend) + "\r\n");
}

void PromptUI::outro(const std::string& message) { impl_->commit(message); }

void PromptUI::cancel(const std::string& message) {
  impl_->commit(red("x " + message));
}

void PromptUI::close() { impl_->close(); }

Key PromptUI::read_key() { return impl_->next_key(); }

void PromptUI::render_lines(const std::vector<std::string>& lines) {
  impl_->render(lines);
}

void PromptUI::clear_lines() { impl_->clear(); }

int PromptUI::terminal_rows() const { return impl_->terminal_rows(); }

int PromptUI::terminal_columns() const {
  return static_cast<int>(impl_->columns());
}

void PromptUI::set_tree_mode(bool enabled) { impl_->tree_mode = enabled; }

}  // namespace cform
