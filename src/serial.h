#pragma once

#include <linux/kvm.h>
#include <pthread.h>

#define COM1_PORT_BASE 0x03f8
#define COM1_PORT_SIZE 8
#define COM1_PORT_END (COM1_PORT_BASE + COM1_PORT_SIZE)

typedef struct serial_dev serial_dev_t;

struct serial_dev {
    void *priv;
    pthread_t main_tid, worker_tid;
    int infd; /* file descriptor for serial input */
};

void serial_console(serial_dev_t *s);
int serial_init(serial_dev_t *s);
void serial_handle(serial_dev_t *s, struct kvm_run *r);
void serial_exit(serial_dev_t *s);
