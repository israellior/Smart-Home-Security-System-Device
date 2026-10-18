// upload-clip: pi/upload-clip.py in C++.
//
// Upload one clip, in the three steps the server expects. Spawned once per
// attempt by porchlightd and judged entirely by its exit code: 0 means the
// confirm succeeded and the daemon may delete its local copy. Anything else
// means keep the file and try the whole sequence again later.
//
//   1. POST /api/clips/<eventId>/upload-url   -> a short-lived signed PUT URL
//   2. PUT  <that url>                        -> the bytes, and only the bytes
//   3. POST /api/clips/<eventId>/confirm      -> the metadata
//
// Step 2 succeeding is not enough. A bucket PUT that is never confirmed is a
// clip the server does not know exists, so only step 3 earns exit 0. Nothing is
// cached between attempts: signed URLs expire, so a retry an hour later asks
// for a new one.
//
// The same arguments, messages and exit codes as the Python, so either can be
// named by server.uploader_path:
//
//   upload-clip --url http://host:4000 --credential-file ./cred
//       --clip /var/lib/porchlight/spool/<eventId>.mp4

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Ordered, so the confirm body keeps the sidecar's field order, as a Python
// dict does.
using json = nlohmann::ordered_json;

// urllib's timeout is per socket operation, not for the whole request: a slow
// upload that keeps moving is fine, one that stalls for this long is not.
constexpr long kTimeoutSeconds = 60;

constexpr const char* kUsage =
    "usage: upload-clip [-h] --url URL --credential-file CREDENTIAL_FILE --clip CLIP\n";

void note(std::string_view text) {
  std::fprintf(stderr, "upload: %.*s\n", static_cast<int>(text.size()), text.data());
  std::fflush(stderr);
}

[[noreturn]] void fail(std::string_view text) {
  note(text);
  std::exit(1);
}

[[noreturn]] void usage_error(std::string_view why) {
  std::fputs(kUsage, stderr);
  std::fprintf(stderr, "upload-clip: error: %.*s\n", static_cast<int>(why.size()), why.data());
  std::exit(2);
}

struct Options {
  std::string url;
  std::string credential_file;
  std::string clip;
};

Options parse_options(int argc, char** argv) {
  Options options;
  bool have_url = false;
  bool have_credential = false;
  bool have_clip = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::fputs(kUsage, stdout);
      std::fputs(
          "\nUpload one clip, in the three steps the server expects.\n"
          "\n"
          "options:\n"
          "  -h, --help            show this help message and exit\n"
          "  --url URL\n"
          "  --credential-file CREDENTIAL_FILE\n"
          "  --clip CLIP           path to the .mp4\n",
          stdout);
      std::exit(0);
    }
    struct Flag {
      std::string_view name;
      std::string* value;
      bool* seen;
    };
    const Flag flags[] = {{"--url", &options.url, &have_url},
                          {"--credential-file", &options.credential_file, &have_credential},
                          {"--clip", &options.clip, &have_clip}};
    bool matched = false;
    for (const Flag& flag : flags) {
      if (arg == flag.name) {
        if (i + 1 >= argc) {
          usage_error(std::format("argument {}: expected one argument", flag.name));
        }
        *flag.value = argv[++i];
      } else if (arg.starts_with(flag.name) && arg.size() > flag.name.size() &&
                 arg[flag.name.size()] == '=') {
        *flag.value = std::string(arg.substr(flag.name.size() + 1));
      } else {
        continue;
      }
      *flag.seen = true;
      matched = true;
      break;
    }
    if (!matched) {
      usage_error(std::format("unrecognized arguments: {}", arg));
    }
  }

  std::string missing;
  for (const auto& [seen, name] : {std::pair{have_url, "--url"},
                                   std::pair{have_credential, "--credential-file"},
                                   std::pair{have_clip, "--clip"}}) {
    if (!seen) {
      missing += missing.empty() ? name : std::string(", ") + name;
    }
  }
  if (!missing.empty()) {
    usage_error("the following arguments are required: " + missing);
  }
  return options;
}

// How Python words an OSError, so a failed open reads the same in the journal
// whichever uploader ran: [Errno 2] No such file or directory: '/path'
std::string os_error(const std::string& path) {
  const int code = errno;
  return std::format("[Errno {}] {}: '{}'", code, std::strerror(code), path);
}

std::string strip(std::string text) {
  const char* space = " \t\n\r\v\f";
  const std::size_t first = text.find_first_not_of(space);
  if (first == std::string::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(space) - first + 1);
}

std::size_t collect(char* data, std::size_t size, std::size_t count, void* body) {
  static_cast<std::string*>(body)->append(data, size * count);
  return size * count;
}

// One request. reached is false when there was no HTTP answer at all - the
// urllib URLError case - and error says why; otherwise status and body are the
// server's, whatever the status.
struct Reply {
  bool reached = false;
  std::string error;
  long status = 0;
  std::string body;

  bool ok() const { return status >= 200 && status < 300; }
};

