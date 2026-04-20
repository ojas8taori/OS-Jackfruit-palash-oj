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
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1U) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0U && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0U && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 0;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1U) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 1;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 1) {
        char path[PATH_MAX];
        int fd;
        size_t written = 0;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0)
            continue;

        while (written < item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            written += (size_t)n;
        }

        close(fd);
    }

    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = arg;

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }

    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }

    if (cfg->log_write_fd != STDOUT_FILENO && cfg->log_write_fd != STDERR_FILENO)
        close(cfg->log_write_fd);

    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("nice");
    }

    if (sethostname(cfg->id, strnlen(cfg->id, sizeof(cfg->id))) < 0)
        perror("sethostname");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount private");
        return 1;
    }

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir rootfs");
        return 1;
    }

    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
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

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

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

    /*
     * TODO:
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */
    {
        struct aux_record {
            char id[CONTAINER_ID_LEN];
            char rootfs[PATH_MAX];
            pid_t pid;
            int log_read_fd;
            int stop_requested;
            int monitor_registered;
            void *child_stack;
            child_config_t *child_cfg;
            struct aux_record *next;
        };
        struct aux_record *aux_head = NULL;
        struct sockaddr_un addr;
        sigset_t signal_set;
        sigset_t old_signal_set;
        int signal_mask_set = 0;
        int logger_started = 0;
        struct timespec no_wait;
        struct stat st;
        struct aux_record *aux_it = NULL;

        no_wait.tv_sec = 0;
        no_wait.tv_nsec = 0;

        if (stat(rootfs, &st) < 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "Invalid base-rootfs: %s\n", rootfs);
            bounded_buffer_begin_shutdown(&ctx.log_buffer);
            bounded_buffer_destroy(&ctx.log_buffer);
            pthread_mutex_destroy(&ctx.metadata_lock);
            return 1;
        }

        if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
            perror("mkdir logs");
            bounded_buffer_begin_shutdown(&ctx.log_buffer);
            bounded_buffer_destroy(&ctx.log_buffer);
            pthread_mutex_destroy(&ctx.metadata_lock);
            return 1;
        }

        ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
        if (ctx.monitor_fd < 0)
            fprintf(stderr,
                    "warning: could not open /dev/container_monitor: %s\n",
                    strerror(errno));

        unlink(CONTROL_PATH);
        ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (ctx.server_fd < 0) {
            perror("socket");
            if (ctx.monitor_fd >= 0)
                close(ctx.monitor_fd);
            bounded_buffer_begin_shutdown(&ctx.log_buffer);
            bounded_buffer_destroy(&ctx.log_buffer);
            pthread_mutex_destroy(&ctx.metadata_lock);
            return 1;
        }

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

        if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind control socket");
            close(ctx.server_fd);
            unlink(CONTROL_PATH);
            if (ctx.monitor_fd >= 0)
                close(ctx.monitor_fd);
            bounded_buffer_begin_shutdown(&ctx.log_buffer);
            bounded_buffer_destroy(&ctx.log_buffer);
            pthread_mutex_destroy(&ctx.metadata_lock);
            return 1;
        }

        if (listen(ctx.server_fd, 32) < 0) {
            perror("listen");
            close(ctx.server_fd);
            unlink(CONTROL_PATH);
            if (ctx.monitor_fd >= 0)
                close(ctx.monitor_fd);
            bounded_buffer_begin_shutdown(&ctx.log_buffer);
            bounded_buffer_destroy(&ctx.log_buffer);
            pthread_mutex_destroy(&ctx.metadata_lock);
            return 1;
        }

        if (fcntl(ctx.server_fd, F_SETFL, O_NONBLOCK) < 0)
            perror("fcntl server nonblock");

        if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
            perror("pthread_create logger_thread");
            close(ctx.server_fd);
            unlink(CONTROL_PATH);
            if (ctx.monitor_fd >= 0)
                close(ctx.monitor_fd);
            bounded_buffer_begin_shutdown(&ctx.log_buffer);
            bounded_buffer_destroy(&ctx.log_buffer);
            pthread_mutex_destroy(&ctx.metadata_lock);
            return 1;
        }
        logger_started = 1;

        sigemptyset(&signal_set);
        sigaddset(&signal_set, SIGCHLD);
        sigaddset(&signal_set, SIGINT);
        sigaddset(&signal_set, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &signal_set, &old_signal_set) == 0)
            signal_mask_set = 1;

        while (!ctx.should_stop) {
            int signal_number;
            pid_t reaped;
            int status;
            int conn_fd;
            struct aux_record *aux_prev;

            while (1) {
                errno = 0;
                signal_number = sigtimedwait(&signal_set, NULL, &no_wait);
                if (signal_number < 0) {
                    if (errno == EAGAIN)
                        break;
                    if (errno == EINTR)
                        continue;
                    break;
                }

                if (signal_number == SIGINT || signal_number == SIGTERM)
                    ctx.should_stop = 1;
            }

            while (1) {
                struct aux_record *aux_hit = NULL;
                int stop_requested = 0;
                container_record_t *record;

                status = 0;
                reaped = waitpid(-1, &status, WNOHANG);
                if (reaped == 0)
                    break;
                if (reaped < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }

                aux_it = aux_head;
                while (aux_it) {
                    if (aux_it->pid == reaped) {
                        aux_hit = aux_it;
                        stop_requested = aux_it->stop_requested;
                        aux_it->pid = 0;
                        if (aux_it->monitor_registered && ctx.monitor_fd >= 0) {
                            if (unregister_from_monitor(ctx.monitor_fd, aux_it->id, reaped) < 0)
                                perror("MONITOR_UNREGISTER");
                            aux_it->monitor_registered = 0;
                        }
                        if (aux_it->child_stack) {
                            free(aux_it->child_stack);
                            aux_it->child_stack = NULL;
                        }
                        if (aux_it->child_cfg) {
                            free(aux_it->child_cfg);
                            aux_it->child_cfg = NULL;
                        }
                        break;
                    }
                    aux_it = aux_it->next;
                }

                pthread_mutex_lock(&ctx.metadata_lock);
                record = ctx.containers;
                while (record) {
                    if (record->host_pid == reaped &&
                        (record->state == CONTAINER_STARTING || record->state == CONTAINER_RUNNING)) {
                        if (WIFEXITED(status)) {
                            record->state = stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                            record->exit_code = WEXITSTATUS(status);
                            record->exit_signal = 0;
                        } else if (WIFSIGNALED(status)) {
                            record->state = stop_requested ? CONTAINER_STOPPED : CONTAINER_KILLED;
                            record->exit_code = -1;
                            record->exit_signal = WTERMSIG(status);
                        }
                        break;
                    }
                    record = record->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);

                (void)aux_hit;
            }

            while (1) {
                control_request_t request;
                control_response_t response;
                size_t bytes_read;

                conn_fd = accept(ctx.server_fd, NULL, NULL);
                if (conn_fd < 0) {
                    if (errno == EINTR)
                        continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    perror("accept");
                    break;
                }

                memset(&request, 0, sizeof(request));
                memset(&response, 0, sizeof(response));

                bytes_read = 0;
                while (bytes_read < sizeof(request)) {
                    ssize_t n = read(conn_fd,
                                     ((char *)&request) + bytes_read,
                                     sizeof(request) - bytes_read);
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        break;
                    }
                    if (n == 0)
                        break;
                    bytes_read += (size_t)n;
                }

                if (bytes_read != sizeof(request)) {
                    close(conn_fd);
                    continue;
                }

                request.container_id[sizeof(request.container_id) - 1] = '\0';
                request.rootfs[sizeof(request.rootfs) - 1] = '\0';
                request.command[sizeof(request.command) - 1] = '\0';

                if (request.kind == CMD_START || request.kind == CMD_RUN) {
                    struct stat rootfs_st;
                    int pipe_fds[2] = {-1, -1};
                    child_config_t *cfg = NULL;
                    void *child_stack = NULL;
                    container_record_t *record = NULL;
                    struct aux_record *aux = NULL;
                    pid_t child_pid;
                    int duplicate = 0;

                    if (request.container_id[0] == '\0' ||
                        request.rootfs[0] == '\0' ||
                        request.command[0] == '\0') {
                        response.status = 1;
                        snprintf(response.message,
                                 sizeof(response.message),
                                 "invalid request: id/rootfs/command required");
                    } else if (stat(request.rootfs, &rootfs_st) < 0 || !S_ISDIR(rootfs_st.st_mode)) {
                        response.status = 1;
                        snprintf(response.message,
                                 sizeof(response.message),
                                 "rootfs is not a directory: %s",
                                 request.rootfs);
                    } else {
                        pthread_mutex_lock(&ctx.metadata_lock);
                        record = ctx.containers;
                        while (record) {
                            if (strncmp(record->id, request.container_id, sizeof(record->id)) == 0) {
                                duplicate = 1;
                                break;
                            }
                            record = record->next;
                        }
                        pthread_mutex_unlock(&ctx.metadata_lock);

                        aux_it = aux_head;
                        while (!duplicate && aux_it) {
                            if (aux_it->pid > 0 &&
                                strncmp(aux_it->rootfs, request.rootfs, sizeof(aux_it->rootfs)) == 0) {
                                duplicate = 1;
                                break;
                            }
                            aux_it = aux_it->next;
                        }

                        if (duplicate) {
                            response.status = 1;
                            snprintf(response.message,
                                     sizeof(response.message),
                                     "container id or rootfs already in use");
                        } else if (pipe(pipe_fds) < 0) {
                            response.status = 1;
                            snprintf(response.message,
                                     sizeof(response.message),
                                     "pipe failed: %s",
                                     strerror(errno));
                        } else {
                            cfg = calloc(1, sizeof(*cfg));
                            child_stack = malloc(STACK_SIZE);
                            record = calloc(1, sizeof(*record));
                            aux = calloc(1, sizeof(*aux));

                            if (!cfg || !child_stack || !record || !aux) {
                                response.status = 1;
                                snprintf(response.message,
                                         sizeof(response.message),
                                         "allocation failed while launching container");
                            } else {
                                memset(cfg, 0, sizeof(*cfg));
                                strncpy(cfg->id, request.container_id, sizeof(cfg->id) - 1);
                                strncpy(cfg->rootfs, request.rootfs, sizeof(cfg->rootfs) - 1);
                                strncpy(cfg->command, request.command, sizeof(cfg->command) - 1);
                                cfg->nice_value = request.nice_value;
                                cfg->log_write_fd = pipe_fds[1];

                                child_pid = clone(child_fn,
                                                  (char *)child_stack + STACK_SIZE,
                                                  CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                                                  cfg);
                                if (child_pid < 0) {
                                    response.status = 1;
                                    snprintf(response.message,
                                             sizeof(response.message),
                                             "clone failed: %s",
                                             strerror(errno));
                                } else {
                                    int log_init_fd;

                                    close(pipe_fds[1]);
                                    pipe_fds[1] = -1;
                                    if (fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) < 0)
                                        perror("fcntl log pipe nonblock");

                                    memset(record, 0, sizeof(*record));
                                    strncpy(record->id, request.container_id, sizeof(record->id) - 1);
                                    record->host_pid = child_pid;
                                    record->started_at = time(NULL);
                                    record->state = CONTAINER_RUNNING;
                                    record->soft_limit_bytes = request.soft_limit_bytes;
                                    record->hard_limit_bytes = request.hard_limit_bytes;
                                    record->exit_code = -1;
                                    record->exit_signal = 0;
                                    snprintf(record->log_path,
                                             sizeof(record->log_path),
                                             "%s/%s.log",
                                             LOG_DIR,
                                             record->id);

                                    log_init_fd = open(record->log_path,
                                                       O_CREAT | O_TRUNC | O_WRONLY,
                                                       0644);
                                    if (log_init_fd >= 0)
                                        close(log_init_fd);

                                    pthread_mutex_lock(&ctx.metadata_lock);
                                    record->next = ctx.containers;
                                    ctx.containers = record;
                                    pthread_mutex_unlock(&ctx.metadata_lock);

                                    memset(aux, 0, sizeof(*aux));
                                    strncpy(aux->id, request.container_id, sizeof(aux->id) - 1);
                                    strncpy(aux->rootfs, request.rootfs, sizeof(aux->rootfs) - 1);
                                    aux->pid = child_pid;
                                    aux->log_read_fd = pipe_fds[0];
                                    pipe_fds[0] = -1;
                                    aux->stop_requested = 0;
                                    aux->monitor_registered = 0;
                                    aux->child_stack = child_stack;
                                    aux->child_cfg = cfg;
                                    aux->next = aux_head;
                                    aux_head = aux;

                                    if (ctx.monitor_fd >= 0) {
                                        if (register_with_monitor(ctx.monitor_fd,
                                                                  record->id,
                                                                  record->host_pid,
                                                                  record->soft_limit_bytes,
                                                                  record->hard_limit_bytes) == 0) {
                                            aux->monitor_registered = 1;
                                        } else {
                                            perror("MONITOR_REGISTER");
                                        }
                                    }

                                    response.status = 0;
                                    snprintf(response.message,
                                             sizeof(response.message),
                                             "accepted id=%s pid=%d",
                                             record->id,
                                             record->host_pid);

                                    record = NULL;
                                    aux = NULL;
                                    cfg = NULL;
                                    child_stack = NULL;
                                }
                            }
                        }
                    }

                    if (pipe_fds[0] >= 0)
                        close(pipe_fds[0]);
                    if (pipe_fds[1] >= 0)
                        close(pipe_fds[1]);
                    if (cfg)
                        free(cfg);
                    if (child_stack)
                        free(child_stack);
                    if (record)
                        free(record);
                    if (aux)
                        free(aux);
                } else if (request.kind == CMD_PS) {
                    size_t used = 0;
                    container_record_t *record;

                    response.status = 0;
                    response.message[0] = '\0';

                    pthread_mutex_lock(&ctx.metadata_lock);
                    record = ctx.containers;
                    if (!record) {
                        snprintf(response.message, sizeof(response.message), "(no containers tracked)\n");
                    } else {
                        while (record && used < sizeof(response.message) - 1) {
                            int n = snprintf(response.message + used,
                                             sizeof(response.message) - used,
                                             "id=%s pid=%d state=%s soft=%lu hard=%lu exit=%d signal=%d\n",
                                             record->id,
                                             record->host_pid,
                                             state_to_string(record->state),
                                             record->soft_limit_bytes >> 20,
                                             record->hard_limit_bytes >> 20,
                                             record->exit_code,
                                             record->exit_signal);
                            if (n < 0)
                                break;
                            if ((size_t)n >= sizeof(response.message) - used) {
                                used = sizeof(response.message) - 1;
                                break;
                            }
                            used += (size_t)n;
                            record = record->next;
                        }
                    }
                    pthread_mutex_unlock(&ctx.metadata_lock);
                } else if (request.kind == CMD_LOGS) {
                    char path[PATH_MAX];
                    int fd;
                    ssize_t n;

                    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, request.container_id);
                    fd = open(path, O_RDONLY);
                    if (fd < 0) {
                        response.status = 1;
                        snprintf(response.message,
                                 sizeof(response.message),
                                 "cannot open log for %s: %s",
                                 request.container_id,
                                 strerror(errno));
                    } else {
                        n = read(fd, response.message, sizeof(response.message) - 1);
                        if (n < 0)
                            n = 0;
                        response.message[n] = '\0';
                        response.status = 0;
                        close(fd);
                    }
                } else if (request.kind == CMD_STOP) {
                    struct aux_record *target = NULL;

                    aux_it = aux_head;
                    while (aux_it) {
                        if (aux_it->pid > 0 &&
                            strncmp(aux_it->id, request.container_id, sizeof(aux_it->id)) == 0) {
                            target = aux_it;
                            break;
                        }
                        aux_it = aux_it->next;
                    }

                    if (!target) {
                        response.status = 1;
                        snprintf(response.message,
                                 sizeof(response.message),
                                 "container not running: %s",
                                 request.container_id);
                    } else {
                        target->stop_requested = 1;
                        if (kill(target->pid, SIGTERM) < 0 && errno != ESRCH) {
                            response.status = 1;
                            snprintf(response.message,
                                     sizeof(response.message),
                                     "failed to stop %s: %s",
                                     request.container_id,
                                     strerror(errno));
                        } else {
                            response.status = 0;
                            snprintf(response.message,
                                     sizeof(response.message),
                                     "stop requested id=%s pid=%d",
                                     request.container_id,
                                     target->pid);
                        }
                    }
                } else {
                    response.status = 1;
                    snprintf(response.message,
                             sizeof(response.message),
                             "unsupported command kind=%d",
                             request.kind);
                }

                {
                    size_t bytes_written = 0;
                    while (bytes_written < sizeof(response)) {
                        ssize_t n = write(conn_fd,
                                          ((const char *)&response) + bytes_written,
                                          sizeof(response) - bytes_written);
                        if (n < 0) {
                            if (errno == EINTR)
                                continue;
                            break;
                        }
                        if (n == 0)
                            break;
                        bytes_written += (size_t)n;
                    }
                }

                close(conn_fd);
            }

            aux_prev = NULL;
            aux_it = aux_head;
            while (aux_it) {
                int remove_aux = 0;

                if (aux_it->log_read_fd >= 0) {
                    while (1) {
                        char chunk[LOG_CHUNK_SIZE];
                        ssize_t n = read(aux_it->log_read_fd, chunk, sizeof(chunk));

                        if (n > 0) {
                            log_item_t item;
                            memset(&item, 0, sizeof(item));
                            strncpy(item.container_id, aux_it->id, sizeof(item.container_id) - 1);
                            item.length = (size_t)n;
                            memcpy(item.data, chunk, (size_t)n);
                            if (bounded_buffer_push(&ctx.log_buffer, &item) != 0)
                                break;
                            continue;
                        }

                        if (n == 0) {
                            close(aux_it->log_read_fd);
                            aux_it->log_read_fd = -1;
                            break;
                        }

                        if (errno == EINTR)
                            continue;

                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;

                        close(aux_it->log_read_fd);
                        aux_it->log_read_fd = -1;
                        break;
                    }
                }

                if (aux_it->pid == 0 && aux_it->log_read_fd < 0)
                    remove_aux = 1;

                if (remove_aux) {
                    struct aux_record *next_aux = aux_it->next;
                    if (aux_prev)
                        aux_prev->next = next_aux;
                    else
                        aux_head = next_aux;
                    free(aux_it);
                    aux_it = next_aux;
                } else {
                    aux_prev = aux_it;
                    aux_it = aux_it->next;
                }
            }

            usleep(10000);
        }

        aux_it = aux_head;
        while (aux_it) {
            if (aux_it->pid > 0) {
                aux_it->stop_requested = 1;
                if (kill(aux_it->pid, SIGTERM) < 0 && errno != ESRCH)
                    perror("shutdown SIGTERM");
            }
            aux_it = aux_it->next;
        }

        {
            int tries;
            for (tries = 0; tries < 30; tries++) {
                int active = 0;

                while (1) {
                    int status2 = 0;
                    pid_t reaped2 = waitpid(-1, &status2, WNOHANG);
                    if (reaped2 <= 0)
                        break;
                }

                aux_it = aux_head;
                while (aux_it) {
                    if (aux_it->pid > 0) {
                        active = 1;
                        break;
                    }
                    aux_it = aux_it->next;
                }

                if (!active)
                    break;
                usleep(100000);
            }
        }

        aux_it = aux_head;
        while (aux_it) {
            if (aux_it->pid > 0) {
                if (kill(aux_it->pid, SIGKILL) < 0 && errno != ESRCH)
                    perror("shutdown SIGKILL");
            }
            aux_it = aux_it->next;
        }

        aux_it = aux_head;
        while (aux_it) {
            struct aux_record *next_aux = aux_it->next;
            if (aux_it->log_read_fd >= 0)
                close(aux_it->log_read_fd);
            if (aux_it->monitor_registered && ctx.monitor_fd >= 0)
                unregister_from_monitor(ctx.monitor_fd, aux_it->id, aux_it->pid);
            if (aux_it->child_stack)
                free(aux_it->child_stack);
            if (aux_it->child_cfg)
                free(aux_it->child_cfg);
            free(aux_it);
            aux_it = next_aux;
        }

        if (ctx.server_fd >= 0)
            close(ctx.server_fd);
        unlink(CONTROL_PATH);

        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);

        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        if (logger_started)
            pthread_join(ctx.logger_thread, NULL);

        {
            container_record_t *record = ctx.containers;
            while (record) {
                container_record_t *next = record->next;
                free(record);
                record = next;
            }
            ctx.containers = NULL;
        }

        if (signal_mask_set)
            pthread_sigmask(SIG_SETMASK, &old_signal_set, NULL);
    }

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    struct sockaddr_un addr;
    control_response_t response;
    int fd;
    size_t off;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    memset(&response, 0, sizeof(response));

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Failed to connect to supervisor (%s): %s\n",
                CONTROL_PATH,
                strerror(errno));
        close(fd);
        return 1;
    }

    off = 0;
    while (off < sizeof(*req)) {
        ssize_t n = write(fd, ((const char *)req) + off, sizeof(*req) - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("write request");
            close(fd);
            return 1;
        }
        off += (size_t)n;
    }

    off = 0;
    while (off < sizeof(response)) {
        ssize_t n = read(fd, ((char *)&response) + off, sizeof(response) - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read response");
            close(fd);
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "Supervisor closed the control connection unexpectedly\n");
            close(fd);
            return 1;
        }
        off += (size_t)n;
    }

    close(fd);

    if (response.message[0] != '\0')
        printf("%s\n", response.message);

    if (req->kind != CMD_RUN)
        return (response.status == 0) ? 0 : 1;

    if (response.status != 0)
        return 1;

    {
        int run_pid = -1;
        char run_id_buf[CONTAINER_ID_LEN];
        sigset_t signal_set;
        sigset_t old_signal_set;
        int signal_mask_set = 0;
        int stop_forwarded = 0;
        struct timespec no_wait;

        memset(run_id_buf, 0, sizeof(run_id_buf));
        if (sscanf(response.message, "accepted id=%31s pid=%d", run_id_buf, &run_pid) < 2)
            run_pid = -1;

        no_wait.tv_sec = 0;
        no_wait.tv_nsec = 0;

        sigemptyset(&signal_set);
        sigaddset(&signal_set, SIGINT);
        sigaddset(&signal_set, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &signal_set, &old_signal_set) == 0)
            signal_mask_set = 1;

        while (1) {
            int sig;
            control_request_t ps_req;
            control_response_t ps_resp;
            int ps_fd;

            while (1) {
                errno = 0;
                sig = sigtimedwait(&signal_set, NULL, &no_wait);
                if (sig < 0) {
                    if (errno == EAGAIN)
                        break;
                    if (errno == EINTR)
                        continue;
                    break;
                }

                if ((sig == SIGINT || sig == SIGTERM) && !stop_forwarded) {
                    control_request_t stop_req;
                    control_response_t stop_resp;
                    size_t stop_off;

                    memset(&stop_req, 0, sizeof(stop_req));
                    stop_req.kind = CMD_STOP;
                    strncpy(stop_req.container_id,
                            req->container_id,
                            sizeof(stop_req.container_id) - 1);

                    memset(&stop_resp, 0, sizeof(stop_resp));

                    ps_fd = socket(AF_UNIX, SOCK_STREAM, 0);
                    if (ps_fd >= 0) {
                        if (connect(ps_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                            stop_off = 0;
                            while (stop_off < sizeof(stop_req)) {
                                ssize_t n = write(ps_fd,
                                                  ((const char *)&stop_req) + stop_off,
                                                  sizeof(stop_req) - stop_off);
                                if (n < 0) {
                                    if (errno == EINTR)
                                        continue;
                                    break;
                                }
                                stop_off += (size_t)n;
                            }

                            stop_off = 0;
                            while (stop_off < sizeof(stop_resp)) {
                                ssize_t n = read(ps_fd,
                                                 ((char *)&stop_resp) + stop_off,
                                                 sizeof(stop_resp) - stop_off);
                                if (n < 0) {
                                    if (errno == EINTR)
                                        continue;
                                    break;
                                }
                                if (n == 0)
                                    break;
                                stop_off += (size_t)n;
                            }
                        }
                        close(ps_fd);
                    }

                    stop_forwarded = 1;
                }
            }

            if (run_pid > 0) {
                errno = 0;
                if (kill(run_pid, 0) < 0 && errno == ESRCH) {
                    if (signal_mask_set)
                        pthread_sigmask(SIG_SETMASK, &old_signal_set, NULL);
                    return stop_forwarded ? 130 : 0;
                }
            }

            memset(&ps_req, 0, sizeof(ps_req));
            ps_req.kind = CMD_PS;
            memset(&ps_resp, 0, sizeof(ps_resp));

            ps_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (ps_fd < 0) {
                perror("socket");
                if (signal_mask_set)
                    pthread_sigmask(SIG_SETMASK, &old_signal_set, NULL);
                return 1;
            }

            if (connect(ps_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                size_t ps_off = 0;
                int ps_ok = 1;

                while (ps_off < sizeof(ps_req)) {
                    ssize_t n = write(ps_fd,
                                      ((const char *)&ps_req) + ps_off,
                                      sizeof(ps_req) - ps_off);
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        ps_ok = 0;
                        break;
                    }
                    ps_off += (size_t)n;
                }

                ps_off = 0;
                while (ps_ok && ps_off < sizeof(ps_resp)) {
                    ssize_t n = read(ps_fd,
                                     ((char *)&ps_resp) + ps_off,
                                     sizeof(ps_resp) - ps_off);
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        ps_ok = 0;
                        break;
                    }
                    if (n == 0) {
                        ps_ok = 0;
                        break;
                    }
                    ps_off += (size_t)n;
                }

                if (ps_ok) {
                    char needle[CONTAINER_ID_LEN + 5];
                    char *line_start;

                    snprintf(needle, sizeof(needle), "id=%s ", req->container_id);
                    line_start = strstr(ps_resp.message, needle);
                    if (line_start) {
                        char line[CONTROL_MESSAGE_LEN];
                        size_t line_len = 0;
                        char id_value[CONTAINER_ID_LEN];
                        char state_value[32];
                        int pid_value;
                        unsigned long soft_value;
                        unsigned long hard_value;
                        int exit_value;
                        int signal_value;

                        while (line_start[line_len] != '\0' &&
                               line_start[line_len] != '\n' &&
                               line_len < sizeof(line) - 1) {
                            line[line_len] = line_start[line_len];
                            line_len++;
                        }
                        line[line_len] = '\0';

                        if (sscanf(line,
                                   "id=%31s pid=%d state=%31s soft=%lu hard=%lu exit=%d signal=%d",
                                   id_value,
                                   &pid_value,
                                   state_value,
                                   &soft_value,
                                   &hard_value,
                                   &exit_value,
                                   &signal_value) == 7) {
                            (void)pid_value;
                            (void)soft_value;
                            (void)hard_value;
                            if (strcmp(state_value, "running") != 0 &&
                                strcmp(state_value, "starting") != 0) {
                                if (signal_mask_set)
                                    pthread_sigmask(SIG_SETMASK, &old_signal_set, NULL);
                                if (signal_value > 0)
                                    return 128 + signal_value;
                                return exit_value;
                            }
                        }
                    }
                }
            }

            close(ps_fd);
            usleep(200000);
        }
    }
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
