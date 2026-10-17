#include "ethernet.h"
#include "pci.h"
#include "../kernel/config.h"

extern void kputs(const char* str, uint32_t color);
extern void kputc(char c, uint32_t color);
extern void itoa(int n, char* str);
extern void hex_str(uint32_t val, char* out);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern uint16_t inw(uint16_t port);
extern void outw(uint16_t port, uint16_t val);
extern uint32_t inl(uint16_t port);
extern void outl(uint16_t port, uint32_t val);
extern void sleep_ms(uint32_t ms);
extern uint32_t timer_ticks;
extern float get_uptime_seconds(void);
extern void kdebug(const char* str, uint32_t color);

typedef struct {
    volatile uint32_t opts1;
    volatile uint32_t opts2;
    volatile uint32_t buf_low;
    volatile uint32_t buf_high;
} __attribute__((packed)) rtl_desc_t;

#define NUM_DESC 16

static uint8_t ring_storage_tx[sizeof(rtl_desc_t) * NUM_DESC + 256];
static uint8_t ring_storage_rx[sizeof(rtl_desc_t) * NUM_DESC + 256];

static rtl_desc_t* tx_ring = 0;
static rtl_desc_t* rx_ring = 0;

static uint8_t tx_buffers[NUM_DESC][2048] __attribute__((aligned(128)));
static uint8_t rx_buffers[NUM_DESC][2048] __attribute__((aligned(128)));

static uint16_t rtl_io_base = 0;
static uint8_t  rtl_mac[6];
static int      tx_idx = 0;
static int      rx_idx = 0;
static int      rtl_initialized = 0;

static uint32_t my_ip            = 0xC0A8006A; // 192.168.0.106
static uint32_t gateway_ip       = 0xC0A80001; // 192.168.0.1
static uint8_t  gateway_mac[6]   = {0};
static int      gateway_resolved = 0;
static int      net_configured   = 0;

static inline uint16_t htons(uint16_t v) { return (v << 8) | (v >> 8); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF000000) >> 24) | ((v & 0x00FF0000) >> 8) |
           ((v & 0x0000FF00) << 8)  | ((v & 0x000000FF) << 24);
}
#define ntohs htons
#define ntohl htonl

static uint16_t net_checksum(void* data, size_t len) {
    uint32_t sum = 0;
    uint16_t* ptr = (uint16_t*)data;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1) sum += *(uint8_t*)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static inline uint32_t virt_to_phys(void* vaddr) {
    uintptr_t va = (uintptr_t)vaddr;
    if (va >= 0xFFFFFFFF80000000ULL) return (uint32_t)(va - 0xFFFFFFFF80000000ULL);
    if (va >= 0xFFFF800000000000ULL) return (uint32_t)(va - 0xFFFF800000000000ULL);
    if (va >= 0x80000000UL)          return (uint32_t)(va - 0x80000000UL);
    return (uint32_t)va;
}

static inline void flush_cache_line(volatile void* p) {
    __asm__ volatile ("clflush (%0)" : : "r"(p) : "memory");
}

static inline void flush_buffer_range(volatile void* p, size_t len) {
    uintptr_t addr = (uintptr_t)p;
    for (size_t i = 0; i < len; i += 64) {
        flush_cache_line((volatile void*)(addr + i));
    }
    __asm__ volatile ("mfence" ::: "memory");
}

static void print_mac(const uint8_t* m) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        char b[4];
        b[0] = hex[(m[i] >> 4) & 0xF];
        b[1] = hex[m[i] & 0xF];
        b[2] = (i < 5) ? ':' : '\0';
        b[3] = '\0';
        kputs(b, 0xFFFF55);
    }
}

static void dbg_print_mac(const uint8_t* m) {
    if (!DEBUG_MODE) return;
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        char b[4];
        b[0] = hex[(m[i] >> 4) & 0xF];
        b[1] = hex[m[i] & 0xF];
        b[2] = (i < 5) ? ':' : '\0';
        b[3] = '\0';
        kdebug(b, 0xFFFF55);
    }
}

static void print_ip(uint32_t ip) {
    char buf[8];
    for (int i = 3; i >= 0; i--) {
        uint8_t octet = (ip >> (i * 8)) & 0xFF;
        itoa(octet, buf);
        kputs(buf, 0xFFFFFF);
        if (i > 0) kputc('.', 0xAAAAAA);
    }
}

static void dbg_print_ip(uint32_t ip) {
    if (DEBUG_MODE) {
        char buf[8];
        for (int i = 3; i >= 0; i--) {
            uint8_t octet = (ip >> (i * 8)) & 0xFF;
            itoa(octet, buf);
            kputs(buf, 0xFFFFFF);
            if (i > 0) kputc('.', 0xAAAAAA);
        }
    }
}

