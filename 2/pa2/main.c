#include <stdio.h>
#include "common.h"
#include "ipc.h"
#include "pa2345.h"

#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>

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

typedef enum {
    READ_STATUS_OK = 0,
    READ_STATUS_EMPTY,
    READ_STATUS_CLOSED,
    READ_STATUS_ERROR
} ReadStatus;

static ReadStatus read_non_blocking(const int fd, char* buffer, const size_t buffer_size) {
    ssize_t bytes_read;
    bytes_read = read(fd, buffer, buffer_size);
    if (bytes_read == 0) {
        return READ_STATUS_CLOSED;
    } else if (bytes_read < 0) {
        if (errno == EAGAIN) {
            return READ_STATUS_EMPTY;
        } else {
            return READ_STATUS_ERROR;
        }
    }
    size_t ptr = bytes_read;
    while (bytes_read < buffer_size) {
        bytes_read = read(fd, buffer + ptr, buffer_size - ptr);
        if (bytes_read <= 0) {
            return READ_STATUS_ERROR;
        }
        ptr += bytes_read;
    }
    return READ_STATUS_OK;
}

static int read_blocking(const int fd, char *buffer, const size_t size) {
    ReadStatus status;
    do {
        status = read_non_blocking(fd, buffer, size);
    } while (status == READ_STATUS_EMPTY);
    return status == READ_STATUS_OK;
}

static int channel_read_blocking(const Channel *const cnl, Message *msg) {
    if (read_blocking(cnl->rfd, (char *) &msg->s_header, sizeof(MessageHeader)) != 0) {
        return -1;
    }
    if (read_blocking(cnl->rfd, msg->s_payload, msg->s_header.s_payload_len) != 0) {
        return -1;
    }
    return 0;
}

static ReadStatus channel_read_non_blocking(const Channel *const cnl, Message *msg) {
    ReadStatus status;
    status = read_non_blocking(cnl->rfd, (char *) &msg->s_header, sizeof(MessageHeader));
    if (status != READ_STATUS_OK) {
        return status;
    }
    if (read_blocking(cnl->rfd, msg->s_payload, msg->s_header.s_payload_len) != 0) {
        return READ_STATUS_ERROR;
    }
    return READ_STATUS_OK;
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

int receive(void *self, local_id from, Message *msg) {
    Process *process = (Process *) self;
    if (from == process->id || from >= process->channels_size || from == PARENT_ID) {
        return -1;
    }
    Channel *channel = &process->channels[from];

    if (channel_read_blocking(channel, msg) != 0) {
        return -1;
    }

    if (msg->s_header.s_magic != MESSAGE_MAGIC) {
        return -1;
    }
    return 0;
}

int receive_any(void *self, Message *msg) {
    Process *process = (Process *) self;
    ReadStatus status;
    bool empty_exists = false;
    do {
        empty_exists = false;
        for (int i = 0; i < process->channels_size; i++) {
            Channel* channel = &process->channels[i];
            status = channel_read_non_blocking(channel, msg);
            switch (status) {
                case READ_STATUS_OK: {
                    return 0;
                }
                case READ_STATUS_ERROR: {
                    return -1;
                }
                case READ_STATUS_EMPTY: {
                    empty_exists = true;
                    break;
                }
                case READ_STATUS_CLOSED: {
                    continue;
                }
            }
        }
    } while (empty_exists);
    return -1;
}

int send(void *self, local_id dst, const Message *msg) {
    if (msg->s_header.s_magic != MESSAGE_MAGIC) {
        return -1;
    }
    Process *process = (Process *) self;
    if (process->id == dst) {
        return -1;
    }
    if (process->id == PARENT_ID) {
        return -1;
    }
    if (dst >= process->channels_size) {
        return -1;
    }

    return channel_write(&process->channels[dst], msg);
}

int send_multicast(void *self, const Message *msg) {
    if (msg->s_header.s_magic != MESSAGE_MAGIC) {
        return -1;
    }
    Process *process = (Process *) self;
    if (process->id == PARENT_ID) {
        return -1;
    }

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

static void child_code_continue(Process *self) {
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
            return;
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
            return;
        }
    }
    printf(log_received_all_done_fmt, self->id);
    fprintf(event_log_fd, log_received_all_done_fmt, self->id);
    fflush(event_log_fd);
}

