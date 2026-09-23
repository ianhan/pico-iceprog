/*
  pio_jtag.c

  Massively based on the great PICO-DirtyJTAG  
 */

#include <hardware/clocks.h>
#include "hardware/dma.h"
#include "pio_jtag.h"
#include "jtag.pio.h"

#include <stdio.h>
#include <string.h>
#include "config.h"

static void dma_init(pio_jtag_inst_t* jtag) {
  // Configure a channel to write a buffer to PIO
  // SM0's TX FIFO, paced by the data request signal from that peripheral.
  jtag->tx_dma_chan = dma_claim_unused_channel(true);
  jtag->tx_c = dma_channel_get_default_config(jtag->tx_dma_chan);
  channel_config_set_transfer_data_size(&jtag->tx_c, DMA_SIZE_8);
  channel_config_set_read_increment(&jtag->tx_c, true);
  channel_config_set_dreq(&jtag->tx_c, PIO_INDEX(jtag)?DREQ_PIO1_TX0:DREQ_PIO0_TX0);
  dma_channel_configure(jtag->tx_dma_chan,
			&jtag->tx_c,
			&jtag->pio->txf[0], // Write address (only need to set this once)
			NULL,               // Don't provide a read address yet
			0,                  // Don't provide the count yet
			false               // Don't start yet
			);
  // Configure a channel to read a buffer from PIO
  // SM0's RX FIFO, paced by the data request signal from that peripheral.
  jtag->rx_dma_chan = dma_claim_unused_channel(true);
  jtag->rx_c = dma_channel_get_default_config(jtag->rx_dma_chan);
  channel_config_set_transfer_data_size(&jtag->rx_c, DMA_SIZE_8);
  channel_config_set_write_increment(&jtag->rx_c, false);
  channel_config_set_read_increment(&jtag->rx_c, false);
  channel_config_set_dreq(&jtag->rx_c, PIO_INDEX(jtag)?DREQ_PIO1_RX0:DREQ_PIO0_RX0);
  dma_channel_configure(jtag->rx_dma_chan,
			&jtag->rx_c,
			NULL,               // Dont provide a write address yet
			&jtag->pio->rxf[0], // Read address (only need to set this once)
			0,                  // Don't provide the count yet
			false               // Don't start yet
			);
}

void pio_jtag_wait(pio_jtag_inst_t *jtag) {
  if(!jtag->pio_enabled) {
    // the pio must be enabled to actually shift data. This happens if e.g. data is being
    // sent in loopback mode as MPSSE is not setup then    
    return;
  }
  
  // wait now, if we've received further data via USB and the last PIO transfer is still in progress
  if(jtag->write_pending) {
    while (dma_channel_is_busy(jtag->rx_dma_chan))
      tight_loop_contents();
    
    jtag->write_pending = false;
  }
}

void pio_set_outputs(pio_jtag_inst_t *jtag, uint8_t bits) {
  pio_jtag_wait(jtag);

  uint32_t pin_mask = (1<<jtag->pin_tdi) | (1<<jtag->pin_tms) | (1<<jtag->pin_tck);
  uint32_t pin_values =
    ((bits & PIO_JTAG_BIT_TDI)?(1<<jtag->pin_tdi):0) |
    ((bits & PIO_JTAG_BIT_TMS)?(1<<jtag->pin_tms):0) |
    ((bits & PIO_JTAG_BIT_TCK)?(1<<jtag->pin_tck):0);
  
  pio_sm_set_enabled(jtag->pio, jtag->sm, false);
  pio_sm_set_pins_with_mask(jtag->pio, jtag->sm, pin_values, pin_mask);
  pio_sm_set_enabled(jtag->pio, jtag->sm, jtag->pio_enabled);
}