static void enable_all_pci_bus_masters(void) {
    for (uint16_t b = 0; b < 8; b++) {
        for (uint8_t s = 0; s < 32; s++) {
            for (uint8_t f = 0; f < 8; f++) {
                uint32_t vd = pci_config_read(b, s, f, 0x00);
                if ((vd & 0xFFFF) == 0xFFFF) continue;
                uint32_t cr = pci_config_read(b, s, f, 0x08);
                uint8_t base_class = (cr >> 24) & 0xFF;
                if (base_class == 0x02 || base_class == 0x06) {
                    uint32_t cmd = pci_config_read(b, s, f, 0x04);
                    pci_config_write(b, s, f, 0x04, cmd | 0x07);
                }
            }
        }
    }
}

static void pci_wake_to_d0(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t status = pci_config_read(bus, slot, func, 0x04);
    if (!(status & (1U << 20))) return;

    uint8_t cap_ptr = pci_config_read(bus, slot, func, 0x34) & 0xFF;
    int limit = 48;
    while (cap_ptr >= 0x40 && limit--) {
        uint32_t cap_hdr = pci_config_read(bus, slot, func, cap_ptr);
        if ((cap_hdr & 0xFF) == 0x01) {
            uint32_t pmcsr = pci_config_read(bus, slot, func, cap_ptr + 4);
            if ((pmcsr & 0x03) != 0) {
                pci_config_write(bus, slot, func, cap_ptr + 4, pmcsr & ~0x03);
                sleep_ms(15);
            }
            return;
        }
        cap_ptr = (cap_hdr >> 8) & 0xFF;
    }
}

static int wait_link_ready(void) {
    kputs("[*] Waiting for Ethernet link...", 0xFFFF55);
    int attempts = 40;
    uint8_t phy = 0;

    while (attempts--) {
        phy = inb(rtl_io_base + 0x6C);
        if (phy & 0x02) break;
        sleep_ms(100);
    }

    if (!(phy & 0x02)) {
        kputs("\n[-] Link is DOWN. Check cable!\n", 0xFF5555);
        return 0;
    }

    sleep_ms(400);
    phy = inb(rtl_io_base + 0x6C);

    kputs(" UP! Speed: ", 0x55FF55);
    if (phy & 0x10)      kputs("1000 Mbps (Gigabit)\n", 0x55FFFF);
    else if (phy & 0x08) kputs("100 Mbps\n", 0x55FFFF);
    else                 kputs("Connected\n", 0x55FFFF);

    return 1;
}

int rtl8168_init(void) {
    if (rtl_initialized) return 1;

    enable_all_pci_bus_masters();

    uint8_t bus = 2, slot = 0, func = 1;
    uint32_t vd = pci_config_read(bus, slot, func, 0x00);

    if ((vd & 0xFFFF) != 0x10EC || ((vd >> 16) & 0xFFFF) != 0x8168) {
        int found = 0;
        for (uint16_t b = 0; b < 16 && !found; b++) {
            for (uint8_t s = 0; s < 32 && !found; s++) {
                for (uint8_t f = 0; f < 8; f++) {
                    uint32_t id = pci_config_read(b, s, f, 0x00);
                    if ((id & 0xFFFF) == 0x10EC && ((id >> 16) & 0xFFFF) == 0x8168) {
                        bus = b; slot = s; func = f;
                        found = 1;
                        break;
                    }
                }
            }
        }
        if (!found) return 0;
    }

    pci_wake_to_d0(bus, slot, func);

    uint32_t pci_cmd = pci_config_read(bus, slot, func, 0x04);
    pci_config_write(bus, slot, func, 0x04, pci_cmd | 0x07);

    uint32_t bar0 = pci_config_read(bus, slot, func, 0x10);
    rtl_io_base = (uint16_t)(bar0 & ~0x3);

    outb(rtl_io_base + 0x37, 0x10);
    int timeout = 1000;
    while ((inb(rtl_io_base + 0x37) & 0x10) && --timeout) {
        sleep_ms(1);
    }

    for (int i = 0; i < 6; i++) {
        rtl_mac[i] = inb(rtl_io_base + i);
    }

    uintptr_t raw_tx = (uintptr_t)ring_storage_tx;
    tx_ring = (rtl_desc_t*)((raw_tx + 255) & ~255);

    uintptr_t raw_rx = (uintptr_t)ring_storage_rx;
    rx_ring = (rtl_desc_t*)((raw_rx + 255) & ~255);

    for (int i = 0; i < NUM_DESC; i++) {
        tx_ring[i].opts1 = (i == NUM_DESC - 1) ? 0x40000000 : 0;
        tx_ring[i].opts2 = 0;
        tx_ring[i].buf_low = virt_to_phys(tx_buffers[i]);
        tx_ring[i].buf_high = 0;
        flush_cache_line(&tx_ring[i]);
    }

    for (int i = 0; i < NUM_DESC; i++) {
        uint32_t flags = 0x80000000 | 1536;
        if (i == NUM_DESC - 1) flags |= 0x40000000;
        rx_ring[i].opts1 = flags;
        rx_ring[i].opts2 = 0;
        rx_ring[i].buf_low = virt_to_phys(rx_buffers[i]);
        rx_ring[i].buf_high = 0;
        flush_cache_line(&rx_ring[i]);
    }

    outb(rtl_io_base + 0x50, 0xC0);
    outw(rtl_io_base + 0xE0, 0x0020);

    uint32_t tx_phys = virt_to_phys(tx_ring);
    uint32_t rx_phys = virt_to_phys(rx_ring);

    outl(rtl_io_base + 0x24, 0);
    outl(rtl_io_base + 0x20, tx_phys);

    outl(rtl_io_base + 0x2C, 0);
    outl(rtl_io_base + 0x28, tx_phys);

    outl(rtl_io_base + 0xE8, 0);
    outl(rtl_io_base + 0xE4, rx_phys);

    outw(rtl_io_base + 0xDA, 0x3FFF);
    outb(rtl_io_base + 0xEC, 0x3B);

    outl(rtl_io_base + 0x40, 0x03000500);
    outl(rtl_io_base + 0x44, 0x0000E50E);

    outb(rtl_io_base + 0x37, 0x0C);
    outb(rtl_io_base + 0x50, 0x00);

    outw(rtl_io_base + 0x3E, 0xFFFF);
    outw(rtl_io_base + 0x3C, 0x0000);

    tx_idx = 0;
    rx_idx = 0;
    rtl_initialized = 1;

    kdebug("[+] RTL8168 controller initialized. MAC: ", 0x55FF55);
    dbg_print_mac(rtl_mac);
    if (DEBUG_MODE) kputc('\n', 0xFFFFFF);

    return 1;
}

