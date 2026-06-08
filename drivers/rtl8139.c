#include "rtl8139.h"
#include "pci.h"

extern void print_string(char* str);
extern void print_string_color(char* str, unsigned char attribute);
extern void print_hex(unsigned int num);
extern void print_int(int num);
extern void print_char(char c);

#define COLOR_LIGHT_CYAN  0x0B
#define COLOR_LIGHT_GREEN 0x0A
#define COLOR_LIGHT_RED   0x0C
#define COLOR_YELLOW      0x0E

#define RTL8139_REG_IDR0 0x00

static int rtl8139_present = 0;
static unsigned char rtl8139_bus = 0;
static unsigned char rtl8139_slot = 0;
static unsigned char rtl8139_func = 0;
static unsigned short rtl8139_io_base = 0;

static unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(unsigned short port, unsigned char value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned short inw(unsigned short port) {
    unsigned short value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outw(unsigned short port, unsigned short value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

extern void* memset(void* dest, int val, unsigned int count);

static void print_hex_byte(unsigned char value) {
    char hex[] = "0123456789ABCDEF";
    print_char(hex[value >> 4]);
    print_char(hex[value & 0x0F]);
}

static int rtl8139_enable_io(void) {
    unsigned int bar0 = pci_read_config32(rtl8139_bus, rtl8139_slot, rtl8139_func, 0x10);
    if ((bar0 & 0x01) == 0) {
        return 0;
    }

    rtl8139_io_base = (unsigned short)(bar0 & 0xFFFC);

    unsigned short command = pci_read_config16(rtl8139_bus, rtl8139_slot, rtl8139_func, 0x04);
    command |= 0x0005; // Enable I/O Space (0x01) and Bus Master (0x04)
    pci_write_config16(rtl8139_bus, rtl8139_slot, rtl8139_func, 0x04, command);

    unsigned short check_cmd = pci_read_config16(rtl8139_bus, rtl8139_slot, rtl8139_func, 0x04);
    print_string("[RTL8139] PCI Command register after write: ");
    print_hex(check_cmd);
    print_string("\n");
    return 1;
}

static void rtl8139_read_mac(unsigned char mac[6]) {
    for (int i = 0; i < 6; i++) {
        mac[i] = inb(rtl8139_io_base + RTL8139_REG_IDR0 + i);
    }
}

int rtl8139_probe(void) {
    pci_device_t devices[PCI_MAX_DEVICES];
    int count = pci_scan_devices(devices, PCI_MAX_DEVICES);

    rtl8139_present = 0;
    rtl8139_io_base = 0;

    for (int i = 0; i < count; i++) {
        if (devices[i].vendor_id == RTL8139_VENDOR_ID &&
            devices[i].device_id == RTL8139_DEVICE_ID) {
            rtl8139_bus = devices[i].bus;
            rtl8139_slot = devices[i].slot;
            rtl8139_func = devices[i].func;
            if (rtl8139_enable_io()) {
                rtl8139_present = 1;
            }
            return rtl8139_present;
        }
    }

    return 0;
}

int rtl8139_is_present(void) {
    return rtl8139_present;
}

void rtl8139_dump_info(void) {
    if (!rtl8139_probe()) {
        print_string_color("RTL8139 not found on PCI bus.\n", COLOR_LIGHT_RED);
        print_string_color("QEMU hint: -netdev user -device rtl8139\n", COLOR_YELLOW);
        return;
    }

    unsigned char mac[6];
    rtl8139_read_mac(mac);

    print_string_color("--- RTL8139 NETWORK ADAPTER ---\n", COLOR_LIGHT_CYAN);
    print_string("PCI location      : ");
    print_int(rtl8139_bus);
    print_char(':');
    print_int(rtl8139_slot);
    print_char('.');
    print_int(rtl8139_func);
    print_char('\n');
    print_string("I/O base          : ");
    print_hex(rtl8139_io_base);
    print_string("\nMAC address       : ");
    for (int i = 0; i < 6; i++) {
        if (i > 0) print_char(':');
        print_hex_byte(mac[i]);
    }
    print_string_color("\nStatus            : active and initialized (TX/RX enabled)\n", COLOR_LIGHT_GREEN);
    extern unsigned int net_ping_replies;
    print_string("Ping replies rcvd : ");
    print_int(net_ping_replies);
    print_char('\n');
}

// ============================================================================
//                  NEWREX OS INTEGRATED NETWORK STACK
// ============================================================================

extern void* kmalloc(unsigned int size);
extern void kfree(void* ptr);

static unsigned char my_ip[4] = {10, 0, 2, 15};
static unsigned char my_mac[6] = {0};
unsigned int net_ping_replies = 0;

static unsigned char* rx_buffer = 0;
static unsigned char* tx_buffers[4] = {0};
static int tx_index = 0;
static int rx_offset = 0;

// Helper memory and string routines
static void* memcpy(void* dest, const void* src, unsigned int count) {
    char* dst_c = (char*)dest;
    const char* src_c = (const char*)src;
    for (unsigned int i = 0; i < count; i++) {
        dst_c[i] = src_c[i];
    }
    return dest;
}

static int memcmp(const void* s1, const void* s2, unsigned int count) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    for (unsigned int i = 0; i < count; i++) {
        if (p1[i] != p2[i]) return p1[i] - p2[i];
    }
    return 0;
}

// Endian conversion routines
static unsigned short ntohs(unsigned short n) {
    return ((n & 0xFF) << 8) | ((n & 0xFF00) >> 8);
}
#define htons(n) ntohs(n)

static unsigned int ntohl(unsigned int n) {
    return ((n & 0xFF) << 24) |
           ((n & 0xFF00) << 8) |
           ((n & 0xFF0000) >> 8) |
           ((n & 0xFF000000) >> 24);
}
#define htonl(n) ntohl(n)

static int parse_ip(const char* str, unsigned char ip[4]) {
    int val = 0;
    int octet = 0;
    while (*str != '\0') {
        if (*str >= '0' && *str <= '9') {
            val = val * 10 + (*str - '0');
        } else if (*str == '.') {
            if (octet >= 3) return 0;
            ip[octet++] = val;
            val = 0;
        } else {
            return 0;
        }
        str++;
    }
    if (octet != 3) return 0;
    ip[octet] = val;
    return 1;
}

// Internet Checksum
static unsigned short ip_checksum(void* vdata, int length) {
    unsigned short* w = (unsigned short*)vdata;
    unsigned int sum = 0;
    for (int i = 0; i < length / 2; i++) {
        sum += w[i];
    }
    if (length & 1) {
        sum += ((unsigned char*)vdata)[length - 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (unsigned short)~sum;
}

// Packet transmit hardware driver interface
void rtl8139_send_packet(void* data, int size) {
    if (!rtl8139_present || rtl8139_io_base == 0) return;

    // Wait until descriptor is free (checking ownership bit of TSD register)
    unsigned int status_reg = rtl8139_io_base + 0x10 + tx_index * 4;
    while ((inw(status_reg) & 0x2000) == 0) {
        // Wait for Tx OK
    }

    // Copy to descriptor buffer
    memcpy(tx_buffers[tx_index], data, size);

    // Set TX Address
    unsigned short addr_port = (unsigned short)(rtl8139_io_base + 0x20 + tx_index * 4);
    unsigned int tx_phys = (unsigned int)tx_buffers[tx_index];
    __asm__ volatile("outl %0, %1" : : "a"(tx_phys), "d"(addr_port));

    // Set TSD packet size (triggers transfer)
    unsigned short status_port = (unsigned short)(rtl8139_io_base + 0x10 + tx_index * 4);
    __asm__ volatile("outl %0, %1" : : "a"(size | 0x0000), "d"(status_port));

    tx_index = (tx_index + 1) % 4;
}

// --- DATA LINK LAYER (ETHERNET & ARP) ---

typedef struct {
    unsigned char dest_mac[6];
    unsigned char src_mac[6];
    unsigned short type;
} __attribute__((packed)) eth_header_t;

typedef struct {
    unsigned short hw_type;
    unsigned short proto_type;
    unsigned char hw_len;
    unsigned char proto_len;
    unsigned short opcode;
    unsigned char sender_mac[6];
    unsigned char sender_ip[4];
    unsigned char target_mac[6];
    unsigned char target_ip[4];
} __attribute__((packed)) arp_packet_t;

typedef struct {
    unsigned char ip[4];
    unsigned char mac[6];
    int active;
} arp_entry_t;

#define ARP_CACHE_SIZE 16
static arp_entry_t arp_cache[ARP_CACHE_SIZE];

static void arp_cache_add(unsigned char ip[4], unsigned char mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].active && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].active) {
            memcpy(arp_cache[i].ip, ip, 4);
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].active = 1;
            return;
        }
    }
}

