
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <unistd.h>

#include <conn_manager.h>
#include <get_ip_addr.h>


#define MSG_BUF_SIZE 1024

#define STR2(x) #x
#define STR(X) STR2(X)


// GET, POST, etc.
#define MAX_TYPE 32
#define MAX_URI 2048
// http/1.1
#define MAX_VERSION 32


struct client {
    // file descriptor on which requests are received and responses are
    // forwarded
    int clientfd;
    // file descriptor on which requests are forwarded and responses are
    // received
    int dstfd;

    // number of bytes to be received from client
    ssize_t n_bytes_client;
    // number of bytes to be received from dst
    ssize_t n_bytes_dst;
};



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

    printf ("host %s:%d\n", ip, port);
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

    q = kqueue();
    if (q < 0) {
        fprintf(stderr, "Unable to initialize kqueue\n");
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
    EV_SET(&e, cm->listenfd, EVFILT_READ, EV_ADD | EV_DISPATCH, 0, 0, NULL);
    if (kevent(cm->qfd, &e, 1, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to add server listenfd to queue\n");
        conn_manager_close(cm);
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
    c->clientfd = connfd;
    // will initialize later
    c->dstfd = -1;

    printf("Connected to client of type %x, len %d\n",
            sa.sa_family, len);

    struct kevent ev[2];
    EV_SET(&ev[0], connfd, EVFILT_READ,
            EV_ADD | EV_DISPATCH, 0, 0, c);
    EV_SET(&ev[1], cm->listenfd, EVFILT_READ,
            EV_ENABLE | EV_DISPATCH, 0, 0, NULL);

    if (kevent(cm->qfd, ev, 2, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to rearm listenfd, reason: %s\n",
                strerror(errno));
        close(connfd);
        return -1;
    }

    return 0;
}


static int _resolve_host(struct client * c, char * buf) {
    char type[MAX_TYPE];
    char uri[MAX_URI];
    char version[MAX_VERSION];

    if (sscanf(buf, "%" STR(MAX_TYPE) "s %" STR(MAX_URI) "s %"
                STR(MAX_VERSION) "s\r\n", type, uri, version)
            != 3) {
        fprintf(stderr, "Request %s could not be parsed\n", buf);
        return -1;
    }

    printf("uri: %s\n", uri);

    return 0;
}


static int read_from(struct conn_manager *cm, struct client * c, int connfd) {
    char buf[MSG_BUF_SIZE];

    ssize_t n_read = read(connfd, buf, sizeof(buf));

    if (n_read == -1) {
        fprintf(stderr, "Unable to read data from connfd %d, reason: %s\n",
                connfd, strerror(errno));
        return -1;
    }

    printf("%s\n", buf);

    if (connfd == c->clientfd) {
        if (c->dstfd == -1) {
            _resolve_host(c, buf);
        }
        // forward to destination
    }
    else {
        // forward to client
    }

    // rearm connfd for more reads
    return 0;
}


static void _print_proxy_info(struct conn_manager *cm) {
    uint16_t port;

    port = ntohs(cm->in.sin_port);

    printf("Proxy listening on port: %s:%d\n", get_ip_addr_str(), port);
}


void conn_manager_start(struct conn_manager *cm) {
    struct kevent event;
    struct client * c;
    int ret;

    _print_proxy_info(cm);

    while (1) {
        if ((ret = kevent(cm->qfd, NULL, 0, &event, 1, NULL)) == -1) {
            fprintf(stderr, "kqueue call failed on fd %d, reason: %s\n",
                    cm->qfd, strerror(errno));
            break;
        }
        int fd = event.ident;

        if (fd == cm->listenfd) {
            // incoming connection to new client
            ret = _accept_connection(cm);
        }
        else {
            // msg from existing connection
            c = event.udata;
            read_from(cm, c, fd);
        }
    }
}

