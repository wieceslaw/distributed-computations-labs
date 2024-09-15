//
// Created by Vyacheslav Lebedev on 16.09.2024.
//

#ifndef PROGRAM_PROCESS_H
#define PROGRAM_PROCESS_H

#include "ipc.h"

typedef struct {
    int rfd;
    int wfd;
} Channel;

typedef struct {
    local_id id;
    local_id channels_size;
    Channel *channels;
} Process;

typedef int (*process_handler)(Process *);

int run_processes(local_id n, process_handler parent_handler, process_handler child_handler);

#endif //PROGRAM_PROCESS_H
