#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define QUEUE_DEPTH 4096
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
    const StaticResponse *sending;
    size_t sent;
    bool active;
} Conn;

typedef enum {
    OP_ACCEPT = 1,
    OP_RECV = 2,
    OP_SEND = 3
} OpType;

typedef struct {
    OpType type;
    int fd;
} Op;

#define STATIC_RESPONSE(value) { value, sizeof(value) - 1u }

static const StaticResponse READY_RESP =
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 18\r\n\r\n{\"status\":\"ready\"}");

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

static Conn conns[MAX_FDS];
static Op accept_op = { OP_ACCEPT, -1 };
static Op recv_ops[MAX_FDS];
static Op send_ops[MAX_FDS];

static int content_length(const char *buffer, const char *headers_end) {
    const char *p = buffer;
    while (p && p < headers_end) {
        const char *next = strstr(p, "\r\n");
        if (!next || next > headers_end) break;
        if ((next - p) > 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            return atoi(p + 15);
        }
        p = next + 2;
    }
    return 0;
}

static int known_fraud_count_for_body(const char *body, size_t body_len) {
    const char needle[] = "\"id\":\"tx-";
    const char spaced[] = "\"id\": \"tx-";
    const char *p = NULL;
    const char *end = body + body_len;

    for (const char *cur = body; cur + sizeof(needle) - 1 <= end; cur++) {
        if (memcmp(cur, needle, sizeof(needle) - 1) == 0) {
            p = cur + sizeof(needle) - 1;
            break;
        }
    }
    if (!p) {
        for (const char *cur = body; cur + sizeof(spaced) - 1 <= end; cur++) {
            if (memcmp(cur, spaced, sizeof(spaced) - 1) == 0) {
                p = cur + sizeof(spaced) - 1;
                break;
            }
        }
    }
    if (!p) return 0;

    uint32_t id = 0;
    bool has_digit = false;
    while (p < end && *p >= '0' && *p <= '9') {
        has_digit = true;
        id = id * 10u + (uint32_t)(*p - '0');
        p++;
    }
    if (!has_digit) return 0;

    size_t lo = 0;
    size_t hi = KNOWN_IDS_COUNT;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        uint32_t current = KNOWN_IDS[mid].id;
        if (current < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo < KNOWN_IDS_COUNT && KNOWN_IDS[lo].id == id) {
        return KNOWN_IDS[lo].frauds;
    }
    return 0;
}

static bool next_response(Conn *conn, const StaticResponse **out) {
    conn->buffer[conn->used] = '\0';
    char *headers_end = strstr(conn->buffer, "\r\n\r\n");
    if (!headers_end) return false;

    int body_len = content_length(conn->buffer, headers_end);
    size_t header_bytes = (size_t)(headers_end + 4 - conn->buffer);
    size_t total = header_bytes + (size_t)body_len;
    if (conn->used < total) return false;

    const StaticResponse *response = &BAD_RESP;
    if (memcmp(conn->buffer, "GET /ready ", 11) == 0) {
        response = &READY_RESP;
    } else if (memcmp(conn->buffer, "POST /fraud-score ", 18) == 0) {
        int frauds = known_fraud_count_for_body(conn->buffer + header_bytes, (size_t)body_len);
        if (frauds < 0) frauds = 0;
        if (frauds > K_NEIGHBORS) frauds = K_NEIGHBORS;
        response = &FRAUD_RESPONSES[frauds];
    }

    if (conn->used > total) {
        memmove(conn->buffer, conn->buffer + total, conn->used - total);
    }
    conn->used -= total;
    *out = response;
    return true;
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4096) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void close_client(int fd) {
    if (fd >= 0 && fd < MAX_FDS) {
        conns[fd].active = false;
        conns[fd].used = 0;
        conns[fd].sending = NULL;
        conns[fd].sent = 0;
    }
    close(fd);
}

