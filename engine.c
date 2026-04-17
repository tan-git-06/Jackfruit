/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Producer-side insertion into the bounded buffer.
 *
 * Blocks when the buffer is full (waiting on not_full).
 * Returns 0 on success, -1 if shutdown began before insertion.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while buffer is full, but bail out if shutting down */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Consumer-side removal from the bounded buffer.
 *
 * Returns  1 when an item was successfully removed.
 * Returns  0 when shutting down and the buffer is now empty (consumer should exit).
 * Returns -1 on unexpected error.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while buffer is empty, unless we are shutting down */
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0) {
        /* Shutting down and nothing left to drain */
        pthread_mutex_unlock(&buffer->mutex);
        return 0;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 1;
}

/*
 * Logging consumer thread.
 *
 * Drains log chunks from the bounded buffer and writes each chunk to the
 * correct per-container log file (identified by container_id in the chunk).
 * Continues draining after shutdown is signalled until the buffer is empty,
 * so no log lines are lost when a container exits.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int rc;

    while (1) {
        rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc == 0)
            break;  /* shutdown + buffer drained */
        if (rc < 0)
            continue;

        /* Build the log file path for this container and append the chunk */
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open log file");
            continue;
        }

        ssize_t written = 0;
        while (written < (ssize_t)item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("logging_thread: write");
                break;
            }
            written += n;
        }
        close(fd);
    }

    return NULL;
}

/*
 * Producer thread: reads from a pipe connected to a single container's
 * stdout+stderr and pushes chunks into the shared bounded buffer.
 */
typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} producer_arg_t;

void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);

        n = read(pa->read_fd, item.data, sizeof(item.data));
        if (n <= 0)
            break;  /* EOF or error: container closed its end */

        item.length = (size_t)n;
        if (bounded_buffer_push(pa->log_buffer, &item) != 0)
            break;  /* buffer shutting down */
    }

    close(pa->read_fd);
    free(pa);
    return NULL;
}

/*
 * Clone child entrypoint.
 *
 * Runs inside the new namespaces (PID, UTS, mount).
 * 1. Redirects stdout and stderr to the log pipe.
 * 2. chroot into the container's rootfs.
 * 3. Mounts /proc so tools like ps work.
 * 4. Applies nice value if requested.
 * 5. Execs the configured command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the log write pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("child_fn: dup2 stdout");
        return 1;
    }
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("child_fn: dup2 stderr");
        return 1;
    }
    close(cfg->log_write_fd);

    /* chroot into the container's rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("child_fn: chdir /");
        return 1;
    }

    /* Mount /proc so PID tools work inside the container */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("child_fn: mount /proc");
        /* non-fatal: carry on */
    }

    /* Apply nice value if set */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) < 0)
            perror("child_fn: nice");
    }

    /* Execute the requested command */
    char *argv[] = { "/bin/sh", "-c", cfg->command, NULL };
    char *envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root",
        "TERM=xterm",
        NULL
    };
    execve("/bin/sh", argv, envp);
    perror("child_fn: execve");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* --------------------------------------------------------------------------
 * Signal handling
 * -------------------------------------------------------------------------- */

/*
 * SIGCHLD handler: reaps all exited children without blocking.
 * Updates container metadata (exit code / signal, state).
 * Sets stop_requested-aware termination classification per the spec.
 */
static void sigchld_handler(int signo)
{
    (void)signo;
    if (!g_ctx)
        return;

    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code   = WEXITSTATUS(status);
                    c->exit_signal = 0;
                    c->state       = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->exit_code   = 128 + c->exit_signal;
                    if (c->stop_requested) {
                        c->state = CONTAINER_STOPPED;
                    } else if (c->exit_signal == SIGKILL) {
                        c->state = CONTAINER_KILLED; /* hard-limit kill */
                    } else {
                        c->state = CONTAINER_EXITED;
                    }
                }
                /* Unregister from kernel monitor */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd,
                                            c->id, c->host_pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }

    errno = saved_errno;
}

