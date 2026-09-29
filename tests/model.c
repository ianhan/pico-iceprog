#include <stdio.h>
#include "model.h"

usb_dpram_t fake_dpram;
usb_hw_t fake_usb, fake_usb_set, fake_usb_clear;
fake_pio fake_pios[2];
uint8_t model_flash[FLASH_SIZE], model_sr[2], model_sram[FLASH_SIZE];
size_t model_sram_bits, model_two_bit_resets;
bool model_loopback, model_power_down;
static bool values[32], outputs[32];
static bool pull_ups[32], pull_downs[32];
static uint functions[32];
static const uint cs_pins[] = {6, 20}, reset_pins[] = {9, 26};
static struct {
  uint8_t command, tx, rx;
  uint32_t address;
  size_t position, bits;
  bool wel;
  unsigned busy;
} flash[2];
static struct {
  dma_channel_config config;
  uint8_t *dst;
  const uint8_t *src;
  size_t count;
  bool busy;
} channels[4];
static int next_channel;

bool model_output(uint pin) {
  assert(pin < 32);
  return functions[pin] ? !!(fake_pios[functions[pin]-1].directions & (1u << pin)) : outputs[pin];
}
bool model_pull_up(uint pin) { assert(pin < 32); return pull_ups[pin]; }
bool model_pull_down(uint pin) { assert(pin < 32); return pull_downs[pin]; }
bool gpio_get(uint pin) {
  assert(pin < 32);
  if(!model_output(pin)) return true; // target pull-ups, including CDONE
  return functions[pin] ? !!(fake_pios[functions[pin]-1].values & (1u << pin)) : values[pin];
}
static void flash_end(uint p) {
  uint8_t cmd = flash[p].command;
  if(flash[p].bits == 2) model_two_bit_resets++;
  if(cmd == 0x06) flash[p].wel = true;
  if(cmd == 0xab) model_power_down = false;
  if(cmd == 0xb9) model_power_down = true;
  if(flash[p].wel) {
    uint32_t size = cmd == 0x20 ? 4096 : cmd == 0x52 ? 32768 : cmd == 0xd8 ? 65536 : cmd == 0xc7 ? FLASH_SIZE : 0;
    if(size && !(model_sr[0] & 0x1c)) {
      uint32_t addr = cmd == 0xc7 ? 0 : flash[p].address & ~(size - 1);
      assert(addr + size <= FLASH_SIZE);
      memset(model_flash + addr, 0xff, size);
    }
    if(size || cmd == 0x02 || cmd == 0x01 || cmd == 0x31) {
      flash[p].wel = false;
      flash[p].busy = 2;
    }
  }
}
static void pin_changed(uint pin, bool before) {
  for(uint p = 0; p < 2; p++) {
    if(pin == cs_pins[p] && before != gpio_get(pin)) {
      if(gpio_get(pin)) flash_end(p);
      else {
        flash[p].command = flash[p].tx = flash[p].rx = 0;
        flash[p].address = flash[p].position = flash[p].bits = 0;
      }
    }
  }
}
void gpio_init(uint pin) { assert(pin < 32); functions[pin] = 0; outputs[pin] = false; values[pin] = false; }
void gpio_set_dir(uint pin, bool out) { bool before = gpio_get(pin); outputs[pin] = out; pin_changed(pin, before); }
void gpio_put(uint pin, bool value) { bool before = gpio_get(pin); values[pin] = value; pin_changed(pin, before); }
void gpio_set_function(uint pin, uint function) { assert(pin < 32); functions[pin] = function; }
void gpio_set_pulls(uint pin, bool up, bool down) {
  assert(pin < 32); pull_ups[pin] = up; pull_downs[pin] = down;
}
void pio_sm_set_enabled(PIO p, uint sm, bool enabled) { (void)sm; p->enabled = enabled; }
void pio_sm_set_pins_with_mask(PIO p, uint sm, uint32_t v, uint32_t m) { (void)sm; p->values = (p->values & ~m) | (v & m); }
void pio_sm_set_pindirs_with_mask(PIO p, uint sm, uint32_t v, uint32_t m) { (void)sm; p->directions = (p->directions & ~m) | (v & m); }
void pio_sm_set_clkdiv_int_frac(PIO p, uint sm, uint16_t div, uint8_t frac) { (void)sm; assert(!frac); p->divider = div; }
void fake_pio_init(PIO p, uint sm, uint16_t div, uint tck, uint tdi, uint tms, uint tdo) {
  (void)sm;
  *p = (fake_pio){ .tck=tck, .tdi=tdi, .tms=tms, .tdo=tdo, .divider=div, .enabled=true,
    .values=1u<<tms, .directions=(1u<<tck)|(1u<<tdi)|(1u<<tms) };
  functions[tck] = functions[tdi] = functions[tms] = p == pio0 ? 1 : 2;
}
void pio_sm_put_blocking(PIO p, uint sm, uint32_t count) { (void)sm; assert(p->enabled && count < 524288); p->bits = count + 1; }
uint32_t pio_sm_get_blocking(PIO p, uint sm) { (void)sm; assert(p->trailer == 1); p->trailer = 0; return 0; }
int dma_claim_unused_channel(bool required) { (void)required; assert(next_channel < 4); return next_channel++; }
dma_channel_config dma_channel_get_default_config(int chan) { (void)chan; return (dma_channel_config){0}; }
void channel_config_set_transfer_data_size(dma_channel_config *c, int s) { (void)c; assert(s == DMA_SIZE_8); }
void channel_config_set_read_increment(dma_channel_config *c, bool v) { c->read_inc = v; }
void channel_config_set_write_increment(dma_channel_config *c, bool v) { c->write_inc = v; }
void channel_config_set_dreq(dma_channel_config *c, uint v) { c->dreq = v; }
void dma_channel_configure(int n, const dma_channel_config *c, void *d, const void *s, size_t count, bool start) {
  (void)d; (void)s; (void)count; assert(!start); channels[n].config = *c;
}
void dma_channel_set_config(int n, const dma_channel_config *c, bool start) { assert(!start); channels[n].config = *c; }
void dma_channel_transfer_to_buffer_now(int n, void *buf, size_t count) {
  channels[n].dst = buf; channels[n].count = count; channels[n].busy = true;
}
void dma_channel_transfer_from_buffer_now(int n, const void *buf, size_t count) {
  channels[n].src = buf; channels[n].count = count; channels[n].busy = true;
}

