
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <conn_manager.h>


// default port to receive requests on
#define DEFAULT_REC_PORT  8080

// default port to send forwarded requests
#define DEFAULT_SEND_PORT 8081


#define OPTSTR "p:"


int usage(char * prog_name) {
    fprintf(stderr, "Usage: %s [-p <rec port>:<send port>]\n", prog_name);
    return -1;
}


int main(int argc, char *argv[]) {
    int c, rec_port, send_port;
    char *endptr;
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
        }
    }

    if (rec_port == send_port) {
        fprintf(stderr, "Receiving port %d and sending port %d must not be "
                "equal\n", rec_port, send_port);
        return -1;
    }

    conn_manager_init(&cm, rec_port, send_port);

    conn_manager_start(&cm);

    conn_manager_close(&cm);

    return 0;
}

