src-y += arch/sys_arch.c

# IPv4 STA transport only: core TCP/UDP, DHCP client, DNS, sockets/netconn and
# Ethernet netif. TiRTC performs TLS through MbedTLS net sockets directly.
LWIP_CORE = \
	core/init.c \
	core/def.c \
	core/dns.c \
	core/inet_chksum.c \
	core/ip.c \
	core/mem.c \
	core/memp.c \
	core/netif.c \
	core/pbuf.c \
	core/raw.c \
	core/stats.c \
	core/sys.c \
	core/tcp.c \
	core/tcp_in.c \
	core/tcp_out.c \
	core/timeouts.c \
	core/udp.c

LWIP_IPV4 = \
	core/ipv4/acd.c \
	core/ipv4/dhcp.c \
	core/ipv4/etharp.c \
	core/ipv4/icmp.c \
	core/ipv4/igmp.c \
	core/ipv4/ip4_frag.c \
	core/ipv4/ip4.c \
	core/ipv4/ip4_addr.c

LWIP_API = \
	api/api_lib.c \
	api/api_msg.c \
	api/err.c \
	api/if_api.c \
	api/netbuf.c \
	api/netdb.c \
	api/netifapi.c \
	api/sockets.c \
	api/tcpip.c

src-y += $(LWIP_CORE) $(LWIP_IPV4) $(LWIP_API) netif/ethernet.c
