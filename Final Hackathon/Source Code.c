#define _POSIX_C_SOURCE 200809L
/* Mini Linux Shell -- a single-command POSIX job-control shell.
 * The optional desktop UI only transports input/output over a PTY.
 * All command parsing, fork/exec, job control and reaping happen here.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_JOBS 32
#define MAX_LINE 4096
#define MAX_ARGS 128

typedef enum { EMPTY, RUNNING, STOPPED, DONE } State;
typedef struct {
    int id;
    pid_t pid;                 /* One external command per process group. */
    State state;
    bool background;
    int result;
    bool has_modes;
    struct termios modes;
    char command[MAX_LINE];
} Job;

static Job jobs[MAX_JOBS];
static int next_id = 1, status_fd = -1, wake_pipe[2] = {-1, -1};
static pid_t shell_pgid, foreground_pgid;
static bool interactive;
static struct termios shell_modes;
static volatile sig_atomic_t shutting_down, interrupted;

static void on_signal(int sig) {
    int saved = errno;
    if (sig == SIGTERM || sig == SIGHUP) shutting_down = 1;
    if (sig == SIGINT) interrupted = 1;
    if (wake_pipe[1] >= 0) {
        char byte = 'x';
        ssize_t written = write(wake_pipe[1], &byte, 1);
        (void)written; /* EAGAIN is harmless: a full pipe already wakes poll. */
    }
    errno = saved;
}

static void set_handler(int sig, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL) < 0) { perror("sigaction"); exit(1); }
}

static void cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static const char *state_name(State s) {
    return s == RUNNING ? "Running" : s == STOPPED ? "Stopped" : "Done";
}

static Job *find_job(int id) {
    for (int i = 0; i < MAX_JOBS; ++i)
        if (jobs[i].state != EMPTY && jobs[i].id == id) return &jobs[i];
    return NULL;
}

