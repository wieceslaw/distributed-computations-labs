#include <stdio.h>
#include "common.h"
#include "ipc.h"
#include "pa1.h"

#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

FILE *pipes_log_fd;
FILE *event_log_fd;
local_id current_id;

typedef struct {
    int rfd;
    int wfd;
} Channel;

typedef struct {
    local_id id;
    local_id channels_size;
    Channel *channels;
} Process;

typedef struct {
    int data[2];
} pipe_desc;

enum {
    FIRST_CHILD_ID = 1,
    EMPTY_DESCRIPTOR = -1
};

static int read_blocking(const int fd, char *buffer, const size_t size) {
    if (size == 0) {
        return 0;
    }
    size_t ptr = 0;
    do {
        ssize_t tmp = read(fd, buffer + ptr, size - ptr);
        if (tmp <= 0) {
            return -1;
        }
        ptr += tmp;
    } while (ptr != size);
    return 0;
}

static int channel_read(const Channel *const cnl, Message *msg) {
    if (read_blocking(cnl->rfd, (char *) &msg->s_header, sizeof(MessageHeader)) != 0) {
        return -1;
    }
    if (read_blocking(cnl->rfd, msg->s_payload, msg->s_header.s_payload_len) != 0) {
        return -1;
    }
    return 0;
}

static int channel_write(const Channel *const cnl, const Message *const msg) {
    const size_t buffer_size = sizeof(MessageHeader) + msg->s_header.s_payload_len;
    const char *buffer = (char *) msg;
    size_t ptr = 0;
    do {
        ssize_t written = write(cnl->wfd, buffer + ptr, buffer_size - ptr);
        if (written == -1) {
            return -1;
        }
        ptr += written;
    } while (ptr != buffer_size);
    return 0;
}

int send(void *self, local_id dst, const Message *msg) {
    if (msg->s_header.s_magic != MESSAGE_MAGIC) {
        return -1;
    }

    Process *process = (Process *) self;
    if (process->id == dst) {
        return -1;
    }
    if (dst < 0 || dst >= process->channels_size) {
        return -1;
    }

    return channel_write(&process->channels[dst], msg);
}

int send_multicast(void *self, const Message *msg) {
    if (msg->s_header.s_magic != MESSAGE_MAGIC) {
        return -1;
    }

    Process *process = (Process *) self;
    for (local_id dst = 0; dst < process->channels_size; dst++) {
        if (process->id == dst) {
            continue;
        }
        Channel *channel = &process->channels[dst];
        if (channel_write(channel, msg) != 0) {
            return -1;
        }
    }
    return 0;
}

int receive(void *self, local_id from, Message *msg) {
    Process *process = (Process *) self;
    if (from == process->id || from < 0 || from >= process->channels_size) {
        return -1;
    }
    Channel *channel = &process->channels[from];

    if (channel_read(channel, msg) != 0) {
        return -1;
    }
    if (msg->s_header.s_magic != MESSAGE_MAGIC) {
        return -1;
    }
    return 0;
}

int receive_any(void *self, Message *msg) {
    return -1;
}

static void init_message(Message* message, MessageType type, char* buffer, size_t buffer_size) {
    *message = (Message) {
            .s_header = (MessageHeader) {
                    .s_magic = MESSAGE_MAGIC,
                    .s_payload_len = buffer_size,
                    .s_local_time = 0,
                    .s_type = type
            }
    };
    memcpy(message->s_payload, buffer, buffer_size);
}

static int child_handle(Process *self) {
    char str_buffer[1024];
    size_t str_size;

    // send started
    str_size = sprintf(str_buffer, log_started_fmt, self->id, getpid(), getppid());
    printf(log_started_fmt, self->id, getpid(), getppid());
    fprintf(event_log_fd, log_started_fmt, self->id, getpid(), getppid());
    fflush(event_log_fd);

    Message start_message;
    init_message(&start_message, STARTED, str_buffer, str_size);
    if (send_multicast(self, &start_message) != 0) {
        perror("Child send multicast");
        return -1;
    }

    // wait all started
    for (local_id i = FIRST_CHILD_ID; i < self->channels_size; i++) {
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

    // do work

    // send finish
    str_size = sprintf(str_buffer, log_done_fmt, self->id);
    printf(log_done_fmt, self->id);
    fprintf(event_log_fd, log_done_fmt, self->id);
    fflush(event_log_fd);

    Message finish_message;
    init_message(&finish_message, DONE, str_buffer, str_size);
    if (send_multicast(self, &finish_message) != 0) {
        perror("Child multicast send");
        return -1;
    }

    // wait all done
    for (local_id i = FIRST_CHILD_ID; i < self->channels_size; i++) {
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

static void parent_handle(Process *self) {
    // wait all started
    for (local_id i = FIRST_CHILD_ID; i < self->channels_size; i++) {
        if (i == self->id) {
            continue;
        }
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != STARTED) {
            perror("receive");
            return;
        }
    }

    // wait all done
    for (local_id i = FIRST_CHILD_ID; i < self->channels_size; i++) {
        if (i == self->id) {
            continue;
        }
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != DONE) {
            perror("receive");
            return;
        }
    }
}

static pipe_desc *matrix_get(pipe_desc *matrix, size_t matrix_size, size_t row, size_t col) {
    return &matrix[row * matrix_size + col];
}

static void matrix_set(pipe_desc *matrix, size_t matrix_size, size_t row, size_t col, pipe_desc value) {
    matrix[row * matrix_size + col] = value;
}

static pipe_desc *open_pipes(size_t n) {
    pipe_desc *matrix = malloc(sizeof(pipe_desc) * n * n);
    if (matrix == NULL) {
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) {
                // Skip self-to-self pipes
                matrix_set(matrix, n, i, j, (pipe_desc) {
                        .data[0] = EMPTY_DESCRIPTOR,
                        .data[1] = EMPTY_DESCRIPTOR
                });
                continue;
            }
            fprintf(pipes_log_fd, "Opened pipe [%d -> %d]\n", i, j);
            fflush(pipes_log_fd);
            if (pipe(matrix_get(matrix, n, i, j)->data) == EMPTY_DESCRIPTOR) {
                perror("Failed to open pipe");
                exit(EXIT_FAILURE);
            }
        }
    }
    return matrix;
}

