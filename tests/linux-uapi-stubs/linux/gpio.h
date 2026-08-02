#ifndef _STUB_LINUX_GPIO_H
#define _STUB_LINUX_GPIO_H
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32;
typedef uint64_t __u64; typedef int32_t __s32;
#define GPIO_MAX_NAME_SIZE 32
#define GPIO_V2_LINES_MAX 64
#define GPIO_V2_LINE_NUM_ATTRS_MAX 10
struct gpiochip_info { char name[32]; char label[32]; __u32 lines; };
struct gpio_v2_line_attribute { __u32 id; __u32 padding; __u64 v; };
struct gpio_v2_line_config_attribute { struct gpio_v2_line_attribute attr; __u64 mask; };
struct gpio_v2_line_config { __u64 flags; __u32 num_attrs; __u32 padding[5];
    struct gpio_v2_line_config_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX]; };
struct gpio_v2_line_request { __u32 offsets[GPIO_V2_LINES_MAX]; char consumer[32];
    struct gpio_v2_line_config config; __u32 num_lines; __u32 event_buffer_size;
    __u32 padding[5]; __s32 fd; };
struct gpio_v2_line_values { __u64 bits; __u64 mask; };
struct gpio_v2_line_event { __u64 timestamp_ns; __u32 id; __u32 offset;
    __u32 seqno; __u32 line_seqno; __u32 padding[6]; };
#define GPIO_V2_LINE_FLAG_INPUT        (1ULL<<2)
#define GPIO_V2_LINE_FLAG_OUTPUT       (1ULL<<3)
#define GPIO_V2_LINE_FLAG_EDGE_RISING  (1ULL<<4)
#define GPIO_V2_LINE_FLAG_EDGE_FALLING (1ULL<<5)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_UP (1ULL<<8)
#define GPIO_V2_LINE_EVENT_RISING_EDGE  1
#define GPIO_V2_LINE_EVENT_FALLING_EDGE 2
#define GPIO_GET_CHIPINFO_IOCTL       _IOR(0xB4, 0x01, struct gpiochip_info)
#define GPIO_V2_GET_LINE_IOCTL        _IOWR(0xB4, 0x07, struct gpio_v2_line_request)
#define GPIO_V2_LINE_SET_VALUES_IOCTL _IOWR(0xB4, 0x0E, struct gpio_v2_line_values)
#endif
