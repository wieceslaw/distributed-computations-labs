//
// Created by Vyacheslav Lebedev on 16.09.2024.
//

#include <sys/wait.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>

#include "ipc.h"
#include "pa2345.h"
#include "process.h"

extern FILE *pipes_log_fd;
extern FILE *event_log_fd;
extern local_id current_id;

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
        sched_yield();
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

static int run_child_process(local_id id, local_id n, pipe_desc *matrix, process_handler child_handler) {
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

    child_handler(&cps);

    free_channels(channels, n);
    fclose(pipes_log_fd);
    fclose(event_log_fd);
    exit(EXIT_SUCCESS);
}

int run_processes(local_id n, process_handler parent_handler, process_handler child_handler) {
    pipe_desc *matrix = open_pipes(n);
    if (matrix == NULL) {
        perror("malloc");
        return -1;
    }

    for (local_id i = 1; i < n; i++) {
        if (run_child_process(i, n, matrix, child_handler) != 0) {
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

    Process parent_process = (Process) {
            .id = 0,
            .channels = channels,
            .channels_size = n
    };

    parent_handler(&parent_process);

    free_channels(channels, n);

    while (wait(NULL) > 0);
    return 0;
}
