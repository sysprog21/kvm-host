#include <fcntl.h>
#include <linux/serial_reg.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "err.h"
#include "serial.h"
#include "utils.h"
#include "vm.h"

#define SERIAL_IRQ 4
#define IO_READ8(data) *((uint8_t *) data)
#define IO_WRITE8(data, value) ((uint8_t *) data)[0] = value

#define IER_MASK 0x0f
#define MCR_MASK 0x1f
#define FCR_MASK 0xc9
#define DEFAULT_MSR UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS

#define SERIAL_EPOLL_EVENT 1
#define SERIAL_EPOLL_OUT 2
#define SERIAL_EPOLL_IN 3

#define SERIAL_TRIG_LEVELS \
    {                      \
        1, 16, 32, 56      \
    }

struct serial_dev_priv {
    /* Device registers */
    uint8_t dll;
    uint8_t dlm;
    uint8_t iir;
    uint8_t ier;
    uint8_t fcr;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
    bool thr_ipending;

    /* Buffers */
    struct fifo tx_buf;
    struct fifo rx_buf;
    uint8_t rxtrig;

    /* File descriptors */
    int infd;
    int outfd;
    int evfd;
    int epollfd;

    /* Worker */
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_mutex_t loopback_lock;
    bool stopped;

    /* Initialized */
    bool initialized;
};

static struct serial_dev_priv serial_dev_priv = {
    .iir = UART_IIR_NO_INT,
    .mcr = UART_MCR_OUT2,
    .lsr = UART_LSR_TEMT | UART_LSR_THRE,
    .msr = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS,
    .rxtrig = 1,
};

static void serial_signal(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint64_t buf = 1;
    if (write(priv->evfd, &buf, 8) < 0)
        throw_err("Failed to write to eventfd\n");
}

/* FIXME: This implementation is incomplete */
static void serial_update_irq(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint8_t iir = UART_IIR_NO_INT;
    uint8_t oiir = priv->iir;

    if ((priv->ier & UART_IER_RLSI) && (priv->lsr & UART_LSR_BRK_ERROR_BITS))
        iir = UART_IIR_RLSI;
    /* If enable receiver data interrupt and receiver data ready */
    else if ((priv->ier & UART_IER_RDI) && (priv->lsr & UART_LSR_DR))
        iir = fifo_level(&priv->rx_buf) < priv->rxtrig ? UART_IIR_RX_TIMEOUT
                                                       : UART_IIR_RDI;
    /* If enable transmiter data interrupt and transmiter empty */
    else if ((priv->ier & UART_IER_THRI) && (priv->lsr & UART_LSR_THRE) &&
             priv->thr_ipending)
        iir = UART_IIR_THRI;
    else if ((priv->msr & UART_MSR_ANY_DELTA) && (priv->ier & UART_IER_MSI))
        iir = UART_IIR_MSI;

    if (priv->fcr & UART_FCR_ENABLE_FIFO)
        iir |= 0xc0;

    __atomic_store_n(&priv->iir, iir, __ATOMIC_RELEASE);

    if ((oiir & UART_IIR_NO_INT) != (iir & UART_IIR_NO_INT)) {
        /* FIXME: the return error of vm_irq_line should be handled */
        vm_irq_line(container_of(s, vm_t, serial), SERIAL_IRQ,
                    !(iir & UART_IIR_NO_INT));
    }
}

/* Read from stdin and write the data to rx_buf */
static void serial_receive(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    struct iovec iov[2];
    int iovc;

    if (fifo_is_full(&priv->rx_buf))
        return;

    /* Transfer data from infd to rx_buf */
    int head = (priv->rx_buf.head - 1) % FIFO_LEN + 1;
    int tail = priv->rx_buf.tail % FIFO_LEN;
    uint8_t *buf = priv->rx_buf.data;
    int len;
    if (tail < head) {
        iov[0].iov_base = &buf[tail];
        iov[0].iov_len = head - tail;
        iovc = 1;
    } else {
        iov[0].iov_base = &buf[head];
        iov[0].iov_len = FIFO_LEN - tail;
        iov[1].iov_base = buf;
        iov[1].iov_len = head;
        iovc = 2;
    }
    len = readv(priv->infd, iov, iovc);
    if (len < 1)
        return;
    priv->rx_buf.tail += len;

    /* Update registers */
    pthread_mutex_lock(&priv->lock);
    if (!fifo_is_empty(&priv->rx_buf)) {
        priv->lsr |= UART_LSR_DR;
        serial_update_irq(s);
    }
    pthread_mutex_unlock(&priv->lock);
}

