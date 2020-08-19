#ifndef _CONNECTION_H
#define _CONNECTION_H


/*
 *
 * Connection manager
 *
 */

#include <netinet/in.h>

#define FORWARDING 0x1

struct conn_manager {
    int listenfd;

    int backlog;

    int flags;

    // kqueue fd
    int qfd;

    struct sockaddr_in in;
    struct sockaddr_in forward_addr;
};


int conn_manager_init(struct conn_manager * cm, int rec_port, int send_port);

/*
 * forwards all connections to this IP address (as another proxy)
 */
int conn_manager_set_forwarding(struct conn_manager * cm,
        struct sockaddr_in in);


void conn_manager_close(struct conn_manager *cm);


void conn_manager_start(struct conn_manager *cm);


#endif /* _CONNECTION_H */