static int net_send_packet(void* packet, size_t len) {
    if (!rtl_initialized && !rtl8168_init()) return 0;

    uint8_t* dst = tx_buffers[tx_idx];
    uint8_t* src = (uint8_t*)packet;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];

    size_t wire_len = len;
    if (wire_len < 60) {
        for (size_t i = wire_len; i < 60; i++) dst[i] = 0;
        wire_len = 60;
    }

    tx_ring[tx_idx].buf_low  = virt_to_phys(tx_buffers[tx_idx]);
    tx_ring[tx_idx].buf_high = 0;
    tx_ring[tx_idx].opts2    = 0;

    uint32_t flags = 0x80000000 | 0x20000000 | 0x10000000 | (wire_len & 0x3FFF);
    if (tx_idx == NUM_DESC - 1) flags |= 0x40000000;

    tx_ring[tx_idx].opts1 = flags;

    flush_buffer_range(tx_buffers[tx_idx], wire_len);
    flush_cache_line(&tx_ring[tx_idx]);

    outw(rtl_io_base + 0x3E, 0xFFFF);
    outb(rtl_io_base + 0x38, 0x40);

    uint32_t wait = 10000000;
    while (--wait) {
        uint16_t isr = inw(rtl_io_base + 0x3E);
        if (isr & 0x0004) {
            outw(rtl_io_base + 0x3E, 0x0004);
            break;
        }

        flush_cache_line(&tx_ring[tx_idx]);
        if (!(tx_ring[tx_idx].opts1 & 0x80000000)) {
            break;
        }
        __asm__ volatile ("pause" ::: "memory");
    }

    if (wait == 0) return 0;

    tx_idx = (tx_idx + 1) % NUM_DESC;
    return 1;
}

static int net_poll_packet(uint8_t* out_buf, size_t max_len) {
    if (!rtl_initialized) return 0;

    flush_cache_line(&rx_ring[rx_idx]);

    if (!(rx_ring[rx_idx].opts1 & 0x80000000)) {
        uint32_t opts = rx_ring[rx_idx].opts1;
        int len = (opts & 0x3FFF) - 4;
        if (len > (int)max_len) len = max_len;

        if (len > 0) {
            flush_buffer_range(rx_buffers[rx_idx], len);
            uint8_t* src = rx_buffers[rx_idx];
            for (int i = 0; i < len; i++) out_buf[i] = src[i];
        }

        uint32_t flags = 0x80000000 | 1536;
        if (rx_idx == NUM_DESC - 1) flags |= 0x40000000;

        rx_ring[rx_idx].opts2 = 0;
        rx_ring[rx_idx].opts1 = flags;
        flush_cache_line(&rx_ring[rx_idx]);

        // Сбрасываем флаги приема и переполнения в ISR
        outw(rtl_io_base + 0x3E, 0x0031);

        rx_idx = (rx_idx + 1) % NUM_DESC;
        return len;
    }
    return 0;
}

// Очистка очереди от скопившегося фонового шума
static void net_drain_rx(void) {
    uint8_t dummy[1536];
    int count = 0;
    while (net_poll_packet(dummy, sizeof(dummy)) > 0 && count < 32) {
        count++;
    }
}