static uint8_t flash_reply(uint p) {
  if(!flash[p].position || model_power_down) return 0xff;
  switch(flash[p].command) {
  case 0x9f: {
    const uint8_t id[] = {0xef, 0x40, 0x15, 0x00};
    return flash[p].position <= sizeof(id) ? id[flash[p].position-1] : 0;
  }
  case 0x05: {
    uint8_t sr = model_sr[0] | (flash[p].wel?2:0) | (flash[p].busy?1:0);
    if(flash[p].busy) flash[p].busy--;
    return sr;
  }
  case 0x35: return model_sr[1];
  case 0x03: return flash[p].position >= 4 ? model_flash[flash[p].address++ % FLASH_SIZE] : 0xff;
  default: return 0xff;
  }
}
static void flash_byte(uint p, uint8_t b) {
  size_t pos = flash[p].position++;
  if(!pos) { flash[p].command = b; return; }
  uint8_t cmd = flash[p].command;
  if((cmd == 0x03 || cmd == 0x02 || cmd == 0x20 || cmd == 0x52 || cmd == 0xd8) && pos <= 3)
    flash[p].address = (flash[p].address << 8) | b;
  if(cmd == 0x02 && pos >= 4 && flash[p].wel && !(model_sr[0] & 0x1c)) {
    assert(flash[p].address < FLASH_SIZE);
    model_flash[flash[p].address] &= b;
    flash[p].address = (flash[p].address & ~255u) | ((flash[p].address + 1) & 255u);
  }
  if(pos == 1 && flash[p].wel) {
    if(cmd == 0x01) model_sr[0] = b;
    if(cmd == 0x31) model_sr[1] = b;
  }
}
static bool spi_bit(uint p, bool out) {
  if(model_loopback) return out;
  if(gpio_get(cs_pins[p])) return true;
  if(gpio_get(reset_pins[p])) {
    assert(model_sram_bits < sizeof(model_sram)*8);
    if(out) model_sram[model_sram_bits/8] |= 1u << (7-model_sram_bits%8);
    model_sram_bits++;
    return true;
  }
  if(!(flash[p].bits % 8)) flash[p].rx = flash_reply(p);
  bool in = (flash[p].rx & (0x80 >> (flash[p].bits % 8))) != 0;
  flash[p].tx = (flash[p].tx << 1) | out;
  flash[p].bits++;
  if(!(flash[p].bits % 8)) flash_byte(p, flash[p].tx);
  return in;
}
bool dma_channel_is_busy(int n) {
  if(!channels[n].busy) return false;
  uint p = channels[n].config.dreq / 2;
  int tx = -1, rx = -1;
  for(int i=0; i<4; i++) {
    if(channels[i].config.dreq == 2*p) tx = i;
    if(channels[i].config.dreq == 2*p+1) rx = i;
  }
  assert(tx >= 0 && rx >= 0 && channels[tx].busy && channels[rx].busy);
  PIO io = &fake_pios[p];
  assert(io->enabled && channels[tx].count == (io->bits+3)/4);
  size_t count = 0;
  uint8_t sampled = 0;
  for(size_t b=0; b<io->bits; b++) {
    uint8_t pair = (channels[tx].src[b/4] >> (6-2*(b%4))) & 3;
    io->values = (io->values & ~((1u<<io->tdi)|(1u<<io->tms))) |
                 ((pair&1)?1u<<io->tdi:0) | ((pair&2)?1u<<io->tms:0);
    sampled = (sampled << 1) | spi_bit(p, pair&1);
    io->clocks++;
    if(b%8 == 7) {
      assert(count < channels[rx].count);
      channels[rx].dst[channels[rx].config.write_inc?count:0] = sampled;
      sampled = 0; count++;
    }
  }
  // jtag.pio's explicit PUSH after its byte autopushes.
  if(count < channels[rx].count) {
    channels[rx].dst[channels[rx].config.write_inc?count:0] = sampled;
    count++; io->trailer = 0;
  } else { assert(io->bits % 8 == 0); io->trailer = 1; }
  assert(count == channels[rx].count);
  channels[tx].busy = channels[rx].busy = false;
  return false;
}

