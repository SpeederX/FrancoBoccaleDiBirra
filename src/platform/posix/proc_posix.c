/* Not yet runtime-tested (development happened on Windows this pass); it
   must be exercised on a real Linux/macOS machine before being relied on. */

#include "platform/proc.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct FbProcess {
    pid_t pid;
    int exited;
    int exit_code;
};

FbProcess *fb_proc_spawn(const char *const *argv, const char *stdout_path,
                          const char *stderr_path) {
    if (!argv || !argv[0]) return NULL;

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    /* New process group (pgid = pid) so the whole tree can be signaled via
       killpg, the same way a Windows Job Object contains it. */
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    /* Separate opens for stdout/stderr, not dup2 of one onto the other:
       dup2 would make them share one file offset/description, hitting the
       same torn-output problem confirmed on the Windows side (independent
       CRT buffering on each stream races when they share one underlying
       write cursor) — see platform/proc.h. */
    if (stdout_path) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path,
                                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (stderr_path) {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path,
                                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (stdout_path || stderr_path) {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }

    /* posix_spawnp (not posix_spawn) so a bare name with no path separator
       is resolved via PATH, mirroring CreateProcess's behavior on Windows —
       see fb_resolve_llama_server(). */
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, &attr, (char *const *)argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return NULL;

    FbProcess *proc = (FbProcess *)malloc(sizeof(FbProcess));
    if (!proc) {
        killpg(pid, SIGKILL);
        return NULL;
    }
    proc->pid = pid;
    proc->exited = 0;
    proc->exit_code = 0;
    return proc;
}

void fb_proc_stop(FbProcess *proc) {
    if (!proc) return;
    killpg(proc->pid, SIGTERM);
}

int fb_proc_exit_code(FbProcess *proc, int *out_exit_code) {
    if (!proc) return -1;
    if (!proc->exited) {
        int status = 0;
        pid_t r = waitpid(proc->pid, &status, WNOHANG);
        if (r == 0) return 0;
        if (r < 0) return -1;
        proc->exited = 1;
        proc->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                            : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
    }
    if (out_exit_code) *out_exit_code = proc->exit_code;
    return 1;
}

int fb_proc_has_exited(FbProcess *proc) {
    return fb_proc_exit_code(proc, NULL) != 0;
}

long fb_proc_pid(const FbProcess *proc) {
    return proc ? (long)proc->pid : 0;
}

void fb_proc_free(FbProcess *proc) {
    free(proc);
}
