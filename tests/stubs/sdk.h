#pragma once
// Minimal register/peripheral model for compiling the actual firmware on a host.
// It models data flow, not RP2040 electrical timing or USB bus arbitration.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define __packed __attribute__((packed))
#define __unused __attribute__((unused))
#define __time_critical_func(x) x
#define __compiler_memory_barrier() ((void)0)
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef unsigned int uint;
typedef volatile uint8_t io_rw_8;
#define GPIO_OUT true
#define GPIO_IN false
#define GPIO_FUNC_SIO 0
#define GPIO_FUNC_PIO0 1
#define GPIO_FUNC_PIO1 2
#define PICO_DEFAULT_LED_PIN 25
#define USBCTRL_IRQ 5
#define RESET_USBCTRL 0
#define clk_sys 0

typedef struct { volatile uint32_t in, out; } ep_regs;
typedef struct {
  uint8_t setup_packet[8];
  ep_regs ep_ctrl[15], ep_buf_ctrl[16];
  uint8_t ep0_buf_a[64], epx_data[4096];
} usb_dpram_t;
typedef struct {
  uint32_t muxing, pwr, main_ctrl, sie_ctrl, inte, dev_addr_ctrl;
  uint32_t buf_status, ints, sie_status, abort, abort_done;
} usb_hw_t;
extern usb_dpram_t fake_dpram;
extern usb_hw_t fake_usb, fake_usb_set, fake_usb_clear;
#define usb_dpram (&fake_dpram)
#define usb_hw (&fake_usb)
#define hw_set_alias_untyped(x) (&fake_usb_set)
#define hw_clear_alias_untyped(x) (&fake_usb_clear)

typedef struct {
  uint32_t txf[4], rxf[4], input_sync_bypass;
  uint32_t values, directions;
  uint tdi, tms, tck, tdo, divider;
  size_t bits, clocks, trailer;
  bool enabled;
} fake_pio;
typedef fake_pio *PIO;
extern fake_pio fake_pios[2];
#define pio0 (&fake_pios[0])
#define pio1 (&fake_pios[1])
typedef struct { bool read_inc, write_inc; uint dreq; } dma_channel_config;
#define DMA_SIZE_8 0
#define DREQ_PIO0_TX0 0
#define DREQ_PIO0_RX0 1
#define DREQ_PIO1_TX0 2
#define DREQ_PIO1_RX0 3

void gpio_init(uint pin);
void gpio_set_dir(uint pin, bool output);
void gpio_put(uint pin, bool value);
bool gpio_get(uint pin);
void gpio_set_function(uint pin, uint function);
void gpio_set_pulls(uint pin, bool up, bool down);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t value, uint32_t mask);
void pio_sm_set_pindirs_with_mask(PIO pio, uint sm, uint32_t dirs, uint32_t mask);
void pio_sm_set_clkdiv_int_frac(PIO pio, uint sm, uint16_t div, uint8_t frac);
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t count);
uint32_t pio_sm_get_blocking(PIO pio, uint sm);
void fake_pio_init(PIO pio, uint sm, uint16_t div, uint tck, uint tdi, uint tms, uint tdo);
int dma_claim_unused_channel(bool required);
dma_channel_config dma_channel_get_default_config(int chan);
void channel_config_set_transfer_data_size(dma_channel_config *c, int size);
void channel_config_set_read_increment(dma_channel_config *c, bool inc);
void channel_config_set_write_increment(dma_channel_config *c, bool inc);
void channel_config_set_dreq(dma_channel_config *c, uint dreq);
void dma_channel_configure(int chan, const dma_channel_config *c, void *dst, const void *src, size_t n, bool start);
void dma_channel_set_config(int chan, const dma_channel_config *c, bool start);
void dma_channel_transfer_to_buffer_now(int chan, void *buf, size_t n);
void dma_channel_transfer_from_buffer_now(int chan, const void *buf, size_t n);
bool dma_channel_is_busy(int chan);
static inline uint32_t clock_get_hz(int clk) { (void)clk; return 120000000; }
static inline void set_sys_clock_khz(uint clk, bool required) { (void)clk; (void)required; }
static inline void stdio_init_all(void) {}
static inline void irq_set_enabled(int irq, bool enabled) { (void)irq; (void)enabled; }
static inline void reset_unreset_block_num_wait_blocking(int n) { (void)n; }
static inline void tight_loop_contents(void) {}
static inline void busy_wait_at_least_cycles(int n) { (void)n; }
static inline int rp2040_chip_version(void) { return 2; }
static inline void pico_get_unique_board_id_string(char *buf, size_t n) {
  const char serial[] = "0123456789ABCDEF";
  assert(n >= sizeof(serial)); memcpy(buf, serial, sizeof(serial));
}
#define panic(...) abort()

#define USB_BUF_CTRL_AVAIL (1u << 10)
#define USB_BUF_CTRL_FULL (1u << 15)
#define USB_BUF_CTRL_DATA1_PID (1u << 13)
#define USB_BUF_CTRL_DATA0_PID 0
#define USB_BUF_CTRL_LEN_MASK 0x3ff
#define EP_CTRL_ENABLE_BITS (1u << 31)
#define EP_CTRL_INTERRUPT_PER_BUFFER (1u << 29)
#define EP_CTRL_BUFFER_TYPE_LSB 26
#define USB_USB_MUXING_TO_PHY_BITS 1
#define USB_USB_MUXING_SOFTCON_BITS 2
#define USB_USB_PWR_VBUS_DETECT_BITS 1
#define USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS 2
#define USB_MAIN_CTRL_CONTROLLER_EN_BITS 1
#define USB_SIE_CTRL_EP0_INT_1BUF_BITS 1
#define USB_INTS_BUFF_STATUS_BITS 1
#define USB_INTS_BUS_RESET_BITS 2
#define USB_INTS_SETUP_REQ_BITS 4
#define USB_SIE_CTRL_PULLUP_EN_BITS 2
#define USB_SIE_STATUS_SETUP_REC_BITS 1
#define USB_SIE_STATUS_BUS_RESET_BITS 2
#define USB_NUM_ENDPOINTS 16
