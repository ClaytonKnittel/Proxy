
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <unistd.h>

#ifdef __APPLE__
#define QUEUE_T "kqueue"
#include <sys/event.h>

#elif __linux__
#define QUEUE_T "epoll"
#include <sys/epoll.h>
#endif

#include <conn_manager.h>
#include <get_ip_addr.h>


#define MSG_BUF_SIZE 0x1000

#define STR2(x) #x
#define STR(X) STR2(X)


// GET, POST, etc.
#define MAX_TYPE 32
#define MAX_HOST 2048
#define MAX_PORT 8
// http/1.1
#define MAX_VERSION 32



#ifdef __linux__
// when on linux, mark lowest bit in pointer to user data object for read/write events on the host
// fd, so we can differentiate that from the client
#define HOST_FD_BMASK 0x1
#endif



// request types
#define TYPE_GET 0
#define TYPE_CONNECT 1


#define CLIENT_READ_END_CLOSED 0x1
#define HOST_READ_END_CLOSED 0x2

#define HOST_IN_QUEUE 0x10


struct client {
    // file descriptor on which requests are received and responses are
    // forwarded
    int clientfd;
    // file descriptor on which requests are forwarded and responses are
    // received
    int dstfd;

    int flags;

    // number of bytes to be received from client
    ssize_t n_bytes_client;
    // number of bytes to be received from dst
    ssize_t n_bytes_dst;

    char current_host[MAX_HOST];

    // client buffer goes to client, host buffer goes to host
    uint64_t client_buffer_offset;
    uint64_t client_buffer_len;
    uint64_t host_buffer_offset;
    uint64_t host_buffer_len;
    char __attribute__((aligned(128))) client_buffer[MSG_BUF_SIZE];
    char __attribute__((aligned(128))) host_buffer[MSG_BUF_SIZE];
};


void client_init(struct client * c) {
    c->clientfd = -1;
    c->dstfd = -1;
    c->flags = 0;
    c->current_host[0] = '\0';
    c->client_buffer_offset = 0;
    c->client_buffer_len = 0;
    c->host_buffer_offset = 0;
    c->host_buffer_len = 0;
}

void close_client(struct conn_manager * cm, struct client * c) {
    struct kevent ev[4];
    int len = 0;

    if (c->clientfd != -1) {
#ifdef __APPLE__
        EV_SET(&ev[0], c->clientfd, EVFILT_READ,
                EV_DELETE, 0, 0, c);
        EV_SET(&ev[1], c->clientfd, EVFILT_WRITE,
                EV_DELETE, 0, 0, c);
        len += 2;
#elif __linux__
        if (epoll_ctl(cm->qfd, EPOLL_CTL_DEL, c->clientfd, NULL) < 0) {
            fprintf(stderr, "Unable to remove client fd from epoll, reason: %s\n",
                    strerror(errno));
        }
#endif
    }

    if (c->dstfd != -1) {
#ifdef __APPLE__
        EV_SET(&ev[len + 0], c->dstfd, EVFILT_READ,
                EV_DELETE, 0, 0, c);
        EV_SET(&ev[len + 1], c->dstfd, EVFILT_WRITE,
                EV_DELETE, 0, 0, c);
        len += 2;
#elif __linux__
        if (epoll_ctl(cm->qfd, EPOLL_CTL_DEL, c->dstfd, NULL) < 0) {
            fprintf(stderr, "Unable to remove host fd from epoll, reason: %s\n",
                    strerror(errno));
        }
#endif
    }

#ifdef __APPLE__
    if (len > 0) {
        if (kevent(cm->qfd, ev, len, NULL, 0, NULL) == -1) {
            fprintf(stderr, "Unable to delete client on (%d, %d), reason: %s\n",
                    c->clientfd, c->dstfd, strerror(errno));
        }
    }
#endif
    if (c->clientfd != -1) {
        close(c->clientfd);
    }
    if (c->dstfd != -1) {
        close(c->dstfd);
    }
    free(c);
}

int client_host_in_queue(struct client * c) {
    return (c->flags & HOST_IN_QUEUE) != 0;
}

void client_set_host_in_queue(struct client * c) {
    c->flags |= HOST_IN_QUEUE;
}