static void send_arp(uint16_t opcode, const uint8_t* target_mac, uint32_t target_ip) {
    uint8_t frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t* eth = (eth_hdr_t*)frame;
    arp_pkt_t* arp = (arp_pkt_t*)(frame + sizeof(eth_hdr_t));

    for (int i = 0; i < 6; i++) {
        eth->dest_mac[i] = (opcode == 1) ? 0xFF : target_mac[i];
        eth->src_mac[i]  = rtl_mac[i];
        arp->src_mac[i]  = rtl_mac[i];
        arp->dst_mac[i]  = (opcode == 1) ? 0x00 : target_mac[i];
    }
    eth->ethertype = htons(0x0806);

    arp->hw_type    = htons(0x0001);
    arp->proto_type = htons(0x0800);
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->opcode     = htons(opcode);
    arp->src_ip     = htonl(my_ip);
    arp->dst_ip     = htonl(target_ip);

    net_send_packet(frame, sizeof(frame));
}

// Настойчивый запрос MAC роутера через ARP
static int resolve_gateway_arp(void) {
    if (gateway_resolved) return 1;

    kputs("[*] Resolving Gateway MAC via ARP...", 0xFFFF55);

    uint8_t rx_buf[1536];

    for (int attempt = 1; attempt <= 4; attempt++) {
        net_drain_rx();
        send_arp(1, 0, gateway_ip);

        uint32_t start = timer_ticks;
        while ((timer_ticks - start) < 800) {
            int len = net_poll_packet(rx_buf, sizeof(rx_buf));
            if (len >= (int)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t))) {
                eth_hdr_t* r_eth = (eth_hdr_t*)rx_buf;
                if (ntohs(r_eth->ethertype) == 0x0806) {
                    arp_pkt_t* r_arp = (arp_pkt_t*)(rx_buf + sizeof(eth_hdr_t));
                    if (ntohs(r_arp->opcode) == 2 && ntohl(r_arp->src_ip) == gateway_ip) {
                        for (int i = 0; i < 6; i++) gateway_mac[i] = r_arp->src_mac[i];
                        gateway_resolved = 1;
                        kputs(" Found: ", 0x55FF55);
                        print_mac(gateway_mac);
                        kputc('\n', 0xFFFFFF);
                        return 1;
                    }
                }
            }
            sleep_ms(1);
        }
    }

    kputs(" FAILED!\n", 0xFF5555);
    return 0;
}

int net_auto_dhcp(void) {
    if (net_configured) return 1;
    if (!rtl_initialized && !rtl8168_init()) return 0;

    net_drain_rx();
    kdebug("[*] DHCP: Discovering IP address...\n", 0x55FFFF);

    uint8_t packet[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_pkt_t)];
    for (size_t i = 0; i < sizeof(packet); i++) packet[i] = 0;

    eth_hdr_t*  eth  = (eth_hdr_t*)packet;
    ipv4_hdr_t* ip   = (ipv4_hdr_t*)(packet + sizeof(eth_hdr_t));
    udp_hdr_t*  udp  = (udp_hdr_t*)(packet + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
    dhcp_pkt_t* dhcp = (dhcp_pkt_t*)(packet + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t));

    for (int i = 0; i < 6; i++) {
        eth->dest_mac[i] = 0xFF;
        eth->src_mac[i]  = rtl_mac[i];
    }
    eth->ethertype = htons(0x0800);

    ip->ihl_ver   = 0x45;
    ip->total_len = htons(sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_pkt_t));
    ip->ttl       = 64;
    ip->protocol  = 17;
    ip->src_ip    = 0;
    ip->dst_ip    = 0xFFFFFFFF;
    ip->checksum  = 0;
    ip->checksum  = net_checksum(ip, sizeof(ipv4_hdr_t));

    udp->src_port = htons(68);
    udp->dst_port = htons(67);
    udp->length   = htons(sizeof(udp_hdr_t) + sizeof(dhcp_pkt_t));
    udp->checksum = 0;

    dhcp->op    = 1;
    dhcp->htype = 1;
    dhcp->hlen  = 6;
    dhcp->xid   = htonl(0x3903F326);
    dhcp->flags = htons(0x8000);
    for (int i = 0; i < 6; i++) dhcp->chaddr[i] = rtl_mac[i];
    dhcp->magic = htonl(0x63825363);

    int opt = 0;
    dhcp->options[opt++] = 53;
    dhcp->options[opt++] = 1;
    dhcp->options[opt++] = 1;
    dhcp->options[opt++] = 55;
    dhcp->options[opt++] = 2;
    dhcp->options[opt++] = 1;
    dhcp->options[opt++] = 3;
    dhcp->options[opt++] = 255;

    net_send_packet(packet, sizeof(packet));

    uint8_t rx_buf[1536];
    uint32_t start = timer_ticks;

    while ((timer_ticks - start) < 800) {
        int len = net_poll_packet(rx_buf, sizeof(rx_buf));
        if (len >= (int)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + 240)) {
            eth_hdr_t* r_eth = (eth_hdr_t*)rx_buf;
            if (ntohs(r_eth->ethertype) == 0x0800) {
                ipv4_hdr_t* r_ip = (ipv4_hdr_t*)(rx_buf + sizeof(eth_hdr_t));
                if (r_ip->protocol == 17) {
                    udp_hdr_t* r_udp = (udp_hdr_t*)(rx_buf + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
                    if (ntohs(r_udp->dst_port) == 68) {
                        dhcp_pkt_t* r_dhcp = (dhcp_pkt_t*)(rx_buf + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t));
                        if (r_dhcp->yiaddr != 0) {
                            my_ip = ntohl(r_dhcp->yiaddr);
                            for (int i = 0; i < 6; i++) gateway_mac[i] = r_eth->src_mac[i];
                            gateway_resolved = 1;

                            uint8_t* opts = r_dhcp->options;
                            int idx = 0;
                            while (idx < 60 && opts[idx] != 255) {
                                uint8_t code = opts[idx++];
                                if (code == 0) continue;
                                uint8_t olen = opts[idx++];
                                if (code == 3 && olen >= 4) {
                                    gateway_ip = ((uint32_t)opts[idx] << 24) |
                                                 ((uint32_t)opts[idx+1] << 16) |
                                                 ((uint32_t)opts[idx+2] << 8) |
                                                 (uint32_t)opts[idx+3];
                                }
                                idx += olen;
                            }

                            if (gateway_ip == 0) gateway_ip = ntohl(r_ip->src_ip);

                            net_configured = 1;
                            kdebug("[+] Network Configured via DHCP!\n", 0x55FF55);
                            return 1;
                        }
                    }
                }
            }
        }
        sleep_ms(1);
    }

    // Fallback: адрес известен, разрешаем MAC шлюза
    net_configured = 1;
    resolve_gateway_arp();
    return 1;
}