void model_init(void) {
  // RP2040 PADS_BANK0 GPIO reset: PUE=0, PDE=1. gpio_init does not alter it.
  memset(pull_ups, 0, sizeof(pull_ups));
  memset(pull_downs, 1, sizeof(pull_downs));
  memset(model_flash, 0xff, sizeof(model_flash));
  if(getenv("TEST_PROTECTED")) model_sr[0] = 0x1c;
  const char *path = getenv("TEST_FLASH");
  if(path) {
    FILE *f = fopen(path, "rb");
    if(f) { assert(fread(model_flash, 1, sizeof(model_flash), f) == sizeof(model_flash)); fclose(f); }
  }
  fake_usb.abort_done = UINT32_MAX;
}
void model_save(void) {
  const char *path = getenv("TEST_FLASH");
  if(path) { FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(model_flash,1,sizeof(model_flash),f)==sizeof(model_flash)); fclose(f); }
  path = getenv("TEST_SRAM");
  if(path) { FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(model_sram,1,(model_sram_bits+7)/8,f)==(model_sram_bits+7)/8); fclose(f); }
  path = getenv("TEST_STATS");
  if(path) {
    FILE *f = fopen(path,"w"); assert(f);
    fprintf(f,"{\"sram_bits\":%zu,\"reset_bits\":%zu,\"sr1\":%u,\"sr2\":%u,\"power_down\":%u,\"div_a\":%u,\"div_b\":%u}\n",
      model_sram_bits, model_two_bit_resets, model_sr[0], model_sr[1], model_power_down, fake_pios[0].divider, fake_pios[1].divider);
    fclose(f);
  }
}