int client_should_close(struct client * c) {
    return ((c->flags & CLIENT_READ_END_CLOSED) && c->host_buffer_len == 0) ||
           ((c->flags & HOST_READ_END_CLOSED) && c->client_buffer_len == 0);
}

void client_disconnect_client(struct conn_manager * cm, struct client * c) {
#ifdef __APPLE__
    struct kevent ev[2];
    EV_SET(&ev[0], c->clientfd, EVFILT_READ,
            EV_DELETE, 0, 0, c);
    EV_SET(&ev[1], c->clientfd, EVFILT_WRITE,
            EV_DELETE, 0, 0, c);
    if (kevent(cm->qfd, ev, 2, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to remove clientfd event, reason: %s\n",
                strerror(errno));
    }
#elif __linux__
    if (epoll_ctl(cm->qfd, EPOLL_CTL_DEL, c->clientfd, NULL) < 0) {
        fprintf(stderr, "Unable to remove client fd from epoll, reason: %s\n",
                strerror(errno));
    }
#endif
    close(c->clientfd);
    c->clientfd = -1;
}

void client_disconnect_host(struct conn_manager * cm, struct client * c) {
#ifdef __APPLE__
    struct kevent ev[2];
    EV_SET(&ev[0], c->dstfd, EVFILT_READ,
            EV_DELETE, 0, 0, c);
    EV_SET(&ev[1], c->dstfd, EVFILT_WRITE,
            EV_DELETE, 0, 0, c);
    if (kevent(cm->qfd, ev, 2, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to remove dstfd event, reason: %s\n",
                strerror(errno));
    }
#elif __linux__
    if (epoll_ctl(cm->qfd, EPOLL_CTL_DEL, c->dstfd, NULL) < 0) {
        fprintf(stderr, "Unable to remove host fd from epoll, reason: %s\n",
                strerror(errno));
    }
#endif
    fprintf(stderr, "\033[0;91mdisconnect host on %d the weird way\033[0;39m\n", c->dstfd);
    close(c->dstfd);
    c->dstfd = -1;
}

int client_rearm(struct conn_manager * cm, struct client * c) {
#ifdef __APPLE__
    struct kevent ev[4];

    int len = 0;//c->dstfd != -1 ? 4 : 2;
#endif

    int client_buffer_full = (c->client_buffer_len + c->client_buffer_offset == sizeof(c->client_buffer));
    int host_buffer_full = (c->host_buffer_len + c->host_buffer_offset == sizeof(c->host_buffer));

    int client_buffer_ready = (c->flags & HOST_READ_END_CLOSED) ||
        (c->client_buffer_len > 0);
    int host_buffer_ready = (c->flags & CLIENT_READ_END_CLOSED) ||
        (c->host_buffer_len > 0);

    /*fprintf(stderr, "Rearm client%s%s\n", !host_buffer_full ? " read" : "",
            client_buffer_ready ? " write" : "");
    if (len == 4) {
        fprintf(stderr, "Rearm host%s%s\n", !client_buffer_full ? " read" : "",
                host_buffer_ready ? " write" : "");
    }*/

#ifdef __APPLE__

    if (c->clientfd != -1) {
        EV_SET(&ev[0], c->clientfd, EVFILT_READ,
                EV_ADD | (host_buffer_full ? EV_DISABLE : EV_ENABLE) | EV_DISPATCH,
                NOTE_LOWAT, 0, c);
        EV_SET(&ev[1], c->clientfd, EVFILT_WRITE,
                EV_ADD | (client_buffer_ready ? EV_ENABLE : EV_DISABLE) | EV_DISPATCH,
                NOTE_LOWAT, 0, c);
        len += 2;
    }
    if (c->dstfd != -1) {
        EV_SET(&ev[len + 0], c->dstfd, EVFILT_READ,
                EV_ADD | (client_buffer_full ? EV_DISABLE : EV_ENABLE) | EV_DISPATCH,
                NOTE_LOWAT, 0, c);
        EV_SET(&ev[len + 1], c->dstfd, EVFILT_WRITE,
                EV_ADD | (host_buffer_ready ? EV_ENABLE : EV_DISABLE) | EV_DISPATCH,
                NOTE_LOWAT, 0, c);
        len += 2;
    }

    if (len > 0) {
        if (kevent(cm->qfd, ev, len, NULL, 0, NULL) == -1) {
            fprintf(stderr, "Unable to rearm client, reason: %s\n",
                    strerror(errno));
            close_client(cm, c);
            return -1;
        }
    }

#elif __linux__

    if (c->clientfd != -1) {
        int events = EPOLLRDHUP | EPOLLONESHOT;
        events |= !host_buffer_full ? EPOLLIN : 0;
        events |= client_buffer_ready ? EPOLLOUT : 0;
        struct epoll_event event = {
            .events = events,
            .data.ptr = c
        };

        if (epoll_ctl(cm->qfd, EPOLL_CTL_ADD, c->clientfd, &event) < 0) {
            fprintf(stderr, "Unable to add client read/write fd to epoll, reason: %s\n",
                    strerror(errno));
            close_client(cm, c);
            return -1;
        }
    }
    if (c->dstfd != -1) {
        int events = EPOLLRDHUP | EPOLLONESHOT;
        events |= !client_buffer_full ? EPOLLIN : 0;
        events |= host_buffer_ready ? EPOLLOUT : 0;
        struct epoll_event event = {
            .events = events,
            .data.ptr = ((uint64_t) c) | HOST_FD_BMASK
        };

        int mode;
        if (client_host_in_queue(c)) {
            mode = EPOLL_CTL_MOD;
        }
        else {
            mode = EPOLL_CTL_ADD;
            client_set_host_in_queue(c);
        }
        if (epoll_ctl(cm->qfd, mode, c->hostfd, &event) < 0) {
            fprintf(stderr, "Unable to add host read/write fd to epoll, reason: %s\n",
                    strerror(errno));
            close_client(cm, c);
            return -1;
        }
    }
#endif
    return 0;
}