/* JSON is telemetry only. A nonblocking Unix datagram never stalls the shell. */
static void append(char *buf, size_t cap, size_t *n, const char *fmt, ...) {
    if (*n >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (len > 0) *n += (size_t)len < cap - *n ? (size_t)len : cap - *n;
}

static void json_string(char *buf, size_t cap, size_t *n, const char *s) {
    append(buf, cap, n, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') append(buf, cap, n, "\\%c", *p);
        else if (*p < 32 || *p >= 127) append(buf, cap, n, "\\u%04x", *p);
        else append(buf, cap, n, "%c", *p);
    }
    append(buf, cap, n, "\"");
}

static void publish_status(void) {
    if (status_fd < 0) return;
    char buf[65536], cwd[PATH_MAX];
    size_t n = 0;
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "(directory unavailable)");
    append(buf, sizeof(buf), &n, "{\"shell_pid\":%ld,\"foreground\":%ld,\"cwd\":",
           (long)getpid(), (long)foreground_pgid);
    json_string(buf, sizeof(buf), &n, cwd);
    append(buf, sizeof(buf), &n, ",\"jobs\":[");
    bool first = true;
    for (int i = 0; i < MAX_JOBS; ++i) {
        Job *j = &jobs[i];
        if (j->state == EMPTY) continue;
        append(buf, sizeof(buf), &n,
               "%s{\"id\":%d,\"pid\":%ld,\"state\":\"%s\",\"background\":%s,\"result\":%d,\"command\":",
               first ? "" : ",", j->id, (long)j->pid, state_name(j->state),
               j->background ? "true" : "false", j->result);
        /* Keep telemetry compact; the shell itself retains the full command. */
        char short_command[161];
        snprintf(short_command, sizeof(short_command), "%.160s", j->command);
        json_string(buf, sizeof(buf), &n, short_command);
        append(buf, sizeof(buf), &n, "}");
        first = false;
    }
    append(buf, sizeof(buf), &n, "]}");
    if (n < sizeof(buf)) (void)send(status_fd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void drain_wakeup(void) {
    char bytes[128];
    while (read(wake_pipe[0], bytes, sizeof(bytes)) > 0) {}
}

/* No printf, allocation, job-table mutation or waitpid in the signal handler. */
static bool reap_children(void) {
    int status;
    pid_t pid;
    bool changed = false, notified = false;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        for (int i = 0; i < MAX_JOBS; ++i) {
            Job *j = &jobs[i];
            if (j->state == EMPTY || j->pid != pid) continue;
            if (WIFSTOPPED(status)) {
                j->state = STOPPED;
                j->result = WSTOPSIG(status);
            } else if (WIFCONTINUED(status)) {
                j->state = RUNNING;
            } else {
                j->state = DONE;
                j->result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            changed = true;
            if (j->background && !WIFCONTINUED(status)) {
                printf("\n[%d] %-7s (PID %ld, status %d) %s\n", j->id,
                       state_name(j->state), (long)j->pid, j->result, j->command);
                notified = true;
            }
            break;
        }
    }
    if (changed) publish_status();
    return notified;
}

static void prompt(void) {
    if (!shutting_down) { printf("mini-shell> "); fflush(stdout); }
}

static void give_terminal(pid_t pgid) {
    if (!interactive) return;
    while (tcsetpgrp(STDIN_FILENO, pgid) < 0) {
        if (errno != EINTR) { perror("tcsetpgrp"); break; }
    }
}

static void wait_foreground(Job *j, bool resume, int release_fd) {
    j->background = false;
    foreground_pgid = j->pid;
    give_terminal(j->pid);
    if (interactive && resume && j->has_modes)
        (void)tcsetattr(STDIN_FILENO, TCSADRAIN, &j->modes);
    if (resume) {
        if (kill(-j->pid, SIGCONT) < 0) perror("fg");
        else j->state = RUNNING;
    }
    /* The child waits on this gate until the parent transfers the terminal. */
    if (release_fd >= 0) close(release_fd);
    publish_status();
    while (j->state == RUNNING && !shutting_down) {
        (void)reap_children();
        if (j->state != RUNNING) break;
        struct pollfd pfd = {wake_pipe[0], POLLIN, 0};
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) { perror("poll"); break; }
        drain_wakeup();
    }
    if (interactive && j->state == STOPPED) {
        j->has_modes = tcgetattr(STDIN_FILENO, &j->modes) == 0;
    }
    give_terminal(shell_pgid);
    if (interactive) (void)tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_modes);
    foreground_pgid = 0;
    interrupted = 0;
    if (j->state == STOPPED) {
        j->background = true;
        printf("[%d] Stopped %s\n", j->id, j->command);
    }
    publish_status();
}

/* Supports whitespace, single/double quotes and backslash escaping.
 * Operators other than a final unquoted & are intentionally rejected.
 */
