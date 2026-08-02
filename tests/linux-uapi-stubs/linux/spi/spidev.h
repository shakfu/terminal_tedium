#ifndef _STUB_SPIDEV_H
#define _STUB_SPIDEV_H
#include <stdint.h>
#include <sys/ioctl.h>
struct spi_ioc_transfer { uint64_t tx_buf; uint64_t rx_buf; uint32_t len;
    uint32_t speed_hz; uint16_t delay_usecs; uint8_t bits_per_word;
    uint8_t cs_change; uint8_t tx_nbits; uint8_t rx_nbits;
    uint8_t word_delay_usecs; uint8_t pad; };
#define SPI_MODE_0 0x00
#define SPI_IOC_MESSAGE(N) _IOW(107, 0, char[N*sizeof(struct spi_ioc_transfer)])
#define SPI_IOC_WR_MODE          _IOW(107, 1, uint8_t)
#define SPI_IOC_WR_BITS_PER_WORD _IOW(107, 3, uint8_t)
#define SPI_IOC_WR_MAX_SPEED_HZ  _IOW(107, 4, uint32_t)
#endif