static Channel *collect_channels(pipe_desc *pipes_matrix, size_t n, size_t proc_id) {
    Channel *channels = malloc(sizeof(Channel) * n);
    if (channels == NULL) {
        return NULL;
    }
    for (size_t id = 0; id < n; id++) {
        if (id == proc_id) {
            // Skip self-to-self channel
            channels[id] = (Channel) {
                    .rfd = EMPTY_DESCRIPTOR,
                    .wfd = EMPTY_DESCRIPTOR
            };
            continue;
        }
        pipe_desc *write_pipe = matrix_get(pipes_matrix, n, proc_id, id);
        pipe_desc *read_pipe = matrix_get(pipes_matrix, n, id, proc_id);
        channels[id] = (Channel) {
                .rfd = read_pipe->data[0],
                .wfd = write_pipe->data[1]
        };
        read_pipe->data[0] = EMPTY_DESCRIPTOR;
        write_pipe->data[1] = EMPTY_DESCRIPTOR;
    }
    for (size_t i = 0; i < n * n; i++) {
        for (size_t j = 0; j < 2; j++) {
            int fd = pipes_matrix[i].data[j];
            if (fd != EMPTY_DESCRIPTOR) {
                fprintf(pipes_log_fd, "Closed file descriptor [%zu -> %zu]\n", i, j);
                fflush(pipes_log_fd);
                if (close(fd) != 0) {
                    perror("Failed to close file descriptor");
                    exit(EXIT_FAILURE);
                }
            }
            pipes_matrix[i].data[j] = EMPTY_DESCRIPTOR;
        }
    }
    return channels;
}

static void free_channels(Channel *channels, local_id channels_size) {
    for (local_id i = 0; i < channels_size; i++) {
        Channel *channel = &channels[i];
        if (channel->rfd != EMPTY_DESCRIPTOR) {
            fprintf(pipes_log_fd, "Closed read file descriptor [%d: %d]\n", current_id, i);
            close(channel->rfd);
        }
        if (channel->wfd != EMPTY_DESCRIPTOR) {
            fprintf(pipes_log_fd, "Closed write file descriptor [%d: %d]\n", current_id, i);
            close(channel->wfd);
        }
    }
    free(channels);
}

static int run_child_process(local_id id, local_id n, pipe_desc *matrix) {
    pid_t pid = fork();
    if (pid == -1) {
        free(matrix);
        perror("fork");
        return -1;
    }
    if (pid > 0) {
        return 0;
    }

    // child code
    current_id = id;
    Channel *channels = collect_channels(matrix, n, id);
    free(matrix);
    if (channels == NULL) {
        perror("malloc");
        return -1;
    }

    Process cps = (Process) {
            .channels = channels,
            .channels_size = n,
            .id = id
    };

    if (child_handle(&cps) != 0) {
        perror("Child process error");
        return -1;
    }

    free_channels(channels, n);
    fclose(pipes_log_fd);
    fclose(event_log_fd);
    return -1;
}

static void wait_all_child_processes(void) {
    while (wait(NULL) > 0);
}

int run_processes(local_id n) {
    pipe_desc *matrix = open_pipes(n);
    if (matrix == NULL) {
        perror("malloc");
        return -1;
    }

    for (local_id i = 1; i < n; i++) {
        if (run_child_process(i, n, matrix) != 0) {
            return -1;
        }
    }

    // parent code
    current_id = PARENT_ID;
    Channel *channels = collect_channels(matrix, n, PARENT_ID);
    free(matrix);
    if (channels == NULL) {
        perror("malloc");
        return -1;
    }

    Process parent_process = (Process) {
            .id = PARENT_ID,
            .channels = channels,
            .channels_size = n,
    };

    parent_handle(&parent_process);
    free_channels(channels, n);
    wait_all_child_processes();
    return 0;
}

struct {
    local_id n;
} arguments;

int parse_arguments(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p':
                arguments.n = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Unknown option %c\n", opt);
                return -1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (parse_arguments(argc, argv) != 0) {
        return -1;
    }

    pipes_log_fd = fopen(pipes_log, "a");
    if (pipes_log_fd == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }
    event_log_fd = fopen(events_log, "a");
    if (event_log_fd == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    local_id processes_count = arguments.n + 1;
    run_processes(processes_count);

    fclose(pipes_log_fd);
    fclose(event_log_fd);
}