static uint32_t parse_ip(const char* str) {
    uint32_t ip = 0;
    int octet = 0;
    while (*str) {
        if (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (*str - '0');
        } else if (*str == '.') {
            ip = (ip << 8) | (octet & 0xFF);
            octet = 0;
        }
        str++;
    }
    return (ip << 8) | (octet & 0xFF);
}

// Список глобальных проверочных серверов (Cloudflare, Google, Yandex, Quad9)
static const uint32_t PROBE_SERVERS[4] = {
    0x01010101, // 1.1.1.1
    0x08080808, // 8.8.8.8
    0x4D580808, // 77.88.8.8
    0x09090909  // 9.9.9.9
};

// Проверка: является ли IP внешним глобальным сервером
static inline int is_external_ip(uint32_t ip) {
    for (int i = 0; i < 4; i++) {
        if (ip == PROBE_SERVERS[i]) return 1;
    }
    // Любой IP за пределами нашей домашней подсети
    if ((ip & 0xFFFFFF00) != (my_ip & 0xFFFFFF00)) return 1;
    return 0;
}

static uint32_t s_last_probe_sent_tick = 0;
static uint32_t s_last_external_pong_tick = 0;

// Отправка одиночного ICMP эхо-запроса
static void send_icmp_probe(uint32_t target_ip) {
    if (!gateway_resolved) return;

    uint8_t pkt[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 16];
    for (size_t k = 0; k < sizeof(pkt); k++) pkt[k] = 0;

    eth_hdr_t*  e = (eth_hdr_t*)pkt;
    ipv4_hdr_t* i = (ipv4_hdr_t*)(pkt + sizeof(eth_hdr_t));
    icmp_hdr_t* c = (icmp_hdr_t*)(pkt + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));

    for (int k = 0; k < 6; k++) {
        e->dest_mac[k] = gateway_mac[k];
        e->src_mac[k]  = rtl_mac[k];
    }
    e->ethertype = htons(0x0800);

    i->ihl_ver    = 0x45;
    i->tos        = 0;
    i->total_len  = htons(sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 16);
    i->id         = htons(0x7700);
    i->flags_frag = 0;
    i->ttl        = 64;
    i->protocol   = 1;
    i->src_ip     = htonl(my_ip);
    i->dst_ip     = htonl(target_ip);
    i->checksum   = 0;
    i->checksum   = net_checksum(i, sizeof(ipv4_hdr_t));

    c->type     = 8; // Echo Request
    c->code     = 0;
    c->id       = htons(0xBEEF);
    c->seq      = htons(1);
    c->checksum = 0;
    for (int k = 0; k < 16; k++) (pkt + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t))[k] = 'Q';
    c->checksum = net_checksum(c, sizeof(icmp_hdr_t) + 16);

    net_send_packet(pkt, sizeof(pkt));
}

