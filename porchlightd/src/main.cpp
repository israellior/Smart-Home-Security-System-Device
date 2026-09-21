#include <cstdio>
#include <exception>
#include <filesystem>
#include <string_view>

#include "config.h"
#include "daemon.h"
#include "io/gst_recorder.h"
#include "logging.h"
#include "reactor.h"

namespace {

constexpr std::string_view kDefaultConfigPath = "/etc/porchlight/porchlightd.json";

void print_usage() {
  std::fprintf(stderr,
               "usage: porchlightd [--print-pipeline] [CONFIG]\n"
               "\n"
               "  CONFIG            path to the JSON configuration file\n"
               "                    (default: %.*s)\n"
               "  --print-pipeline  show the recorder's command line and exit\n",
               static_cast<int>(kDefaultConfigPath.size()), kDefaultConfigPath.data());
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{kDefaultConfigPath};
  bool print_pipeline = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--print-pipeline") {
      print_pipeline = true;
      continue;
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

    if (print_pipeline) {
      // The recorder runs this without a shell, so what prints here is exactly
      // the argv - paste it after a shell and quoting may differ.
      const auto args =
          porch::build_pipeline(config.recorder, config.spool.path / "<eventId>.mp4");
      for (std::size_t i = 0; i < args.size(); ++i) {
        std::fputs(args[i].c_str(), stdout);
        std::fputc(i + 1 < args.size() ? ' ' : '\n', stdout);
      }
      return 0;
    }

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
