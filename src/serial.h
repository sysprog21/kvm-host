#pragma once

#include <linux/kvm.h>
#include <pthread.h>
#include "bus.h"

#define COM1_PORT_BASE 0x03f8
#define COM1_PORT_SIZE 8

typedef struct serial_dev serial_dev_t;

struct serial_dev {
    void *priv;
    pthread_t main_tid, worker_tid;
    int infd; /* file descriptor for serial input */
    struct dev dev;
    int irq_num;
};

void serial_console(serial_dev_t *s);
int serial_init(serial_dev_t *s, struct bus *bus);
void serial_exit(serial_dev_t *s);