void host_read_end_closed(struct conn_manager * cm, struct client * c) {
    fprintf(stderr, "\033[0;91mdisconnect host on %d\033[0;39m\n", c->dstfd);
    client_disconnect_host(cm, c);
    c->flags |= HOST_READ_END_CLOSED;
}

void client_read_end_closed(struct conn_manager * cm, struct client * c) {
    fprintf(stderr, "\033[0;31mdisconnect client on %d\033[0;39m\n", c->clientfd);
    client_disconnect_client(cm, c);
    c->flags |= CLIENT_READ_END_CLOSED;
}




static int _connect(struct conn_manager *cm) {
    
    if (bind(cm->listenfd, (struct sockaddr *) &cm->in,
                sizeof(struct sockaddr_in)) == -1) {
        fprintf(stderr, "Unable to bind socket to port %d, reason \"%s\"\n",
                ntohs(cm->in.sin_port), strerror(errno));
        return -1;
    }

    if (listen(cm->listenfd, cm->backlog) == -1) {
        fprintf(stderr, "Unable to listen, reason \"%s\"\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void _print_ipv4(struct sockaddr *s) {
    struct sockaddr_in *sin = (struct sockaddr_in *)s;
    char ip[INET_ADDRSTRLEN];
    uint16_t port;

    inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof (ip));
    port = htons (sin->sin_port);

    printf("host %s:%d\n", ip, port);
}



int conn_manager_init(struct conn_manager *cm, int rec_port, int send_port) {
    int sock, q;
    struct kevent e;

    __builtin_memset(&cm->in, 0, sizeof(cm->in));

    // allow any internet connections
    cm->in.sin_family = AF_INET;
    cm->in.sin_port = htons(rec_port);
    cm->in.sin_addr.s_addr = INADDR_ANY;

    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        fprintf(stderr, "Unable to initialize socket on AF_INET\n");
        return -1;
    }

    q =
#ifdef __APPLE__
        kqueue();
#elif __linux__
        epoll_create1(0);
#endif
    if (q < 0) {
        fprintf(stderr, "Unable to initialize " QUEUE_T "\n");
        close(sock);
        return -1;
    }

    cm->listenfd = sock;

    cm->backlog = 16;

    cm->qfd = q;

    if (_connect(cm) != 0) {
        conn_manager_close(cm);
        return -1;
    }


    // add listenfd to queue
#ifdef __APPLE__
    EV_SET(&e, cm->listenfd, EVFILT_READ, EV_ADD | EV_DISPATCH, 0, 0, NULL);
    if (kevent(cm->qfd, &e, 1, NULL, 0, NULL) == -1) {
#elif __linux__

    struct epoll_event listen_ev = {
        .events = EPOLLIN | EPOLLEXCLUSIVE,
        // safe as long as the rest of data is never accessed, which it
        // shouldn't be for the sockfd. Do this so each epolldata object
        // can be treated as a client object for the purposes of finding out
        // which file descriptor the event is associated with
        .data.ptr = ((char*) &cm->listenfd) -
            offsetof(epoll_data_ptr_t, struct client);
    };
    if (epoll_ctl(server->qfd, EPOLL_CTL_ADD, server->sockfd, &listen_ev) < 0) {
#endif
        fprintf(stderr, "Unable to add server listenfd to " QUEUE_T "\n");
        conn_manager_close(cm);
        return -1;
    }

    return 0;
}

/*
 * forwards all connections to this IP address (as another proxy)
 */
int conn_manager_set_forwarding(struct conn_manager * cm,
        struct sockaddr_in addr) {

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        fprintf(stderr, "Unable to initialize socket on AF_INET\n");
        return -1;
    }

    if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        char ip_str[16] = { 0 };
        inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
        fprintf(stderr, "failed to initialize forwarding connection: "
                "%s:%d, reason %s\n", ip_str, ntohs(addr.sin_port),
                strerror(errno));
        return -1;
    }

    return 0;
}


