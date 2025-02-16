#define _GNU_SOURCE
#include "arena.h"
#include "freezer.h"
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Arena, arena_free)

struct submit_data {
    pid_t pid;
    char *path;
    char buf[4096];
};

static void ring_perror(int err, const char *where) { fprintf(stderr, "%s: %s\n", where, strerror(-err)); }

static int filter_pids(const struct dirent *dent)
{
    char *x = (char *)dent->d_name;
    while (*x)
        if (!isdigit(*x++))
            return 0;
    return 1;
}

static int create_sqe(struct io_uring *ring, int procfd, int idx, const char *filename, Arena *arena)
{
    struct submit_data *data = arena_alloc(arena, sizeof(*data));
    assert(data != NULL);
    data->path = arena_sprintf(arena, "%s/status", filename);

    char *endptr = NULL;
    data->pid = strtoul(filename, &endptr, 10);
    assert(*filename != '\0' && *endptr == '\0');

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return -1;
    sqe->flags |= IOSQE_IO_LINK;
    io_uring_prep_openat_direct(sqe, procfd, data->path, O_RDONLY, 0, idx);

    struct io_uring_sqe *sqe2 = io_uring_get_sqe(ring);
    if (!sqe2)
        return -1;
    sqe2->flags |= (IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK);
    io_uring_prep_read(sqe2, idx, data->buf, sizeof(data->buf), 0);
    io_uring_sqe_set_data(sqe2, data);

    struct io_uring_sqe *sqe3 = io_uring_get_sqe(ring);
    if (!sqe3)
        return -1;
    io_uring_prep_close_direct(sqe3, idx);

    return 0;
}

static void close_fd(int *fd)
{
    if (fd && *fd >= 0)
        close(*fd);
}

static void free_namelist(struct dirent **namelist, int count)
{
    while (count--)
        free(namelist[count]);
    free(namelist);
}

static pid_t parse_ppid(const char *buf)
{
    char *x = strstr(buf, "PPid:");
    assert(x != NULL);
    x += strlen("PPid:");
    while (*x == ' ')
        ++x;
    assert(*x != '\0');
    char *endptr = NULL;
    pid_t ppid = strtoul(x, &endptr, 10);
    assert(*endptr == '\0' || *endptr == '\n');
    return ppid;
}

static GHashTable *get_pid_relationships(Arena *arena, pid_t pid)
{
    CLEANUP(close_fd) int procfd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (procfd < 0) {
        perror("/proc");
        return NULL;
    }

    struct dirent **namelist;
    int count = scandirat(procfd, ".", &namelist, filter_pids, NULL);
    if (count < 0) {
        perror("scandirat");
        return NULL;
    }

    CLEANUP(io_uring_queue_exit) struct io_uring ring;
    int rv = io_uring_queue_init(count * 3, &ring, IORING_SETUP_SINGLE_ISSUER);
    if (rv < 0) {
        ring_perror(rv, "io_uring_queue_init");
        free_namelist(namelist, count);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        if (create_sqe(&ring, procfd, i, namelist[i]->d_name, arena) < 0) {
            fprintf(stderr, "io_uring too small!\n");
            abort();
        }
    }

    free_namelist(namelist, count);

    int *fds = arena_alloc(arena, count * sizeof(fds[0]));
    assert(fds != NULL);
    memset(fds, -1, count * sizeof(fds[0]));
    int ret = io_uring_register_files(&ring, fds, count);
    if (ret < 0) {
        ring_perror(ret, "io_uring_register_files");
        return NULL;
    }

    rv = io_uring_submit(&ring);
    if (rv < 0) {
        ring_perror(rv, "io_uring_submit");
        return NULL;
    }

    int pending = count;
    struct io_uring_cqe *cqes[count];
    GHashTable *pidmap = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)g_list_free);

    while (pending) {
        int n_cqes = io_uring_peek_batch_cqe(&ring, cqes, count);
        if (!n_cqes) {
            ret = io_uring_wait_cqe_nr(&ring, &cqes[0], pending);
            if (ret < 0) {
                ring_perror(ret, "io_uring_wait_cqe");
                goto err;
            }
            n_cqes = 1;
        }

        for (int i = 0; i < n_cqes; i++) {
            struct io_uring_cqe *cqe = cqes[i];
            struct submit_data *data = io_uring_cqe_get_data(cqe);

            if (cqe->res < 0) {
                if (cqe->res != -ENOENT && cqe->res != -ECANCELED && cqe->res != -ESRCH) {
                    ring_perror(cqe->res, "cqe result");
                    goto err;
                }
                pending--;
            } else if (data) {
                assert(cqe->res > 0);
                pid_t ppid = parse_ppid(data->buf);
                /* don't bother with kernel threads */
                if (ppid) {
                    GList *pids = g_hash_table_lookup(pidmap, GUINT_TO_POINTER(ppid));
                    if (!pids) {
                        pids = g_list_append(NULL, GUINT_TO_POINTER(data->pid));
                        g_hash_table_insert(pidmap, GUINT_TO_POINTER(ppid), pids);
                    } else
                        pids = g_list_append(pids, GUINT_TO_POINTER(data->pid));
                }

                pending--;
            }

            io_uring_cqe_seen(&ring, cqe);
        }
    }

    return pidmap;

err:
    g_hash_table_unref(pidmap);
    return NULL;
}

pid_t *get_pid_children(pid_t pid)
{
    g_auto(Arena) arena = {0};

    g_autoptr(GHashTable) pidmap = get_pid_relationships(&arena, pid);
    if (!pidmap)
        return NULL;

    GArray *result = g_array_new(true, false, sizeof(pid_t));

    g_autoptr(GQueue) queue = g_queue_new();
    g_queue_push_head(queue, GUINT_TO_POINTER(pid));

    while (!g_queue_is_empty(queue)) {
        pid_t p = GPOINTER_TO_UINT(g_queue_pop_tail(queue));
        g_array_append_val(result, p);

        GList *children = g_hash_table_lookup(pidmap, GUINT_TO_POINTER(p));
        if (children) {
            for (GList *e = children; e != NULL; e = e->next)
                g_queue_push_head(queue, e->data);
        }
    }

    return (pid_t *)g_array_free(result, false);
}