// Залповая отправка запросов на группу серверов
static void send_multi_probes(void) {
    send_icmp_probe(0x01010101); // Cloudflare 1.1.1.1
    send_icmp_probe(0x08080808); // Google 8.8.8.8
    send_icmp_probe(0x4D580808); // Yandex 77.88.8.8
}

// Честный опрос 3 состояний сети
int net_get_status_info(void* out_buf) {
    if (!out_buf) return -1;

    typedef struct {
        uint32_t state;
        uint32_t ip;
        uint32_t gateway_ip;
        uint16_t link_speed;
    } __attribute__((packed)) k_net_info_t;

    k_net_info_t* info = (k_net_info_t*)out_buf;

    // Если сетевая карта физически отсутствует — возвращаем ошибку (-1)!
    if (!rtl_initialized && !rtl8168_init()) {
        info->state = 0;
        return -1;
    }

    // 1. Аппаратный статус разъема RJ45
    uint8_t phy = inb(rtl_io_base + 0x6C);
    if (!(phy & 0x02)) {
        info->state = 0; // СОСТОЯНИЕ 0: Кабель отключен
        info->ip = 0;
        info->gateway_ip = 0;
        info->link_speed = 0;
        net_configured = 0;
        gateway_resolved = 0;
        return 0; // Драйвер работает, но кабеля нет
    }

    if (phy & 0x10)      info->link_speed = 1000;
    else if (phy & 0x08) info->link_speed = 100;
    else                 info->link_speed = 10;

    if (!net_configured) {
        net_auto_dhcp();
    }

    if (!gateway_resolved) {
        resolve_gateway_arp();
    }

    info->ip = my_ip;
    info->gateway_ip = gateway_ip;

    // 2. Фоновая вычитка очереди
    uint8_t rx_tmp[1536];
    int rlen;
    while ((rlen = net_poll_packet(rx_tmp, sizeof(rx_tmp))) > 0) {
        eth_hdr_t* eth = (eth_hdr_t*)rx_tmp;

        if (ntohs(eth->ethertype) == 0x0806 && rlen >= (int)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t))) {
            arp_pkt_t* arp = (arp_pkt_t*)(rx_tmp + sizeof(eth_hdr_t));
            if (ntohs(arp->opcode) == 1 && ntohl(arp->dst_ip) == my_ip) {
                send_arp(2, arp->src_mac, ntohl(arp->src_ip));
            }
        }
        else if (ntohs(eth->ethertype) == 0x0800 && rlen >= (int)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t))) {
            ipv4_hdr_t* ip = (ipv4_hdr_t*)(rx_tmp + sizeof(eth_hdr_t));
            if (ip->protocol == 1) {
                icmp_hdr_t* icmp = (icmp_hdr_t*)(rx_tmp + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
                if (icmp->type == 0 && is_external_ip(ntohl(ip->src_ip))) {
                    s_last_external_pong_tick = timer_ticks;
                }
            }
        }
    }

    // 3. Отправка мульти-зонда каждые 3.5 секунды
    if (net_configured && gateway_resolved) {
        if ((timer_ticks - s_last_probe_sent_tick) >= 3500 || s_last_probe_sent_tick == 0) {
            s_last_probe_sent_tick = timer_ticks;
            send_multi_probes();
        }
    }

    // 4. Финальный статус
    if (net_configured && s_last_external_pong_tick != 0 && (timer_ticks - s_last_external_pong_tick < 15000)) {
        info->state = 2; // NET_STATE_ONLINE
    } else if (net_configured) {
        info->state = 1; // NET_STATE_LOCAL_ONLY
    } else {
        info->state = 0; // NET_STATE_DISCONNECTED
    }

    return 0;
}

// Запуск сети на этапе загрузки ядра
int net_bootstrap(void) {
    kdebug("[*] Initializing Network Subsystem...\n", 0x55FFFF);

    // 1. Проверяем наличие и инициализацию чипа на шине PCI
    if (!rtl8168_init()) {
        kdebug("[-] Network: RTL8168 controller not detected.\n", 0xFF5555);
        return -1; // Контроллера нет на плате — честный FAIL
    }

    // Железо найдено и готово к работе!
    kdebug("[+] Network: RTL8168 hardware adapter initialized.\n", 0x55FF55);

    // 2. Проверяем физический линк (подключен ли кабель)
    uint8_t phy = inb(rtl_io_base + 0x6C);
    if (!(phy & 0x02)) {
        kdebug("[!] Network: Cable is disconnected. Skipping DHCP.\n", 0xFFFF55);
        // Адаптер инициализирован, просто нет кабеля. Возвращаем 0 (OK)!
        return 0;
    }

    // 3. Кабель подключен — запрашиваем IP по DHCP
    if (net_auto_dhcp()) {
        kdebug("[+] Network ready: IP ", 0x55FF55);
        dbg_print_ip(my_ip);
        kdebug(" (Gateway ", 0xAAAAAA);
        dbg_print_ip(gateway_ip);
        kdebug(")\n", 0xAAAAAA);

        if (gateway_resolved) {
            send_multi_probes();
            uint8_t rx_b[1536];
            uint32_t t0 = timer_ticks;
            while ((timer_ticks - t0) < 180) {
                int rlen = net_poll_packet(rx_b, sizeof(rx_b));
                if (rlen >= (int)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t))) {
                    eth_hdr_t* e = (eth_hdr_t*)rx_b;
                    if (ntohs(e->ethertype) == 0x0800) {
                        ipv4_hdr_t* ip = (ipv4_hdr_t*)(rx_b + sizeof(eth_hdr_t));
                        if (ip->protocol == 1) {
                            icmp_hdr_t* icmp = (icmp_hdr_t*)(rx_b + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
                            if (icmp->type == 0 && is_external_ip(ntohl(ip->src_ip))) {
                                s_last_external_pong_tick = timer_ticks;
                                break;
                            }
                        }
                    }
                }
                sleep_ms(1);
            }
        }
    }

    return 0; // Инициализация подсистемы успешна
}