/* SIGINT / SIGTERM: ask the supervisor event loop to stop */
static void sigterm_handler(int signo)
{
    (void)signo;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* --------------------------------------------------------------------------
 * Supervisor: launch a container
 * -------------------------------------------------------------------------- */

static int launch_container(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             container_record_t **out_record)
{
    /* Create a pipe: child writes, supervisor reads */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("launch_container: pipe");
        return -1;
    }

    /* Build child config */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,       CHILD_COMMAND_LEN - 1);
    cfg->nice_value    = req->nice_value;
    cfg->log_write_fd  = pipefd[1];

    /* Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);
    free(stack);

    if (pid < 0) {
        perror("launch_container: clone");
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* Close the write end in the supervisor */
    close(pipefd[1]);

    /* Spawn a producer thread to read from the pipe */
    producer_arg_t *pa = calloc(1, sizeof(*pa));
    if (!pa) {
        /* Non-fatal: just close the fd */
        close(pipefd[0]);
    } else {
        pa->read_fd    = pipefd[0];
        pa->log_buffer = &ctx->log_buffer;
        strncpy(pa->container_id, req->container_id, CONTAINER_ID_LEN - 1);

        pthread_t prod_tid;
        if (pthread_create(&prod_tid, NULL, producer_thread, pa) != 0) {
            perror("launch_container: pthread_create producer");
            close(pipefd[0]);
            free(pa);
        } else {
            pthread_detach(prod_tid);
        }
    }

    free(cfg); /* child_fn already copied what it needs onto its own stack */

    /* Create metadata record */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        /* Container is running but we can't track it -- kill it */
        kill(pid, SIGKILL);
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    rec->exit_code         = 0;
    rec->exit_signal       = 0;
    rec->stop_requested    = 0;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd, rec->id, pid,
                                  rec->soft_limit_bytes,
                                  rec->hard_limit_bytes) < 0)
            perror("launch_container: register_with_monitor");
    }

    /* Prepend to the container list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (out_record)
        *out_record = rec;

    return 0;
}

/* --------------------------------------------------------------------------
 * Supervisor: handle one control request from a connected client fd
 * -------------------------------------------------------------------------- */

static void handle_control_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;

    memset(&resp, 0, sizeof(resp));

    ssize_t n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Failed to read request");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START: {
        /* Check for duplicate ID */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *existing = ctx->containers;
        while (existing) {
            if (strncmp(existing->id, req.container_id, CONTAINER_ID_LEN) == 0 &&
                existing->state == CONTAINER_RUNNING) {
                break;
            }
            existing = existing->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (existing) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' is already running", req.container_id);
            break;
        }

        if (launch_container(ctx, &req, NULL) < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to launch container '%s'", req.container_id);
        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' started", req.container_id);
        }
        break;
    }

    case CMD_RUN: {
        container_record_t *rec = NULL;
        if (launch_container(ctx, &req, &rec) < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to launch container '%s'", req.container_id);
            break;
        }

        /* Block until the container exits */
        pid_t target = rec->host_pid;
        int wstatus;
        while (waitpid(target, &wstatus, 0) < 0 && errno == EINTR)
            ;

        pthread_mutex_lock(&ctx->metadata_lock);
        if (WIFEXITED(wstatus)) {
            rec->exit_code   = WEXITSTATUS(wstatus);
            rec->exit_signal = 0;
            rec->state       = CONTAINER_EXITED;
        } else if (WIFSIGNALED(wstatus)) {
            rec->exit_signal = WTERMSIG(wstatus);
            rec->exit_code   = 128 + rec->exit_signal;
            rec->state       = rec->stop_requested ? CONTAINER_STOPPED
                                                   : CONTAINER_KILLED;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = rec->exit_code;
        snprintf(resp.message, sizeof(resp.message),
                 "Container '%s' exited with code %d",
                 req.container_id, rec->exit_code);
        break;
    }

    case CMD_PS: {
        /* Build a multi-line text table */
        char *buf = resp.message;
        int remaining = sizeof(resp.message);
        int written = 0;

        int w = snprintf(buf + written, remaining,
                         "%-16s %-8s %-10s %-26s %-6s\n",
                         "ID", "PID", "STATE", "STARTED", "EXIT");
        if (w > 0) { written += w; remaining -= w; }

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && remaining > 1) {
            char tsbuf[32];
            struct tm *tm_info = localtime(&c->started_at);
            strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tm_info);

            w = snprintf(buf + written, remaining,
                         "%-16s %-8d %-10s %-26s %-6d\n",
                         c->id, c->host_pid,
                         state_to_string(c->state),
                         tsbuf,
                         c->exit_code);
            if (w > 0) { written += w; remaining -= w; }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        break;
    }

    case CMD_LOGS: {
        char log_path[PATH_MAX];
        snprintf(log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req.container_id);

        FILE *f = fopen(log_path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "No log file found for '%s'", req.container_id);
            break;
        }

        /* Stream file content back in fixed chunks */
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "Log for '%s':", req.container_id);
        send(client_fd, &resp, sizeof(resp), 0);

        char chunk[sizeof(resp.message)];
        size_t nread;
        while ((nread = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            memset(&resp, 0, sizeof(resp));
            resp.status = 1; /* 1 = more data follows */
            memcpy(resp.message, chunk, nread);
            send(client_fd, &resp, sizeof(resp), 0);
        }
        fclose(f);

        /* Send end-of-log sentinel */
        memset(&resp, 0, sizeof(resp));
        resp.status = 2; /* 2 = end of log */
        snprintf(resp.message, sizeof(resp.message), "<end-of-log>");
        send(client_fd, &resp, sizeof(resp), 0);
        return; /* already sent multiple responses */
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strncmp(c->id, req.container_id, CONTAINER_ID_LEN) == 0)
                break;
            c = c->next;
        }

        if (!c || (c->state != CONTAINER_RUNNING &&
                   c->state != CONTAINER_STARTING)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' not found or not running", req.container_id);
            break;
        }

        c->stop_requested = 1;
        pid_t target = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Graceful stop: SIGTERM first, then SIGKILL after a short wait */
        kill(target, SIGTERM);

        /* Give the container up to 3 seconds to exit */
        int waited = 0;
        for (; waited < 30; waited++) {
            usleep(100000); /* 100 ms */
            pthread_mutex_lock(&ctx->metadata_lock);
            container_state_t st = c->state;
            pthread_mutex_unlock(&ctx->metadata_lock);
            if (st != CONTAINER_RUNNING && st != CONTAINER_STARTING)
                break;
        }

        /* If still running, force kill */
        pthread_mutex_lock(&ctx->metadata_lock);
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
            kill(c->host_pid, SIGKILL);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "Stop signal sent to container '%s'", req.container_id);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        break;
    }

    send(client_fd, &resp, sizeof(resp), 0);
}