/* Read from tx_buf and write the data to stdout */
static void serial_transmit(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    struct iovec iov[2];
    int iovc;
    if (fifo_is_empty(&priv->tx_buf))
        return;

    /* Transfer data from tx_buf to outfd */
    int head = priv->tx_buf.head % FIFO_LEN;
    int tail = (priv->tx_buf.tail - 1) % FIFO_LEN + 1;
    uint8_t *buf = priv->tx_buf.data;
    int len;
    if (head < tail) {
        iov[0].iov_base = &buf[head];
        iov[0].iov_len = tail - head;
        iovc = 1;
    } else {
        iov[0].iov_base = &buf[head];
        iov[0].iov_len = FIFO_LEN - head;
        iov[1].iov_base = buf;
        iov[1].iov_len = tail;
        iovc = 2;
    }
    len = writev(priv->outfd, iov, iovc);
    if (len < 1)
        return;
    priv->tx_buf.head += len;

    /* Update registers */
    pthread_mutex_lock(&priv->lock);
    if (fifo_is_empty(&priv->tx_buf)) {
        uint8_t lsr = priv->lsr;
        lsr |= UART_LSR_THRE | UART_LSR_TEMT;
        __atomic_store_n(&priv->lsr, lsr, __ATOMIC_RELAXED);
        priv->thr_ipending = true;
        serial_update_irq(s);
    }
    pthread_mutex_unlock(&priv->lock);
}

static void serial_loopback(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint8_t tmp;
    /* Transfer data from tx_buf to rx_buf */
    while (!fifo_is_empty(&priv->tx_buf) && !fifo_is_full(&priv->rx_buf)) {
        if (!fifo_get(&priv->tx_buf, tmp))
            break;
        fifo_put(&priv->rx_buf, tmp);
    }
    if (!fifo_is_empty(&priv->tx_buf)) {
        /* rx_buf overrun */
        fifo_clear(&priv->tx_buf);
        priv->lsr |= UART_LSR_OE;
    }
    if (fifo_is_empty(&priv->tx_buf))
        priv->lsr |= UART_LSR_TEMT | UART_LSR_THRE;
    if (!fifo_is_empty(&priv->rx_buf))
        priv->lsr |= UART_LSR_DR;
    serial_update_irq(s);
}

static void *serial_thread(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    int epollfd = priv->epollfd;
    struct epoll_event event;
    uint64_t tmp;

    while (!__atomic_load_n(&priv->stopped, __ATOMIC_RELAXED)) {
        int ret = epoll_wait(epollfd, &event, 1, -1);
        if (ret < 1)
            continue;
        if (event.data.u32 == SERIAL_EPOLL_EVENT)
            if (read(priv->evfd, &tmp, 8) < 0)
                throw_err("Failed to read from eventfd\n");
        pthread_mutex_lock(&priv->loopback_lock);
        switch (event.data.u32) {
        case SERIAL_EPOLL_EVENT:
            serial_transmit(s);
            serial_receive(s);
        case SERIAL_EPOLL_OUT:
            serial_transmit(s);
        case SERIAL_EPOLL_IN:
            serial_receive(s);
            break;
        }
        pthread_mutex_unlock(&priv->loopback_lock);
    }
    return NULL;
}

static void serial_set_trigger_level(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    const uint8_t trigs[] = SERIAL_TRIG_LEVELS;
    if (!(priv->fcr & UART_FCR_ENABLE_FIFO)) {
        priv->rxtrig = 1;
        return;
    }
    int trigbits = UART_FCR_R_TRIG_BITS(priv->fcr);
    priv->rxtrig = trigs[trigbits];
}

static void serial_set_msr(serial_dev_t *s, uint8_t msr)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint8_t omsr = priv->mcr;
    msr &= UART_MSR_ANY_DELTA;
    msr |= (omsr & UART_MSR_ANY_DELTA);

    if ((msr & UART_MSR_DSR) != (omsr & UART_MSR_DSR))
        msr |= UART_MSR_DDSR;
    if ((msr & UART_MSR_CTS) != (omsr & UART_MSR_CTS))
        msr |= UART_MSR_DCTS;
    if (!(msr & UART_MSR_RI) && (omsr & UART_MSR_RI))
        msr |= UART_MSR_RI;
    if ((msr & UART_MSR_DCD) != (omsr & UART_MSR_DCD))
        msr |= UART_MSR_DDCD;

    pthread_mutex_lock(&priv->lock);
    priv->msr = msr;
    serial_update_irq(s);
    pthread_mutex_unlock(&priv->lock);
}

