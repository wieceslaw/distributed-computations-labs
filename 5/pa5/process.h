//
// Created by Vyacheslav Lebedev on 16.09.2024.
//

#ifndef PROGRAM_PROCESS_H
#define PROGRAM_PROCESS_H

#include "ipc.h"
#include "banking.h"

enum {
    DEFERRED_MAX_SIZE = MAX_PROCESS_ID + 1,
    EMPTY_REQUEST_TIME = MAX_T + 1,
    FIRST_CHILD_ID = 1
};

typedef struct {
    int rfd;
    int wfd;
} Channel;

typedef struct {
    local_id id;
    local_id channels_size;
    Channel *channels;
    bool deferred[DEFERRED_MAX_SIZE];
    local_id done_count;
    timestamp_t request_time;
} Process;

typedef int (*process_handler)(Process *);

int run_processes(local_id n, process_handler parent_handler, process_handler child_handler);

int send_cs_multicast(Process* self, MessageType type);

int send_cs(Process* self, local_id dst, MessageType type);

#endif //PROGRAM_PROCESS_H