/* --------------------------------------------------------------------------
 * Supervisor main
 * -------------------------------------------------------------------------- */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* 1) Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* 2) Open /dev/container_monitor (optional: continue if not loaded) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open /dev/container_monitor: %s "
                        "(kernel module not loaded?)\n", strerror(errno));

    /* 3) Create the UNIX domain socket control channel */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    /* Remove stale socket file */
    unlink(CONTROL_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }

    /* 4) Install signal handlers */
    struct sigaction sa_chld, sa_term;

    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sa_term.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    /* 5) Spawn the logging consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        goto cleanup;
    }

    fprintf(stderr, "Supervisor started (base-rootfs: %s, socket: %s)\n",
            rootfs, CONTROL_PATH);

    /* 6) Event loop: accept connections and handle commands */
    while (!ctx.should_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }
        if (sel == 0)
            continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        handle_control_request(&ctx, client_fd);
        close(client_fd);
    }

    fprintf(stderr, "Supervisor shutting down...\n");

    /* Orderly shutdown: SIGTERM all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for all children */
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR)
        ;

    /* Drain and stop the logging thread */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

cleanup:
    if (ctx.server_fd >= 0) {
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
    }
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        container_record_t *next = cur->next;
        free(cur);
        cur = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    g_ctx = NULL;
    fprintf(stderr, "Supervisor exited cleanly.\n");
    return 0;
}

/* --------------------------------------------------------------------------
 * Client: connect to supervisor and send a control request
 * -------------------------------------------------------------------------- */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    /* Send the request */
    if (send(fd, req, sizeof(*req), 0) != sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    /* Receive one or more responses */
    control_response_t resp;
    int exit_status = 0;

    while (1) {
        ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n <= 0)
            break;

        if (resp.status == 1) {
            /* Streaming log data chunk */
            fwrite(resp.message, 1, strnlen(resp.message, sizeof(resp.message)),
                   stdout);
            continue;
        }

        if (resp.status == 2) {
            /* End-of-log sentinel */
            break;
        }

        /* Normal single response */
        printf("%s\n", resp.message);
        exit_status = (resp.status < 0) ? 1 : resp.status;
        break;
    }

    close(fd);
    return exit_status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
