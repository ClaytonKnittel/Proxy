
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stddef.h>
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
#include <random.h>


#define MSG_BUF_SIZE 0x1000

#define STR2(x) #x
#define STR(X) STR2(X)


// GET, POST, etc.
#define MAX_TYPE 32
#define MAX_HOST 2048
#define MAX_PORT 8
// http/1.1
#define MAX_VERSION 32


#ifndef EPOLLEXCLUSIVE
#define EPOLLEXCLUSIVE (1 << 28)
#endif



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


// if this message is sent over a socket, it is expected to be requesting
// itself as a private forwarding connection
#define MAGIC_STRING "onaVcyTQLx93slLFZsacAdrFigoQ10Yp"

// set when this client is forwarding connections to this proxy privately,
// meaning each request should be responded with a new connection initialized
// on this machine
#define CLIENT_FORWARDER 0x40

// to be set when we have a private forward, and this client has already requested
// a connection with the forward
#define CLIENT_AWAITING_FORWARD 0x80



/*
 * write n_bytes from buffer onto c's write queue
 */
static int put_bytes(struct conn_manager * cm, struct client * c, int connfd,
        const char * buf, ssize_t n_bytes);


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

    // singly linked list of client objects that are waiting to be connected
    // to a private forwarded proxy (only used when being forwarded to private
    // proxy)
    struct client * next;

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
    c->next = NULL;
    c->client_buffer_offset = 0;
    c->client_buffer_len = 0;
    c->host_buffer_offset = 0;
    c->host_buffer_len = 0;
}

