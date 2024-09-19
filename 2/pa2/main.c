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

typedef struct {
    bool valid;
    local_id n;
    balance_t s[MAX_PROCESS_ID + 1];
} Arguments;

Arguments parse_arguments(int argc, char *argv[]) {
    Arguments args = (Arguments) {.valid = true};

    int opt;
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p':
                args.n = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Unknown option %c\n", opt);
                args.valid = false;
                return args;
        }
    }

    int optlen = argc - optind;
    if (args.n != optlen) {
        fprintf(stderr, "Wrong number of options: should be %d \n", optlen);
        args.valid = false;
        return args;
    }


    for (int i = optind; i < argc; i++) {
        args.s[i - optind] = atoi(argv[i]);
    }

    return args;
}

void transfer(void *parent_data, local_id src, local_id dst, balance_t amount) {
    Process* process = parent_data;
    if (src < 0 || src >= process->channels_size || dst < 0 || dst >= process->channels_size) {
        fprintf(stderr, "Incorrect transfer ids: src: %d, dst: %d", src, dst);
        exit(EXIT_FAILURE);
    }

    TransferOrder order = (TransferOrder) {
        .s_amount = amount,
        .s_src = src,
        .s_dst = dst
    };

    Message message = (Message) {
        .s_header = (MessageHeader) {
            .s_magic = MESSAGE_MAGIC,
            .s_type = TRANSFER,
            .s_local_time = get_physical_time(),
            .s_payload_len = sizeof(TransferOrder)
        },
    };

    memcpy(&message.s_payload, &order, sizeof(TransferOrder));

    if (send(process, src, &message) != 0) {
        fprintf(stderr, "Failed to send message to id: %d", src);
        exit(EXIT_FAILURE);
    }

    if (receive(process, dst, &message) != 0) {
        fprintf(stderr, "Failed to receive message from id: %d", dst);
        exit(EXIT_FAILURE);
    }
    if (message.s_header.s_type != ACK) {
        fprintf(stderr, "Wrong message type: %d", message.s_header.s_type);
        exit(EXIT_FAILURE);
    }
}

static int child_start(Process *self) {
    char str_buffer[1024];
    timestamp_t time;
    size_t str_size;

    // send started
    time = get_physical_time();
    str_size = sprintf(str_buffer, log_started_fmt, time, self->id, getpid(), getppid(), self->balance);
    printf(log_started_fmt, time, self->id, getpid(), getppid(), self->balance);
    fprintf(event_log_fd, log_started_fmt, time, self->id, getpid(), getppid(), self->balance);
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
    for (local_id i = 1; i < self->channels_size; i++) {
        if (i == self->id) {
            continue;
        }
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != STARTED) {
            perror("Child receive");
            return -1;
        }
    }
    time = get_physical_time();
    printf(log_received_all_started_fmt, time, self->id);
    fprintf(event_log_fd, log_received_all_started_fmt, time, self->id);
    fflush(event_log_fd);
    return 0;
}

static int child_handle_transfer(Process *self, Message *message) {
    timestamp_t time = get_physical_time();
    TransferOrder *order = (TransferOrder *) message->s_payload;
    if (self->id == order->s_src) {
        printf(log_transfer_out_fmt, time, self->id, order->s_amount, order->s_dst);
        fprintf(event_log_fd, log_transfer_out_fmt, time, self->id, order->s_amount, order->s_dst);
        self->balance -= order->s_amount;

        BalanceState state = (BalanceState) {
                .s_balance = self->balance,
                .s_time = time,
                .s_balance_pending_in = 0
        };
        self->history.s_history[time] = state;
        self->history.s_history_len = time + 1;

        return send(self, order->s_dst, message);
    } else if (self->id == order->s_dst) {
        printf(log_transfer_in_fmt, time, self->id, order->s_amount, order->s_src);
        fprintf(event_log_fd, log_transfer_in_fmt, time, self->id, order->s_amount, order->s_src);
        self->balance += order->s_amount;
        Message ack_message = (Message) {
            .s_header = (MessageHeader) {
                .s_magic = MESSAGE_MAGIC,
                .s_type = ACK,
                .s_local_time = time,
                .s_payload_len = 0
            }
        };

        BalanceState state = (BalanceState) {
                .s_balance = self->balance,
                .s_time = time,
                .s_balance_pending_in = 0
        };
        self->history.s_history[time] = state;
        self->history.s_history_len = time + 1;

        return send(self, PARENT_ID, &ack_message);
    }
    return -1;
}

