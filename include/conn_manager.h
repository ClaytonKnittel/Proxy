#ifndef _CONNECTION_H
#define _CONNECTION_H


/*
 *
 * Connection manager
 *
 */

#include <netinet/in.h>

// forwards to another public proxy
#define FORWARDING 0x1
// forwards to another private proxy
#define PRIVATE_FORWARDING 0x2

// is a private proxy retrieving from a public proxy
#define PRIVATE_RETRIEVAL 0x4

struct conn_manager {
    int listenfd;

    int backlog;

    int flags;

    // kqueue fd
    int qfd;

    struct sockaddr_in in;

    // address we have requested to receive data from
    struct sockaddr_in r_addr;
    int r_addr_fd;

    union {
        struct sockaddr_in forward_addr;
        struct {
            struct sockaddr_in pf_addr;
            struct client * private_forward;
            // list of clients waiting to be matched with a private connection
            struct client * list_front, * list_back;
            uint64_t magic_number;
        };
    };
};


int conn_manager_init(struct conn_manager * cm, int rec_port);

/*
 * forwards all connections to this IP address (as another proxy)
 */
int conn_manager_set_forwarding(struct conn_manager * cm,
        struct sockaddr_in in);

/*
 * sets up a private forwarding link to this machine from the supplied public
 * IP
 */
int conn_manager_set_private_retrieval(struct conn_manager * cm,
        struct sockaddr_in in);


void conn_manager_close(struct conn_manager *cm);


void conn_manager_start(struct conn_manager *cm);


#endif /* _CONNECTION_H */