void close_client(struct conn_manager * cm, struct client * c) {
#ifdef __APPLE__
    struct kevent ev[4];
    int len = 0;
#endif

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

    int len = 0;
#endif

    int client_buffer_full = (c->client_buffer_len + c->client_buffer_offset == sizeof(c->client_buffer));
    int host_buffer_full = (c->host_buffer_len + c->host_buffer_offset == sizeof(c->host_buffer));

    // FIXME double check this l8tr
    int client_buffer_ready = !(c->flags & CLIENT_AWAITING_FORWARD) &&
        ((c->flags & CLIENT_READ_END_CLOSED) ||
         (c->client_buffer_len > 0));
    int host_buffer_ready = (c->flags & HOST_READ_END_CLOSED) ||
        (c->host_buffer_len > 0);

    /*
    if (c->clientfd != -1) {
        fprintf(stderr, "Rearm client%s%s\n", !host_buffer_full ? " read" : "",
                client_buffer_ready ? " write" : "");
    }
    if (c->dstfd != -1) {
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

        if (epoll_ctl(cm->qfd, EPOLL_CTL_MOD, c->clientfd, &event) < 0) {
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
            .data.u64 = ((uint64_t) c) | HOST_FD_BMASK
        };

        int mode;
        if (client_host_in_queue(c)) {
            mode = EPOLL_CTL_MOD;
        }
        else {
            mode = EPOLL_CTL_ADD;
            client_set_host_in_queue(c);
        }
        if (epoll_ctl(cm->qfd, mode, c->dstfd, &event) < 0) {
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


static void client_list_init(struct conn_manager * cm) {
    cm->list_front = NULL;
    cm->list_back = NULL;
}

static void client_list_append(struct conn_manager * cm, struct client * c) {
    if (cm->list_back == NULL) {
        cm->list_back = c;
        cm->list_front = c;
    }
    else {
        cm->list_back->next = c;
        cm->list_back = c;
    }
}

static struct client * client_list_pop(struct conn_manager * cm) {
    struct client * ret;
    if (cm->list_back == cm->list_front) {
        ret = cm->list_back;
        cm->list_back = NULL;
        cm->list_front = NULL;
    }
    else {
        ret = cm->list_front;
        cm->list_front = ret->next;
    }
    return ret;
}


static int has_private_forward(struct conn_manager * cm) {
    return (cm->flags & PRIVATE_FORWARDING) != 0;
}


static int is_requesting_private_forward(const char * buf, ssize_t buf_len) {
    return buf_len >= sizeof(MAGIC_STRING) - 1 &&
        __builtin_memcmp(buf, MAGIC_STRING, sizeof(MAGIC_STRING) - 1) == 0;
}


static int designate_private_forward(struct conn_manager * cm,
        struct client * c) {

    socklen_t len = sizeof(cm->pf_addr);

    __builtin_memset(&cm->pf_addr, 0, sizeof(cm->pf_addr));

    if (getpeername(c->clientfd, (struct sockaddr *) &cm->pf_addr, &len) < 0) {
        fprintf(stderr, "Unable to get peer name of private forward, reason: %s\n",
                strerror(errno));
        return -1;
    }
    printf("Peer's IP address is: %s\n", inet_ntoa(cm->pf_addr.sin_addr));
    printf("Peer's port is: %d\n", (int) ntohs(cm->pf_addr.sin_port));

    // let them know we have chosen them to be a private forward
    char affirmative[] = MAGIC_STRING;
    if (write(c->clientfd, affirmative, sizeof(affirmative) - 1) <
            sizeof(affirmative) - 1) {
        fprintf(stderr, "Unable to affirm private forward of role, aborting\n");
        return -1;
    }

    cm->flags |= PRIVATE_FORWARDING;
    cm->private_forward = c;
    client_list_init(cm);

    return 0;
}


static void request_dup_private_forward(struct conn_manager * cm,
        struct client * c) {
    const static char buf[] = MAGIC_STRING;
    // put bytes to private_forward->dstfd = -1, so it appears as though
    // they are being received by the nonexistent host and should be relayed
    // back
    put_bytes(cm, cm->private_forward, -1, buf, sizeof(buf) - 1);
    c->flags |= CLIENT_AWAITING_FORWARD;
    client_list_append(cm, c);
}

static int is_private_connection(struct conn_manager * cm, int connfd) {
    struct sockaddr_in addr;

    __builtin_memset(&addr, 0, sizeof(addr));

    socklen_t len = sizeof(addr);

    if (getpeername(connfd, (struct sockaddr *) &addr, &len) < 0) {
        fprintf(stderr, "Unable to check peer name, reason: %s\n", strerror(errno));
        return 0;
    }

    char ip_str[16] = { 0 };
    inet_ntop(AF_INET, &cm->pf_addr.sin_addr, ip_str, sizeof(ip_str));
    fprintf(stderr, "private forward on %s:%d\n", ip_str,
            ntohs(cm->pf_addr.sin_port));

    inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
    fprintf(stderr, "request on %s:%d\n", ip_str,
            ntohs(addr.sin_port));

    return __builtin_memcmp(&addr.sin_addr, &cm->pf_addr.sin_addr, sizeof(struct in_addr)) == 0;
}

static void delegate_private_connection(struct conn_manager * cm, int connfd) {
    struct client * next = client_list_pop(cm);

    if (next == NULL) {
        printf("nobody in queue, forward %d closing\n", connfd);
        close(connfd);
    }
    else {
        printf("Giving private forward (%d) to %d\n", connfd, next->clientfd);
        next->next = NULL;
        next->flags &= ~CLIENT_AWAITING_FORWARD;
        next->dstfd = connfd;
        client_rearm(cm, next);
    }
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



int conn_manager_init(struct conn_manager *cm, int rec_port) {
    int sock, q;
#ifdef __APPLE__
    struct kevent e;
#endif

    uint64_t t = time(NULL);
    seed_rand(t, t * t);

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

    cm->flags = 0;

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
            offsetof(struct client, clientfd)
    };
    if (epoll_ctl(cm->qfd, EPOLL_CTL_ADD, cm->listenfd, &listen_ev) < 0) {
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

    printf("set forwarding\n");
    cm->flags |= FORWARDING;
    __builtin_memcpy(&cm->forward_addr, &addr, sizeof(struct sockaddr_in));

    return 0;
}

static int conn_manager_dup_forward_fd(struct conn_manager * cm) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        fprintf(stderr, "Unable to initialize socket on AF_INET\n");
        return -1;
    }

    if (connect(sock, (struct sockaddr *) &cm->forward_addr,
                sizeof(cm->forward_addr)) != 0) {
        char ip_str[16] = { 0 };
        inet_ntop(AF_INET, &cm->forward_addr.sin_addr, ip_str, sizeof(ip_str));
        fprintf(stderr, "failed to initialize forwarding connection: "
                "%s:%d, reason %s\n", ip_str, ntohs(cm->forward_addr.sin_port),
                strerror(errno));
        return -1;
    }

    return sock;
}

static int conn_manager_has_forward(struct conn_manager * cm) {
    return (cm->flags & FORWARDING) != 0;
}



int conn_manager_set_private_retrieval(struct conn_manager * cm,
        struct sockaddr_in in) {

    // get password
    char * input = getpass("Enter proxy password: ");

    size_t len = strlen(input);
    char * passwd = (char *) malloc(len);
    memcpy(passwd, input, len * sizeof(char));

    input = getpass("Enter proxy password again: ");

    printf("\"%s\", \"%s\"\n", passwd, input);

    if (strncmp(passwd, input, len) != 0) {
        fprintf(stderr, "Passwords do not match!\n");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        fprintf(stderr, "Unable to create socket, reason: %s\n",
                strerror(errno));
        return -1;
    }

    if (connect(sock, (struct sockaddr *) &in, sizeof(in)) != 0) {
        char ip_str[16] = { 0 };
        inet_ntop(AF_INET, &in.sin_addr, ip_str, sizeof(ip_str));
        fprintf(stderr, "Failed to connect to forwarding proxy on %s:%d, "
                "reason: %s\n", ip_str, ntohs(in.sin_port), strerror(errno));
        close(sock);
        return -1;
    }

    // attempt to establish self as the private forwarding proxy
    const static char msg[] = MAGIC_STRING;
    write(sock, msg, sizeof(msg) - 1);

    // blocking wait for response
    char response[sizeof(MAGIC_STRING) - 1];
    if (read(sock, response, sizeof(response)) < sizeof(response)) {
        fprintf(stderr, "Could not establish private forwarding, aborting\n");
        close(sock);
        return -1;
    }

    printf("Successfully established private forwarding to this machine\n");


    // now designate socket as nonblocking and add to the event queue
    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        fprintf(stderr, "Unable to set connection to nonblocking, reason: %s\n",
                strerror(errno));
        close(sock);
        return -1;
    }

    cm->flags |= PRIVATE_RETRIEVAL;
    __builtin_memcpy(&cm->r_addr, &in, sizeof(struct sockaddr_in));
    cm->r_addr_fd = sock;

    struct client * c = (struct client *) malloc(sizeof(struct client));
    client_init(c);
    c->clientfd = sock;
    // will always stay -1
    c->dstfd = -1;
    // set forwarding flag in this client struct so we know to expect only
    // connection request messages
    c->flags |= CLIENT_FORWARDER;

    // and put it in the event queue
    client_rearm(cm, c);

    free(passwd);
    return 0;
}

/*
 * initialize another connection to the proxy hosted over the connection in c
 */
static int _initialize_connection_to(struct conn_manager * cm, struct client * c) {

    // flush the socket
    char buf[sizeof(MAGIC_STRING) - 1];
    if (read(c->clientfd, buf, sizeof(buf)) != sizeof(buf)) {
        fprintf(stderr, "bad init connection request\n");
        client_rearm(cm, c);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Failed to initialize socket, reason: %s\n",
                strerror(errno));
        client_rearm(cm, c);
        return -1;
    }

    if (connect(sock, (struct sockaddr *) &cm->r_addr, sizeof(cm->r_addr)) != 0) {
        fprintf(stderr, "Failed to etablish connection with reverse fowrard, "
                "reason: %s\n", strerror(errno));
        client_rearm(cm, c);
        return -1;
    }

    struct client * c2 = (struct client *) malloc(sizeof(struct client));
    client_init(c2);
    c2->clientfd = sock;
    // will initialize later
    c2->dstfd = -1;

    client_rearm(cm, c);
    client_rearm(cm, c2);
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


static int rearm_listenfd(struct conn_manager * cm) {
#ifdef __APPLE__
    struct kevent ev;
    EV_SET(&ev, cm->listenfd, EVFILT_READ,
            EV_ENABLE | EV_DISPATCH, 0, 0, NULL);

    if (kevent(cm->qfd, &ev, 1, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to rearm listenfd, reason: %s\n",
                strerror(errno));
        return -1;
    }
#endif
    return 0;
}


static int _accept_connection(struct conn_manager *cm) {
    struct sockaddr sa;
    socklen_t len = sizeof(sa);
    struct client *c;

    int connfd = accept(cm->listenfd, &sa, &len);

    if (connfd == -1) {
        fprintf(stderr, "Unable to accept client, reason: %s\n",
                strerror(errno));
        rearm_listenfd(cm);
        return -1;
    }

    if (fcntl(connfd, F_SETFL, O_NONBLOCK) == -1) {
        fprintf(stderr, "Unable to set connection to nonblocking, reason: %s\n",
                strerror(errno));
        close(connfd);
        rearm_listenfd(cm);
        return -1;
    }

    if (has_private_forward(cm) && is_private_connection(cm, connfd)) {
        delegate_private_connection(cm, connfd);
        rearm_listenfd(cm);
        return 0;
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

    if (kevent(cm->qfd, ev, 3, NULL, 0, NULL) == -1)
#elif __linux__
    struct epoll_event read_event = {
        .events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT,
        .data.ptr = c
    };

    if (epoll_ctl(cm->qfd, EPOLL_CTL_ADD, connfd, &read_event) < 0)
#endif
    {
        fprintf(stderr, "Unable to rearm listenfd, reason: %s\n",
                strerror(errno));
        close(connfd);
        free(c);
        return -1;
    }

    return 0;
}


static int _resolve_host(struct conn_manager * cm, struct client * c,
        const char * buf, int * req_type) {
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
            fprintf(stderr, "Unable to convert port \"%*s\" to unsigned long\n",
                    (int) (end - colon + 1), colon + 1);
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



static int register_bytes(struct conn_manager * cm, struct client * c, int connfd,
        ssize_t n_bytes) {

    int from_client = (connfd == c->clientfd);

    if (from_client) {

        // record data written to buffer
        c->host_buffer_len += n_bytes;

        //printf("read some data, now host buffer len is %llu\n", c->host_buffer_len);

        return client_rearm(cm, c);
    }
    else {

        c->client_buffer_len += n_bytes;

        //printf("read some data, now client buffer len is %llu\n", c->client_buffer_len);

        return client_rearm(cm, c);
    }
}

static int put_bytes(struct conn_manager * cm, struct client * c, int srcfd,
        const char * buf, ssize_t n_bytes) {

    char * dst_buf;
    ssize_t dst_buf_size;

    int from_client = (srcfd == c->clientfd);

    if (from_client) {
        dst_buf = c->host_buffer + c->host_buffer_offset + c->host_buffer_len;
        dst_buf_size = MSG_BUF_SIZE - (c->host_buffer_offset + c->host_buffer_len);
    }
    else {
        dst_buf = c->client_buffer + c->client_buffer_offset + c->client_buffer_len;
        dst_buf_size = MSG_BUF_SIZE - (c->client_buffer_offset + c->client_buffer_len);
    }

    if (dst_buf_size < n_bytes) {
        fprintf(stderr, "Could not write \"%*s\" to buffer, not enough space\n",
                (int) n_bytes, buf);
        return -1;
    }

    memcpy(dst_buf, buf, n_bytes);

    return register_bytes(cm, c, srcfd, n_bytes);
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

    if (from_client && c->dstfd == -1) {
        if (conn_manager_has_forward(cm)) {
            printf("plain forward\n");
            c->dstfd = conn_manager_dup_forward_fd(cm);
        }
        else if (has_private_forward(cm)) {
            printf("%d requests private forward\n", connfd);
            request_dup_private_forward(cm, c);
        }
        else if (is_requesting_private_forward(buf, buf_size)) {
            printf("designating\n");
            designate_private_forward(cm, c);
            return client_rearm(cm, c);
        }
        else {
            printf("regular\n");
            int req_type;
            int res = _resolve_host(cm, c, buf, &req_type);
            if (res == -1) {
                const static char bad_request[] = "HTTP/1.1 400 BAD REQUEST\r\n\r\n";
                return put_bytes(cm, c, -1, bad_request, sizeof(bad_request) - 1);
                /*
                write(c->clientfd, bad_request, sizeof(bad_request) - 1);

                // rearm clientfd for reading in event queue
                return client_rearm(cm, c);*/
            }
            c->dstfd = res;

            if (req_type == TYPE_CONNECT) {
                printf("was CONNECT request\n");
                // write OK response
                const static char ok_response[] = "HTTP/1.1 200 OK\r\n\r\n";
                return put_bytes(cm, c, c->dstfd, ok_response, sizeof(ok_response) - 1);
                /*
                if (write(c->clientfd, ok_response, sizeof(ok_response) - 1) !=
                        sizeof(ok_response) - 1) {
                    fprintf(stderr, "ERROR: Unable to write back OK response\n");
                }

                // rearm clientfd for reading in event queue
                return client_rearm(cm, c);*/
            }
        }
    }

    //printf("buf: \"%s\"\n", buf);
    return register_bytes(cm, c, connfd, n_read);
}

static int write_to(struct conn_manager * cm, struct client * c, int connfd) {
    char * buf;
    size_t buf_size;

    int to_client = connfd == c->clientfd;

    if (to_client) {
        buf = c->client_buffer + c->client_buffer_offset;
        buf_size = c->client_buffer_len;

        //printf("Writing %zu to client\n", buf_size);
        printf("To client:\n\033[0;37m%*s\033[0;39m\n", (int) buf_size,
                buf);
    }
    else {
        buf = c->host_buffer + c->host_buffer_offset;
        buf_size = c->host_buffer_len;

        //printf("Writing %zu to host\n", buf_size);
        printf("To host:\n\033[0;36m%*s\033[0;39m\n", (int) buf_size,
                buf);
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
                    kevent(cm->qfd, NULL, 0, &event, 1, NULL)
#elif __linux__
                    epoll_wait(cm->qfd, &event, 1, -1)
#endif
                ) == -1) {
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
            _accept_connection(cm);
        }
        else {
            if (
#ifdef __APPLE__
                    event.filter == EVFILT_WRITE
#elif __linux__
                    event.events & EPOLLOUT
#endif
                    ) {
                write_to(cm, c, fd);
            }
            else if (
#ifdef __APPLE__
                    event.filter == EVFILT_READ
#elif __linux__
                    event.events & EPOLLIN
#endif
                    ) {
                if (c->flags & CLIENT_FORWARDER) {
                    // this is the proxy forwarding to us, we should respond by
                    // establishing a new connection
                    printf("responding to connection request\n");
                    _initialize_connection_to(cm, c);
                }
                else {
                    // msg from existing connection
                    read_from(cm, c, fd);
                }
            }
            if (
#ifdef __APPLE__
                    event.flags & EV_EOF
#elif __linux__
                    event.events & EPOLLRDHUP
#endif
                    ) {
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