static int child_work(Process *self) {
    char str_buffer[1024];
    timestamp_t time;
    size_t str_size;

    // receive TRANSFER or STOP
    Message message = (Message) {.s_header.s_type = TRANSFER};
    while (message.s_header.s_type == TRANSFER) {
        if (receive_any(self, &message) != 0) {
            perror("Child receive_any");
            return -1;
        }
        if (message.s_header.s_type != TRANSFER && message.s_header.s_type != STOP) {
            perror("Unexpected type");
            return -1;
        }
        if (message.s_header.s_type == TRANSFER) {
            if (child_handle_transfer(self, &message) != 0) {
                perror("Child transfer");
                return -1;
            }
        }
    }

    // send done
    time = get_physical_time();
    str_size = sprintf(str_buffer, log_done_fmt, time, self->id, self->balance);
    printf(log_done_fmt, time, self->id, self->balance);
    fprintf(event_log_fd, log_done_fmt, time, self->id, self->balance);
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

    // receive TRANSFER and DONE
    for (local_id id = 1; id < self->channels_size; id++) {
        if (id == self->id) {
            continue;
        }
        Message msg = (Message) {.s_header.s_type = TRANSFER};
        while (msg.s_header.s_type == TRANSFER) {
            if (receive(self, id, &msg) != 0) {
                perror("Child receive: TRANSFER and DONE");
                return -1;
            }
            if (msg.s_header.s_type == TRANSFER) {
                child_handle_transfer(self, &msg);
            } else if (msg.s_header.s_type == DONE) {
                break;
            } else {
                fprintf(stderr, "Unexpected message type: %d", msg.s_header.s_type);
                return -1;
            }
        }
    }

    time = get_physical_time();
    printf(log_received_all_done_fmt, time, self->id);
    fprintf(event_log_fd, log_received_all_done_fmt, time, self->id);
    fflush(event_log_fd);

    // send history
    time = get_physical_time();
    BalanceHistory *history = &self->history;
    size_t history_header_size = sizeof(history->s_id) + sizeof(history->s_history_len);
    size_t history_payload_size = sizeof(history->s_history[0]) * history->s_history_len;
    size_t history_size = history_header_size + history_payload_size;
    Message history_message = (Message) {
            .s_header = (MessageHeader) {
                    .s_magic = MESSAGE_MAGIC,
                    .s_payload_len = history_size,
                    .s_type = BALANCE_HISTORY,
                    .s_local_time = time
            }
    };
    BalanceHistory* payload = (BalanceHistory *) history_message.s_payload;
    *payload = *history;
    if (send(self, PARENT_ID, &history_message) != 0) {
        perror("Child send: BALANCE_HISTORY");
        return -1;
    }

    return 0;
}

static int child_run(Process *self) {
    if (child_start(self) != 0) {
        return -1;
    }
    if (child_work(self) != 0) {
        return -1;
    }
    return 0;
}

static void continue_history(BalanceHistory* history, uint8_t max_t) {
    BalanceState *state_ptr = &history->s_history[0];
    for (uint8_t t = 1; t < max_t; t++) {
        BalanceState *state = &history->s_history[t];
        if (state->s_time == -1 || t >= history->s_history_len) {
            *state = *state_ptr;
            state->s_time = t;
        }
        state_ptr = state;
    }
    history->s_history_len = max_t;
}

static void continue_all_history(AllHistory* all_history) {
    uint8_t max_t = 0;
    for (size_t i = 0; i < all_history->s_history_len; i++) {
        BalanceHistory *history = &all_history->s_history[i];
        max_t = MAX(max_t, history->s_history_len);
    }
    for (size_t i = 0; i < all_history->s_history_len; i++) {
        BalanceHistory *history = &all_history->s_history[i];
        continue_history(history, max_t);
    }
}

static int parent_code(Process *self) {
    timestamp_t time;

    // wait all started
    for (local_id i = 1; i < self->channels_size; i++) {
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != STARTED) {
            perror("Parent receive");
            return -1;
        }
    }

    bank_robbery(self, self->channels_size - 1);

    // send stop
    time = get_physical_time();
    Message message = (Message) {
        .s_header = (MessageHeader) {
            .s_magic = MESSAGE_MAGIC,
            .s_type = STOP,
            .s_local_time = time,
            .s_payload_len = 0
        }
    };
    if (send_multicast(self, &message) != 0) {
        perror("Parent send multicast");
        return -1;
    }

    // wait all done
    for (local_id i = 1; i < self->channels_size; i++) {
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != DONE) {
            perror("Parent receive");
            return -1;
        }
    }

    // get all_history
    AllHistory all_history = (AllHistory) {.s_history_len = self->channels_size - 1};
    for (local_id i = 1; i < self->channels_size; i++) {
        Message msg;
        if (receive(self, i, &msg) != 0 || msg.s_header.s_type != BALANCE_HISTORY) {
            perror("Parent receive: BALANCE_HISTORY");
            return -1;
        }
        BalanceHistory* balance_history = (BalanceHistory *) msg.s_payload;
        all_history.s_history[i - 1] = *balance_history;
    }

    continue_all_history(&all_history);
    print_history(&all_history);
    return 0;
}

int main(int argc, char *argv[]) {
    Arguments args = parse_arguments(argc, argv);
    if (!args.valid) {
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
    if (run_processes(args.n + 1, parent_code, child_run, args.s) != 0) {
        fclose(pipes_log_fd);
        fclose(event_log_fd);
        return EXIT_FAILURE;
    }

    fclose(pipes_log_fd);
    fclose(event_log_fd);
    return 0;
}