static int parse(const char *line, char *storage, char **argv, bool *background) {
    const char *p = line;
    char *out = storage;
    int argc = 0;
    *background = false;
    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (*p == '&') {
            ++p;
            while (isspace((unsigned char)*p)) ++p;
            if (*p || !argc) { fprintf(stderr, "Syntax: & must follow a command at the end.\n"); return -1; }
            *background = true;
            break;
        }
        if (argc >= MAX_ARGS - 1) { fprintf(stderr, "Too many arguments.\n"); return -1; }
        argv[argc++] = out;
        char quote = 0;
        while (*p) {
            char c = *p;
            if (!quote && (isspace((unsigned char)c) || c == '&')) break;
            if (!quote && strchr("|<>;", c)) {
                fprintf(stderr, "Pipes, redirection and command chaining are not supported.\n");
                return -1;
            }
            ++p;
            if (!quote && (c == '\'' || c == '"')) { quote = c; continue; }
            if (quote && c == quote) { quote = 0; continue; }
            if (c == '\\' && quote != '\'') {
                if (!*p) { fprintf(stderr, "Incomplete escape.\n"); return -1; }
                if (quote == '"' && !strchr("\\\"$`\n", *p)) {
                    *out++ = '\\';
                    continue;
                }
                c = *p++;
            }
            *out++ = c;
        }
        if (quote) { fprintf(stderr, "Unclosed quote.\n"); return -1; }
        *out++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static Job *job_argument(const char *arg) {
    if (!arg) { fprintf(stderr, "Specify a job, for example %%1.\n"); return NULL; }
    if (*arg == '%') ++arg;
    char *end;
    errno = 0;
    long id = strtol(arg, &end, 10);
    if (errno || !*arg || *end || id < 1 || id > INT_MAX) {
        fprintf(stderr, "Invalid job ID. Use jobs to see valid IDs.\n"); return NULL;
    }
    Job *j = find_job((int)id);
    if (!j || j->state == DONE) { fprintf(stderr, "No active job with ID %ld.\n", id); return NULL; }
    return j;
}

static bool builtin(int argc, char **argv, bool background) {
    const char *name = argv[0];
    bool known = !strcmp(name, "help") || !strcmp(name, "jobs") || !strcmp(name, "fg") ||
                 !strcmp(name, "bg") || !strcmp(name, "kill") || !strcmp(name, "cd") ||
                 !strcmp(name, "pwd") || !strcmp(name, "exit");
    if (!known) return false;
    if (background) { fprintf(stderr, "Built-in commands do not accept &.\n"); return true; }
    if (!strcmp(name, "help")) {
        puts("MINI LINUX SHELL | C / POSIX process management\n"
             "  command [args]      execute a foreground program\n"
             "  command [args] &    execute in the background\n"
             "  jobs                list active / completed background jobs\n"
             "  fg %ID              give a job the terminal and wait\n"
             "  bg %ID              continue a stopped job in the background\n"
             "  kill [-SIGNAL] %ID  signal a tracked job only\n"
             "                      signals: TERM, INT, KILL, STOP, CONT (or 9)\n"
             "  cd [directory]      change the shell's directory\n"
             "  pwd                 print the current directory\n"
             "  exit                stop tracked jobs and leave the shell\n"
             "  Ctrl+C interrupt | Ctrl+Z stop | Ctrl+D end input\n"
             "  Quotes/backslashes supported. No pipes, redirection, expansion\n"
             "  or compound shell syntax. One external program per job.");
    } else if (!strcmp(name, "exit")) {
        if (argc != 1) fprintf(stderr, "Usage: exit\n"); else shutting_down = 1;
    } else if (!strcmp(name, "pwd")) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) puts(cwd); else perror("pwd");
    } else if (!strcmp(name, "cd")) {
        const char *path = argc == 1 ? getenv("HOME") : argv[1];
        if (argc > 2) fprintf(stderr, "Usage: cd [directory]\n");
        else if (!path) fprintf(stderr, "cd: HOME is not set.\n");
        else if (chdir(path) < 0) perror("cd");
        publish_status();
    } else if (!strcmp(name, "jobs")) {
        (void)reap_children();
        bool any = false;
        puts("JOB    PID       STATE     COMMAND");
        for (int i = 0; i < MAX_JOBS; ++i) {
            Job *j = &jobs[i];
            if (j->state == EMPTY || (j->state == DONE && !j->background)) continue;
            printf("[%-3d] %-9ld %-9s %s\n", j->id, (long)j->pid, state_name(j->state), j->command);
            any = true;
        }
        if (!any) puts("No background jobs.");
    } else if (!strcmp(name, "fg") || !strcmp(name, "bg")) {
        if (argc != 2) { fprintf(stderr, "Usage: %s %%ID\n", name); return true; }
        Job *j = job_argument(argv[1]);
        if (!j) return true;
        if (!strcmp(name, "fg")) wait_foreground(j, j->state == STOPPED, -1);
        else if (j->state != STOPPED) fprintf(stderr, "bg: job is already running.\n");
        else if (kill(-j->pid, SIGCONT) < 0) perror("bg");
        else {
            j->state = RUNNING;
            j->background = true;
            printf("[%d] Running %s\n", j->id, j->command);
            publish_status();
        }
    } else if (!strcmp(name, "kill")) {
        int sig = SIGTERM, pos = 1;
        if (argc > 1 && argv[1][0] == '-') {
            const char *s = argv[1] + 1;
            if (!strncmp(s, "SIG", 3)) s += 3;
            if (!strcmp(s, "TERM")) sig = SIGTERM;
            else if (!strcmp(s, "INT")) sig = SIGINT;
            else if (!strcmp(s, "KILL") || !strcmp(s, "9")) sig = SIGKILL;
            else if (!strcmp(s, "STOP")) sig = SIGSTOP;
            else if (!strcmp(s, "CONT")) sig = SIGCONT;
            else { fprintf(stderr, "Unsupported signal. Use TERM, INT, KILL, STOP or CONT.\n"); return true; }
            pos = 2;
        }
        if (argc != pos + 1) { fprintf(stderr, "Usage: kill [-SIGNAL] %%ID\n"); return true; }
        Job *j = job_argument(argv[pos]);
        if (j) {
            if (kill(-j->pid, sig) < 0) perror("kill");
            else {
                if (sig == SIGTERM && j->state == STOPPED) (void)kill(-j->pid, SIGCONT);
                printf("Signal %d sent to job [%d] (PGID %ld).\n", sig, j->id, (long)j->pid);
            }
        }
    }
    return true;
}

