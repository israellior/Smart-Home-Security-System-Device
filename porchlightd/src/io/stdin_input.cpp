#include "io/stdin_input.h"

#include <unistd.h>

#include <utility>

#include "logging.h"

namespace porch {
namespace {

constexpr int kStdin = 0;

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

// Splits "viewer 7" into "viewer" and "7".
std::pair<std::string_view, std::string_view> split_word(std::string_view line) {
  const std::size_t space = line.find(' ');
  if (space == std::string_view::npos) {
    return {line, {}};
  }
  return {line.substr(0, space), trim(line.substr(space + 1))};
}

}  // namespace

StdinInput::StdinInput(Reactor& reactor, EventSink sink, LinkControl link)
    : reactor_(reactor), sink_(std::move(sink)), link_(std::move(link)) {}

StdinInput::~StdinInput() {
  if (watching_) {
    reactor_.unwatch(kStdin);
  }
}

void StdinInput::start() {
  reactor_.watch(kStdin, [this] { on_readable(); });
  watching_ = true;
  print_help();
}

void StdinInput::on_readable() {
  char chunk[256];
  const ssize_t got = ::read(kStdin, chunk, sizeof(chunk));
  if (got == 0) {
    // End of input, from a pipe or a closed terminal. Without unwatching, the
    // descriptor stays readable forever and the loop spins at 100%.
    log(Level::Info, "input", "stdin closed; no more typed commands");
    reactor_.unwatch(kStdin);
    watching_ = false;
    return;
  }
  if (got < 0) {
    return;
  }

  buffer_.append(chunk, static_cast<std::size_t>(got));
  std::size_t newline = buffer_.find('\n');
  while (newline != std::string::npos) {
    handle_line(trim(std::string_view(buffer_).substr(0, newline)));
    buffer_.erase(0, newline + 1);
    newline = buffer_.find('\n');
  }
}

void StdinInput::handle_line(std::string_view line) {
  if (line.empty()) {
    return;
  }
  const auto [command, argument] = split_word(line);

  if (command == "button") {
    sink_(ButtonPressed{});
  } else if (command == "motion") {
    sink_(MotionDetected{});
  } else if (command == "viewer") {
    last_peer_ = argument.empty() ? last_peer_ : std::string(argument);
    sink_(ViewerRequested{last_peer_});
  } else if (command == "call-ended") {
    const std::string peer = argument.empty() ? last_peer_ : std::string(argument);
    sink_(CallEnded{peer, "typed"});
  } else if (command == "online") {
    link_(true);
  } else if (command == "offline") {
    link_(false);
  } else if (command == "quit") {
    sink_(Shutdown{});
  } else if (command == "help") {
    print_help();
  } else {
    log(Level::Warn, "input", "unknown command '{}'; try help", command);
  }
}

void StdinInput::print_help() const {
  log(Level::Info, "input",
      "commands: button | motion | viewer <peer> | call-ended [peer] | online | offline | quit");
}

}  // namespace porch
