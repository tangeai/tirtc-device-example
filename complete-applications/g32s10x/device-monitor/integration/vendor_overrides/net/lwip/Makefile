src-y += arch/sys_arch.c

# Keep the product network image focused on IPv4 STA transport, while retaining
# the ALTCP, MQTT and TLS sources required by the TiRTC cloud control channel.
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
	core/altcp.c \
	core/altcp_alloc.c \
	core/altcp_tcp.c \
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

LWIP_TIRTC_CLOUD = \
	apps/mqtt/mqtt.c \
	apps/altcp_tls/altcp_tls_mbedtls.c \
	apps/altcp_tls/altcp_tls_mbedtls_mem.c

src-y += $(LWIP_CORE) $(LWIP_IPV4) $(LWIP_API) $(LWIP_TIRTC_CLOUD) \
	netif/ethernet.c
