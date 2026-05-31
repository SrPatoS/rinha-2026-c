#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef TCP_QUICKACK
#define TCP_QUICKACK 12
#endif

#ifndef TCP_DEFER_ACCEPT
#define TCP_DEFER_ACCEPT 9
#endif

#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN 23
#endif

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

#define MAX_BACKENDS 16
#define DEFAULT_BACKLOG 65535
#define DEFAULT_ACCEPT_BATCH 128

static const char READY_RESPONSE[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 18\r\n\r\n{\"status\":\"ready\"}";

typedef struct {
    int fd;
    char byte;
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    struct cmsghdr *cmsg;
} Backend;

static int env_int(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    int parsed = atoi(value);
    return parsed > 0 ? parsed : fallback;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int wait_for_path(const char *path) {
    for (int i = 0; i < 600; i++) {
        struct stat st;
        if (stat(path, &st) == 0) return 0;
        sleep_ms(50);
    }
    return -1;
}

static int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int buf = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void init_backend(Backend *backend, int fd) {
    memset(backend, 0, sizeof(*backend));
    backend->fd = fd;
    backend->byte = 1;
    backend->iov.iov_base = &backend->byte;
    backend->iov.iov_len = 1;
    backend->msg.msg_iov = &backend->iov;
    backend->msg.msg_iovlen = 1;
    backend->msg.msg_control = backend->control;
    backend->msg.msg_controllen = sizeof(backend->control);
    backend->cmsg = CMSG_FIRSTHDR(&backend->msg);
    backend->cmsg->cmsg_level = SOL_SOCKET;
    backend->cmsg->cmsg_type = SCM_RIGHTS;
    backend->cmsg->cmsg_len = CMSG_LEN(sizeof(int));
}

static int send_fd(Backend *backend, int fd) {
    backend->msg.msg_controllen = sizeof(backend->control);
    memcpy(CMSG_DATA(backend->cmsg), &fd, sizeof(fd));
    for (;;) {
        ssize_t sent = sendmsg(backend->fd, &backend->msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) return 0;
        if (sent < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int split_paths(char *text, char *paths[MAX_BACKENDS]) {
    int count = 0;
    char *save = NULL;
    for (char *part = strtok_r(text, ",", &save);
         part && count < MAX_BACKENDS;
         part = strtok_r(NULL, ",", &save)) {
        while (*part == ' ' || *part == '\t') part++;
        if (*part) paths[count++] = part;
    }
    return count;
}

static void tune_listener(int fd) {
    int yes = 1;
    int defer_accept = 1;
    int fast_open = 256;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer_accept, sizeof(defer_accept));
    setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &fast_open, sizeof(fast_open));
}

static void tune_client(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &yes, sizeof(yes));
}

static bool maybe_handle_ready(int fd) {
    char peek[16];
    ssize_t n = recv(fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);
    if (n < 11) return false;
    if (memcmp(peek, "GET /ready ", 11) != 0) return false;

    size_t sent = 0;
    size_t len = sizeof(READY_RESPONSE) - 1u;
    while (sent < len) {
        ssize_t written = send(fd, READY_RESPONSE + sent, len - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        break;
    }
    return true;
}

static int listen_tcp(int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    tune_listener(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int port = env_int("PORT", 9999);
    int backlog = env_int("LB_BACKLOG", DEFAULT_BACKLOG);
    int accept_batch = env_int("LB_ACCEPT_BATCH", DEFAULT_ACCEPT_BATCH);

    char sockets_buf[1024];
    const char *env = getenv("API_SOCKETS");
    snprintf(sockets_buf, sizeof(sockets_buf), "%s",
             env && *env ? env : "/sockets/api1.sock,/sockets/api2.sock");

    char *paths[MAX_BACKENDS] = {0};
    int backend_count = split_paths(sockets_buf, paths);
    if (backend_count <= 0) return 2;

    Backend backends[MAX_BACKENDS];
    for (int i = 0; i < backend_count; i++) {
        if (wait_for_path(paths[i]) < 0) return 3;
        int fd = -1;
        for (int t = 0; t < 100 && fd < 0; t++) {
            fd = connect_unix(paths[i]);
            if (fd < 0) sleep_ms(20);
        }
        if (fd < 0) return 4;
        init_backend(&backends[i], fd);
    }

    int server = listen_tcp(port, backlog);
    if (server < 0) return 5;

    int rr = 0;
    for (;;) {
        int accepted = 0;
        while (accepted < accept_batch) {
            int client = accept4(server, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                continue;
            }
            accepted++;
            tune_client(client);

            if (maybe_handle_ready(client)) {
                close(client);
                continue;
            }

            int target = rr;
            rr = (rr + 1) % backend_count;
            if (send_fd(&backends[target], client) < 0) {
                target = rr;
                rr = (rr + 1) % backend_count;
                (void)send_fd(&backends[target], client);
            }
            close(client);
        }

        if (accepted == 0) {
            struct pollfd pfd;
            pfd.fd = server;
            pfd.events = POLLIN;
            pfd.revents = 0;
            poll(&pfd, 1, -1);
        }
    }
}
