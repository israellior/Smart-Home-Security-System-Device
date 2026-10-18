// record-clip: pi/record-clip.py in C++.
//
// gst-launch-1.0 -e, plus the one call it cannot make. porchlightd records a
// clip by running this with a pipeline description and stops it with SIGINT.
// SIGINT becomes EOS, EOS flushes the muxer, and the exit code says whether it
// got that far.
//
//   record-clip [--system-clock] [--camera-property NAME=VALUE ...]
//               -- mp4mux name=mux ! filesink location=... ...
//
// The same arguments, messages and exit codes as the Python, so either can be
// named by recorder.script. See record-clip.py's docstring for why
// --system-clock and --camera-property exist at all.
//
// Exit codes: 0 the muxer saw EOS and the file is finished, 1 GStreamer
// reported an error, 2 the arguments or the description did not parse.

#include <glib-unix.h>
#include <gst/gst.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kUsage =
    "usage: record-clip [-h] [--system-clock] [--camera-property NAME=VALUE] ...\n";

void note(std::string_view text) {
  std::fprintf(stderr, "record-clip: %.*s\n", static_cast<int>(text.size()), text.data());
  std::fflush(stderr);
}

// What argparse does with a bad command line: usage, the reason, exit 2.
[[noreturn]] void usage_error(std::string_view why) {
  std::fputs(kUsage, stderr);
  std::fprintf(stderr, "record-clip: error: %.*s\n", static_cast<int>(why.size()), why.data());
  std::exit(2);
}

void print_help() {
  std::fputs(kUsage, stdout);
  std::fputs(
      "\n"
      "gst-launch-1.0 -e, plus the one call it cannot make.\n"
      "\n"
      "positional arguments:\n"
      "  pipeline              the pipeline, as gst-launch-1.0 would take it, after --\n"
      "\n"
      "options:\n"
      "  -h, --help            show this help message and exit\n"
      "  --system-clock        pin the pipeline to the system monotonic clock\n"
      "  --camera-property NAME=VALUE\n"
      "                        set on the element named camera if it has it (repeatable)\n",
      stdout);
}

struct Options {
  bool system_clock = false;
  std::vector<std::string> camera_properties;
  std::vector<std::string> pipeline;
};

// The options come first; the first word that is not one of them starts the
// pipeline, and everything from there on is the pipeline untouched - which is
// what argparse.REMAINDER does. A leading "--" is dropped, as the Python does.
Options parse_options(int argc, char** argv) {
  Options options;
  int i = 1;
  for (; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    }
    if (arg == "--system-clock") {
      options.system_clock = true;
    } else if (arg == "--camera-property") {
      if (i + 1 >= argc) {
        usage_error("argument --camera-property: expected one argument");
      }
      options.camera_properties.emplace_back(argv[++i]);
    } else if (arg.starts_with("--camera-property=")) {
      options.camera_properties.emplace_back(arg.substr(arg.find('=') + 1));
    } else if (arg == "--" || !arg.starts_with('-')) {
      break;
    } else {
      usage_error(std::format("unrecognized arguments: {}", arg));
    }
  }
  if (i < argc && std::string_view(argv[i]) == "--") {
    ++i;
  }
  for (; i < argc; ++i) {
    options.pipeline.emplace_back(argv[i]);
  }
  if (options.pipeline.empty()) {
    usage_error("no pipeline given");
  }
  return options;
}

// Set NAME=VALUE on the element named "camera", each only if it exists.
// gst_util_set_object_arg parses the value as the property's own type - an enum
// for af-mode, a structure for sensor-config - which is what the launch-string
// parser did with the same text.
void set_camera_properties(GstElement* pipeline, const std::vector<std::string>& settings) {
  GstElement* camera = gst_bin_get_by_name(GST_BIN(pipeline), "camera");
  if (camera == nullptr) {
    note("no element named camera; camera properties not set");
    return;
  }
  GObjectClass* klass = G_OBJECT_GET_CLASS(camera);
  for (const std::string& setting : settings) {
    const std::size_t equals = setting.find('=');
    const std::string name = setting.substr(0, equals);
    const std::string value = equals == std::string::npos ? "" : setting.substr(equals + 1);
    if (g_object_class_find_property(klass, name.c_str()) == nullptr) {
      note(std::format("camera: this libcamerasrc has no {}; recording without it", name));
      continue;
    }
    gst_util_set_object_arg(G_OBJECT(camera), name.c_str(), value.c_str());
    note(std::format("camera: {} set", name));
  }
  gst_object_unref(camera);
}