void conn_manager_close(struct conn_manager *cm) {
    if (close(cm->qfd) == -1) {
        fprintf(stderr, "Unable to close qfd\n");
    }
    if (close(cm->listenfd) == -1) {
        fprintf(stderr, "Unable to close listenfd\n");
    }
}


static int _accept_connection(struct conn_manager *cm) {
    struct sockaddr sa;
    socklen_t len = sizeof(sa);
    struct client *c;

    int connfd = accept(cm->listenfd, &sa, &len);

    if (connfd == -1) {
        fprintf(stderr, "Unable to accept client, reason: %s\n",
                strerror(errno));
        return -1;
    }

    if (fcntl(connfd, F_SETFL, O_NONBLOCK) == -1) {
        fprintf(stderr, "Unable to set connection to nonblocking, reason: %s\n",
                strerror(errno));
        close(connfd);
        return -1;
    }

    c = (struct client *) malloc(sizeof(struct client));
    client_init(c);
    c->clientfd = connfd;
    // will initialize later
    c->dstfd = -1;

    printf("\033[0;32mConnected to client on %d\033[0;39m\n", connfd);

#ifdef __APPLE__
    struct kevent ev[3];
    EV_SET(&ev[0], connfd, EVFILT_READ,
            EV_ADD | EV_DISPATCH, 0, 0, c);
    EV_SET(&ev[1], connfd, EVFILT_WRITE,
            EV_ADD | EV_DISABLE | EV_DISPATCH, 0, 0, c);
    EV_SET(&ev[2], cm->listenfd, EVFILT_READ,
            EV_ENABLE | EV_DISPATCH, 0, 0, NULL);

    if (kevent(cm->qfd, ev, 3, NULL, 0, NULL) == -1) {
#elif __linux__
    struct epoll_event read_event = {
        .events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT,
        .data.ptr = c
    };

    if (epoll_ctl(cm->qfd, EPOLL_CTL_ADD, connfd, &read_event) < 0) {
#endif
        fprintf(stderr, "Unable to rearm listenfd, reason: %s\n",
                strerror(errno));
        close(connfd);
        free(c);
        return -1;
    }

    return 0;
}


static int _resolve_host(struct conn_manager * cm, struct client * c, char * buf, int * req_type) {
    int port;

    struct hostent * h;

    char * end = strchr(buf, ' ');
    if (end == NULL) {
        return -1;
    }
    size_t len = ((uint64_t) end) - ((uint64_t) buf);
    if (len > MAX_TYPE - 1) {
        return -1;
    }
    const static char connect_str[] = "CONNECT";
    if (__builtin_memcmp(connect_str, buf, sizeof(connect_str) - 1) == 0) {
        *req_type = TYPE_CONNECT;
    }
    else {
        // assume GET for now
        *req_type = TYPE_GET;
    }

    buf += len + 1;
    end = strchr(buf, ' ');
    if (end == NULL) {
        return -1;
    }
    if (end > buf && *(end - 1) == '/') {
        end--;
    }
    char prior = *end;
    *end = '\0';

    char * prot;
    if ((prot = strstr(buf, "://")) != NULL) {
        buf = prot + 3;
    }

    char * colon = strchr(buf, ':');
    if (colon && colon + 1 != end && *(colon + 1) != '/') {
        // port follows
        port = (int) strtoul(colon + 1, &end, 10);
        if (*(colon + 1) == '\0' || *end != '\0') {
            fprintf(stderr, "Unable to convert port \"%*s\" to unsigned long\n", (int) (end - colon + 1), colon + 1);
            *end = prior;
            return -1;
        }

        *colon = '\0';
    }
    else {
        colon = NULL;
        port = 80;
    }

    //printf("Host: %s\nPort: %d\n", buf, port);

    if (strncmp(buf, c->current_host, MAX_HOST) == 0) {
        // we are already connected to this host!

        if (colon != NULL) {
            *colon = ':';
        }
        *end = prior;
        return c->dstfd;
    }
    else {
        //printf("Changing host from \"%s\"\n", c->current_host);
        // need to terminate current 
        if (c->dstfd != -1) {
            client_disconnect_host(cm, c);
        }
        strcpy(c->current_host, buf);
    }

    h = gethostbyname(buf);

    if (h == NULL) {
        fprintf(stderr, "Unable to resolve host \"%s\"\n", buf);

        if (colon != NULL) {
            *colon = ':';
        }
        *end = prior;
        return -1;
    }

    if (colon != NULL) {
        *colon = ':';
    }
    *end = prior;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    __builtin_memset(&addr, 0, sizeof(addr));

    addr.sin_family = h->h_addrtype;
    addr.sin_port = htons(port);

    for (int i = 0; h->h_addr_list[i] != NULL; i++) {
        char * ip_addr = h->h_addr_list[i];
        __builtin_memcpy(&addr.sin_addr, ip_addr, h->h_length);

        if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
            fprintf(stderr, "failed on %s\n", ip_addr);
            continue;
        }
        //fprintf(stderr, "succeeded on %s\n", ip_addr);

        printf("\033[0;92mConnected to host on %d\033[0;39m\n", sock);
        return sock;
    }

    printf("No connections worked\n");

    close(sock);

    return -1;
}