static void parent_code_continue(Process *self) {
    // wait all started
    for (local_id i = 1; i < self->channels_size; i++) {
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
    for (local_id i = 1; i < self->channels_size; i++) {
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
                matrix_set(matrix, n, i, j, (pipe_desc) {
                        .data[0] = -1,
                        .data[1] = -1
                });
                continue;
            }
            fprintf(pipes_log_fd, "Opened pipe [%d -> %d]\n", i, j);
            fflush(pipes_log_fd);
            if (pipe(matrix_get(matrix, n, i, j)->data) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
        }
    }
    return matrix;
}

static Channel *extract_channels(pipe_desc *pipes_matrix, size_t n, size_t x) {
    Channel *channels = malloc(sizeof(Channel) * n);
    if (channels == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (i == x) {
            channels[i] = (Channel) {
                    .rfd = -1,
                    .wfd = -1
            };
            continue;
        }
        pipe_desc *write_pipe = matrix_get(pipes_matrix, n, x, i);
        pipe_desc *read_pipe = matrix_get(pipes_matrix, n, i, x);
        channels[i] = (Channel) {
                .rfd = read_pipe->data[0],
                .wfd = write_pipe->data[1]
        };
        read_pipe->data[0] = -1;
        write_pipe->data[1] = -1;
    }
    for (size_t i = 0; i < n * n; i++) {
        for (size_t j = 0; j < 2; j++) {
            int fd = pipes_matrix[i].data[j];
            if (fd != -1) {
                fprintf(pipes_log_fd, "Closed fd [%zu -> %zu]\n", i, j);
                fflush(pipes_log_fd);
                close(fd);
            }
            pipes_matrix[i].data[j] = -1;
        }
    }

    for (size_t i = 0; i < n; i++) {
        int fd = channels[i].rfd;
        if (fd > 0) {
            if (fcntl(channels[i].rfd, F_SETFL, O_NONBLOCK) < 0) {
                exit(EXIT_FAILURE);
            }
        }
    }
    return channels;
}

static void free_channels(Channel *channels, local_id channels_size) {
    for (local_id i = 0; i < channels_size; i++) {
        Channel *channel = &channels[i];
        if (channel->rfd != -1) {
            fprintf(pipes_log_fd, "Closed rfd [%d: %d]\n", current_id, i);
            close(channel->rfd);
        }
        if (channel->wfd != -1) {
            fprintf(pipes_log_fd, "Closed wfd [%d: %d]\n", current_id, i);
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
    Channel *channels = extract_channels(matrix, n, id);
    free(matrix);
    if (channels == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    Process cps = (Process) {
            .channels = channels,
            .channels_size = n,
            .id = id
    };

    child_code_continue(&cps);

    free_channels(channels, n);
    fclose(pipes_log_fd);
    fclose(event_log_fd);
    exit(EXIT_SUCCESS);
}

static int run_processes(local_id n) {
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
    Channel *channels = extract_channels(matrix, n, 0);
    free(matrix);
    if (channels == NULL) {
        perror("malloc");
        return -1;
    }

    parent_code_continue(&(Process) {
            .id = 0,
            .channels = channels,
            .channels_size = n
    });

    free_channels(channels, n);

    while (wait(NULL) > 0);
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

    if (run_processes(child_processes_number + 1) != 0) {
        fclose(pipes_log_fd);
        fclose(event_log_fd);
        return EXIT_FAILURE;
    }

    fclose(pipes_log_fd);
    fclose(event_log_fd);
    return 0;
}
