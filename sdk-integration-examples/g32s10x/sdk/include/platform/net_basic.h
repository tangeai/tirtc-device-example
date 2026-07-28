#pragma once

#include "platforms.h"

#ifdef __cplusplus
extern "C" {
#endif
extern SA_BOOL SA_resolve_host(const char* host, uint32_t* pIP);
char* SA_ip2str(unsigned int ip, char ips[16]);
//* shost can be like:  xx.xx.xxx:port
extern int SA_initsai(struct sockaddr_in* sai, const char* shost, unsigned short def_port);

//bind_ip: ip to bind to. network byte order
//port: port to bind. host byte order
int SA_sock_newbind(int sock_type, uint32_t bind_ip, unsigned short port);
//call listen() if sock_type is SOCK_STREAM
int SA_sock_newserver(int sock_type, uint32_t bind_ip, unsigned short port);

SA_BOOL SA_get_gateway(unsigned int *ip_gw, unsigned int *ip_local);

SA_BOOL SA_is_internet(struct in_addr addr);

typedef struct ether_nic {
#ifdef WIN32
	int ifIdx;
#elif defined(__LINUX__) || defined(__MAC_OS__) || defined(__LITEOS__) || defined(__ANDROID__) || defined(__LWIP__) || defined(__ALI_OS__)
	char name[16];//name[IFNAMSIZ];
	int flags;
    //int mtu;
#endif
	struct in_addr addr;
	struct in_addr netmask;
} ETHERNIC;
int SA_getip(ETHERNIC *pAN, int size);

#define SA_POLLIN   0x01
#define SA_POLLOUT  0x02
#define SA_POLLERR  0x04
int SA_pollfd(SA_SOCKET fd, int *events, int ms);
//wait to be readable
int SA_wait_timed(SA_SOCKET fd, int ms);
//wait to be writable
int SA_wait_timed_w(SA_SOCKET fd, int ms);
int SA_recv_timed(SA_SOCKET sk, void* ptr, int size, int ms);

int SA_recvfrom_timed(SA_SOCKET sk, void* ptr, int size, struct sockaddr* addr, unsigned int *sock_len, int ms);
int SA_setnblk(SA_SOCKET s, SA_BOOL b);
int SA_setlinger(SA_SOCKET s, int onoff, int linger);

//for socket
int SA_wouldblock(int err);
int SA_inprogress(int err);

#ifdef __cplusplus
}
#endif
