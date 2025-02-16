#define _GNU_SOURCE
#include "freezer.h"
#include "ipc-client.h"
#include <assert.h>
#include <glib.h>
#include <jansson.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

const uint8_t DELAY_S = 2;

struct context {
    int sway_ipc_fd;
    char **proc_names;
    int proc_count;
    GHashTable *suspended_procs;
};

static const char *json_string_or_die(json_t *h, const char *name, bool nullable)
{
    json_t *ptr = json_object_get(h, name);
    if (!ptr)
        die("invalid type for '%s'", name);
    bool is_null = json_is_null(ptr);
    if (!(json_is_string(ptr) || (nullable && is_null)))
        die("invalid type for '%s'", name);
    return is_null ? NULL : json_string_value(ptr);
}

static json_t *json_object_or_die(json_t *h, const char *name)
{
    json_t *ptr = json_object_get(h, name);
    if (!ptr || !json_is_object(ptr))
        die("invalid type for '%s'", name);
    return ptr;
}

static int json_int_or_die(json_t *h, const char *name)
{
    json_t *ptr = json_object_get(h, name);
    if (!ptr || !json_is_integer(ptr))
        die("invalid type for '%s'", name);
    return json_integer_value(ptr);
}

static bool json_bool_or_die(json_t *h, const char *name)
{
    json_t *ptr = json_object_get(h, name);
    if (!ptr || !json_is_boolean(ptr))
        die("invalid type for '%s'", name);
    return json_boolean_value(ptr);
}

static int watch_window_events(void)
{
    int fd = ipc_open_socket();
    const char *payload = "[\"window\"]";
    uint32_t len = strlen(payload);
    char *resp = ipc_single_command(fd, IPC_SUBSCRIBE, payload, &len);

    json_error_t error;
    json_t *root = json_loads(resp, 0, &error);
    free(resp);
    if (!root)
        die("failed to parse json: %s", error.text);

    bool success = json_bool_or_die(root, "success");
    json_decref(root);
    if (!success)
        die("ipc request failed");

    return fd;
}

static json_t *read_window_event(int fd)
{
    struct ipc_response *resp = ipc_recv_response(fd);
    if (!resp)
        die("failed to read sway ipc response");
    json_error_t error;
    json_t *root = json_loads(resp->payload, 0, &error);
    if (!root)
        die("failed to parse json: %s", error.text);
    free_ipc_response(resp);
    return root;
}

static const char *get_focused_app(json_t *root, pid_t *pid)
{
    if (strcmp(json_string_or_die(root, "change", false), "focus"))
        return NULL;
    json_t *container_ptr = json_object_or_die(root, "container");
    if (pid)
        *pid = json_int_or_die(container_ptr, "pid");
    return json_string_or_die(container_ptr, "app_id", true);
}

struct window_tree_iter {
    json_t *root;
    GQueue *queue;
};

static struct window_tree_iter *get_sway_tree_iter(int fd)
{
    uint32_t len = 0;
    char *resp = ipc_single_command(fd, IPC_GET_TREE, NULL, &len);

    json_error_t error;
    json_t *root = json_loads(resp, 0, &error);
    if (!root)
        die("failed to parse json: %s", error.text);

    free(resp);

    struct window_tree_iter *it = calloc(1, sizeof(*it));
    assert(it != NULL);

    it->root = root;
    it->queue = g_queue_new();
    g_queue_push_head(it->queue, root);

    return it;
}

static json_t *sway_tree_iter_next(struct window_tree_iter *it)
{
    json_t *node = g_queue_pop_tail(it->queue);
    if (!node)
        return NULL;

    json_t *children = json_object_get(node, "nodes");
    if (!children || !json_is_array(children))
        die("invalid json type for 'nodes'");
    for (int i = 0; i < json_array_size(children); i++)
        g_queue_push_head(it->queue, json_array_get(children, i));

    return node;
}

static void sway_tree_iter_free(struct window_tree_iter *it)
{
    json_decref(it->root);
    g_queue_free(it->queue);
    free(it);
}

struct window_info {
    const char *app_id;
    pid_t pid;
    bool focused;
};

static bool iter_sway_apps(struct window_tree_iter *it, struct window_info *win)
{
    while (true) {
        json_t *node = sway_tree_iter_next(it);
        if (!node)
            return false;

        json_t *ptr = json_object_get(node, "app_id");
        if (!ptr)
            continue;
        if (json_is_null(ptr))
            continue;
        if (!json_is_string(ptr))
            die("invalid type for 'app_id'");

        if (win) {
            win->app_id = json_string_value(ptr);
            win->pid = json_int_or_die(node, "pid");
            win->focused = json_bool_or_die(node, "focused");
        }
        return true;
    }
}

static void start_timer(int timerfd)
{
    struct itimerspec tim = {
        .it_value = {.tv_sec = DELAY_S},
    };
    if (timerfd_settime(timerfd, 0, &tim, NULL) < 0)
        die("timerfd_settime failed: %m");
}

