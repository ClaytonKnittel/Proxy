#ifndef _CONNECTION_H
#define _CONNECTION_H


/*
 *
 * Connection manager
 *
 */

#include <netinet/in.h>


struct conn_manager {
    int listenfd;

    int backlog;

    // kqueue fd
    int qfd;

    struct sockaddr_in in;
};


int conn_manager_init(struct conn_manager *cm, int rec_port, int send_port);


void conn_manager_close(struct conn_manager *cm);


void conn_manager_start(struct conn_manager *cm);


#endif /* _CONNECTION_H */