static void __time_critical_func(pio_jtag_blocking)(pio_jtag_inst_t *jtag, const uint8_t *bsrc, uint8_t *bdst, size_t len) {
  // iceprog sends command bytes and payload in separate USB writes.
  if(!len) return;
  size_t byte_length = (len+7)/8;  // bytes needed for len bits
  size_t last_shift = ((byte_length << 3) - len);
  size_t tx_remain = (len+3)/4, rx_remain = last_shift ? byte_length : byte_length+1;
  static uint8_t x; // scratch local to receive data if no receive buffer is provided

  /* don't do anything if pio is not enabled at all */
  if(!jtag->pio_enabled) {
    printf("Warning: Ignoring non JTAG/SPI shift\n");
    return;
  }
  
  pio_jtag_wait(jtag);

  pio_sm_put_blocking(jtag->pio, jtag->sm, len-1);
  
  channel_config_set_write_increment(&jtag->rx_c, bdst?true:false);
  channel_config_set_read_increment(&jtag->tx_c, true);
  dma_channel_set_config(jtag->rx_dma_chan, &jtag->rx_c, false);
  dma_channel_set_config(jtag->tx_dma_chan, &jtag->tx_c, false);
  
  // The PIO program always pushes a final word. After a whole-byte transfer
  // this is an empty trailer, not another byte belonging to the caller.
  dma_channel_transfer_to_buffer_now(jtag->rx_dma_chan, (void*)(bdst?bdst:&x), bdst?byte_length:rx_remain);

  if(bdst)  {
    dma_channel_transfer_from_buffer_now(jtag->tx_dma_chan, (void*)bsrc, tx_remain);
    while (dma_channel_is_busy(jtag->rx_dma_chan)) tight_loop_contents();
    if(!last_shift) (void)pio_sm_get_blocking(jtag->pio, jtag->sm);
  } else {
    // use buffer, so USB can already refill the current buffer 
    assert(tx_remain <= sizeof(jtag->write_buffer));
    memcpy(jtag->write_buffer, bsrc, tx_remain);
    dma_channel_transfer_from_buffer_now(jtag->tx_dma_chan, jtag->write_buffer, tx_remain);  
    jtag->write_pending = true;
  }
    
  // stop the compiler hoisting a non volatile buffer access above the DMA completion.
  __compiler_memory_barrier();
}

static uint16_t jtag_get_clk_divider(int freq_khz) {
  uint clk_sys_freq_khz_4 = clock_get_hz(clk_sys) / (4*1000);
  uint16_t divider = clk_sys_freq_khz_4 / freq_khz;
  if (divider < 2) divider =  2; //max reliable freq 
  
  printf("Clk %d kHz, divider for %d kHz = %d\n", 4*clk_sys_freq_khz_4, freq_khz, divider);
  printf("  -> real freq = %d kHz\n", clk_sys_freq_khz_4 / divider);
  return divider;
}

void pio_jtag_set_clk_freq(pio_jtag_inst_t *jtag, uint freq_khz) {
  pio_jtag_wait(jtag);
  pio_sm_set_clkdiv_int_frac(jtag->pio, jtag->sm, jtag_get_clk_divider(freq_khz), 0);
}

// tdi/tms bits "expanded" for interleaved tdi/tms tranmission
static uint16_t interleave_table[256];
static uint8_t reverse_table[256];

void pio_jtag_enable(pio_jtag_inst_t* jtag, bool enable) {
  // Also apply direction changes when PIO remains enabled (SPI leaves D3
  // as an input, while JTAG uses it as the TMS output).
  pio_jtag_wait(jtag);
  pio_sm_set_enabled(jtag->pio, jtag->sm, false);

  if(enable) {
    uint32_t outputs = (1u << jtag->pin_tck) | (1u << jtag->pin_tdi);
    if(jtag->gpio_dir & 0x08) outputs |= 1u << jtag->pin_tms;
    pio_sm_set_pindirs_with_mask(jtag->pio, jtag->sm, outputs,
        (1u << jtag->pin_tck) | (1u << jtag->pin_tdi) |
        (1u << jtag->pin_tms) | (1u << jtag->pin_tdo));
    int gpio_func_pio = PIO_INDEX(jtag)?GPIO_FUNC_PIO1:GPIO_FUNC_PIO0;
    gpio_set_function(jtag->pin_tdi, gpio_func_pio);
    gpio_set_function(jtag->pin_tms, gpio_func_pio);
    gpio_set_function(jtag->pin_tck, gpio_func_pio);
    // TDO/MISO is sampled by PIO but stays muxed to SIO. Its SIO direction
    // must therefore also be input after SET_BITMODE's initial output mask.
    gpio_set_dir(jtag->pin_tdo, GPIO_IN);
  } else {    
    // revert to bitbang and set pin direction as requested

    // remember last state of TMS and TDI and restore these as well
    gpio_put(jtag->pin_tck, gpio_get(jtag->pin_tck));
    gpio_set_function(jtag->pin_tck, GPIO_FUNC_SIO);
    gpio_set_dir(jtag->pin_tck, (jtag->gpio_dir&(1<<0))?GPIO_OUT:GPIO_IN);

    gpio_put(jtag->pin_tdi, gpio_get(jtag->pin_tdi));
    gpio_set_function(jtag->pin_tdi, GPIO_FUNC_SIO);
    gpio_set_dir(jtag->pin_tdi, (jtag->gpio_dir&(1<<1))?GPIO_OUT:GPIO_IN);

    gpio_set_function(jtag->pin_tdo, GPIO_FUNC_SIO);
    gpio_set_dir(jtag->pin_tdo, (jtag->gpio_dir&(1<<2))?GPIO_OUT:GPIO_IN);
    
    gpio_put(jtag->pin_tms, gpio_get(jtag->pin_tms));
    gpio_set_function(jtag->pin_tms, GPIO_FUNC_SIO);
    gpio_set_dir(jtag->pin_tms, (jtag->gpio_dir&(1<<3))?GPIO_OUT:GPIO_IN);   
  }

  pio_sm_set_enabled(jtag->pio, jtag->sm, enable);
  jtag->pio_enabled = enable;
}

