#include "child_process.h"

#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>

#include <cerrno>

extern char** environ;

namespace porch {

pid_t spawn_child(const std::vector<std::string>& args) {
  if (args.empty()) {
    errno = EINVAL;
    return -1;
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  posix_spawnattr_t attr;
  const int attr_failure = ::posix_spawnattr_init(&attr);
  if (attr_failure != 0) {
    errno = attr_failure;
    return -1;
  }

  sigset_t unblocked;
  ::sigemptyset(&unblocked);
  sigset_t everything;
  ::sigfillset(&everything);
  ::posix_spawnattr_setsigmask(&attr, &unblocked);
  ::posix_spawnattr_setsigdefault(&attr, &everything);
  ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

  pid_t pid = -1;
  const int failure = ::posix_spawnp(&pid, argv[0], nullptr, &attr, argv.data(), environ);
  ::posix_spawnattr_destroy(&attr);

  if (failure != 0) {
    errno = failure;
    return -1;
  }
  return pid;
}

UniqueFd watch_child(pid_t pid) {
  return UniqueFd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0u)));
}

}  // namespace porch
