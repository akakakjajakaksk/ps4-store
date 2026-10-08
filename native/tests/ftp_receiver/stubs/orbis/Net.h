#pragma once
#include <stddef.h>
#include <stdint.h>
enum OrbisNetType { ORBIS_NET_AF_INET = 2, ORBIS_NET_SOCK_STREAM = 1 };
typedef int32_t OrbisNetId;
typedef uint32_t OrbisNetSocklen_t;
struct OrbisNetSockaddr { uint8_t len, sa_family; char sa_data[14]; };
extern "C" {
int32_t sceNetInit();
int32_t sceNetPoolCreate(const char*, int32_t, int32_t);
void sceNetPoolDestroy(int32_t);
OrbisNetId sceNetSocket(const char*, int32_t, int32_t, int);
int32_t sceNetSetsockopt(OrbisNetId, int32_t, int32_t, const void*, OrbisNetSocklen_t);
int32_t sceNetBind(OrbisNetId, const OrbisNetSockaddr*, OrbisNetSocklen_t);
int32_t sceNetListen(OrbisNetId, int);
int32_t sceNetGetsockname(OrbisNetId, OrbisNetSockaddr*, OrbisNetSocklen_t*);
OrbisNetId sceNetAccept(OrbisNetId, OrbisNetSockaddr*, OrbisNetSocklen_t*);
int32_t sceNetRecv(OrbisNetId, void*, size_t, int);
int32_t sceNetSend(OrbisNetId, const void*, size_t, int);
int32_t sceNetSocketClose(OrbisNetId);
uint32_t sceNetHtonl(uint32_t);
uint32_t sceNetNtohl(uint32_t);
uint16_t sceNetHtons(uint16_t);
}
