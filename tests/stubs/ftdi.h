#pragma once
#include <stdint.h>
enum ftdi_interface { INTERFACE_ANY, INTERFACE_A, INTERFACE_B, INTERFACE_C, INTERFACE_D };
#define BITMODE_MPSSE 2
struct ftdi_context { unsigned port; unsigned char readbuf[62]; int pos, len; };
int ftdi_init(struct ftdi_context *f);
int ftdi_set_interface(struct ftdi_context *f, enum ftdi_interface i);
int ftdi_usb_open(struct ftdi_context *f, int vendor, int product);
int ftdi_usb_open_string(struct ftdi_context *f, const char *s);
int ftdi_usb_reset(struct ftdi_context *f);
int ftdi_usb_purge_buffers(struct ftdi_context *f);
int ftdi_get_latency_timer(struct ftdi_context *f, unsigned char *v);
int ftdi_set_latency_timer(struct ftdi_context *f, unsigned char v);
int ftdi_set_bitmode(struct ftdi_context *f, unsigned char mask, unsigned char mode);
int ftdi_disable_bitbang(struct ftdi_context *f);
int ftdi_write_data(struct ftdi_context *f, const unsigned char *data, int n);
int ftdi_read_data(struct ftdi_context *f, unsigned char *data, int n);
int ftdi_usb_close(struct ftdi_context *f);
void ftdi_deinit(struct ftdi_context *f);
const char *ftdi_get_error_string(struct ftdi_context *f);
