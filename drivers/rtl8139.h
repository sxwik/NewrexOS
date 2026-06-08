#ifndef RTL8139_H
#define RTL8139_H

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

int rtl8139_probe(void);
int rtl8139_is_present(void);
void rtl8139_dump_info(void);
void rtl8139_init(void);
void net_ping(const char* cmd_args);
void net_udp_send(const char* cmd_args);

#endif
