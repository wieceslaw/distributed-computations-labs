#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/param.h>

#include "banking.h"
#include "common.h"
#include "process.h"
#include "pa2345.h"

FILE *pipes_log_fd;
FILE *event_log_fd;
local_id current_id;
timestamp_t local_time = 0;

static struct {
    bool valid;
    local_id n;
    bool use_mutex;
} arguments = {
        .valid = true,
        .use_mutex = false,
        .n = 0
};

void parse_arguments(int argc, char *argv[]) {
    int opt;
    static struct option long_options[] = {
            {"mutexl", no_argument, 0, 'm' },
            {0, 0, 0, 0 }
    };

    while ((opt = getopt_long(argc, argv, "p:m:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p':
                arguments.n = atoi(optarg);
                break;
            case 'm':
                arguments.use_mutex = true;
                break;
            default:
                fprintf(stderr, "Usage: %s [-p N] [--mutex]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
}

static int child_work(Process *self) {
    int n = self->id * 5;
    char buffer[1024];
    for (int i = 1; i < n + 1; i++) {
        if (arguments.use_mutex) {
            if (request_cs(self) != 0) {
                perror("Child mutex request");
                return -1;
            }
        }

        sprintf(buffer, log_loop_operation_fmt, self->id, i, n);
        print(buffer);

        if (arguments.use_mutex) {
            if (release_cs(self) != 0) {
                perror("Child mutex release");
                return -1;
            }
        }
    }
    return 0;
}

static int child_run(Process *self) {
    char str_buffer[1024];
    timestamp_t time;
    size_t str_size;

    // send started
    local_time++;
    time = get_lamport_time();
    str_size = sprintf(str_buffer, log_started_fmt, time, self->id, getpid(), getppid(), 0);
    printf(log_started_fmt, time, self->id, getpid(), getppid(), 0);
    fprintf(event_log_fd, log_started_fmt, time, self->id, getpid(), getppid(), 0);
    fflush(event_log_fd);

    Message start_message = (Message) {
            .s_header = (MessageHeader) {
                    .s_magic = MESSAGE_MAGIC,
                    .s_payload_len = str_size,
                    .s_local_time = time,
                    .s_type = STARTED
            }
    };
    memcpy(start_message.s_payload, str_buffer, str_size);
    if (send_multicast(self, &start_message) != 0) {
        perror("Child multicast");
        return -1;
    }

    // wait all started
    for (local_id i = FIRST_CHILD_ID; i < self->channels_size; i++) {
        if (i == self->id) {
            continue;
        }
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != STARTED) {
            perror("Child receive");
            return -1;
        }
    }
    time = get_lamport_time();
    printf(log_received_all_started_fmt, time, self->id);
    fprintf(event_log_fd, log_received_all_started_fmt, time, self->id);
    fflush(event_log_fd);

    if (child_work(self) != 0) {
        perror("Child work");
        return -1;
    }

    // send done
    local_time++;
    time = get_lamport_time();
    str_size = sprintf(str_buffer, log_done_fmt, time, self->id, 0);
    printf(log_done_fmt, time, self->id, 0);
    fprintf(event_log_fd, log_done_fmt, time, self->id, 0);
    fflush(event_log_fd);

    Message finish_message = (Message) {
            .s_header = (MessageHeader) {
                    .s_magic = MESSAGE_MAGIC,
                    .s_payload_len = str_size,
                    .s_local_time = time,
                    .s_type = DONE
            }
    };
    memcpy(finish_message.s_payload, str_buffer, str_size);
    if (send_multicast(self, &finish_message) != 0) {
        perror("Child work multicast");
        return -1;
    }

    while (self->done_count != self->channels_size - 2) {
        Message msg;
        local_id id = receive_any(self, &msg);
        if (id == -1) {
            return -1;
        }
        if (arguments.use_mutex) {
            if (msg.s_header.s_type == CS_REQUEST) {
                queue_put(&self->queue, id, msg.s_header.s_local_time);
                local_time++;
                if (send_cs(self, id, CS_REPLY) != 0) {
                    return -1;
                }
            } else if (msg.s_header.s_type == CS_RELEASE) {
                queue_pop(&self->queue, id);
            } else if (msg.s_header.s_type == DONE) {
                self->done_count++;
            } else {
                return -1;
            }
        } else {
             if (msg.s_header.s_type == DONE) {
                 self->done_count++;
            } else {
                return -1;
            }
        }
    }

    time = get_lamport_time();
    printf(log_received_all_done_fmt, time, self->id);
    fprintf(event_log_fd, log_received_all_done_fmt, time, self->id);
    fflush(event_log_fd);
    return 0;
}

static int parent_run(Process *self) {
    // wait all started
    for (local_id i = FIRST_CHILD_ID; i < self->channels_size; i++) {
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != STARTED) {
            perror("Parent receive");
            return -1;
        }
    }

    // wait all done
    for (local_id i = FIRST_CHILD_ID; i < self->channels_size; i++) {
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != DONE) {
            perror("Parent receive");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    parse_arguments(argc, argv);
    if (!arguments.valid) {
        return EXIT_FAILURE;
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
    if (run_processes(arguments.n + 1, parent_run, child_run) != 0) {
        fclose(pipes_log_fd);
        fclose(event_log_fd);
        return EXIT_FAILURE;
    }

    fclose(pipes_log_fd);
    fclose(event_log_fd);
    return 0;
}
