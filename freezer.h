#pragma once

#include <glib.h>
#include <jansson.h>

#define CLEANUP(func) __attribute__((cleanup(func)))

G_DEFINE_AUTOPTR_CLEANUP_FUNC(json_t, json_decref)

pid_t *get_pid_children(pid_t pid);
