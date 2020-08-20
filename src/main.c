
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <conn_manager.h>


// default port to receive requests on
#define DEFAULT_REC_PORT  8080


#define OPTSTR "hp:f:r:"


int usage(char * prog_name) {
    fprintf(stderr, "Usage: %s\n"
            "\t\t[-p <listening_port>]\n"
            "\t\t[-f <forward_ip_addr>[:<port>]]\n"
            "\t\t[-r <private_rec_ip_addr>[:<port>]]\n", prog_name);
    return -1;
}


int main(int argc, char *argv[]) {
    int c, rec_port;
    char *endptr, *ip_addr_str;
    int port, forward = 0, reverse = 0;
    struct sockaddr_in addr, r_addr;
    struct conn_manager cm;

    rec_port = DEFAULT_REC_PORT;

#define NUM_OPT(str) \
    strtol((str), &endptr, 0);               \
    if (*(str) == '\0' || *endptr != '\0') { \
        return usage(argv[0]);                \
    }

    while ((c = getopt(argc, argv, OPTSTR)) != -1) {
        switch (c) {
            case 'p':
                rec_port = NUM_OPT(optarg);
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
                __builtin_memset(&r_addr, 0, sizeof(r_addr));
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
                if (inet_pton(AF_INET, ip_addr_str, &r_addr.sin_addr) <= 0) {
                    fprintf(stderr, "error parsing %s:%d as an IP address\n",
                            ip_addr_str, port);
                    return usage(argv[0]);
                }
                r_addr.sin_port = htons(port);
                r_addr.sin_family = AF_INET;
                reverse = 1;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return -1;
        }
    }

    conn_manager_init(&cm, rec_port);

    if (forward) {
        conn_manager_set_forwarding(&cm, addr);
    }

    if (reverse) {
        int res = conn_manager_set_private_retrieval(&cm, r_addr);

        if (res != 0) {
            conn_manager_close(&cm);
            return res;
        }
    }

    conn_manager_start(&cm);

    conn_manager_close(&cm);

    return 0;
}