static int read_from(struct conn_manager * cm, struct client * c, int connfd) {
    char * buf;
    size_t buf_size;

    int from_client = (connfd == c->clientfd);

    if (from_client) {
        buf = c->host_buffer + c->host_buffer_offset + c->host_buffer_len;
        buf_size = MSG_BUF_SIZE - (c->host_buffer_offset + c->host_buffer_len);
    }
    else {
        buf = c->client_buffer + c->client_buffer_offset + c->client_buffer_len;
        buf_size = MSG_BUF_SIZE - (c->client_buffer_offset + c->client_buffer_len);
    }

    if (buf_size == 0) {
        fprintf(stderr, "ERROR: buffer of size 0 was in kqueue for reading\n");
        client_rearm(cm, c);
        return -1;
    }

    ssize_t n_read = read(connfd, buf, buf_size);

    //printf("read %zd from %s\n", n_read, from_client ? "client" : "host");

    if (n_read <= 0) {
        return client_rearm(cm, c);
    }

    //printf("buf: \"%s\"\n", buf);

    if (from_client) {
        if (c->dstfd == -1) {
            int req_type;
            int res = _resolve_host(cm, c, buf, &req_type);
            if (res == -1) {
                // TODO buffer this
                const static char bad_request[] = "HTTP/1.1 400 BAD REQUEST\r\n\r\n";
                write(c->clientfd, bad_request, sizeof(bad_request) - 1);

                // rearm clientfd for reading in event queue
                return client_rearm(cm, c);
            }
            c->dstfd = res;

            if (req_type == TYPE_CONNECT) {
                // write OK response
                // TODO buffer this
                const static char ok_response[] = "HTTP/1.1 200 OK\r\n\r\n";
                if (write(c->clientfd, ok_response, sizeof(ok_response) - 1) != sizeof(ok_response) - 1) {
                    fprintf(stderr, "ERROR: Unable to write back OK response\n");
                }

                // rearm clientfd for reading in event queue
                return client_rearm(cm, c);
            }
        }

        // record data written to buffer
        c->host_buffer_len += n_read;

        //printf("read some data, now host buffer len is %llu\n", c->host_buffer_len);

        // forward, unmodified, to destination
        //write(c->dstfd, buf, n_read);

        return client_rearm(cm, c);
    }
    else {

        c->client_buffer_len += n_read;

        //printf("read some data, now client buffer len is %llu\n", c->client_buffer_len);

        return client_rearm(cm, c);
    }
}

