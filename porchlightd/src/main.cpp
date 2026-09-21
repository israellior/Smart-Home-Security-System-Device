#include <cstdio>
#include <exception>
#include <filesystem>
#include <string_view>

#include "config.h"
#include "daemon.h"
#include "logging.h"
#include "reactor.h"

namespace {

constexpr std::string_view kDefaultConfigPath = "/etc/porchlight/porchlightd.json";

void print_usage() {
  std::fprintf(stderr,
               "usage: porchlightd [CONFIG]\n"
               "\n"
               "  CONFIG  path to the JSON configuration file\n"
               "          (default: %.*s)\n",
               static_cast<int>(kDefaultConfigPath.size()), kDefaultConfigPath.data());
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{kDefaultConfigPath};

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg.starts_with('-')) {
      std::fprintf(stderr, "porchlightd: unknown option %.*s\n", static_cast<int>(arg.size()),
                   arg.data());
      print_usage();
      return 2;
    }
    config_path = arg;
  }

  try {
    const porch::Config config = porch::load_config(config_path);
    porch::set_min_level(config.log_level);
    porch::log(porch::Level::Info, "main", "starting device={} config={}", config.device_id,
               config_path.string());

    porch::Reactor reactor;
    porch::Daemon daemon(config, reactor);
    daemon.run();

    porch::log(porch::Level::Info, "main", "stopped");
    return 0;
  } catch (const porch::ConfigError& error) {
    porch::log(porch::Level::Error, "config", "{}", error.what());
    return 1;
  } catch (const std::exception& error) {
    porch::log(porch::Level::Error, "main", "{}", error.what());
    return 1;
  }
}
