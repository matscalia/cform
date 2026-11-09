// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matteo Scalia

// Wasm/Node terminal backend.
//
// The C++ TUI keeps all of its rendering and key-decoding logic; this class
// only forwards the byte-level terminal operations to JavaScript callbacks
// installed by the Node wrapper (wasm/index.js) on the Module object:
//
//   cformWrite(ptr, size)      write raw bytes to the terminal
//   cformRead(ptr, size)       blocking read, returns byte count
//   cformWait(timeoutMs)       wait for input; 1 = ready, 0 = timeout
//   cformColumns() / cformRows()
//   cformSetRaw(enabled)
//   cformCwd()                 current working directory
//   cformListDir(path)         [{ name, dir }] entries
//   cformEdit(initial)         edited text, or null when unavailable
//   cformClose()               optional cleanup
#include "terminal.hpp"

#include <emscripten.h>
#include <emscripten/val.h>
#include <stdexcept>

namespace cform {

namespace {

EM_JS(void, js_write, (const char* data, int size), {
  Module.cformWrite(data, size);
});

EM_JS(int, js_read, (char* buffer, int size), {
  return Module.cformRead(buffer, size);
});

EM_JS(int, js_wait, (int timeout_ms), {
  return Module.cformWait(timeout_ms);
});

EM_JS(int, js_columns, (), { return Module.cformColumns(); });
EM_JS(int, js_rows, (), { return Module.cformRows(); });

EM_JS(void, js_set_raw, (int enabled), {
  Module.cformSetRaw(!!enabled);
});

EM_JS(void, js_close, (), {
  if (Module.cformClose) Module.cformClose();
});

emscripten::val callback(const char* name) {
  emscripten::val fn = emscripten::val::module_property(name);
  if (fn.isUndefined() || fn.isNull()) {
    throw std::runtime_error(
        std::string("cform wasm: missing JS terminal callback `") + name +
        "` (load the module through wasm/index.js)");
  }
  return fn;
}

class WasmTerminal : public Terminal {
 public:
  WasmTerminal() {
    // Fail early with a clear message instead of trapping inside EM_JS.
    callback("cformWrite");
    callback("cformRead");
    callback("cformWait");
    callback("cformColumns");
    callback("cformRows");
    callback("cformSetRaw");
    callback("cformCwd");
    callback("cformListDir");
    set_raw(true);
  }

  ~WasmTerminal() override { close(); }

  void write(const std::string& data) override {
    js_write(data.data(), static_cast<int>(data.size()));
  }

  int read(char* buffer, std::size_t size) override {
    return js_read(buffer, static_cast<int>(size));
  }

  TerminalEvent wait_event(int timeout_ms) override {
    const int result = js_wait(timeout_ms);
    if (result < 0) return TerminalEvent::Resize;
    return result > 0 ? TerminalEvent::Input : TerminalEvent::Timeout;
  }

  int columns() override { return js_columns(); }
  int rows() override { return js_rows(); }

  void set_raw(bool enabled) override { js_set_raw(enabled ? 1 : 0); }

  void close() override {
    if (closed_) return;
    closed_ = true;
    set_raw(false);
    js_close();
  }

  std::string current_directory() override {
    emscripten::val fn = emscripten::val::module_property("cformCwd");
    if (fn.isUndefined() || fn.isNull()) return std::string();
    return fn().as<std::string>();
  }

  std::vector<DirEntry> list_directory(const std::string& directory) override {
    std::vector<DirEntry> entries;
    emscripten::val fn = emscripten::val::module_property("cformListDir");
    if (fn.isUndefined() || fn.isNull()) return entries;
    emscripten::val result = fn(directory);
    if (result.isUndefined() || result.isNull()) return entries;
    const unsigned length = result["length"].as<unsigned>();
    entries.reserve(length);
    for (unsigned index = 0; index < length; index++) {
      emscripten::val item = result[index];
      DirEntry entry;
      entry.name = item["name"].as<std::string>();
      entry.is_directory = item["dir"].as<bool>();
      entries.push_back(std::move(entry));
    }
    return entries;
  }

  bool edit(const std::string& initial, std::string& value) override {
    emscripten::val fn = emscripten::val::module_property("cformEdit");
    if (fn.isUndefined() || fn.isNull()) return false;
    emscripten::val result = fn(initial);
    if (result.isUndefined() || result.isNull()) return false;
    value = result.as<std::string>();
    return true;
  }

  std::string editor_name() override {
    emscripten::val fn = emscripten::val::module_property("cformEditorName");
    if (fn.isUndefined() || fn.isNull()) return "vi";
    return fn().as<std::string>();
  }

 private:
  bool closed_ = false;
};

}  // namespace

std::unique_ptr<Terminal> make_terminal() {
  return std::make_unique<WasmTerminal>();
}

}  // namespace cform
