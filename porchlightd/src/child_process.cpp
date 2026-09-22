#include "child_process.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

extern char** environ;

namespace porch {
namespace {

// The mask and dispositions the child must start with. Shared by both spawns,
// because a child that inherits our blocked SIGINT cannot be asked to stop -
// which once cost every recording its EOS.
class SpawnAttributes {
 public:
  SpawnAttributes() : ok_(::posix_spawnattr_init(&attr_) == 0) {
    if (!ok_) {
      return;
    }
    sigset_t unblocked;
    ::sigemptyset(&unblocked);
    sigset_t everything;
    ::sigfillset(&everything);
    ::posix_spawnattr_setsigmask(&attr_, &unblocked);
    ::posix_spawnattr_setsigdefault(&attr_, &everything);
    ::posix_spawnattr_setflags(&attr_, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  }

  ~SpawnAttributes() {
    if (ok_) {
      ::posix_spawnattr_destroy(&attr_);
    }
  }

  SpawnAttributes(const SpawnAttributes&) = delete;
  SpawnAttributes& operator=(const SpawnAttributes&) = delete;

  bool ok() const { return ok_; }
  posix_spawnattr_t* get() { return &attr_; }

 private:
  posix_spawnattr_t attr_{};
  bool ok_ = false;
};

std::vector<char*> as_argv(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  return argv;
}

}  // namespace

pid_t spawn_child(const std::vector<std::string>& args) {
  if (args.empty()) {
    errno = EINVAL;
    return -1;
  }

  std::vector<char*> argv = as_argv(args);

  SpawnAttributes attr;
  if (!attr.ok()) {
    return -1;
  }

  pid_t pid = -1;
  const int failure = ::posix_spawnp(&pid, argv[0], nullptr, attr.get(), argv.data(), environ);
  if (failure != 0) {
    errno = failure;
    return -1;
  }
  return pid;
}

PipedChild spawn_child_with_pipes(const std::vector<std::string>& args) {
  PipedChild child;
  if (args.empty()) {
    errno = EINVAL;
    return child;
  }

  // O_CLOEXEC on both, so the ends we keep are not inherited by anything else
  // we spawn later. The child's own ends are dup2'd, which clears the flag.
  int downward[2];  // parent writes, child reads as stdin
  int upward[2];    // child writes as stdout, parent reads
  if (::pipe2(downward, O_CLOEXEC) != 0) {
    return child;
  }
  if (::pipe2(upward, O_CLOEXEC) != 0) {
    ::close(downward[0]);
    ::close(downward[1]);
    return child;
  }

  UniqueFd child_stdin(downward[0]);
  UniqueFd our_write(downward[1]);
  UniqueFd our_read(upward[0]);
  UniqueFd child_stdout(upward[1]);

  posix_spawn_file_actions_t actions;
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    return child;
  }
  ::posix_spawn_file_actions_adddup2(&actions, child_stdin.get(), STDIN_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, child_stdout.get(), STDOUT_FILENO);

  SpawnAttributes attr;
  if (!attr.ok()) {
    ::posix_spawn_file_actions_destroy(&actions);
    return child;
  }

  std::vector<char*> argv = as_argv(args);
  pid_t pid = -1;
  const int failure =
      ::posix_spawnp(&pid, argv[0], &actions, attr.get(), argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);

  if (failure != 0) {
    errno = failure;
    return child;
  }

  // Ours would otherwise never see EOF when the child exits.
  child_stdin.reset();
  child_stdout.reset();

  // A child that stops reading must not be able to stall the event loop.
  ::fcntl(our_write.get(), F_SETFL, O_NONBLOCK);

  child.pid = pid;
  child.to_child = std::move(our_write);
  child.from_child = std::move(our_read);
  return child;
}

UniqueFd watch_child(pid_t pid) {
  return UniqueFd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0u)));
}

}  // namespace porch
