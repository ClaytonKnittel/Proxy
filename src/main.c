
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <conn_manager.h>


// default port to receive requests on
#define DEFAULT_REC_PORT  8080

// default port to send forwarded requests
#define DEFAULT_SEND_PORT 8081


#define OPTSTR "p:f:r:"


int usage(char * prog_name) {
    fprintf(stderr, "Usage: %s [-p <rec port>:<send port>]\n", prog_name);
    return -1;
}


int main(int argc, char *argv[]) {
    int c, rec_port, send_port;
    char *endptr, *ip_addr_str;
    int port, forward = 0;
    struct sockaddr_in addr;
    struct conn_manager cm;

    rec_port = DEFAULT_REC_PORT;
    send_port = DEFAULT_SEND_PORT;

#define NUM_OPT(str) \
    strtol((str), &endptr, 0);               \
    if (*(str) == '\0' || *endptr != '\0') { \
        return usage(argv[0]);                \
    }

    while ((c = getopt(argc, argv, OPTSTR)) != -1) {
        switch (c) {
            case 'p':
                endptr = strchr(optarg, ':');
                if (endptr != NULL) {
                    *endptr = '\0';
                    rec_port = NUM_OPT(optarg);
                    send_port = NUM_OPT(endptr + 1);
                }
                else {
                    return usage(argv[0]);
                }
                break;
            case 'f':
                // forward all requests to
                __builtin_memset(&addr, 0, sizeof(addr));
                endptr = strchr(optarg, ':');
                if (endptr != NULL) {
                    *endptr = '\0';
                    ip_addr_str = optarg;
                    port = NUM_OPT(endptr + 1);
                }
                else {
                    ip_addr_str = optarg;
                    port = 80;
                }
                if (inet_pton(AF_INET, ip_addr_str, &addr.sin_addr) <= 0) {
                    fprintf(stderr, "error parsing %s:%d as an IP address\n",
                            ip_addr_str, port);
                    return usage(argv[0]);
                }
                addr.sin_port = htons(port);
                addr.sin_family = AF_INET;
                forward = 1;
                break;
            case 'r':
                // receive from (used to bypass firewall by initiating
                // connection to proxy that will be forwarding to us)
                break;
        }
    }

    if (rec_port == send_port) {
        fprintf(stderr, "Receiving port %d and sending port %d must not be "
                "equal\n", rec_port, send_port);
        return -1;
    }

    conn_manager_init(&cm, rec_port, send_port);
    if (forward) {
        conn_manager_set_forwarding(&cm, addr);
    }

    conn_manager_start(&cm);

    conn_manager_close(&cm);

    return 0;
}

