//
// Created by Vyacheslav Lebedev on 16.09.2024.
//

#ifndef PROGRAM_PROCESS_H
#define PROGRAM_PROCESS_H

#include "ipc.h"
#include "banking.h"

typedef struct {
    int rfd;
    int wfd;
} Channel;

typedef struct {
    local_id id;
    local_id channels_size;
    Channel *channels;
    balance_t balance;
    BalanceHistory history;
} Process;

typedef int (*process_handler)(Process *);

int run_processes(
        local_id n,
        process_handler parent_handler,
        process_handler child_handler,
        balance_t balances[MAX_PROCESS_ID + 1]
);

#endif //PROGRAM_PROCESS_H
