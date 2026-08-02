/*
 * Linux backend: MCP3208 over spidev, GPIO over the kernel character device.
 *
 * Two things here are deliberately different from the legacy code.
 *
 * SPI. The old external issued one ioctl per byte per channel -- eighteen
 * syscalls for a six channel scan. This issues one. The MCP3208 needs chip
 * select to fall between conversions, so the scan is built as N three-byte
 * transfers inside a single SPI_IOC_MESSAGE with cs_change set, which lets
 * the driver toggle CS in hardware without returning to userspace.
 *
 * GPIO. wiringPi was deprecated by its author in 2019 and its register-mmap
 * approach cannot work on a Pi 5, where GPIO lives behind the RP1
 * southbridge. This uses the /dev/gpiochip line-event interface directly --
 * no libgpiod link dependency, just the kernel uapi -- which additionally
 * gives every edge a CLOCK_MONOTONIC timestamp taken in the interrupt
 * handler. That timestamp is what makes sample-accurate trigger placement
 * possible; polling a pin from the audio thread never could be.
 *
 * The v2 uapi (linux 5.10+) is used when available, with a v1 fallback for
 * older kernels.
 */

#ifdef __linux__

#include "tt_hal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/gpio.h>
#include <linux/spi/spidev.h>

#ifdef GPIO_V2_GET_LINE_IOCTL
  #define TT_HAVE_GPIO_V2 1
#else
  #define TT_HAVE_GPIO_V2 0
#endif

#define TT_MAX_LINES (TT_MAX_TRIGGERS + TT_MAX_BUTTONS)

typedef struct {
    int      fd;          /* line request fd, or -1 */
    uint8_t  gpio;
    uint8_t  type;        /* tt_event_type */
    uint8_t  index;
} in_line;

typedef struct {
    const tt_board_desc *bd;

    int      spifd;
    uint32_t spi_speed;

    int      chipfd;
    in_line  in[TT_MAX_LINES];
    int      n_in;

    int      gate_fd[TT_MAX_GATES];

    /* Reusable transfer descriptors, so the scan path allocates nothing. */
    struct spi_ioc_transfer xfer[TT_MAX_CV];
    uint8_t  txbuf[TT_MAX_CV][3];
    uint8_t  rxbuf[TT_MAX_CV][3];
} lin_dev;

/* ------------------------------------------------------------------ */
/* SPI                                                                 */
/* ------------------------------------------------------------------ */