struct Run {
  GMainLoop* loop = nullptr;
  GstElement* pipeline = nullptr;
  int code = 1;  // only EOS earns a 0
  bool stop_sent = false;
};

gboolean on_message(GstBus*, GstMessage* message, gpointer data) {
  Run* run = static_cast<Run*>(data);
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
    run->code = 0;
    g_main_loop_quit(run->loop);
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    const char* source =
        GST_MESSAGE_SRC(message) != nullptr ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : "?";
    note(std::format("error from {}: {}", source, error->message));
    if (debug != nullptr && *debug != '\0') {
      note(debug);
    }
    g_clear_error(&error);
    g_free(debug);
    g_main_loop_quit(run->loop);
  }
  return TRUE;
}

// Once only. The daemon sends one SIGINT and then waits; a second EOS would say
// nothing new, and it is SIGKILL that follows if this hangs.
gboolean on_stop(gpointer data) {
  Run* run = static_cast<Run*>(data);
  if (!run->stop_sent) {
    run->stop_sent = true;
    gst_element_send_event(run->pipeline, gst_event_new_eos());
  }
  return G_SOURCE_CONTINUE;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);

  gst_init(nullptr, nullptr);

  // parse_launchv is what gst-launch itself calls on its argv, so the daemon's
  // quoting - device="hw:CARD=..." - means here exactly what it meant there.
  std::vector<const gchar*> words;
  for (const std::string& word : options.pipeline) {
    words.push_back(word.c_str());
  }
  words.push_back(nullptr);

  GError* error = nullptr;
  GstElement* parsed = gst_parse_launchv(words.data(), &error);
  if (error != nullptr) {
    note(std::format("the pipeline does not parse: {}", error->message));
    g_clear_error(&error);
    if (parsed != nullptr) {
      gst_object_unref(parsed);
    }
    return 2;
  }
  // A description of one element parses to that element rather than to a
  // pipeline, and gst-launch wraps it in one. So does this, so that the clock
  // and the bus below always have a pipeline to belong to.
  if (!GST_IS_PIPELINE(parsed)) {
    GstElement* wrapper = gst_pipeline_new(nullptr);
    gst_bin_add(GST_BIN(wrapper), parsed);
    parsed = wrapper;
  }
  GstElement* pipeline = GST_ELEMENT(gst_object_ref_sink(parsed));

  if (!options.camera_properties.empty()) {
    set_camera_properties(pipeline, options.camera_properties);
  }
  if (options.system_clock) {
    GstClock* clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
    gst_object_unref(clock);
  }

  Run run;
  run.loop = g_main_loop_new(nullptr, FALSE);
  run.pipeline = pipeline;

  GstBus* bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, on_message, &run);
  // Through GLib rather than signal(), so the handler runs on the main loop
  // between messages instead of interrupting it mid-dispatch.
  g_unix_signal_add_full(G_PRIORITY_HIGH, SIGINT, on_stop, &run, nullptr);
  g_unix_signal_add_full(G_PRIORITY_HIGH, SIGTERM, on_stop, &run, nullptr);

  int code = 1;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    // Most often the camera or the card held by something else. Waiting in the
    // loop would only turn that into a SIGKILL five seconds later.
    GstMessage* message = gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR);
    if (message != nullptr) {
      on_message(bus, message, &run);
      gst_message_unref(message);
    }
    note("the pipeline would not start");
  } else {
    g_main_loop_run(run.loop);
    code = run.code;
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);

  gst_bus_remove_watch(bus);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  g_main_loop_unref(run.loop);
  return code;
}