static void launch(int argc, char **argv, bool background, const char *line) {
    (void)argc;
    Job *slot = NULL;
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].state == EMPTY) { slot = &jobs[i]; break; }
        if (jobs[i].state == DONE && (!slot || jobs[i].id < slot->id)) slot = &jobs[i];
    }
    if (!slot) { fprintf(stderr, "Job table full: finish a job before starting another.\n"); return; }
    int gate[2];
    if (pipe(gate) < 0) { perror("pipe"); return; }
    cloexec(gate[0]); cloexec(gate[1]);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(gate[0]); close(gate[1]); return; }
    if (pid == 0) {
        close(gate[1]);
        if (setpgid(0, 0) < 0) _exit(126);
        int signals[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGTERM, SIGHUP, SIGPIPE};
        for (size_t k = 0; k < sizeof(signals) / sizeof(signals[0]); ++k)
            set_handler(signals[k], SIG_DFL);
        close(wake_pipe[0]); close(wake_pipe[1]);
        if (status_fd >= 0) close(status_fd);
        char token;
        while (read(gate[0], &token, 1) < 0 && errno == EINTR) {}
        close(gate[0]);
        execvp(argv[0], argv);
        int failure = errno;
        fprintf(stderr, "%s: %s\n", argv[0], strerror(failure));
        _exit(failure == ENOENT ? 127 : 126);
    }
    close(gate[0]);
    if (setpgid(pid, pid) < 0 && errno != EACCES && errno != ESRCH) perror("setpgid");
    memset(slot, 0, sizeof(*slot));
    slot->id = next_id++;
    slot->pid = pid;
    slot->state = RUNNING;
    slot->background = background;
    snprintf(slot->command, sizeof(slot->command), "%s", line);
    if (background) {
        close(gate[1]);
        printf("[%d] %ld\n", slot->id, (long)pid);
        publish_status();
    } else wait_foreground(slot, false, gate[1]);
}

static void execute(const char *line) {
    char storage[MAX_LINE + MAX_ARGS];
    char *argv[MAX_ARGS];
    bool background;
    int argc = parse(line, storage, argv, &background);
    if (argc <= 0) return;
    (void)reap_children();
    if (!builtin(argc, argv, background)) launch(argc, argv, background, line);
}

static bool active_jobs(void) {
    for (int i = 0; i < MAX_JOBS; ++i)
        if (jobs[i].state == RUNNING || jobs[i].state == STOPPED) return true;
    return false;
}

static void signal_active(int sig) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        Job *j = &jobs[i];
        if (j->state != RUNNING && j->state != STOPPED) continue;
        (void)kill(-j->pid, sig);
        if (sig != SIGKILL) (void)kill(-j->pid, SIGCONT);
    }
}

