/* https://github.com/swaywm/sway/blob/master/common/ipc-client.c */
#include "ipc-client.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char ipc_magic[] = {'i', '3', '-', 'i', 'p', 'c'};

#define IPC_HEADER_SIZE (sizeof(ipc_magic) + 8)

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

const char *get_socketpath(void)
{
    const char *swaysock = getenv("SWAYSOCK");
    if (!swaysock)
        die("SWAYSOCK env var not defined");
    return swaysock;
}

int ipc_open_socket(void)
{
    const char *socket_path = get_socketpath();

    struct sockaddr_un addr;
    int socketfd;
    if ((socketfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        die("Unable to open Unix socket");
    }
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
    int l = sizeof(struct sockaddr_un);
    if (connect(socketfd, (struct sockaddr *)&addr, l) == -1) {
        die("Unable to connect to %s", socket_path);
    }
    return socketfd;
}

bool ipc_set_recv_timeout(int socketfd, struct timeval tv)
{
    if (setsockopt(socketfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
        fprintf(stderr, "Failed to set ipc recv timeout\n");
        return false;
    }
    return true;
}

struct ipc_response *ipc_recv_response(int socketfd)
{
    char data[IPC_HEADER_SIZE];

    size_t total = 0;
    while (total < IPC_HEADER_SIZE) {
        ssize_t received = recv(socketfd, data + total, IPC_HEADER_SIZE - total, 0);
        if (received <= 0) {
            die("Unable to receive IPC response");
        }
        total += received;
    }

    struct ipc_response *response = malloc(sizeof(struct ipc_response));
    if (!response) {
        goto error_1;
    }

    memcpy(&response->size, data + sizeof(ipc_magic), sizeof(uint32_t));
    memcpy(&response->type, data + sizeof(ipc_magic) + sizeof(uint32_t), sizeof(uint32_t));

    char *payload = malloc(response->size + 1);
    if (!payload) {
        goto error_2;
    }

    total = 0;
    while (total < response->size) {
        ssize_t received = recv(socketfd, payload + total, response->size - total, 0);
        if (received < 0) {
            die("Unable to receive IPC response");
        }
        total += received;
    }
    payload[response->size] = '\0';
    response->payload = payload;

    return response;
error_2:
    free(response);
error_1:
    fprintf(stderr, "Unable to allocate memory for IPC response\n");
    return NULL;
}

void free_ipc_response(struct ipc_response *response)
{
    free(response->payload);
    free(response);
}

char *ipc_single_command(int socketfd, uint32_t type, const char *payload, uint32_t *len)
{
    char data[IPC_HEADER_SIZE];
    memcpy(data, ipc_magic, sizeof(ipc_magic));
    memcpy(data + sizeof(ipc_magic), len, sizeof(*len));
    memcpy(data + sizeof(ipc_magic) + sizeof(*len), &type, sizeof(type));

    if (write(socketfd, data, IPC_HEADER_SIZE) == -1) {
        die("Unable to send IPC header");
    }

    if (*len) {
        if (write(socketfd, payload, *len) == -1) {
            die("Unable to send IPC payload");
        }
    }

    struct ipc_response *resp = ipc_recv_response(socketfd);
    char *response = resp->payload;
    *len = resp->size;
    free(resp);

    return response;
}
