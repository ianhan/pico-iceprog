// Compile the production USB/parser and PIO/DMA C implementations unchanged.
// Only SDK peripherals and the libftdi USB transport are replaced by models.
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include "model.h"
#include "ftdi.h"
static int firmware_printf(const char *format, ...) { (void)format; return 0; }
#define printf firmware_printf
#define main firmware_main
#include "../pico_mpsse/pico_mpsse.c"
#undef main
#include "../pico_mpsse/pio_jtag.c"
#undef printf

static int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t *data, uint16_t length) {
  struct usb_setup_packet packet = {type, request, value, index, length};
  memcpy(fake_dpram.setup_packet, &packet, sizeof(packet));
  *dev_config.endpoints[1].buffer_control = 0;
  usb_handle_setup_packet();
  uint32_t result = *dev_config.endpoints[1].buffer_control;
  assert(result & USB_BUF_CTRL_AVAIL);
  assert(result & USB_BUF_CTRL_DATA1_PID);
  uint16_t size = result & USB_BUF_CTRL_LEN_MASK;
  if(data && size) memcpy(data, (const void *)dev_config.endpoints[1].data_buffer, size);
  *dev_config.endpoints[1].buffer_control &= ~USB_BUF_CTRL_AVAIL;
  ep0_in_handler(NULL, size);
  if(type & USB_DIR_IN)
    assert(*dev_config.endpoints[0].buffer_control & USB_BUF_CTRL_DATA1_PID);
  return size;
}
static size_t receive(uint p, uint8_t *data) {
  struct usb_endpoint_configuration *ep = &dev_config.ports[p].endpoints[0];
  assert(*ep->buffer_control & USB_BUF_CTRL_AVAIL);
  size_t n = *ep->buffer_control & USB_BUF_CTRL_LEN_MASK;
  assert(n >= 2 && n <= 64);
  assert(ep->data_buffer[0] == 0x32 && ep->data_buffer[1] == 0x60);
  memcpy(data, (const void *)(ep->data_buffer+2), n-2);
  *ep->buffer_control &= ~USB_BUF_CTRL_AVAIL;
  ep->handler(NULL, n);
  return n-2;
}
static void transmit(uint p, const uint8_t *data, size_t n) {
  struct usb_endpoint_configuration *ep = &dev_config.ports[p].endpoints[1];
  assert(n <= 64 && (*ep->buffer_control & USB_BUF_CTRL_AVAIL));
  *ep->buffer_control = n | USB_BUF_CTRL_FULL;
  memcpy((void *)ep->data_buffer, data, n);
  ep->handler((uint8_t *)ep->data_buffer, n);
}
static void start(void) {
  model_init();
  jtag_init(&dev_config.ports[0].jtag.pio);
  jtag_init(&dev_config.ports[1].jtag.pio);
  control(USB_DIR_OUT, USB_REQUEST_SET_CONFIGURATION, 1, 0, NULL, 0);
}

