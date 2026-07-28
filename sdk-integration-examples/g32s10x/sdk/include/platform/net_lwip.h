#ifndef __net_lwip_h__
#define __net_lwip_h__

#define __LWIP__

//#include "basedef.h"
//#include <lwip/opt.h>
//#include <lwip/init.h>

//#include <lwip/mem.h>
//#include <lwip/memp.h>
//#include <lwip/sys.h>
//#include <lwip/stats.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/icmp.h>
//#include <lwip/tcpip.h>
//#include <lwip/igmp.h>

#ifdef __cplusplus
extern "C" {
#endif

#if 0
//#define malloc mem_malloc
//#define calloc mem_calloc
//#define free mem_free
//void *mem_realloc(void *ptr, int new_size);
//#define realloc mem_realloc
#endif

/*
 * SOCKET Functions
 */
#define SA_NetLibInit() lwip_socket_init()
#define SA_NetLibUninit()

#define SA_SOCKET	int
#define SA_INVALID_SOCKET	-1
#define SA_SocketIsValid(s) (s>=0)
int SA_SocketGetError(int s); //errno  //get_socket(s)->err
void SA_SocketSetError(int s, int err);
#define SA_SOCKET_ERROR	-1	//return value of socket operations


#define SA_SocketClose lwip_close
#define SA_Send lwip_send
#define SA_SendTo lwip_sendto
#define SA_Recv lwip_recv
#define SA_RecvFrom(s, buf, size, flags, paddr, paddr_len) lwip_recvfrom(s, buf, size, flags, paddr, (socklen_t*)paddr_len)
#define SA_GetSockName(s, paddr, paddr_len) lwip_getsockname(s, paddr, (socklen_t*)paddr_len)
#define SA_GetPeerName(s, paddr, paddr_len) lwip_getpeername(s, paddr, (socklen_t*)paddr_len)
#define SA_Accept(s, paddr, paddr_len) lwip_accept(s, paddr, (socklen_t*)paddr_len)
#define SA_GetSockOpt(s, level, optname, optval, optlen) lwip_getsockopt(s, level, optname, optval, (socklen_t*)optlen)
#define SA_SetSockOpt lwip_setsockopt
#define SA_shutdown lwip_shutdown


int SA_SocketSetNBlk(SA_SOCKET s, SA_BOOL b);
int SA_SocketSetLinger(SA_SOCKET s, int onoff, int linger);

#define SA_IOVEC	struct iovec
#define SA_IoVecGetPtr(pvec) ((pvec)->iov_base)
#define SA_IoVecGetLen(pvec) ((pvec)->iov_len)
#define SA_IoVecSetPtr(pvec, ptr) (pvec)->iov_base = (void*)(ptr)
#define SA_IoVecSetLen(pvec, len) (pvec)->iov_len = len

char *SA_SocketStrError(int err);

#ifdef __cplusplus
}
#endif

#endif	//#ifndef __net_lwip_h