static void cancel_timer(int timerfd)
{
    struct itimerspec tim = {0};
    if (timerfd_settime(timerfd, 0, &tim, NULL) < 0)
        die("timerfd_settime failed: %m");
}

static bool should_suspend(struct context *ctx, const char *app_id)
{
    for (int i = 0; i < ctx->proc_count; i++) {
        if (!strcmp(ctx->proc_names[i], app_id))
            return true;
    }
    return false;
}

static bool is_suspended(struct context *ctx, const char *app_id)
{
    return g_hash_table_contains(ctx->suspended_procs, app_id);
}

static bool kill_all(pid_t pid, int signum)
{
    pid_t *pids = get_pid_children(pid);
    if (!pids)
        return false;
    for (pid_t *p = pids; *p; p++) {
        if (kill(*p, signum) < 0)
            perror("kill");
    }
    return true;
}

static bool resume_app(struct context *ctx, const char *app_id, pid_t pid)
{
    if (!kill_all(pid, SIGCONT))
        return false;
    g_hash_table_remove(ctx->suspended_procs, app_id);
    return true;
}

static bool suspend_app(struct context *ctx, const char *app_id, pid_t pid)
{
    if (!kill_all(pid, SIGSTOP))
        return false;
    g_hash_table_add(ctx->suspended_procs, strdup(app_id));
    return true;
}

static void resume_all_apps(struct context *ctx)
{
    struct window_tree_iter *it = get_sway_tree_iter(ctx->sway_ipc_fd);
    struct window_info win;
    while (iter_sway_apps(it, &win)) {
        if (should_suspend(ctx, win.app_id)) {
            if (resume_app(ctx, win.app_id, win.pid))
                g_debug("resumed %s processes", win.app_id);
        }
    }
    sway_tree_iter_free(it);
}

static void suspend_all_apps(struct context *ctx)
{
    struct window_tree_iter *it = get_sway_tree_iter(ctx->sway_ipc_fd);
    struct window_info win;
    while (iter_sway_apps(it, &win)) {
        if (!win.focused && should_suspend(ctx, win.app_id) && !is_suspended(ctx, win.app_id)) {
            if (suspend_app(ctx, win.app_id, win.pid)) {
                g_debug("suspended %s processes", win.app_id);
            }
        }
    }
    sway_tree_iter_free(it);
}

static void atexit_handler(int x, void *user_data)
{
    struct context *ctx = user_data;
    resume_all_apps(ctx);
}

static void signal_handler(int signum) { exit(0); }

int main(int argc, char *argv[])
{
    if (argc <= 1) {
        fprintf(stderr, "usage: sway-freezer <app_id> [<app_id> ...]\n");
        return 1;
    }

    struct context ctx = {0};
    ctx.proc_names = &argv[1];
    ctx.proc_count = argc - 1;
    ctx.suspended_procs = g_hash_table_new_full(g_str_hash, g_str_equal, free, NULL);

    int timerfd = timerfd_create(CLOCK_REALTIME, 0);
    if (timerfd < 0)
        die("timerfd_create failed: %m");

    ctx.sway_ipc_fd = ipc_open_socket();

    struct window_tree_iter *it = get_sway_tree_iter(ctx.sway_ipc_fd);
    struct window_info win;
    while (iter_sway_apps(it, &win)) {
        if (should_suspend(&ctx, win.app_id)) {
            start_timer(timerfd);
            break;
        }
    }
    sway_tree_iter_free(it);

    if (on_exit(atexit_handler, &ctx))
        die("on_exit");
    if (signal(SIGINT, signal_handler) < 0)
        die("signal");
    if (signal(SIGTERM, signal_handler) < 0)
        die("signal");

    int events_fd = watch_window_events();

    while (true) {
        struct pollfd fds[] = {
            {.fd = events_fd, .events = POLLIN},
            {.fd = timerfd, .events = POLLIN},
        };

        if (poll(fds, sizeof(fds) / sizeof(fds[0]), -1) < 0) {
            if (errno != EINTR)
                die("poll failed");
            continue;
        }

        if (fds[0].revents) {
            g_autoptr(json_t) resp = read_window_event(events_fd);
            pid_t pid = 0;
            const char *app_id = get_focused_app(resp, &pid);
            if (app_id) {
                if (should_suspend(&ctx, app_id)) {
                    cancel_timer(timerfd);
                    if (is_suspended(&ctx, app_id)) {
                        if (resume_app(&ctx, app_id, pid)) {
                            g_debug("resumed %s processes", app_id);
                        }
                    }
                } else if (ctx.proc_count != g_hash_table_size(ctx.suspended_procs))
                    start_timer(timerfd);
            }
        }

        if (fds[1].revents) {
            uint64_t val;
            if (read(timerfd, &val, sizeof(val)) < 0)
                die("timerfd: read failed: %m");
            suspend_all_apps(&ctx);
        }
    }

    g_hash_table_unref(ctx.suspended_procs);

    return 0;
}
