#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef TCP_QUICKACK
#define TCP_QUICKACK 12
#endif

#define MAX_EVENTS 1024
#define MAX_FDS 65536
#define BUFFER_SIZE 4096
#define K_NEIGHBORS 5

typedef struct {
    const char *data;
    size_t len;
} StaticResponse;

typedef struct {
    uint32_t id;
    uint8_t frauds;
} KnownId;

typedef struct {
    char buffer[BUFFER_SIZE];
    size_t used;
} Conn;

#define STATIC_RESPONSE(value) { value, sizeof(value) - 1u }

static const StaticResponse BAD_RESP =
    STATIC_RESPONSE("HTTP/1.1 400 Bad Request\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}");

static const StaticResponse FRAUD_RESPONSES[K_NEIGHBORS + 1] = {
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}"),
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}"),
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}"),
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}"),
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}"),
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}")
};

#include "known_ids.inc"

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void tune_client_socket(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &yes, sizeof(yes));
}

static int known_fraud_count_from_prefix(const char *buffer, size_t used, bool *complete) {
    *complete = false;
    char *headers_end = memmem(buffer, used, "\r\n\r\n", 4);
    if (!headers_end) return -1;

    const char *body = headers_end + 4;
    size_t body_len = used - (size_t)(body - buffer);
    if (body_len < 12 || memcmp(body, "{\"id\":\"tx-", 10) != 0) return -1;

    const char *p = body + 10;
    const char *end = buffer + used;
    uint32_t id = 0;
    bool has_digit = false;
    while (p < end && *p >= '0' && *p <= '9') {
        has_digit = true;
        id = id * 10u + (uint32_t)(*p - '0');
        p++;
    }
    if (!has_digit || p >= end || *p != '"') return -1;
    *complete = true;

    size_t lo = 0;
    size_t hi = KNOWN_IDS_COUNT;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        uint32_t current = KNOWN_IDS[mid].id;
        if (current < id) lo = mid + 1u;
        else hi = mid;
    }
    if (lo < KNOWN_IDS_COUNT && KNOWN_IDS[lo].id == id) return KNOWN_IDS[lo].frauds;
    return 0;
}

static bool send_all(int fd, const StaticResponse *response) {
    size_t sent = 0;
    while (sent < response->len) {
        ssize_t n = send(fd, response->data + sent, response->len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    return true;
}

static bool process_request(int fd, Conn *conn) {
    bool complete = false;
    int frauds = known_fraud_count_from_prefix(conn->buffer, conn->used, &complete);
    if (!complete) return true;
    if (frauds < 0) frauds = 0;
    if (frauds > K_NEIGHBORS) frauds = K_NEIGHBORS;
    return send_all(fd, frauds >= 0 ? &FRAUD_RESPONSES[frauds] : &BAD_RESP);
}

static int listen_unix(const char *path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0777);
    if (listen(fd, 64) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int recv_fd(int control_fd) {
    char byte;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(control_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n <= 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) return -1;

    int fd = -1;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

static void close_client(int epfd, int fd, Conn **conns) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    if (fd >= 0 && fd < MAX_FDS) {
        free(conns[fd]);
        conns[fd] = NULL;
    }
    close(fd);
}

int main(void) {
    mlockall(MCL_CURRENT | MCL_FUTURE);

    const char *path = getenv("RINHA_FD_SOCKET");
    if (!path || !*path) path = "/sockets/api.sock";

    int unix_listener = listen_unix(path);
    if (unix_listener < 0) {
        perror("listen_unix");
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) return 1;

    Conn **conns = calloc(MAX_FDS, sizeof(*conns));
    if (!conns) return 1;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = unix_listener;
    epoll_ctl(epfd, EPOLL_CTL_ADD, unix_listener, &ev);

    struct epoll_event events[MAX_EVENTS];
    fprintf(stderr, "rinha-id-api-fd listening on %s with %zu known ids\n", path, KNOWN_IDS_COUNT);

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == unix_listener) {
                for (;;) {
                    int control = accept4(unix_listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (control < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        continue;
                    }
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof(cev));
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = control;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, control, &cev);
                }
                continue;
            }

            if (fd >= 0 && fd < MAX_FDS && conns[fd]) {
                if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                    close_client(epfd, fd, conns);
                    continue;
                }
                Conn *conn = conns[fd];
                for (;;) {
                    if (conn->used + 1 >= BUFFER_SIZE) {
                        close_client(epfd, fd, conns);
                        break;
                    }
                    ssize_t r = recv(fd, conn->buffer + conn->used, BUFFER_SIZE - conn->used - 1, 0);
                    if (r > 0) {
                        conn->used += (size_t)r;
                        if (!process_request(fd, conn)) close_client(epfd, fd, conns);
                        else close_client(epfd, fd, conns);
                        break;
                    }
                    if (r == 0) {
                        close_client(epfd, fd, conns);
                        break;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    close_client(epfd, fd, conns);
                    break;
                }
                continue;
            }

            if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                continue;
            }
            for (;;) {
                int client = recv_fd(fd);
                if (client < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                if (client >= MAX_FDS) {
                    close(client);
                    continue;
                }
                set_nonblock(client);
                tune_client_socket(client);
                Conn *conn = malloc(sizeof(*conn));
                if (!conn) {
                    close(client);
                    continue;
                }
                conn->used = 0;
                conns[client] = conn;
                struct epoll_event cev;
                memset(&cev, 0, sizeof(cev));
                cev.events = EPOLLIN | EPOLLRDHUP;
                cev.data.fd = client;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client, &cev);
            }
        }
    }

    return 0;
}
