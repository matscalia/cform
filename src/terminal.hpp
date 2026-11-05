// Byte-level terminal + host filesystem operations used by the prompt UI.
//
// The native build implements this with termios on /dev/tty. The wasm build
// implements it in JavaScript (see terminal_wasm.cpp), so the whole prompt UI
// can run inside Node without touching emscripten's virtual filesystem.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace cform {

struct DirEntry {
  std::string name;
  bool is_directory = false;
};

enum class TerminalEvent { Timeout, Input, Resize };

class Terminal {
 public:
  virtual ~Terminal() = default;

  /// Writes raw bytes (escape sequences included) to the terminal.
  virtual void write(const std::string& data) = 0;

  /// Blocking read of at most `size` bytes. Returns the number of bytes read,
  /// 0 on end of input and a negative value on a read error.
  virtual int read(char* buffer, std::size_t size) = 0;

  /// Waits for input, a timeout, or a terminal resize. `timeout_ms < 0`
  /// means wait indefinitely (polling for resizes at a low frequency).
  virtual TerminalEvent wait_event(int timeout_ms) = 0;

  virtual int columns() = 0;
  virtual int rows() = 0;

  /// Enables/disables raw (no echo, no line buffering) input mode.
  virtual void set_raw(bool enabled) = 0;

  /// Restores the terminal and releases resources. Idempotent.
  virtual void close() = 0;

  // --- host filesystem, used by `{file:...}` and `{lib:...}` completion -----
  virtual std::string current_directory() = 0;
  virtual std::vector<DirEntry> list_directory(const std::string& directory) = 0;

  /// Opens `initial` in the user's editor (blocking). Returns false when no
  /// editor is available; otherwise stores the edited text in `value`.
  virtual bool edit(const std::string& initial, std::string& value) = 0;

  /// Editor name shown in the multiline prompt hint (e.g. "vi").
  virtual std::string editor_name() = 0;
};

/// Creates the platform terminal. Throws std::runtime_error when no
/// interactive terminal is available.
std::unique_ptr<Terminal> make_terminal();

}  // namespace cform