static int spi_setup(lin_dev *d, const char *dev, uint32_t speed,
                     char *err, size_t errlen)
{
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    int i;

    d->spifd = open(dev, O_RDWR);
    if (d->spifd < 0) {
        snprintf(err, errlen, "cannot open %s: %s. Is dtparam=spi=on set in "
                 "/boot/firmware/config.txt?", dev, strerror(errno));
        return -1;
    }
    if (ioctl(d->spifd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(d->spifd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(d->spifd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        snprintf(err, errlen, "cannot configure %s: %s", dev, strerror(errno));
        close(d->spifd);
        d->spifd = -1;
        return -1;
    }
    d->spi_speed = speed;

    /* One three-byte transfer per channel. cs_change makes the driver raise
     * CS between them, which the MCP3208 requires to start a new conversion,
     * while still keeping the whole scan inside one ioctl. */
    for (i = 0; i < TT_MAX_CV; i++) {
        memset(&d->xfer[i], 0, sizeof(d->xfer[i]));
        d->xfer[i].tx_buf        = (unsigned long)d->txbuf[i];
        d->xfer[i].rx_buf        = (unsigned long)d->rxbuf[i];
        d->xfer[i].len           = 3;
        d->xfer[i].speed_hz      = speed;
        d->xfer[i].bits_per_word = 8;
        d->xfer[i].cs_change     = 1;

        /* Single-ended conversion request for channel i. */
        d->txbuf[i][0] = (uint8_t)(0x06 | ((i >> 2) & 0x01));
        d->txbuf[i][1] = (uint8_t)(i << 6);
        d->txbuf[i][2] = 0x00;
    }
    return 0;
}

static int lin_adc_scan(void *self, uint16_t *raw, int nch)
{
    lin_dev *d = (lin_dev *)self;
    int n = d->bd->num_cv;
    int i;

    if (nch < n) n = nch;
    if (n <= 0) return -1;

    /* The last transfer must leave CS deasserted. */
    d->xfer[n - 1].cs_change = 0;

    if (ioctl(d->spifd, SPI_IOC_MESSAGE(n), d->xfer) < 0) return -1;

    d->xfer[n - 1].cs_change = 1;

    for (i = 0; i < n; i++)
        raw[i] = (uint16_t)(((d->rxbuf[i][1] & 0x0f) << 8) | d->rxbuf[i][2]);
    for (; i < nch; i++) raw[i] = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* GPIO inputs                                                         */
/* ------------------------------------------------------------------ */

/*
 * Trigger and button inputs are pulled up and pull low when active, so a
 * FALLING edge is "active" and RISING is "released". Normalising here means
 * no patch, opcode or chugin ever has to know about the inversion.
 */

static int request_input(lin_dev *d, int gpio, char *err, size_t errlen)
{
#if TT_HAVE_GPIO_V2
    struct gpio_v2_line_request req;

    memset(&req, 0, sizeof(req));
    req.offsets[0]   = (uint32_t)gpio;
    req.num_lines    = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT |
                       GPIO_V2_LINE_FLAG_EDGE_RISING |
                       GPIO_V2_LINE_FLAG_EDGE_FALLING |
                       GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    snprintf(req.consumer, sizeof(req.consumer), "tedium");

    if (ioctl(d->chipfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        snprintf(err, errlen, "cannot request GPIO %d for input: %s",
                 gpio, strerror(errno));
        return -1;
    }
    return req.fd;
#else
    struct gpioevent_request req;

    memset(&req, 0, sizeof(req));
    req.lineoffset  = (uint32_t)gpio;
    req.handleflags = GPIOHANDLE_REQUEST_INPUT;
    req.eventflags  = GPIOEVENT_REQUEST_BOTH_EDGES;
    snprintf(req.consumer_label, sizeof(req.consumer_label), "tedium");

    if (ioctl(d->chipfd, GPIO_GET_LINEEVENT_IOCTL, &req) < 0) {
        snprintf(err, errlen, "cannot request GPIO %d for input: %s "
                 "(kernel too old for pull-up configuration; set the pull-up "
                 "via gpio=%d=pu in /boot/firmware/config.txt)",
                 gpio, strerror(errno), gpio);
        return -1;
    }
    return req.fd;
#endif
}

static int request_output(lin_dev *d, int gpio, char *err, size_t errlen)
{
#if TT_HAVE_GPIO_V2
    struct gpio_v2_line_request req;

    memset(&req, 0, sizeof(req));
    req.offsets[0]   = (uint32_t)gpio;
    req.num_lines    = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    snprintf(req.consumer, sizeof(req.consumer), "tedium");

    if (ioctl(d->chipfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        snprintf(err, errlen, "cannot request GPIO %d for output: %s",
                 gpio, strerror(errno));
        return -1;
    }
    return req.fd;
#else
    struct gpiohandle_request req;

    memset(&req, 0, sizeof(req));
    req.lineoffsets[0] = (uint32_t)gpio;
    req.lines          = 1;
    req.flags          = GPIOHANDLE_REQUEST_OUTPUT;
    snprintf(req.consumer_label, sizeof(req.consumer_label), "tedium");

    if (ioctl(d->chipfd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        snprintf(err, errlen, "cannot request GPIO %d for output: %s",
                 gpio, strerror(errno));
        return -1;
    }
    return req.fd;
#endif
}

static int lin_gate_set(void *self, int index, int on)
{
    lin_dev *d = (lin_dev *)self;

    if (index < 0 || index >= d->bd->num_gates) return -1;
    if (d->gate_fd[index] < 0) return -1;

#if TT_HAVE_GPIO_V2
    {
        struct gpio_v2_line_values v;
        memset(&v, 0, sizeof(v));
        v.mask = 1;
        v.bits = on ? 1 : 0;
        return ioctl(d->gate_fd[index], GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0
               ? -1 : 0;
    }
#else
    {
        struct gpiohandle_data data;
        memset(&data, 0, sizeof(data));
        data.values[0] = on ? 1 : 0;
        return ioctl(d->gate_fd[index], GPIOHANDLE_SET_LINE_VALUES_IOCTL,
                     &data) < 0 ? -1 : 0;
    }
#endif
}

static int lin_gpio_poll(void *self, tt_event *buf, int max)
{
    lin_dev *d = (lin_dev *)self;
    struct pollfd pfd[TT_MAX_LINES];
    int i, n = 0;

    if (d->n_in == 0 || max <= 0) return 0;

    for (i = 0; i < d->n_in; i++) {
        pfd[i].fd      = d->in[i].fd;
        pfd[i].events  = POLLIN;
        pfd[i].revents = 0;
    }

    /* Zero timeout: the sampling thread already has its own cadence, and
     * blocking here would delay the ADC scan. */
    if (poll(pfd, (nfds_t)d->n_in, 0) <= 0) return 0;

    for (i = 0; i < d->n_in && n < max; i++) {
        if (!(pfd[i].revents & POLLIN)) continue;

        while (n < max) {
#if TT_HAVE_GPIO_V2
            struct gpio_v2_line_event ev;
            ssize_t r = read(d->in[i].fd, &ev, sizeof(ev));
            if (r != (ssize_t)sizeof(ev)) break;

            buf[n].time_ns = ev.timestamp_ns;
            buf[n].value   = (ev.id == GPIO_V2_LINE_EVENT_FALLING_EDGE) ? 1 : 0;
#else
            struct gpioevent_data ev;
            ssize_t r = read(d->in[i].fd, &ev, sizeof(ev));
            if (r != (ssize_t)sizeof(ev)) break;

            buf[n].time_ns = ev.timestamp;
            buf[n].value   = (ev.id == GPIOEVENT_EVENT_FALLING_EDGE) ? 1 : 0;
#endif
            buf[n].type  = d->in[i].type;
            buf[n].index = d->in[i].index;
            buf[n]._pad  = 0;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Pi 5 moved the main header GPIO to a different chip. Probe the usual
 * candidates and pick the one whose label looks like the header controller. */
static int open_gpiochip(const char *want, char *err, size_t errlen)
{
    static const char *candidates[] = {
        "/dev/gpiochip0", "/dev/gpiochip4", "/dev/gpiochip512", NULL
    };
    struct gpiochip_info info;
    int i, fd;

    if (want && *want) {
        fd = open(want, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            snprintf(err, errlen, "cannot open %s: %s", want, strerror(errno));
        return fd;
    }

    for (i = 0; candidates[i]; i++) {
        fd = open(candidates[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && info.lines >= 28)
            return fd;
        close(fd);
    }

    snprintf(err, errlen, "no usable /dev/gpiochip found. Try setting "
             "gpio_chip explicitly, and check that the user is in the "
             "'gpio' group.");
    return -1;
}

static void lin_close(void *self)
{
    lin_dev *d = (lin_dev *)self;
    int i;

    if (!d) return;
    for (i = 0; i < d->n_in; i++)
        if (d->in[i].fd >= 0) close(d->in[i].fd);
    for (i = 0; i < TT_MAX_GATES; i++)
        if (d->gate_fd[i] >= 0) close(d->gate_fd[i]);
    if (d->chipfd >= 0) close(d->chipfd);
    if (d->spifd >= 0) close(d->spifd);
    free(d);
}

static int lin_open(void **self, const tt_config *cfg, const tt_board_desc *bd,
                    char *err, size_t errlen)
{
    lin_dev *d;
    const char *spidev = cfg->spi_dev ? cfg->spi_dev : "/dev/spidev0.1";
    int i;

    d = (lin_dev *)calloc(1, sizeof(*d));
    if (!d) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    d->bd = bd;
    d->spifd = -1;
    d->chipfd = -1;
    for (i = 0; i < TT_MAX_GATES; i++) d->gate_fd[i] = -1;
    for (i = 0; i < TT_MAX_LINES; i++) d->in[i].fd = -1;

    if (spi_setup(d, spidev, (uint32_t)cfg->spi_speed_hz, err, errlen) != 0) {
        lin_close(d);
        return -1;
    }

    d->chipfd = open_gpiochip(cfg->gpio_chip, err, errlen);
    if (d->chipfd < 0) { lin_close(d); return -1; }

    for (i = 0; i < bd->num_triggers; i++) {
        int fd = request_input(d, bd->trigger_gpio[i], err, errlen);
        if (fd < 0) { lin_close(d); return -1; }
        d->in[d->n_in].fd    = fd;
        d->in[d->n_in].gpio  = bd->trigger_gpio[i];
        d->in[d->n_in].type  = TT_EV_TRIGGER;
        d->in[d->n_in].index = (uint8_t)i;
        d->n_in++;
    }
    for (i = 0; i < bd->num_buttons; i++) {
        int fd = request_input(d, bd->button_gpio[i], err, errlen);
        if (fd < 0) { lin_close(d); return -1; }
        d->in[d->n_in].fd    = fd;
        d->in[d->n_in].gpio  = bd->button_gpio[i];
        d->in[d->n_in].type  = TT_EV_BUTTON;
        d->in[d->n_in].index = (uint8_t)i;
        d->n_in++;
    }
    for (i = 0; i < bd->num_gates; i++) {
        d->gate_fd[i] = request_output(d, bd->gate_gpio[i], err, errlen);
        if (d->gate_fd[i] < 0) { lin_close(d); return -1; }
        lin_gate_set(d, i, 0);
    }

    *self = d;
    return 0;
}

static const tt_hal_ops k_linux_ops = {
    "linux",
    lin_open,
    lin_close,
    lin_adc_scan,
    lin_gpio_poll,
    lin_gate_set,
    NULL, NULL, NULL
};

const tt_hal_ops *tt_hal_linux(void)
{
    return &k_linux_ops;
}

#endif /* __linux__ */