// Консольный ping в TTY
void cmd_ping(const char* target_ip_str) {
    if (!target_ip_str || !target_ip_str[0]) {
        target_ip_str = "1.1.1.1";
    }

    uint32_t target_ip = parse_ip(target_ip_str);
    if (!net_configured && !net_auto_dhcp()) return;

    if (!gateway_resolved && !resolve_gateway_arp()) {
        kputs("[-] Ping aborted: Router MAC is unknown.\n", 0xFF5555);
        return;
    }

    kputs("PING ", 0x55FFFF);
    kputs(target_ip_str, 0xFFFFFF);
    kputs(" (payload 32 bytes):\n", 0xAAAAAA);

    uint8_t packet[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 32];
    eth_hdr_t*  eth  = (eth_hdr_t*)packet;
    ipv4_hdr_t* ip   = (ipv4_hdr_t*)(packet + sizeof(eth_hdr_t));
    icmp_hdr_t* icmp = (icmp_hdr_t*)(packet + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
    uint8_t*    data = packet + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t);

    for (int seq = 1; seq <= 4; seq++) {
        for (int i = 0; i < 6; i++) {
            eth->dest_mac[i] = gateway_mac[i];
            eth->src_mac[i]  = rtl_mac[i];
        }
        eth->ethertype = htons(0x0800);

        ip->ihl_ver    = 0x45;
        ip->tos        = 0;
        ip->total_len  = htons(sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 32);
        ip->id         = htons(0x7100 + seq);
        ip->flags_frag = 0;
        ip->ttl        = 64;
        ip->protocol   = 1;
        ip->src_ip     = htonl(my_ip);
        ip->dst_ip     = htonl(target_ip);
        ip->checksum   = 0;
        ip->checksum   = net_checksum(ip, sizeof(ipv4_hdr_t));

        icmp->type     = 8; // Echo Request
        icmp->code     = 0;
        icmp->id       = htons(0xBEEF);
        icmp->seq      = htons(seq);
        icmp->checksum = 0;

        for (int i = 0; i < 32; i++) data[i] = (uint8_t)('A' + (i % 26));
        icmp->checksum = net_checksum(icmp, sizeof(icmp_hdr_t) + 32);

        float send_time = get_uptime_seconds();
        if (!net_send_packet(packet, sizeof(packet))) break;

        int received = 0;
        uint8_t rx_buf[1536];
        uint32_t t_start = timer_ticks;

        while ((timer_ticks - t_start) < 1500) {
            int len = net_poll_packet(rx_buf, sizeof(rx_buf));
            if (len >= (int)sizeof(eth_hdr_t)) {
                eth_hdr_t* r_eth = (eth_hdr_t*)rx_buf;

                if (ntohs(r_eth->ethertype) == 0x0806 && len >= (int)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t))) {
                    arp_pkt_t* r_arp = (arp_pkt_t*)(rx_buf + sizeof(eth_hdr_t));
                    if (ntohs(r_arp->opcode) == 1 && ntohl(r_arp->dst_ip) == my_ip) {
                        send_arp(2, r_arp->src_mac, ntohl(r_arp->src_ip));
                    }
                }
                else if (ntohs(r_eth->ethertype) == 0x0800 && len >= (int)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t))) {
                    ipv4_hdr_t* r_ip = (ipv4_hdr_t*)(rx_buf + sizeof(eth_hdr_t));
                    if (r_ip->protocol == 1) {
                        icmp_hdr_t* r_icmp = (icmp_hdr_t*)(rx_buf + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
                        
                        // Ответ от внешнего хоста продлевает онлайн десктопа
                        if (r_icmp->type == 0 && is_external_ip(ntohl(r_ip->src_ip))) {
                            s_last_external_pong_tick = timer_ticks;
                        }

                        if (ntohl(r_ip->src_ip) == target_ip && r_icmp->type == 0 && ntohs(r_icmp->seq) == seq) {
                            float rtt = (get_uptime_seconds() - send_time) * 1000.0f;
                            char nb[16];

                            kputs("64 bytes from ", 0x55FF55);
                            kputs(target_ip_str, 0xFFFFFF);
                            kputs(": icmp_seq=", 0xAAAAAA);
                            itoa(seq, nb); kputs(nb, 0xFFFFFF);
                            kputs(" ttl=", 0xAAAAAA);
                            itoa(r_ip->ttl, nb); kputs(nb, 0xFFFFFF);
                            kputs(" time=", 0xAAAAAA);
                            itoa((int)rtt, nb); kputs(nb, 0x55FFFF);
                            kputs(" ms\n", 0x55FFFF);

                            received = 1;
                            break;
                        }
                    }
                }
            }
            sleep_ms(1);
        }

        if (!received) {
            char nb[16];
            kputs("Request timeout for icmp_seq ", 0xFF5555);
            itoa(seq, nb); kputs(nb, 0xFFFFFF);
            kputc('\n', 0xFFFFFF);
        }

        sleep_ms(400);
    }
}

