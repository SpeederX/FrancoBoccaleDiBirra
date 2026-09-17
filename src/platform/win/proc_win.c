#include "platform/proc.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

struct FbProcess {
    HANDLE process;
    HANDLE job;
    DWORD pid;
};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynStr;

static int dynstr_ensure(DynStr *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return 0;
    size_t new_cap = s->cap == 0 ? 256 : s->cap * 2;
    if (new_cap < s->len + extra + 1) new_cap = s->len + extra + 1;
    char *nd = (char *)realloc(s->data, new_cap);
    if (!nd) return -1;
    s->data = nd;
    s->cap = new_cap;
    return 0;
}

static void dynstr_char(DynStr *s, char c) {
    if (dynstr_ensure(s, 1) != 0) return;
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
}

static void dynstr_str(DynStr *s, const char *str) {
    size_t n = strlen(str);
    if (dynstr_ensure(s, n) != 0) return;
    memcpy(s->data + s->len, str, n);
    s->len += n;
    s->data[s->len] = '\0';
}

/* Standard Windows command-line argument quoting (the same algorithm
   CommandLineToArgvW expects on the way back): wraps an argument in quotes
   if it contains a space/tab or is empty, doubling backslashes that
   immediately precede a quote (literal or closing). */
static void append_quoted_arg(DynStr *out, const char *arg) {
    int needs_quotes = (arg[0] == '\0') || strchr(arg, ' ') || strchr(arg, '\t');
    if (!needs_quotes) {
        dynstr_str(out, arg);
        return;
    }

    dynstr_char(out, '"');
    const char *p = arg;
    while (*p) {
        int backslashes = 0;
        while (*p == '\\') { backslashes++; p++; }

        if (*p == '\0') {
            for (int i = 0; i < backslashes * 2; ++i) dynstr_char(out, '\\');
            break;
        } else if (*p == '"') {
            for (int i = 0; i < backslashes * 2 + 1; ++i) dynstr_char(out, '\\');
            dynstr_char(out, '"');
            p++;
        } else {
            for (int i = 0; i < backslashes; ++i) dynstr_char(out, '\\');
            dynstr_char(out, *p);
            p++;
        }
    }
    dynstr_char(out, '"');
}

static char *build_cmdline(const char *const *argv) {
    DynStr s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; argv[i] != NULL; ++i) {
        if (i > 0) dynstr_char(&s, ' ');
        append_quoted_arg(&s, argv[i]);
    }
    return s.data; /* caller frees */
}

static HANDLE open_inheritable_log(const char *path, SECURITY_ATTRIBUTES *sa) {
    if (!path) return INVALID_HANDLE_VALUE;
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, sa, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
}

FbProcess *fb_proc_spawn(const char *const *argv, const char *stdout_path,
                          const char *stderr_path) {
    if (!argv || !argv[0]) return NULL;

    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (!job) return NULL;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    memset(&jeli, 0, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        CloseHandle(job);
        return NULL;
    }

    char *cmdline = build_cmdline(argv);
    if (!cmdline) {
        CloseHandle(job);
        return NULL;
    }

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    /* Two independently-opened handles, even when the two paths happen to
       differ (they always should — see the header comment on why they must
       never be the same file). */
    HANDLE stdout_handle = open_inheritable_log(stdout_path, &sa);
    HANDLE stderr_handle = open_inheritable_log(stderr_path, &sa);
    HANDLE null_input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL inherit_handles = FALSE;

    if (stdout_handle != INVALID_HANDLE_VALUE && stderr_handle != INVALID_HANDLE_VALUE &&
        null_input != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = stdout_handle;
        si.hStdError = stderr_handle;
        si.hStdInput = null_input;
        inherit_handles = TRUE;
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    /* CREATE_SUSPENDED so the process is assigned to the job before it can
       run (and possibly spawn its own children) at all. */
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, inherit_handles,
                              CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                              &si, &pi);
    free(cmdline);
    if (stdout_handle != INVALID_HANDLE_VALUE) CloseHandle(stdout_handle);
    if (stderr_handle != INVALID_HANDLE_VALUE) CloseHandle(stderr_handle);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
    if (!ok) {
        CloseHandle(job);
        return NULL;
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return NULL;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    FbProcess *proc = (FbProcess *)malloc(sizeof(FbProcess));
    if (!proc) {
        TerminateJobObject(job, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return NULL;
    }
    proc->process = pi.hProcess;
    proc->job = job;
    proc->pid = pi.dwProcessId;
    return proc;
}

void fb_proc_stop(FbProcess *proc) {
    if (!proc) return;
    TerminateJobObject(proc->job, 0);
}

int fb_proc_has_exited(FbProcess *proc) {
    if (!proc) return 1;
    DWORD code = 0;
    if (!GetExitCodeProcess(proc->process, &code)) return 1;
    return code != STILL_ACTIVE;
}

long fb_proc_pid(const FbProcess *proc) {
    return proc ? (long)proc->pid : 0;
}

void fb_proc_free(FbProcess *proc) {
    if (!proc) return;
    CloseHandle(proc->process);
    CloseHandle(proc->job);
    free(proc);
}
