#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "banking.h"
#include "common.h"
#include "process.h"
#include "pa2345.h"

FILE *pipes_log_fd;
FILE *event_log_fd;
local_id current_id;

void transfer(void *parent_data, local_id src, local_id dst, balance_t amount) {
    // student, please implement me
}

static int child_code(Process *self) {
    // started
    char str_buffer[1024];
    size_t str_size = sprintf(str_buffer, log_started_fmt, self->id, getpid(), getppid());
    printf(log_started_fmt, self->id, getpid(), getppid());
    fprintf(event_log_fd, log_started_fmt, self->id, getpid(), getppid());
    fflush(event_log_fd);

    Message start_message = (Message) {
            .s_header = (MessageHeader) {
                    .s_magic = MESSAGE_MAGIC,
                    .s_payload_len = str_size,
                    .s_local_time = 0,
                    .s_type = STARTED
            }
    };
    memcpy(start_message.s_payload, str_buffer, str_size);
    send_multicast(self, &start_message);

    // wait all started
    for (local_id i = 1; i < self->channels_size; i++) {
        if (i == self->id) {
            continue;
        }
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != STARTED) {
            perror("receive");
            return -1;
        }
    }
    printf(log_received_all_started_fmt, self->id);
    fprintf(event_log_fd, log_received_all_started_fmt, self->id);
    fflush(event_log_fd);

    // todo: work

    // finish
    str_size = sprintf(str_buffer, log_done_fmt, self->id);
    printf(log_done_fmt, self->id);
    fprintf(event_log_fd, log_done_fmt, self->id);
    fflush(event_log_fd);

    Message finish_message = (Message) {
            .s_header = (MessageHeader) {
                    .s_magic = MESSAGE_MAGIC,
                    .s_payload_len = str_size,
                    .s_local_time = 0,
                    .s_type = DONE
            }
    };
    memcpy(finish_message.s_payload, str_buffer, str_size);
    send_multicast(self, &finish_message);

    // wait all done
    for (local_id i = 1; i < self->channels_size; i++) {
        if (i == self->id) {
            continue;
        }
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != DONE) {
            perror("receive");
            return -1;
        }
    }
    printf(log_received_all_done_fmt, self->id);
    fprintf(event_log_fd, log_received_all_done_fmt, self->id);
    fflush(event_log_fd);
    return 0;
}

static int parent_code(Process *self) {
    // wait all started
    for (local_id i = 1; i < self->channels_size; i++) {
        if (i == self->id) {
            continue;
        }
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != STARTED) {
            perror("receive");
            return -1;
        }
    }

    // wait all done
    for (local_id i = 1; i < self->channels_size; i++) {
        if (i == self->id) {
            continue;
        }
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != DONE) {
            perror("receive");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    char *process_str = NULL;
    const char *short_options = "p:";
    const struct option long_options[] = {
            {"p", required_argument, NULL, 'p'},
            {NULL, 0,                NULL, 0}
    };
    int rez;
    int option_index;
    while ((rez = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
        switch (rez) {
            case 'p': {
                process_str = optarg;
                break;
            }
            default: {
                fprintf(stderr, "Unknown option: %s", optarg);
                return EXIT_FAILURE;
            }
        }
    }

    pipes_log_fd = fopen(pipes_log, "a");
    if (pipes_log_fd == NULL) {
        perror("fopen");
        return EXIT_FAILURE;
    }
    event_log_fd = fopen(events_log, "a");
    if (event_log_fd == NULL) {
        perror("fopen");
        return EXIT_FAILURE;
    }
    current_id = PARENT_ID;

    local_id child_processes_number = atoi(process_str); // number of child processes

    if (run_processes(child_processes_number + 1, parent_code, child_code) != 0) {
        fclose(pipes_log_fd);
        fclose(event_log_fd);
        return EXIT_FAILURE;
    }

    fclose(pipes_log_fd);
    fclose(event_log_fd);
    return 0;
}
