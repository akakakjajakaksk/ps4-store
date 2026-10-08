#pragma once
#include <stdint.h>
const int ORBIS_NET_CTL_INFO_IP_ADDRESS = 14;
union OrbisNetCtlInfo { char ip_address[16]; };
extern "C" {
int32_t sceNetCtlInit();
int32_t sceNetCtlGetInfo(int, OrbisNetCtlInfo*);
}