static void cleanup(void) {
    signal_active(SIGHUP);
    /* Bounded grace period, then terminate any tracked job that ignores HUP. */
    for (int n = 0; n < 10 && active_jobs(); ++n) {
        struct timespec pause = {0, 30000000};
        (void)nanosleep(&pause, NULL);
        (void)reap_children();
    }
    signal_active(SIGKILL);
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, 0)) > 0 || (pid < 0 && errno == EINTR)) {}
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].state == RUNNING || jobs[i].state == STOPPED) {
            jobs[i].state = DONE;
            jobs[i].result = 128 + SIGKILL;
        }
    }
    foreground_pgid = 0;
    publish_status();
    give_terminal(shell_pgid);
    if (interactive) (void)tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_modes);
    if (status_fd >= 0) close(status_fd);
    close(wake_pipe[0]); close(wake_pipe[1]);
    puts("Shell closed. Tracked jobs cleaned up.");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    interactive = isatty(STDIN_FILENO);
    shell_pgid = getpgrp();
    if (interactive) {
        pid_t owner;
        while ((owner = tcgetpgrp(STDIN_FILENO)) >= 0 && owner != getpgrp())
            (void)kill(-getpgrp(), SIGTTIN);
        if (getpgrp() != getpid() && setpgid(0, 0) < 0) { perror("shell setpgid"); return 1; }
        shell_pgid = getpgrp();
        set_handler(SIGTTOU, SIG_IGN);
        give_terminal(shell_pgid);
        if (tcgetattr(STDIN_FILENO, &shell_modes) < 0) { perror("tcgetattr"); return 1; }
    }
    if (pipe(wake_pipe) < 0) { perror("self-pipe"); return 1; }
    for (int i = 0; i < 2; ++i) {
        cloexec(wake_pipe[i]);
        (void)fcntl(wake_pipe[i], F_SETFL, O_NONBLOCK);
    }
    set_handler(SIGCHLD, on_signal);
    set_handler(SIGINT, on_signal);
    set_handler(SIGTERM, on_signal);
    set_handler(SIGHUP, on_signal);
    set_handler(SIGQUIT, SIG_IGN);
    set_handler(SIGTSTP, SIG_IGN);
    set_handler(SIGTTIN, SIG_IGN);
    set_handler(SIGTTOU, SIG_IGN);
    set_handler(SIGPIPE, SIG_IGN);
    const char *fd_env = getenv("MINISHELL_STATUS_FD");
    if (fd_env) {
        char *end;
        long fd = strtol(fd_env, &end, 10);
        if (*fd_env && !*end && fd > STDERR_FILENO && fd <= INT_MAX) {
            status_fd = (int)fd;
            cloexec(status_fd);
        }
        unsetenv("MINISHELL_STATUS_FD");
    }
    puts("Mini Linux Shell | type help to begin");
    publish_status();
    prompt();
    char line[MAX_LINE];
    size_t used = 0;
    bool overflow = false;
    while (!shutting_down) {
        struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {wake_pipe[0], POLLIN, 0}};
        int result = poll(fds, 2, -1);
        if (result < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        if (fds[1].revents & POLLIN) {
            drain_wakeup();
            bool notification = reap_children();
            if (interrupted) {
                interrupted = 0;
                used = 0;
                overflow = false;
                putchar('\n');
                notification = true;
            }
            if (notification) prompt();
        }
        if (shutting_down) break;
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            /* Read exactly one byte: do not prefetch input intended for a child. */
            char c;
            ssize_t got = read(STDIN_FILENO, &c, 1);
            if (got == 0) {
                if (used && !overflow) { line[used] = '\0'; execute(line); }
                shutting_down = 1;
                break;
            }
            if (got < 0) { if (errno == EINTR) continue; perror("read"); break; }
            if (c == '\n') {
                if (overflow) fprintf(stderr, "Input too long (maximum %d bytes).\n", MAX_LINE - 1);
                else { line[used] = '\0'; execute(line); }
                used = 0;
                overflow = false;
                prompt();
            } else if (used < sizeof(line) - 1) line[used++] = c;
            else overflow = true;
        }
    }
    cleanup();
    return 0;
}