static int arp_resolve(unsigned char ip[4], unsigned char mac_out[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].active && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}



void arp_send_request(unsigned char target_ip[4]) {
    unsigned char packet[60];
    memset(packet, 0, sizeof(packet));

    eth_header_t* eth = (eth_header_t*)packet;
    memset(eth->dest_mac, 0xFF, 6);
    memcpy(eth->src_mac, my_mac, 6);
    eth->type = htons(0x0806);

    arp_packet_t* arp = (arp_packet_t*)(packet + sizeof(eth_header_t));
    arp->hw_type = htons(1);
    arp->proto_type = htons(0x0800);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(1); // Request
    memcpy(arp->sender_mac, my_mac, 6);
    memcpy(arp->sender_ip, my_ip, 4);
    memset(arp->target_mac, 0x00, 6);
    memcpy(arp->target_ip, target_ip, 4);

    rtl8139_send_packet(packet, 60);
}

void handle_arp_packet(unsigned char* packet, int size) {
    if (size < (int)(sizeof(eth_header_t) + sizeof(arp_packet_t))) return;

    arp_packet_t* arp = (arp_packet_t*)(packet + sizeof(eth_header_t));
    unsigned short opcode = ntohs(arp->opcode);

    arp_cache_add(arp->sender_ip, arp->sender_mac);

    if (opcode == 1) { // Request
        if (memcmp(arp->target_ip, my_ip, 4) == 0) {
            unsigned char reply[60];
            memset(reply, 0, sizeof(reply));

            eth_header_t* eth = (eth_header_t*)reply;
            memcpy(eth->dest_mac, arp->sender_mac, 6);
            memcpy(eth->src_mac, my_mac, 6);
            eth->type = htons(0x0806);

            arp_packet_t* arp_rep = (arp_packet_t*)(reply + sizeof(eth_header_t));
            arp_rep->hw_type = htons(1);
            arp_rep->proto_type = htons(0x0800);
            arp_rep->hw_len = 6;
            arp_rep->proto_len = 4;
            arp_rep->opcode = htons(2); // Reply
            memcpy(arp_rep->sender_mac, my_mac, 6);
            memcpy(arp_rep->sender_ip, my_ip, 4);
            memcpy(arp_rep->target_mac, arp->sender_mac, 6);
            memcpy(arp_rep->target_ip, arp->sender_ip, 4);

            rtl8139_send_packet(reply, 60);
        }
    } else if (opcode == 2) {
        // Reply received
    }
}

