// Native terminal backend: raw mode and sizing through termios/ioctl on the
// controlling terminal (/dev/tty), like the original single-file TUI.
#include "terminal.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace cform {

namespace {

std::string choose_editor() {
  const char* visual = std::getenv("VISUAL");
  if (visual && *visual) return std::string(visual);
  const char* editor = std::getenv("EDITOR");
  if (editor && *editor) return std::string(editor);
  return std::string("vi");
}

class PosixTerminal : public Terminal {
 public:
  PosixTerminal() {
    input_fd_ = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    output_fd_ = open("/dev/tty", O_WRONLY | O_CLOEXEC);
    if (input_fd_ < 0 || output_fd_ < 0) {
      if (input_fd_ >= 0) ::close(input_fd_);
      if (output_fd_ >= 0) ::close(output_fd_);
      throw std::runtime_error(
          "no interactive terminal available (" +
          std::string(std::strerror(errno)) + ")");
    }
    if (tcgetattr(input_fd_, &original_) != 0) {
      ::close(input_fd_);
      ::close(output_fd_);
      input_fd_ = output_fd_ = -1;
      throw std::runtime_error("cannot read the terminal mode");
    }
    set_raw(true);
    last_columns_ = columns();
    last_rows_ = rows();
  }

  ~PosixTerminal() override { close(); }

  void write(const std::string& data) override {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const ssize_t written =
          ::write(output_fd_, data.data() + offset, data.size() - offset);
      if (written < 0) {
        if (errno == EINTR) continue;
        return;
      }
      offset += static_cast<std::size_t>(written);
    }
  }

  int read(char* buffer, std::size_t size) override {
    for (;;) {
      const ssize_t count = ::read(input_fd_, buffer, size);
      if (count >= 0) return static_cast<int>(count);
      if (errno != EINTR) return -1;
    }
  }

  TerminalEvent wait_event(int timeout_ms) override {
    for (;;) {
      if (size_changed()) return TerminalEvent::Resize;
      struct pollfd descriptor {input_fd_, POLLIN, 0};
      // When waiting forever, wake periodically to notice terminal resizes.
      const int wait = timeout_ms < 0 ? 100 : timeout_ms;
      const int ready = poll(&descriptor, 1, wait);
      if (ready > 0) return TerminalEvent::Input;
      if (ready == 0) {
        if (size_changed()) return TerminalEvent::Resize;
        if (timeout_ms >= 0) return TerminalEvent::Timeout;
        continue;
      }
      if (errno == EINTR) continue;
      return TerminalEvent::Input;  // let read() surface the error
    }
  }

  bool size_changed() {
    struct winsize size {};
    if (ioctl(output_fd_, TIOCGWINSZ, &size) != 0 || size.ws_col == 0 ||
        size.ws_row == 0)
      return false;
    const int columns = size.ws_col;
    const int rows = size.ws_row;
    if (columns != last_columns_ || rows != last_rows_) {
      last_columns_ = columns;
      last_rows_ = rows;
      return true;
    }
    return false;
  }

  int columns() override {
    struct winsize size {};
    if (ioctl(output_fd_, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
      return size.ws_col;
    return 80;
  }

  int rows() override {
    struct winsize size {};
    if (ioctl(output_fd_, TIOCGWINSZ, &size) == 0 && size.ws_row > 0)
      return size.ws_row;
    return 24;
  }

  void set_raw(bool enabled) override {
    if (input_fd_ < 0) return;
    if (enabled) {
      struct termios raw_mode = original_;
      cfmakeraw(&raw_mode);
      // keep output post-processing off but write explicit "\r\n"
      tcsetattr(input_fd_, TCSAFLUSH, &raw_mode);
      raw_ = true;
    } else {
      tcsetattr(input_fd_, TCSAFLUSH, &original_);
      raw_ = false;
    }
  }

  void close() override {
    if (closed_) return;
    closed_ = true;
    set_raw(false);
    if (input_fd_ >= 0) ::close(input_fd_);
    if (output_fd_ >= 0) ::close(output_fd_);
    input_fd_ = -1;
    output_fd_ = -1;
  }

  std::string current_directory() override {
    char buffer[4096];
    if (getcwd(buffer, sizeof(buffer))) return std::string(buffer);
    return std::string();
  }

  std::vector<DirEntry> list_directory(const std::string& directory) override {
    std::vector<DirEntry> entries;
    DIR* dir = opendir(directory.empty() ? "/" : directory.c_str());
    if (!dir) return entries;
    while (dirent* entry = readdir(dir)) {
      const std::string name = entry->d_name;
      if (name.empty() || name[0] == '.') continue;
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
    return entries;
  }

  bool edit(const std::string& initial, std::string& value) override {
    const std::string editor_command = choose_editor();
    char directory[] = "/tmp/cform-edit-XXXXXX";
    if (!mkdtemp(directory)) return false;
    const std::string file = std::string(directory) + "/value.txt";
    {
      FILE* handle = std::fopen(file.c_str(), "wb");
      if (handle) {
        std::fwrite(initial.data(), 1, initial.size(), handle);
        std::fclose(handle);
      }
    }
    std::system((editor_command + " \"" + file + "\"").c_str());
    std::string edited;
    {
      FILE* handle = std::fopen(file.c_str(), "rb");
      if (handle) {
        char buffer[4096];
        std::size_t count;
        while ((count = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
          edited.append(buffer, count);
        }
        std::fclose(handle);
      }
    }
    std::filesystem::remove_all(directory);
    value = std::move(edited);
    return true;
  }

  std::string editor_name() override { return choose_editor(); }

 private:
  int input_fd_ = -1;
  int output_fd_ = -1;
  struct termios original_ {};
  bool raw_ = false;
  bool closed_ = false;
  int last_columns_ = 0;
  int last_rows_ = 0;
};

}  // namespace

std::unique_ptr<Terminal> make_terminal() {
  return std::make_unique<PosixTerminal>();
}

}  // namespace cform