int ftdi_init(struct ftdi_context *f) { memset(f,0,sizeof(*f)); start(); atexit(model_save); return 0; }
int ftdi_set_interface(struct ftdi_context *f, enum ftdi_interface i) { f->port = i?i-1:0; return f->port < 2?0:-1; }
int ftdi_usb_open(struct ftdi_context *f, int vendor, int product) {
  if(vendor != device_descriptor.idVendor || product != device_descriptor.idProduct || f->port >= 2) return -1;
  uint8_t config;
  assert(control(USB_DIR_IN, USB_REQUEST_GET_CONFIGURATION, 0, 0, &config, 1) == 1 && config == 1);
  ftdi_usb_reset(f);
  return control(USB_VENDOR_OUT, 3, 0x4138, f->port+1, NULL, 0);
}
int ftdi_usb_open_string(struct ftdi_context *f, const char *s) { (void)s; return ftdi_usb_open(f,0x403,0x6010); }
int ftdi_usb_reset(struct ftdi_context *f) { return control(USB_VENDOR_OUT, 0, 0, f->port+1, NULL, 0); }
int ftdi_usb_purge_buffers(struct ftdi_context *f) {
  control(USB_VENDOR_OUT, 0, 1, f->port+1, NULL, 0);
  return control(USB_VENDOR_OUT, 0, 2, f->port+1, NULL, 0);
}
int ftdi_get_latency_timer(struct ftdi_context *f, unsigned char *v) { return control(USB_VENDOR_IN, 0x0a, 0, f->port+1, v, 1)==1?0:-1; }
int ftdi_set_latency_timer(struct ftdi_context *f, unsigned char v) { return control(USB_VENDOR_OUT, 9, v, f->port+1, NULL, 0); }
int ftdi_set_bitmode(struct ftdi_context *f, unsigned char mask, unsigned char mode) { return control(USB_VENDOR_OUT, 0xb, (mode<<8)|mask, f->port+1, NULL, 0); }
int ftdi_disable_bitbang(struct ftdi_context *f) { return ftdi_set_bitmode(f,0,0); }
int ftdi_write_data(struct ftdi_context *f, const unsigned char *data, int n) {
  size_t max = getenv("TEST_PACKET_SIZE")?atoi(getenv("TEST_PACKET_SIZE")):64;
  assert(max && max <= 64);
  for(int pos=0; pos<n;) {
    int count = MIN((size_t)(n-pos), max);
    transmit(f->port,data+pos,count); pos+=count;
  }
  return n;
}
int ftdi_read_data(struct ftdi_context *f, unsigned char *data, int n) {
  int count = 0;
  while(count < n) {
    if(f->pos == f->len) {
      f->len = receive(f->port,f->readbuf); f->pos = 0;
      // An idle status packet may already have been armed before the reply.
      if(!f->len) f->len = receive(f->port,f->readbuf);
      if(!f->len) break;
    }
    int size = MIN(n-count,f->len-f->pos);
    memcpy(data+count,f->readbuf+f->pos,size); count+=size; f->pos+=size;
  }
  return count;
}
int ftdi_usb_close(struct ftdi_context *f) {
  assert(!model_output(f->port?20:6) && !model_output(f->port?26:9));
  if(!dev_config.ports[f->port].jtag.mode)
    assert(dev_config.ports[f->port].jtag.latency_timer == 16);
  return 0;
}
void ftdi_deinit(struct ftdi_context *f) { (void)f; }
const char *ftdi_get_error_string(struct ftdi_context *f) { (void)f; return "firmware model rejected request"; }
int usleep(useconds_t usec) { (void)usec; return 0; }