static void serial_in(serial_dev_t *s, uint16_t offset, void *data)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;

    switch (offset) {
    case UART_RX:
        if (priv->lcr & UART_LCR_DLAB) {
            IO_WRITE8(data, priv->dll);
            return;
        }

        uint8_t value;
        if (!fifo_get(&priv->rx_buf, value))
            return;
        IO_WRITE8(data, value);

        int level = fifo_level(&priv->rx_buf);
        if (level == 0 || level == priv->rxtrig - 1) {
            pthread_mutex_lock(&priv->lock);
            /* check again the fifo level before modify the register */
            level = fifo_level(&priv->rx_buf);
            if (level == 0)
                priv->lsr &= ~UART_LSR_DR;
            serial_update_irq(s);
            pthread_mutex_unlock(&priv->lock);
        }
        if (level == FIFO_LEN - 1)
            serial_signal(s);
        break;
    case UART_IER:
        if (priv->lcr & UART_LCR_DLAB)
            IO_WRITE8(data, priv->dlm);
        else
            IO_WRITE8(data, priv->ier);
        break;
    case UART_IIR:
        uint8_t iir = __atomic_load_n(&priv->iir, __ATOMIC_ACQUIRE);
        IO_WRITE8(data, iir);
        if ((iir & UART_IIR_ID) == UART_IIR_THRI) {
            /* The spec says that the THR empty interrupt should be clear after
             * IIR is read */
            pthread_mutex_lock(&priv->lock);
            priv->thr_ipending = false;
            serial_update_irq(s);
            pthread_mutex_unlock(&priv->lock);
        }
        break;
    case UART_LCR:
        IO_WRITE8(data, priv->lcr);
        break;
    case UART_MCR:
        IO_WRITE8(data, priv->mcr);
        break;
    case UART_LSR:
        uint8_t lsr = __atomic_load_n(&priv->lsr, __ATOMIC_RELAXED);
        IO_WRITE8(data, lsr);
        if (lsr & UART_LSR_BRK_ERROR_BITS) {
            /* clear error bits */
            pthread_mutex_lock(&priv->lock);
            priv->lsr &= ~UART_LSR_BRK_ERROR_BITS;
            serial_update_irq(s);
            pthread_mutex_unlock(&priv->lock);
        }
        break;
    case UART_MSR:
        IO_WRITE8(data, priv->msr);
        if (priv->msr & UART_MSR_ANY_DELTA) {
            /* clear delta bits */
            pthread_mutex_lock(&priv->lock);
            priv->msr &= ~UART_MSR_ANY_DELTA;
            serial_update_irq(s);
            pthread_mutex_unlock(&priv->lock);
        }
        break;
    case UART_SCR:
        IO_WRITE8(data, priv->scr);
        break;
    default:
        break;
    }
}

static void serial_out(serial_dev_t *s, uint16_t offset, void *data)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint8_t orig, value;
    switch (offset) {
    case UART_TX:
        if (priv->lcr & UART_LCR_DLAB) {
            priv->dll = IO_READ8(data);
            return;
        }
        if (!fifo_put(&priv->tx_buf, IO_READ8(data)))
            break;
        if (priv->mcr & UART_MCR_LOOP) {
            /* Loopback mode, drain tx */
            serial_loopback(s);
            break;
        }
        int level = fifo_level(&priv->tx_buf);
        if (level == 1) {
            pthread_mutex_lock(&priv->lock);
            /* check again the fifo level before modify any registers */
            level = fifo_level(&priv->tx_buf);
            if (level == 1) {
                priv->lsr &= ~(UART_LSR_TEMT | UART_LSR_THRE);
                serial_update_irq(s);
            }
            pthread_mutex_unlock(&priv->lock);
        }
        if (level == 1)
            serial_signal(s);
        break;
    case UART_IER:
        if (!(priv->lcr & UART_LCR_DLAB)) {
            pthread_mutex_lock(&priv->lock);
            priv->ier = IO_READ8(data) & IER_MASK;
            serial_update_irq(s);
            pthread_mutex_unlock(&priv->lock);
        } else {
            priv->dlm = IO_READ8(data);
        }
        break;
    case UART_FCR:
        value = IO_READ8(data);
        pthread_mutex_lock(&priv->lock);
        priv->fcr = value & FCR_MASK;
        if (value & UART_FCR_CLEAR_RCVR) {
            /* Clear receive buffer */
            fifo_clear(&priv->rx_buf);
            priv->lsr &= ~UART_LSR_DR;
        }
        if (value & UART_FCR_CLEAR_XMIT) {
            /* Clear transmit buffer */
            fifo_clear(&priv->tx_buf);
            priv->lsr |= UART_LSR_TEMT | UART_LSR_THRE;
            priv->thr_ipending = true;
        }
        serial_set_trigger_level(s);
        serial_update_irq(s);
        pthread_mutex_unlock(&priv->lock);
        break;
    case UART_LCR:
        priv->lcr = IO_READ8(data);
        break;
    case UART_MCR:
        orig = priv->mcr;
        value = IO_READ8(data);
        priv->mcr = value & MCR_MASK;
        if ((orig & UART_MCR_LOOP) && !(value & UART_MCR_LOOP)) {
            /* Leave the loopback mode */
            serial_set_msr(s, DEFAULT_MSR);
            pthread_mutex_unlock(&priv->loopback_lock);
        }
        if (!(orig & UART_MCR_LOOP) && (value & UART_MCR_LOOP)) {
            /* Enter the loopback mode */
            pthread_mutex_lock(&priv->loopback_lock);
            serial_loopback(s);
        }
        if (value & UART_MCR_LOOP) {
            /* In loopback mode, the output pins are wired to the input pins */
            uint8_t msr;
            msr = (value & 0xc0U) << 4;
            msr |= (value & 0x02U) << 3;
            msr |= (value & 0x01U) << 5;
            serial_set_msr(s, msr);
        }
        break;
    case UART_LSR: /* factory test */
    case UART_MSR: /* not used */
        break;
    case UART_SCR:
        priv->scr = IO_READ8(data);
        break;
    default:
        break;
    }
}


