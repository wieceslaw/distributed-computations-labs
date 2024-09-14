#include <stdio.h>
#include "common.h"
#include "ipc.h"
#include "pa1.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    int rfd;
    int wfd;
} Channel;

typedef struct {
    local_id id;
    local_id channels_size;
    Channel *channels;
} ChildProcess;

int channel_read(const Channel* const cnl, Message* msg) {
    if (read(cnl->rfd, &msg->s_header, sizeof(MessageHeader)) <= 0) {
        return -1;
    }
    if (read(cnl->rfd, msg->s_payload, msg->s_header.s_payload_len) <= 0) {
        return -1;
    }
    return 0;
}

int channel_write(const Channel* const cnl, const Message* const msg) {
    // TODO: Implement
    return 0;
}

void foo(int child_processes_number) {

}

void child_code_continue(ChildProcess *self) {
    Message start_message = {
//            .s_header=...,
//            .s_payload=...
    };
    send_multicast(self, &start_message);
    // TODO: Implement
    // waits for all start messages

    // ...

    // send finished
}

int main(int argc, char *argv[]) {
    int x; // number of processes

    const int child_processes_number = x;
    const int bi_child_channels_number = child_processes_number * (child_processes_number - 1) / 2;
    const int uni_channel_size = sizeof(int) * 2;
    const int bi_channel_size = uni_channel_size * 2;

    const int channels_size = bi_child_channels_number;
    malloc(bi_channel_size);

    int pipefd[2];
    pid_t cpid;
    char buf;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <string>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    for (...) {
        cpid = fork();
        if (cpid == -1) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (cpid == 0) {
            child_code_continue();
            // child code
        }
    }

    if (cpid == 0) {    /* Child reads from pipe */
        close(pipefd[1]);          /* Close unused write end */

        while (read(pipefd[0], &buf, 1) > 0)
            write(STDOUT_FILENO, &buf, 1);

        write(STDOUT_FILENO, "\n", 1);
        close(pipefd[0]);
        _exit(EXIT_SUCCESS);

    } else {            /* Parent writes argv[1] to pipe */
        close(pipefd[0]);          /* Close unused read end */
        write(pipefd[1], argv[1], strlen(argv[1]));
        close(pipefd[1]);          /* Reader will see EOF */
        wait(NULL);                /* Wait for child */
        exit(EXIT_SUCCESS);
    }
}
