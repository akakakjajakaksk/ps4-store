#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int OrbisPthread;
typedef int OrbisPthreadAttr;
enum OrbisSysModuleInternal { ORBIS_SYSMODULE_INTERNAL_NET=0x8000001c,
 ORBIS_SYSMODULE_INTERNAL_SSL=0x8000000b,ORBIS_SYSMODULE_INTERNAL_HTTP=0x8000000a };
enum {ORBIS_HTTP_VERSION_1_1=2,ORBIS_METHOD_GET=0,ORBIS_HTTP_CONTENTLEN_EXIST=0};
extern "C" {
int32_t sceKernelUsleep(uint32_t);
int32_t scePthreadAttrInit(OrbisPthreadAttr*);
int32_t scePthreadAttrDestroy(OrbisPthreadAttr*);
int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr*,int);
int32_t scePthreadCreate(OrbisPthread*,const OrbisPthreadAttr*,void*(*)(void*),void*,const char*);
uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal);
int32_t sceNetInit();
int32_t sceNetPoolCreate(const char*,int32_t,int32_t);
void sceNetPoolDestroy(int32_t);
int32_t sceSslInit(size_t);
void sceSslTerm();
int32_t sceHttpInit(int32_t,int32_t,size_t);
int32_t sceHttpCreateTemplate(int32_t,const char*,int32_t,int32_t);
int32_t sceHttpsEnableOption(int32_t,uint32_t);
void sceHttpSetAutoRedirect();
void sceHttpSetRecvTimeOut();
void sceHttpsGetSslError();
int32_t sceHttpSetResolveTimeOut(int32_t,uint32_t);
int32_t sceHttpSetConnectTimeOut(int32_t,uint32_t);
int32_t sceHttpSetSendTimeOut(int32_t,uint32_t);
int32_t sceHttpCreateConnectionWithURL(int32_t,const char*,bool);
int32_t sceHttpCreateRequestWithURL(int32_t,int32_t,const char*,uint64_t);
int32_t sceHttpAddRequestHeader(int32_t,const char*,const char*,int32_t);
int32_t sceHttpSendRequest(int32_t,const void*,size_t);
int32_t sceHttpGetStatusCode(int32_t,int32_t*);
int32_t sceHttpGetAllResponseHeaders(int32_t,char**,size_t*);
int32_t sceHttpGetResponseContentLength(int32_t,int32_t*,size_t*);
int32_t sceHttpReadData(int32_t,void*,uint32_t);
int32_t sceHttpAbortRequest(int32_t);
int32_t sceHttpDeleteRequest(int32_t);
int32_t sceHttpDeleteConnection(int32_t);
int32_t sceHttpDeleteTemplate(int32_t);
int32_t sceHttpTerm(int32_t);
}
