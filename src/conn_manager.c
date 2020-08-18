
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <unistd.h>

#include <conn_manager.h>
#include <get_ip_addr.h>


#define MSG_BUF_SIZE 0x10000

#define STR2(x) #x
#define STR(X) STR2(X)


// GET, POST, etc.
#define MAX_TYPE 32
#define MAX_HOST 2048
#define MAX_PORT 8
// http/1.1
#define MAX_VERSION 32


// request types
#define TYPE_GET 0
#define TYPE_CONNECT 1


struct client {
    // file descriptor on which requests are received and responses are
    // forwarded
    int clientfd;
    // file descriptor on which requests are forwarded and responses are
    // received
    int dstfd;

    char current_host[MAX_HOST];

    // number of bytes to be received from client
    ssize_t n_bytes_client;
    // number of bytes to be received from dst
    ssize_t n_bytes_dst;
};


void client_init(struct client * c) {
    c->clientfd = -1;
    c->dstfd = -1;
    c->current_host[0] = '\0';
}

void close_client(struct conn_manager * cm, struct client * c) {
    struct kevent ev[2];
    int len = 1;
    if (c->dstfd != -1) {
        EV_SET(&ev[1], c->dstfd, EVFILT_READ,
                EV_DELETE, 0, 0, 0);
        len++;
    }
    EV_SET(&ev[0], c->clientfd, EVFILT_READ,
            EV_DELETE, 0, 0, 0);

    if (kevent(cm->qfd, ev, len, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to delete client, reason: %s\n",
                strerror(errno));
    }
    if (c->dstfd != -1) {
        close(c->dstfd);
    }
    close(c->clientfd);
    free(c);
}

void client_disconnect_host(struct conn_manager * cm, struct client * c) {
    struct kevent ev;
    EV_SET(&ev, c->dstfd, EVFILT_READ,
            EV_DELETE, 0, 0, 0);
    if (kevent(cm->qfd, &ev, 1, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to remove dstfd event, reason: %s\n",
                strerror(errno));
    }
    close(c->dstfd);
    c->dstfd = -1;
}

int client_rearm_host(struct conn_manager * cm, struct client * c);

int client_rearm_client(struct conn_manager * cm, struct client * c) {
    struct kevent ev;
    EV_SET(&ev, c->clientfd, EVFILT_READ,
            EV_ADD | EV_ENABLE | EV_DISPATCH, 0, 0, c);

    if (kevent(cm->qfd, &ev, 1, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to arm dstfd, reason: %s\n",
                strerror(errno));
        close(c->clientfd);
        c->clientfd = -1;
        return -1;
    }
    client_rearm_host(cm, c);
    return 0;
}

int client_rearm_host(struct conn_manager * cm, struct client * c) {
    struct kevent ev;
    EV_SET(&ev, c->dstfd, EVFILT_READ,
            EV_ADD | EV_ENABLE | EV_DISPATCH, 0, 0, c);

    if (kevent(cm->qfd, &ev, 1, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Unable to arm dstfd, reason: %s\n",
                strerror(errno));
        close(c->dstfd);
        c->dstfd = -1;
        return -1;
    }
    return 0;
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
    client_init(c);
    c->clientfd = connfd;
    // will initialize later
    c->dstfd = -1;

    printf("Connected to client of type %x\n",
            sa.sa_family);

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

    printf("Host: %s\nPort: %d\n", buf, port);

    if (strncmp(buf, c->current_host, MAX_HOST) == 0) {
        // we are already connected to this host!
        printf("Same host \"%s\"\n", c->current_host);

        if (colon != NULL) {
            *colon = ':';
        }
        *end = prior;
        return c->dstfd;
    }
    else {
        printf("Changing host from \"%s\"\n", c->current_host);
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
        fprintf(stderr, "succeeded on %s\n", ip_addr);
        return sock;
    }

    printf("No connections worked\n");

    close(sock);

    return -1;
}


static int read_from(struct conn_manager * cm, struct client * c, int connfd) {
    char buf[MSG_BUF_SIZE];

    ssize_t n_read = read(connfd, buf, sizeof(buf));

    if (n_read == -1) {
        fprintf(stderr, "Unable to read data from connfd %d, reason: %s\n",
                connfd, strerror(errno));
        if (connfd == c->clientfd) {
            fprintf(stderr, "was client\n");
            close_client(cm, c);
        }
        else {
            fprintf(stderr, "was host\n");
            client_disconnect_host(cm, c);
            client_rearm_client(cm, c);
        }
        return -1;
    }

    //printf("buf: \"%s\"\n", buf);

    if (connfd == c->clientfd) {
        if (c->dstfd == -1) {
            int req_type;
            int res = _resolve_host(cm, c, buf, &req_type);
            if (res == -1) {
                fprintf(stderr, "AHH BAD\n");
                const static char bad_request[] = "HTTP/1.1 400 BAD REQUEST\r\n\r\n";
                write(c->clientfd, bad_request, sizeof(bad_request) - 1);

                // rearm clientfd for reading in event queue
                return client_rearm_client(cm, c);
            }
            c->dstfd = res;

            if (req_type == TYPE_CONNECT) {
                // write OK response
                fprintf(stderr, "Send back OK, we were able to connect\n");
                const static char ok_response[] = "HTTP/1.1 200 OK\r\n\r\n";
                write(c->clientfd, ok_response, sizeof(ok_response) - 1);

                // rearm clientfd for reading in event queue
                return client_rearm_client(cm, c);
            }
        }

        // forward, unmodified, to destination
        write(c->dstfd, buf, n_read);

        // arm dstfd for reading in event queue
        //return client_rearm_host(cm, c);
        return client_rearm_client(cm, c);
    }
    else {
        printf("sending back response\n");

        // forward to client
        write(c->clientfd, buf, n_read);

        // rearm clientfd for reading in event queue
        return client_rearm_client(cm, c);
    }
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