// --- NETWORK LAYER (IPv4 & ICMP) ---

typedef struct {
    unsigned char ver_ihl;
    unsigned char tos;
    unsigned short len;
    unsigned short id;
    unsigned short flags_frag;
    unsigned char ttl;
    unsigned char proto;
    unsigned short checksum;
    unsigned char src_ip[4];
    unsigned char dest_ip[4];
} __attribute__((packed)) ipv4_header_t;

typedef struct {
    unsigned char type;
    unsigned char code;
    unsigned short checksum;
    unsigned short id;
    unsigned short seq;
} __attribute__((packed)) icmp_header_t;

void handle_udp_packet(unsigned char* packet, int size, int payload_offset, int ip_header_len);
void handle_tcp_packet(unsigned char* packet, int size, int payload_offset, int ip_header_len);

void handle_ipv4_packet(unsigned char* packet, int size) {
    if (size < (int)(sizeof(eth_header_t) + sizeof(ipv4_header_t))) return;

    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    if (memcmp(ip->dest_ip, my_ip, 4) != 0 && memcmp(ip->dest_ip, (unsigned char[]){255, 255, 255, 255}, 4) != 0) {
        return;
    }

    int ip_header_len = (ip->ver_ihl & 0x0F) * 4;
    int payload_offset = sizeof(eth_header_t) + ip_header_len;

    if (ip->proto == 1) { // ICMP
        int icmp_len = ntohs(ip->len) - ip_header_len;
        if (size < payload_offset + icmp_len) return;

        icmp_header_t* icmp = (icmp_header_t*)(packet + payload_offset);
        if (icmp->type == 8) { // Echo Request
            unsigned char reply[1024];
            int reply_size = sizeof(eth_header_t) + ip_header_len + icmp_len;
            if (reply_size > 1024) return;

            memcpy(reply, packet, reply_size);

            eth_header_t* reply_eth = (eth_header_t*)reply;
            memcpy(reply_eth->dest_mac, reply_eth->src_mac, 6);
            memcpy(reply_eth->src_mac, my_mac, 6);

            ipv4_header_t* reply_ip = (ipv4_header_t*)(reply + sizeof(eth_header_t));
            memcpy(reply_ip->dest_ip, reply_ip->src_ip, 4);
            memcpy(reply_ip->src_ip, my_ip, 4);
            reply_ip->checksum = 0;
            reply_ip->checksum = ip_checksum(reply_ip, ip_header_len);

            icmp_header_t* reply_icmp = (icmp_header_t*)(reply + payload_offset);
            reply_icmp->type = 0; // Echo Reply
            reply_icmp->checksum = 0;
            reply_icmp->checksum = ip_checksum(reply_icmp, icmp_len);

            rtl8139_send_packet(reply, reply_size);
        } else if (icmp->type == 0) { // Echo Reply
            unsigned short received_checksum = icmp->checksum;
            icmp->checksum = 0;
            unsigned short calculated_checksum = ip_checksum(icmp, icmp_len);
            icmp->checksum = received_checksum;

            if (received_checksum != calculated_checksum) {
                print_string_color("Warning: ICMP Ping checksum invalid.\n", COLOR_LIGHT_RED);
                return;
            }

            net_ping_replies++;

            print_string("Reply from ");
            print_int(ip->src_ip[0]); print_char('.');
            print_int(ip->src_ip[1]); print_char('.');
            print_int(ip->src_ip[2]); print_char('.');
            print_int(ip->src_ip[3]);
            print_string("\n");
        }
    } else if (ip->proto == 17) { // UDP
        handle_udp_packet(packet, size, payload_offset, ip_header_len);
    } else if (ip->proto == 6) { // TCP
        handle_tcp_packet(packet, size, payload_offset, ip_header_len);
    }
}