Reply perform(const char* method, const std::string& url, const std::vector<std::string>& headers,
              const std::string& data) {
  Reply reply;
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    reply.error = "cannot start libcurl";
    return reply;
  }

  curl_slist* list = nullptr;
  for (const std::string& header : headers) {
    list = curl_slist_append(list, header.c_str());
  }
  // libcurl would add "Expect: 100-continue" to a large body and urllib never
  // does. An empty value removes it.
  list = curl_slist_append(list, "Expect:");

  char error[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply.body);
  // A body with no Content-Type of its own goes as
  // application/x-www-form-urlencoded, which is also what urllib does.
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(data.size()));
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

  const CURLcode result = curl_easy_perform(curl);
  if (result == CURLE_OK) {
    reply.reached = true;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.status);
  } else {
    reply.error = error[0] != '\0' ? error : curl_easy_strerror(result);
  }

  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return reply;
}

// The app server's two endpoints. A reply that is not JSON ends the program,
// as an uncaught JSONDecodeError ends the Python.
json request_json(const std::string& step, const std::string& url, const std::string& credential,
                  const json& payload) {
  const Reply reply = perform("POST", url,
                              {"Authorization: Bearer " + credential,
                               "Content-Type: application/json"},
                              payload.dump(-1, ' ', true));
  if (!reply.reached) {
    fail(std::format("{} failed: {}", step, reply.error));
  }
  if (!reply.ok()) {
    fail(std::format("{} failed: HTTP {} {}", step, reply.status, reply.body));
  }
  if (reply.body.empty()) {
    return json::object();
  }
  try {
    return json::parse(reply.body);
  } catch (const json::exception& error) {
    fail(std::format("{} answered with something that is not JSON: {}", step, error.what()));
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  curl_global_init(CURL_GLOBAL_DEFAULT);

  const std::string& clip = options.clip;
  const std::string sidecar = std::filesystem::path(clip).replace_extension(".json").string();
  std::string base = options.url;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }

  std::error_code ec;
  if (!std::filesystem::exists(clip, ec)) {
    fail(std::format("{} is gone", clip));
  }

  std::string credential;
  {
    std::ifstream in(options.credential_file);
    if (!in) {
      fail(std::format("cannot read credential: {}", os_error(options.credential_file)));
    }
    credential = strip(std::string(std::istreambuf_iterator<char>(in), {}));
  }

  json metadata;
  {
    std::ifstream in(sidecar);
    if (!in) {
      // Without it there is nothing to confirm with, and a clip the server
      // cannot describe is worse than one it never received.
      fail(std::format("cannot read sidecar {}: {}", sidecar, os_error(sidecar)));
    }
    try {
      metadata = json::parse(in);
    } catch (const json::exception& error) {
      fail(std::format("cannot read sidecar {}: {}", sidecar, error.what()));
    }
    if (!metadata.is_object()) {
      fail(std::format("cannot read sidecar {}: not a JSON object", sidecar));
    }
  }

  // The daemon always writes eventId as a string; the file name is the
  // fallback for a sidecar that lost it.
  std::string event_id;
  if (const auto it = metadata.find("eventId");
      it != metadata.end() && it->is_string() && !it->get<std::string>().empty()) {
    event_id = it->get<std::string>();
  } else {
    event_id = std::filesystem::path(clip).stem().string();
  }
  metadata.erase("eventId");
  const std::uintmax_t bytes = std::filesystem::file_size(clip, ec);
  if (ec) {
    fail(std::format("cannot measure {}: {}", clip, ec.message()));
  }
  metadata["bytes"] = bytes;

  // The size is signed into the grant, so the bucket takes exactly this file
  // and nothing larger. The server will come to require it.
  const json grant = request_json("step 1", base + "/api/clips/" + event_id + "/upload-url",
                                  credential, json{{"bytes", bytes}});

  std::string put_url;
  if (grant.is_object() && grant.contains("url") && grant["url"].is_string()) {
    put_url = grant["url"].get<std::string>();
  }
  if (put_url.empty()) {
    fail(std::format("step 1 returned no url: {}", grant.dump()));
  }

  // Exactly the headers step 1 handed back. The signature covers them, so an
  // extra one here does not add metadata - it makes the upload fail.
  std::vector<std::string> put_headers;
  if (grant.contains("headers") && grant["headers"].is_object()) {
    for (const auto& [name, value] : grant["headers"].items()) {
      put_headers.push_back(name + ": " +
                            (value.is_string() ? value.get<std::string>() : value.dump()));
    }
  }

  std::string data;
  {
    std::ifstream in(clip, std::ios::binary);
    if (!in) {
      fail(std::format("step 2 failed: {}", os_error(clip)));
    }
    data.assign(std::istreambuf_iterator<char>(in), {});
  }
  const Reply put = perform("PUT", put_url, put_headers, data);
  if (!put.reached) {
    fail(std::format("step 2 failed: {}", put.error));
  }
  if (!put.ok()) {
    fail(std::format("step 2 failed: HTTP {} {}", put.status, put.body));
  }
  note(std::format("step 2 uploaded {} bytes, HTTP {}", bytes, put.status));

  request_json("step 3", base + "/api/clips/" + event_id + "/confirm", credential, metadata);

  note(std::format("confirmed {}", event_id));
  return 0;
}