void pio_jtag_init(pio_jtag_inst_t* jtag, uint freq) {
  uint16_t clkdiv = jtag_get_clk_divider(freq);   // clkdiv = 31;  // around 1 MHz @ 125MHz clk_sys
  pio_jtag_io_init(jtag->pio, jtag->sm, clkdiv, jtag->pin_tck, jtag->pin_tdi, jtag->pin_tms, jtag->pin_tdo);
  dma_init(jtag);    
  jtag->pio_enabled = true;
  
  // Setup the reverse and interleave tables.
  // The reverse table does two things:
  // - it expands with every second bit being 0
  // - it swaps high and low byte in the 16 bit target
  for(uint16_t i=0;i<256;i++) {
    uint8_t rbyte = ((i & 0x55) << 1) | ((i & 0xaa) >> 1);
    rbyte = ((rbyte & 0x33) << 2) | ((rbyte & 0xcc) >> 2);
    reverse_table[i] = ((rbyte & 0x0f) << 4) | ((rbyte & 0xf0) >> 4);
    
    interleave_table[i] =
	((i&0x01)<< 8) | ((i&0x02)<< 9) | ((i&0x04)<<10) | ((i&0x08)<<11) |
	((i&0x10)>> 4) | ((i&0x20)>> 3) | ((i&0x40)>> 2) | ((i&0x80)>> 1);
  }
}

void pio_jtag_write_tms(pio_jtag_inst_t* jtag, bool lsb, uint tdi, const uint8_t *src, uint8_t *dst, size_t len) {
  if(!len) return;
  uint16_t wlen = (len+7)/8;
  uint16_t tx_buffer[wlen];  // bytes are expanded to words for interleaved two-bit-transmission
  uint16_t tdi_mask = tdi?0x5555:0x0000;
  
  for(int i=0;i<wlen;i++)
    tx_buffer[i] = (interleave_table[lsb?reverse_table[src[i]]:src[i]]<<1) | tdi_mask;

  pio_jtag_blocking(jtag, (const uint8_t *)tx_buffer, dst, len);  

  // bit reverse result
  if(dst && lsb)
    for(int i=0;i<wlen;i++)
      dst[i] = reverse_table[dst[i]];  
}

void pio_jtag_write_tdi_read_tdo(pio_jtag_inst_t* jtag, bool lsb, const uint8_t *src, uint8_t *dst, size_t len) {
  if(!len) return;
  uint16_t wlen = (len+7)/8;
  uint16_t tx_buffer[wlen];  // bytes are expanded to words for interleaved two-bit-transmission

  // drive TMS constant 0 when writing tdi
  for(int i=0;i<wlen;i++)
    tx_buffer[i] = src?interleave_table[lsb?reverse_table[src[i]]:src[i]]:0x5555;

  pio_jtag_blocking(jtag, (const uint8_t *)tx_buffer, dst, len);

  // bit reverse result if lsb first was requested
  if(dst && lsb)
    for(int i=0;i<wlen;i++)
      dst[i] = reverse_table[dst[i]];  
}

void pio_jtag_write_tdi(pio_jtag_inst_t* jtag, bool lsb, const uint8_t *src, size_t len) {
  pio_jtag_write_tdi_read_tdo(jtag, lsb, src, NULL, len);
}

void pio_jtag_read_tdo(pio_jtag_inst_t* jtag, bool lsb, uint8_t *dst, size_t len) {
  pio_jtag_write_tdi_read_tdo(jtag, lsb, NULL, dst, len);
}

// MPSSE 0x8e/0x8f: clock without changing either data output or returning data.
// Reuse the existing interleaved PIO engine in bounded USB-sized chunks.
void pio_jtag_clock(pio_jtag_inst_t *jtag, size_t bits) {
  pio_jtag_wait(jtag);
  uint8_t pair = (gpio_get(jtag->pin_tdi)?1:0) |
                 (gpio_get(jtag->pin_tms)?2:0);
  uint8_t data[128];
  memset(data, pair * 0x55, sizeof(data));
  while(bits) {
    size_t count = MIN(bits, sizeof(data) * 4);
    pio_jtag_blocking(jtag, data, NULL, count);
    bits -= count;
  }
  pio_jtag_wait(jtag);
}