// --- TRANSPORT LAYER (UDP) ---

typedef struct {
    unsigned short src_port;
    unsigned short dest_port;
    unsigned short len;
    unsigned short checksum;
} __attribute__((packed)) udp_header_t;

void udp_send(unsigned char dest_ip[4], unsigned short src_port, unsigned short dest_port, const unsigned char* payload, int len) {
    unsigned char packet[1500];
    int ip_len = sizeof(ipv4_header_t) + sizeof(udp_header_t) + len;
    int eth_len = sizeof(eth_header_t) + ip_len;

    if (eth_len > 1500) return;

    eth_header_t* eth = (eth_header_t*)packet;
    memcpy(eth->src_mac, my_mac, 6);

    unsigned char dest_mac[6];
    if (!arp_resolve(dest_ip, dest_mac)) {
        arp_send_request(dest_ip);
        memset(dest_mac, 0xFF, 6); // Broadcast fallback
    }
    memcpy(eth->dest_mac, dest_mac, 6);
    eth->type = htons(0x0800);

    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len = htons(ip_len);
    ip->id = htons(1);
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = 17; // UDP
    ip->checksum = 0;
    memcpy(ip->src_ip, my_ip, 4);
    memcpy(ip->dest_ip, dest_ip, 4);
    ip->checksum = ip_checksum(ip, sizeof(ipv4_header_t));

    udp_header_t* udp = (udp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    udp->src_port = htons(src_port);
    udp->dest_port = htons(dest_port);
    udp->len = htons(sizeof(udp_header_t) + len);
    udp->checksum = 0;

    memcpy(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(udp_header_t), payload, len);

    rtl8139_send_packet(packet, eth_len);
}

void handle_udp_packet(unsigned char* packet, int size, int payload_offset, int ip_header_len) {
    (void)size;
    (void)ip_header_len;
    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    udp_header_t* udp = (udp_header_t*)(packet + payload_offset);

    int udp_len = ntohs(udp->len);
    int data_len = udp_len - sizeof(udp_header_t);
    unsigned char* data = packet + payload_offset + sizeof(udp_header_t);

    print_string("\n[UDP Packet] From ");
    print_int(ip->src_ip[0]); print_char('.');
    print_int(ip->src_ip[1]); print_char('.');
    print_int(ip->src_ip[2]); print_char('.');
    print_int(ip->src_ip[3]);
    print_string(":");
    print_int(ntohs(udp->src_port));
    print_string(" -> ");

    for (int i = 0; i < data_len && i < 64; i++) {
        char c = data[i];
        if (c >= 32 && c <= 126) print_char(c);
        else print_char('.');
    }
    print_string("\n");
}

// --- TRANSPORT LAYER (STATEFUL TCP RESPONDER) ---

typedef struct {
    unsigned short src_port;
    unsigned short dest_port;
    unsigned int seq_num;
    unsigned int ack_num;
    unsigned short flags;
    unsigned short window;
    unsigned short checksum;
    unsigned short urgent_ptr;
} __attribute__((packed)) tcp_header_t;

#define TCP_STATE_CLOSED      0
#define TCP_STATE_LISTEN      1
#define TCP_STATE_SYN_RCVD    2
#define TCP_STATE_ESTABLISHED 3
#define TCP_STATE_FIN_WAIT    4

static int tcp_state = TCP_STATE_LISTEN;
static unsigned int tcp_local_port = 80;
static unsigned int tcp_remote_port = 0;
static unsigned char tcp_remote_ip[4] = {0};
static unsigned int tcp_seq = 1000;
static unsigned int tcp_ack = 0;

void send_tcp_packet(unsigned char dest_ip[4], unsigned short src_port, unsigned short dest_port, unsigned int seq, unsigned int ack, unsigned char flags) {
    unsigned char packet[1500];
    int ip_len = sizeof(ipv4_header_t) + sizeof(tcp_header_t);
    int eth_len = sizeof(eth_header_t) + ip_len;

    eth_header_t* eth = (eth_header_t*)packet;
    memcpy(eth->src_mac, my_mac, 6);

    unsigned char dest_mac[6];
    if (!arp_resolve(dest_ip, dest_mac)) {
        memset(dest_mac, 0xFF, 6);
    }
    memcpy(eth->dest_mac, dest_mac, 6);
    eth->type = htons(0x0800);

    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len = htons(ip_len);
    ip->id = htons(1);
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = 6; // TCP
    ip->checksum = 0;
    memcpy(ip->src_ip, my_ip, 4);
    memcpy(ip->dest_ip, dest_ip, 4);
    ip->checksum = ip_checksum(ip, sizeof(ipv4_header_t));

    tcp_header_t* tcp = (tcp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    tcp->src_port = htons(src_port);
    tcp->dest_port = htons(dest_port);
    tcp->seq_num = htonl(seq);
    tcp->ack_num = htonl(ack);
    tcp->flags = htons((5 << 12) | flags);
    tcp->window = htons(8192);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    struct {
        unsigned char src_ip[4];
        unsigned char dest_ip[4];
        unsigned char zero;
        unsigned char proto;
        unsigned short tcp_len;
    } __attribute__((packed)) pseudo;
    
    memcpy(pseudo.src_ip, my_ip, 4);
    memcpy(pseudo.dest_ip, dest_ip, 4);
    pseudo.zero = 0;
    pseudo.proto = 6;
    pseudo.tcp_len = htons(sizeof(tcp_header_t));

    unsigned char temp_buf[sizeof(pseudo) + sizeof(tcp_header_t)];
    memcpy(temp_buf, &pseudo, sizeof(pseudo));
    memcpy(temp_buf + sizeof(pseudo), tcp, sizeof(tcp_header_t));

    tcp->checksum = ip_checksum(temp_buf, sizeof(temp_buf));

    rtl8139_send_packet(packet, eth_len);
}

void handle_tcp_packet(unsigned char* packet, int size, int payload_offset, int ip_header_len) {
    (void)size;
    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    tcp_header_t* tcp = (tcp_header_t*)(packet + payload_offset);

    unsigned short dest_port = ntohs(tcp->dest_port);
    unsigned short src_port = ntohs(tcp->src_port);
    unsigned short tcp_flags = ntohs(tcp->flags) & 0x3F;

    if (dest_port != tcp_local_port) return;

    unsigned int seq = ntohl(tcp->seq_num);

    if (tcp_flags & 0x02) { // SYN
        tcp_remote_port = src_port;
        memcpy(tcp_remote_ip, ip->src_ip, 4);
        tcp_ack = seq + 1;
        tcp_seq = 2000;
        tcp_state = TCP_STATE_SYN_RCVD;

        send_tcp_packet(tcp_remote_ip, tcp_local_port, tcp_remote_port, tcp_seq, tcp_ack, 0x12);
        tcp_seq++;
    }
    else if ((tcp_flags & 0x10) && tcp_state == TCP_STATE_SYN_RCVD) { // ACK
        tcp_state = TCP_STATE_ESTABLISHED;
        print_string("\n[TCP Connection] Established connection with remote host!\n");
    }
    else if ((tcp_flags & 0x08) || (tcp_flags & 0x10)) { // PSH or ACK
        int tcp_header_len = ((ntohs(tcp->flags) >> 12) & 0x0F) * 4;
        int data_len = ntohs(ip->len) - ip_header_len - tcp_header_len;
        if (data_len > 0) {
            unsigned char* data = packet + payload_offset + tcp_header_len;
            print_string("\n[TCP Data] Received: ");
            for (int i = 0; i < data_len && i < 128; i++) {
                char c = data[i];
                if (c >= 32 && c <= 126) print_char(c);
                else print_char('.');
            }
            print_string("\n");

            tcp_ack = seq + data_len;
            send_tcp_packet(tcp_remote_ip, tcp_local_port, tcp_remote_port, tcp_seq, tcp_ack, 0x10);
        }
    }
    if (tcp_flags & 0x01) { // FIN
        tcp_ack = seq + 1;
        send_tcp_packet(tcp_remote_ip, tcp_local_port, tcp_remote_port, tcp_seq, tcp_ack, 0x11);
        tcp_state = TCP_STATE_CLOSED;
        print_string("\n[TCP Connection] Connection closed by remote host.\n");
    }
}

// --- DRIVER INTERRUPT & RECEIVE LOGIC ---

void handle_ethernet_packet(unsigned char* packet, int size) {
    if (size < (int)sizeof(eth_header_t)) return;

    eth_header_t* eth = (eth_header_t*)packet;
    unsigned short type = ntohs(eth->type);

    if (type == 0x0806) {
        handle_arp_packet(packet, size);
    } else if (type == 0x0800) {
        handle_ipv4_packet(packet, size);
    }
}

void rtl8139_handle_receive(void) {
    while ((inb(rtl8139_io_base + 0x37) & 0x01) == 0) {
        unsigned short* header = (unsigned short*)(rx_buffer + rx_offset);
        unsigned short status = header[0];
        unsigned short size = header[1];

        if (!(status & 0x01)) {
            break;
        }

        unsigned char packet_temp[2048];
        if (rx_offset + size + 4 > 8192) {
            int first_part = 8192 - (rx_offset + 4);
            memcpy(packet_temp, rx_buffer + rx_offset + 4, first_part);
            memcpy(packet_temp + first_part, rx_buffer, size - 4 - first_part);
        } else {
            memcpy(packet_temp, rx_buffer + rx_offset + 4, size - 4);
        }

        handle_ethernet_packet(packet_temp, size - 4);

        rx_offset = (rx_offset + size + 4 + 3) & ~3;
        rx_offset %= 8192;
        
        outw(rtl8139_io_base + 0x38, rx_offset - 16);
    }
}

void rtl8139_isr_handler(void) {
    unsigned short status = inw(rtl8139_io_base + 0x3E);
    outw(rtl8139_io_base + 0x3E, status);

    if (status & 0x01) { // Rx OK
        rtl8139_handle_receive();
    }

    outb(0x20, 0x20);
    outb(0xA0, 0x20);
}

// Inline ASM ISR Stub
void rtl8139_isr_handler(void);

__asm__ (
    ".global rtl8139_isr_asm\n"
    "rtl8139_isr_asm:\n"
    "    pushal\n"
    "    pushl %ds\n"
    "    pushl %es\n"
    "    pushl %fs\n"
    "    pushl %gs\n"
    "    movw $0x10, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw %ax, %fs\n"
    "    movw %ax, %gs\n"
    "    cld\n"
    "    call rtl8139_isr_handler\n"
    "    popl %gs\n"
    "    popl %fs\n"
    "    popl %es\n"
    "    popl %ds\n"
    "    popal\n"
    "    iret\n"
);

void rtl8139_init(void) {
    if (!rtl8139_probe()) {
        return;
    }

    rtl8139_read_mac(my_mac);

    unsigned char irq = pci_read_config8(rtl8139_bus, rtl8139_slot, rtl8139_func, 0x3C);

    print_string("[RTL8139] Initializing hardware. IRQ: ");
    print_int(irq);
    print_string("\n");

    if (rx_buffer == 0) {
        rx_buffer = (unsigned char*)kmalloc(8192 + 16);
    }
    for (int i = 0; i < 4; i++) {
        if (tx_buffers[i] == 0) {
            tx_buffers[i] = (unsigned char*)kmalloc(2048);
        }
    }
    tx_index = 0;
    rx_offset = 0;

    outb(rtl8139_io_base + 0x37, 0x10);
    while (inb(rtl8139_io_base + 0x37) & 0x10) {
        // Wait
    }

    unsigned int rx_phys = (unsigned int)rx_buffer;
    unsigned short rx_port = (unsigned short)(rtl8139_io_base + 0x30);
    __asm__ volatile("outl %0, %1" : : "a"(rx_phys), "d"(rx_port));

    outw(rtl8139_io_base + 0x3C, 0x007F);

    unsigned int rx_config = 0x0000068F;
    unsigned short rx_config_port = (unsigned short)(rtl8139_io_base + 0x44);
    __asm__ volatile("outl %0, %1" : : "a"(rx_config), "d"(rx_config_port));

    outb(rtl8139_io_base + 0x37, 0x0C);

    extern void rtl8139_isr_asm(void);
    extern void set_idt_gate(unsigned char vector, unsigned int isr_address);
    set_idt_gate(32 + irq, (unsigned int)rtl8139_isr_asm);

    if (irq < 8) {
        outb(0x21, inb(0x21) & ~(1 << irq));
    } else {
        outb(0xA1, inb(0xA1) & ~(1 << (irq - 8)));
    }
}

// --- SHELL COMMAND ENTRIES ---

void icmp_send_ping(unsigned char dest_ip[4]);

void net_ping(const char* cmd_args) {
    if (!rtl8139_present) {
        print_string_color("Error: RTL8139 not detected.\n", COLOR_LIGHT_RED);
        return;
    }
    unsigned char dest_ip[4];
    if (!parse_ip(cmd_args, dest_ip)) {
        print_string("Usage: ping <ip>\n");
        return;
    }
    icmp_send_ping(dest_ip);
}

void icmp_send_ping(unsigned char dest_ip[4]) {
    unsigned char packet[1500];
    int ip_len = sizeof(ipv4_header_t) + sizeof(icmp_header_t) + 32;
    int eth_len = sizeof(eth_header_t) + ip_len;

    eth_header_t* eth = (eth_header_t*)packet;
    memcpy(eth->src_mac, my_mac, 6);

    unsigned char dest_mac[6];
    if (!arp_resolve(dest_ip, dest_mac)) {
        arp_send_request(dest_ip);
        
        print_string("Resolving hardware address (ARP)...\n");
        int resolved = 0;
        for (volatile int delay = 0; delay < 50000000; delay++) {
            if (arp_resolve(dest_ip, dest_mac)) {
                resolved = 1;
                break;
            }
        }
        
        if (!resolved) {
            print_string_color("Error: ARP resolution failed. Host unreachable.\n", COLOR_LIGHT_RED);
            return;
        }
    }
    memcpy(eth->dest_mac, dest_mac, 6);
    eth->type = htons(0x0800);

    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(eth_header_t));
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len = htons(ip_len);
    ip->id = htons(2);
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = 1;
    ip->checksum = 0;
    memcpy(ip->src_ip, my_ip, 4);
    memcpy(ip->dest_ip, dest_ip, 4);
    ip->checksum = ip_checksum(ip, sizeof(ipv4_header_t));

    icmp_header_t* icmp = (icmp_header_t*)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(1234);
    icmp->seq = htons(1);

    unsigned char* payload = packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(icmp_header_t);
    for (int i = 0; i < 32; i++) {
        payload[i] = 'A' + (i % 26);
    }

    icmp->checksum = ip_checksum(icmp, sizeof(icmp_header_t) + 32);

    print_string("Sending ICMP Echo Request to ");
    print_int(dest_ip[0]); print_char('.');
    print_int(dest_ip[1]); print_char('.');
    print_int(dest_ip[2]); print_char('.');
    print_int(dest_ip[3]);
    print_string("...\n");

    rtl8139_send_packet(packet, eth_len);
}

void net_udp_send(const char* cmd_args) {
    if (!rtl8139_present) {
        print_string_color("Error: RTL8139 not detected.\n", COLOR_LIGHT_RED);
        return;
    }
    
    char ip_str[32];
    int i = 0;
    while (cmd_args[i] != ' ' && cmd_args[i] != '\0' && i < 31) {
        ip_str[i] = cmd_args[i];
        i++;
    }
    ip_str[i] = '\0';

    if (cmd_args[i] == '\0') {
        print_string("Usage: udpsend <ip> <port> <message>\n");
        return;
    }
    while (cmd_args[i] == ' ') i++;

    int port = 0;
    while (cmd_args[i] >= '0' && cmd_args[i] <= '9') {
        port = port * 10 + (cmd_args[i] - '0');
        i++;
    }

    if (cmd_args[i] == '\0') {
        print_string("Usage: udpsend <ip> <port> <message>\n");
        return;
    }
    while (cmd_args[i] == ' ') i++;
    const char* message = cmd_args + i;

    unsigned char dest_ip[4];
    if (!parse_ip(ip_str, dest_ip)) {
        print_string("Error: Invalid IP address format.\n");
        return;
    }

    print_string("Sending UDP payload to ");
    print_string(ip_str);
    print_string("...\n");

    int msg_len = 0;
    while (message[msg_len] != '\0') msg_len++;

    udp_send(dest_ip, 1234, port, (const unsigned char*)message, msg_len);
}