#ifdef TEST_PROTOCOL
static void loopback(uint p, size_t bytes, size_t packet) {
  const uint8_t gpio[] = {0x80,0,0x03};
  transmit(p,gpio,sizeof(gpio));
  assert(dev_config.ports[p].jtag.pio.pio_enabled);
  assert(!model_output(fake_pios[p].tms));
  assert(!model_output(fake_pios[p].tdo));
  uint8_t hdr[] = {0x31,(bytes-1)&255,(bytes-1)>>8};
  for(size_t i=0;i<sizeof(hdr);i++) transmit(p,hdr+i,1);
  assert(dev_config.ports[p].jtag.pending_writes == bytes);
  uint8_t input[64], output[62];
  size_t sent=0, got=0;
  while(sent < bytes) {
    size_t n = MIN(packet,bytes-sent);
    for(size_t i=0;i<n;i++) input[i]=(uint8_t)((sent+i)*37+11);
    transmit(p,input,n); sent+=n;
    // Deliberately wait for backpressure to exercise the finite reply FIFO.
    if(dev_config.ports[p].jtag.rx_disabled || sent == bytes) {
      while(got < sent) {
        size_t nr = receive(p,output);
        for(size_t i=0;i<nr;i++) assert(output[i] == (uint8_t)((got+i)*37+11));
        got+=nr;
      }
    }
  }
  assert(got == bytes && !dev_config.ports[p].jtag.pending_writes);
}
int main(void) {
  start(); model_loopback = true;
  uint8_t reply[64];
  // Releasing an open-drain control must not leave the RP2040's reset
  // pull-down fighting the target's weak pull-up, particularly on CDONE.
  const uint control_pins[] = {6, 7, 8, 9, 20, 21, 22, 26};
  for(size_t i=0; i<sizeof(control_pins)/sizeof(control_pins[0]); i++) {
    assert(!model_output(control_pins[i]));
    assert(!model_pull_up(control_pins[i]));
    assert(!model_pull_down(control_pins[i]));
  }
  assert(control(USB_DIR_IN,USB_REQUEST_GET_CONFIGURATION,0,0,reply,1)==1 && reply[0]==1);
  assert(control(USB_DIR_IN,USB_REQUEST_GET_DESCRIPTOR,USB_DT_CONFIG<<8,0,reply,64)==55);
  for(uint p=0;p<2;p++) {
    assert(control(USB_VENDOR_IN,0xa,0,p+1,reply,1)==1 && reply[0]==16);
    control(USB_VENDOR_OUT,9,1,p+1,NULL,0);
    assert(control(USB_VENDOR_IN,0xa,0,p+1,reply,1)==1 && reply[0]==1);
    control(USB_VENDOR_OUT,0xb,0x2ff,p+1,NULL,0);
    assert(!model_output(fake_pios[p].tdo));
    for(size_t packet=1;packet<=64;packet++) loopback(p,257,packet);
    loopback(p,4096,64);
    loopback(p,65536,64);
    // GPIO direction is the open-drain CS/reset value used by iceprog.
    const uint8_t select[] = {0x80,0,0x93};
    transmit(p,select,3);
    assert(!gpio_get(p?20:6));
    const uint8_t release[] = {0x80,0,0x03};
    transmit(p,release,3);
    assert(!model_output(p?20:6) && !model_output(p?26:9));
    // The two-bit flash reset followed by SRAM's exact 48+1 clocks.
    const uint8_t bits[] = {0x33,1,0xff};
    transmit(p,bits,3);
    while(!receive(p,reply)) {}
    assert(reply[0]==3);
    size_t clocks=fake_pios[p].clocks;
    const uint8_t dummy[] = {0x8f,5,0,0x8e,0};
    for(size_t i=0;i<sizeof(dummy);i++) transmit(p,dummy+i,1);
    assert(fake_pios[p].clocks == clocks+49 && gpio_get(fake_pios[p].tdi));
    const uint8_t slow[] = {0x8b,0x86,119,0}; transmit(p,slow,4);
    assert(fake_pios[p].divider == 600);
    const uint8_t fast[] = {0x86,0,0}; transmit(p,fast,3);
    assert(fake_pios[p].divider == 5);
    // Purge an incomplete command without disturbing GPIO or the other port.
    transmit(p,(const uint8_t[]){0x31,9},2);
    control(USB_VENDOR_OUT,0,2,p+1,NULL,0);
    assert(!dev_config.ports[p].jtag.cmd_buf.len);
    loopback(p,1,1);
    transmit(p,(const uint8_t[]){0x81},1);
    control(USB_VENDOR_OUT,0,1,p+1,NULL,0);
    assert(!receive(p,reply));
    // A caller owns only one byte of RX storage, with no trailer space.
    uint8_t *one = malloc(1), source=0xa5;
    pio_jtag_write_tdi_read_tdo(&dev_config.ports[p].jtag.pio,false,&source,one,8);
    assert(*one==source); free(one);
    control(USB_VENDOR_OUT,0xb,0,p+1,NULL,0);
    assert(!dev_config.ports[p].jtag.pio.pio_enabled);
  }
  usb_bus_reset();
  control(USB_DIR_OUT,USB_REQUEST_SET_CONFIGURATION,1,0,NULL,0);
  control(USB_VENDOR_OUT,0xb,0x2ff,1,NULL,0);
  loopback(0,256,64);
  puts("protocol: USB setup, both ports, all packet splits, backpressure, 64 KiB lengths, GPIO, clocks, purge, DMA bounds passed");
  return 0;
}
#endif