static void serial_handle_io(void *owner,
                             void *data,
                             uint8_t is_write,
                             uint64_t offset,
                             uint8_t size)
{
    serial_dev_t *s = (serial_dev_t *) owner;
    void (*serial_op)(serial_dev_t *, uint16_t, void *) =
        is_write ? serial_out : serial_in;

    serial_op(s, offset, data);
}

int serial_init(serial_dev_t *s, struct bus *bus)
{
    struct serial_dev_priv *priv = &serial_dev_priv;
    struct epoll_event event;

    if (priv->initialized)
        return throw_err("Serial device is already initialized\n");

    s->priv = &serial_dev_priv;

    /* Create necessory file descriptors */
    int evfd, infd, outfd, epollfd;
    evfd = infd = outfd = epollfd = -1;

    evfd = eventfd(0, EFD_NONBLOCK);
    if (evfd < 0) {
        throw_err("Failed to create eventfd\n");
        goto err;
    }
    infd = open("/dev/stdin", O_RDONLY | O_NONBLOCK);
    if (infd < 0) {
        throw_err("Failed to open stdin device\n");
        goto err;
    }
    outfd = open("/dev/stdout", O_WRONLY | O_NONBLOCK);
    if (outfd < 0) {
        throw_err("Failed to open stdout device\n");
        goto err;
    }
    epollfd = epoll_create1(0);
    if (epollfd < 0) {
        throw_err("Failed to create epoll file descriptor\n");
        goto err;
    }

    priv->evfd = evfd;
    priv->infd = infd;
    priv->outfd = outfd;
    priv->epollfd = epollfd;

    /* Setup epoll */
    event.events = EPOLLIN;
    event.data.u32 = SERIAL_EPOLL_EVENT;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, evfd, &event) < 0) {
        throw_err("Failed to add eventfd to epoll\n");
        goto err;
    }
    event.events = EPOLLIN | EPOLLET;
    event.data.u32 = SERIAL_EPOLL_IN;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, infd, &event) < 0) {
        throw_err("Failed to add stdin to epoll\n");
        goto err;
    }
    event.events = EPOLLOUT | EPOLLET;
    event.data.u32 = SERIAL_EPOLL_OUT;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, outfd, &event) < 0) {
        if (errno != EPERM) {
            throw_err("Failed to add stdout to epoll\n");
            goto err;
        }
    }

    /* Setup mutex */
    pthread_mutex_init(&priv->lock, NULL);
    pthread_mutex_init(&priv->loopback_lock, NULL);

    /* Create the thread*/
    priv->stopped = false;
    if (pthread_create(&priv->worker, NULL, (void *) serial_thread,
                       (void *) s) < 0) {
        throw_err("Failed to create worker thread\n");
        goto err;
    }

    dev_init(&s->dev, COM1_PORT_BASE, COM1_PORT_SIZE, s, serial_handle_io);
    bus_register_dev(bus, &s->dev);

    priv->initialized = true;

    return 0;

err:
    close(infd);
    close(outfd);
    close(evfd);
    close(epollfd);

    return -1;
}

void serial_exit(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    __atomic_store_n(&priv->stopped, true, __ATOMIC_RELAXED);
    pthread_join(priv->worker, NULL);

    /* If exiting in the loop back mode, unlock the lookback lock. */
    if (priv->mcr & UART_MCR_LOOP)
        pthread_mutex_unlock(&priv->loopback_lock);

    close(priv->evfd);
    close(priv->infd);
    close(priv->outfd);
    close(priv->epollfd);
}
