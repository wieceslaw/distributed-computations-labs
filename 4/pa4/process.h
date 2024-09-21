//
// Created by Vyacheslav Lebedev on 16.09.2024.
//

#ifndef PROGRAM_PROCESS_H
#define PROGRAM_PROCESS_H

#include "ipc.h"
#include "banking.h"

enum {
    QUEUE_EMPTY_VALUE = INT16_MAX,
    QUEUE_MAX_SIZE = MAX_PROCESS_ID + 1,
    INVALID_ID = -1,
    FIRST_CHILD_ID = 1
};


typedef struct {
    timestamp_t requests[QUEUE_MAX_SIZE];
    local_id size;
} Queue;

typedef struct {
    int rfd;
    int wfd;
} Channel;

typedef struct {
    local_id id;
    local_id channels_size;
    Channel *channels;
    Queue queue;
    local_id done_count;
} Process;

typedef int (*process_handler)(Process *);

int run_processes(
        local_id n,
        process_handler parent_handler,
        process_handler child_handler
);

bool queue_empty(Queue *q);

void queue_put(Queue *q, local_id id, timestamp_t t);

local_id queue_min(Queue *q);

timestamp_t queue_pop(Queue *q, local_id id);

int send_cs_multicast(Process* self, MessageType type);

int send_cs(Process* self, local_id dst, MessageType type);

#endif //PROGRAM_PROCESS_H