static int write_to(struct conn_manager * cm, struct client * c, int connfd) {
    char * buf;
    size_t buf_size;

    int to_client = connfd == c->clientfd;

    if (to_client) {
        buf = c->client_buffer + c->client_buffer_offset;
        buf_size = c->client_buffer_len;

        //printf("Writing %zu to client\n", buf_size);
    }
    else {
        buf = c->host_buffer + c->host_buffer_offset;
        buf_size = c->host_buffer_len;

        //printf("Writing %zu to host\n", buf_size);
    }

    ssize_t n_written = write(connfd, buf, buf_size);

    if (to_client) {
        c->client_buffer_len -= n_written;
        if (c->client_buffer_len == 0) {
            c->client_buffer_offset = 0;
        }
        else {
            c->client_buffer_offset += n_written;
        }
    }
    else {
        c->host_buffer_len -= n_written;
        if (c->host_buffer_len == 0) {
            c->host_buffer_offset = 0;
        }
        else {
            c->host_buffer_offset += n_written;
        }
    }

    return client_rearm(cm, c);

}


static void _print_proxy_info(struct conn_manager *cm) {
    uint16_t port;

    port = ntohs(cm->in.sin_port);

    printf("Proxy listening on port: %s:%d\n", get_ip_addr_str(), port);
}


void conn_manager_start(struct conn_manager *cm) {
#ifdef __APPLE__
    struct kevent event;
#elif __linux__
    struct epoll_event event;
#endif
    struct client * c;
    int ret;

    _print_proxy_info(cm);

    while (1) {
        if ((ret =
#ifdef __APPLE__
                    kevent(cm->qfd, NULL, 0, &event, 1, NULL))
#elif __linux__
                    epoll(cm->qfd, &event, 1, -1)
#endif
                == -1) {
            fprintf(stderr, QUEUE_T " call failed on fd %d, reason: %s\n",
                    cm->qfd, strerror(errno));
            break;
        }
#ifdef __APPLE__
        c = event.udata;
        int fd = event.ident;
#elif __linux__
        c = (struct client *) ((event.data.u64) & ~HOST_FD_BMASK);
        int hostfd = ((event.data.u64) & HOST_FD_BMASK);
        int fd = hostfd ? c->dstfd : c->clientfd;
#endif

        if (fd == cm->listenfd) {
            // incoming connection to new client
            ret = _accept_connection(cm);
        }
        else {
            if (event.filter == EVFILT_WRITE) {
                write_to(cm, c, fd);
            }
            else if (event.filter == EVFILT_READ) {
                // msg from existing connection
                read_from(cm, c, fd);
            }
            if (event.flags & EV_EOF) {
                if (c->clientfd == fd) {
                    client_read_end_closed(cm, c);
                }
                else {
                    host_read_end_closed(cm, c);
                }
            }

            if (client_should_close(c)) {
                fprintf(stderr, "\033[0;31mClosing client on (%d, %d)\033[0;39m\n", c->clientfd, c->dstfd);
                close_client(cm, c);
            }
        }
    }
}