// Режим эхо-сервера: отвечает на ARP и Ping с других компьютеров
void cmd_pong(void) {
    if (!net_configured && !net_auto_dhcp()) return;

    kputs("[*] Echo server started! Waiting for pings from local network...\n", 0x55FFFF);
    kputs("    IP: ", 0xAAAAAA); print_ip(my_ip);
    kputs(" (Press ESC to stop)\n\n", 0xAAAAAA);

    uint8_t rx_buf[1536];

    while (1) {
        // Выход по клавише ESC (сканкод 0x01)
        if (inb(0x64) & 1) {
            if (inb(0x60) == 0x01) {
                kputs("\n[*] Echo server stopped.\n", 0xFFFF55);
                break;
            }
        }

        int len = net_poll_packet(rx_buf, sizeof(rx_buf));
        if (len < (int)sizeof(eth_hdr_t)) {
            sleep_ms(1);
            continue;
        }

        eth_hdr_t* eth = (eth_hdr_t*)rx_buf;

        // 1. Отвечаем на ARP-запрос ПК ("Кто такой 192.168.0.106?")
        if (ntohs(eth->ethertype) == 0x0806 && len >= (int)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t))) {
            arp_pkt_t* arp = (arp_pkt_t*)(rx_buf + sizeof(eth_hdr_t));
            if (ntohs(arp->opcode) == 1 && ntohl(arp->dst_ip) == my_ip) {
                send_arp(2, arp->src_mac, ntohl(arp->src_ip));
                kputs("[ARP] Told ", 0x55FF55);
                print_ip(ntohl(arp->src_ip));
                kputs(" our MAC\n", 0xAAAAAA);
            }
        }

        // 2. Отвечаем на ICMP Echo Request (Ping)
        else if (ntohs(eth->ethertype) == 0x0800 && len >= (int)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t))) {
            ipv4_hdr_t* ip = (ipv4_hdr_t*)(rx_buf + sizeof(eth_hdr_t));

            // Проверяем, что пакет адресован нам и это ICMP
            if (ip->protocol == 1 && ntohl(ip->dst_ip) == my_ip) {
                icmp_hdr_t* icmp = (icmp_hdr_t*)(rx_buf + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));

                // Type 8 = Echo Request
                if (icmp->type == 8) {
                    uint32_t sender_ip = ntohl(ip->src_ip);

                    // Меняем местами MAC-адреса Ethernet
                    for (int i = 0; i < 6; i++) {
                        eth->dest_mac[i] = eth->src_mac[i];
                        eth->src_mac[i]  = rtl_mac[i];
                    }

                    // Меняем местами IP-адреса
                    ip->dst_ip = ip->src_ip;
                    ip->src_ip = htonl(my_ip);
                    ip->ttl    = 64;
                    ip->checksum = 0;
                    ip->checksum = net_checksum(ip, sizeof(ipv4_hdr_t));

                    // Превращаем Request (8) в Reply (0)
                    icmp->type = 0;
                    icmp->checksum = 0;
                    size_t icmp_len = len - sizeof(eth_hdr_t) - sizeof(ipv4_hdr_t);
                    icmp->checksum = net_checksum(icmp, icmp_len);

                    // Отправляем эхо-ответ обратно на ПК
                    net_send_packet(rx_buf, len);

                    char seq_buf[16];
                    itoa(ntohs(icmp->seq), seq_buf);
                    kputs("[PING] Replied to ", 0x55FF55);
                    print_ip(sender_ip);
                    kputs(" | icmp_seq=", 0xAAAAAA);
                    kputs(seq_buf, 0xFFFF55);
                    kputc('\n', 0xFFFFFF);
                }
            }
        }
    }
}