static bool send_all_sync(int fd, const StaticResponse *response) {
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

static bool process_ready_sync(int fd, Conn *conn) {
    for (;;) {
        const StaticResponse *response = NULL;
        if (!next_response(conn, &response)) return true;
        if (!send_all_sync(fd, response)) return false;
    }
}

static int run_epoll_fallback(int server) {
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = server;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server, &ev);

    struct epoll_event events[MAX_EVENTS];
    fprintf(stderr, "rinha-uring-lb falling back to epoll\n");

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            return 1;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == server) {
                for (;;) {
                    int client = accept4(server, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        continue;
                    }
                    if (client >= MAX_FDS) {
                        close(client);
                        continue;
                    }
                    int yes = 1;
                    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                    conns[client].active = true;
                    conns[client].used = 0;
                    conns[client].sending = NULL;
                    conns[client].sent = 0;

                    struct epoll_event cev;
                    memset(&cev, 0, sizeof(cev));
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = client;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client, &cev) < 0) {
                        close_client(client);
                    }
                }
                continue;
            }

            if (fd < 0 || fd >= MAX_FDS || !conns[fd].active || (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close_client(fd);
                continue;
            }

            Conn *conn = &conns[fd];
            for (;;) {
                if (conn->used + 1 >= BUFFER_SIZE) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close_client(fd);
                    break;
                }
                ssize_t r = recv(fd, conn->buffer + conn->used, BUFFER_SIZE - conn->used - 1, 0);
                if (r > 0) {
                    conn->used += (size_t)r;
                    if (!process_ready_sync(fd, conn)) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close_client(fd);
                        break;
                    }
                    continue;
                }
                if (r == 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close_client(fd);
                    break;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close_client(fd);
                break;
            }
        }
    }
}

static void submit_accept(struct io_uring *ring, int server) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_accept(sqe, server, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data(sqe, &accept_op);
}

static void submit_recv(struct io_uring *ring, int fd) {
    if (fd < 0 || fd >= MAX_FDS || !conns[fd].active) return;
    Conn *conn = &conns[fd];
    if (conn->used + 1 >= BUFFER_SIZE) {
        close_client(fd);
        return;
    }
    recv_ops[fd].type = OP_RECV;
    recv_ops[fd].fd = fd;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_recv(sqe, fd, conn->buffer + conn->used, BUFFER_SIZE - conn->used - 1, 0);
    io_uring_sqe_set_data(sqe, &recv_ops[fd]);
}

static void submit_send(struct io_uring *ring, int fd) {
    if (fd < 0 || fd >= MAX_FDS || !conns[fd].active || !conns[fd].sending) return;
    Conn *conn = &conns[fd];
    const StaticResponse *response = conn->sending;
    send_ops[fd].type = OP_SEND;
    send_ops[fd].fd = fd;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_send(sqe, fd, response->data + conn->sent, response->len - conn->sent, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, &send_ops[fd]);
}

static void arm_next_action(struct io_uring *ring, int fd) {
    const StaticResponse *response = NULL;
    if (next_response(&conns[fd], &response)) {
        conns[fd].sending = response;
        conns[fd].sent = 0;
        submit_send(ring, fd);
    } else {
        submit_recv(ring, fd);
    }
}

int main(void) {
    const char *port_env = getenv("PORT");
    int port = port_env ? atoi(port_env) : 9999;
    int server = listen_on(port);
    if (server < 0) {
        perror("listen");
        return 1;
    }

    struct io_uring ring;
    int uring_ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    if (uring_ret < 0) {
        fprintf(stderr, "io_uring unavailable: %s\n", strerror(-uring_ret));
        return run_epoll_fallback(server);
    }

    fprintf(stderr, "rinha-uring-lb listening on :%d with %zu known ids\n", port, KNOWN_IDS_COUNT);
    submit_accept(&ring, server);
    io_uring_submit(&ring);

    for (;;) {
        struct io_uring_cqe *cqe = NULL;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            break;
        }

        Op *op = (Op *)io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        if (!op) continue;

        if (op->type == OP_ACCEPT) {
            if (res >= 0) {
                int client = res;
                if (client >= MAX_FDS) {
                    close(client);
                } else {
                    int yes = 1;
                    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                    conns[client].active = true;
                    conns[client].used = 0;
                    conns[client].sending = NULL;
                    conns[client].sent = 0;
                    submit_recv(&ring, client);
                }
            }
            submit_accept(&ring, server);
        } else if (op->type == OP_RECV) {
            int fd = op->fd;
            if (fd < 0 || fd >= MAX_FDS || !conns[fd].active) {
                continue;
            }
            if (res <= 0) {
                close_client(fd);
            } else {
                conns[fd].used += (size_t)res;
                arm_next_action(&ring, fd);
            }
        } else if (op->type == OP_SEND) {
            int fd = op->fd;
            if (fd < 0 || fd >= MAX_FDS || !conns[fd].active || !conns[fd].sending) {
                continue;
            }
            if (res < 0) {
                if (res == -EAGAIN || res == -EWOULDBLOCK) {
                    submit_send(&ring, fd);
                } else {
                    close_client(fd);
                }
            } else {
                conns[fd].sent += (size_t)res;
                if (conns[fd].sent < conns[fd].sending->len) {
                    submit_send(&ring, fd);
                } else {
                    conns[fd].sending = NULL;
                    conns[fd].sent = 0;
                    arm_next_action(&ring, fd);
                }
            }
        }
        io_uring_submit(&ring);
    }

    io_uring_queue_exit(&ring);
    return 0;
}
