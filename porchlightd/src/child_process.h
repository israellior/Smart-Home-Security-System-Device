#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "unique_fd.h"

namespace porch {

// Spawns a child with its signal mask cleared and its dispositions defaulted.
//
// The clearing is not optional. The reactor blocks SIGINT and SIGTERM
// process-wide so signalfd can be their only reader, posix_spawn hands that
// mask to the child, and a child that cannot receive SIGINT cannot be asked to
// stop politely. That once cost every recording its EOS and left a string of
// zero-byte files, so it lives here rather than in each caller.
//
// No shell: the shell would receive the signals meant for the program.
//
// Returns the pid, or -1 with errno set.
pid_t spawn_child(const std::vector<std::string>& args);

// The same, with the child's stdin and stdout wired to pipes. Our write end is
// non-blocking, so a child that stops reading slows nothing down: the write
// fails and the caller decides, rather than stalling the whole event loop.
// The child's stderr is left alone so its own messages reach the journal.
struct PipedChild {
  pid_t pid = -1;
  UniqueFd to_child;    // write here
  UniqueFd from_child;  // watch this for readability
};

PipedChild spawn_child_with_pipes(const std::vector<std::string>& args);

// The child's exit as a readable descriptor, so the reactor can wait for it
// the way it waits for everything else. Invalid on failure, with errno set.
UniqueFd watch_child(pid_t pid);

}  // namespace porch
