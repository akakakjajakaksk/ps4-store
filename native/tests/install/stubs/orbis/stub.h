#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int OrbisPthread;
typedef int OrbisPthreadAttr;
enum OrbisSysModuleInternal : uint32_t {
    ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL = 0x80000014,
    ORBIS_SYSMODULE_INTERNAL_BGFT = 0x8000002a
};
extern "C" {
int32_t sceKernelUsleep(uint32_t);
int32_t scePthreadAttrInit(OrbisPthreadAttr*);
int32_t scePthreadAttrDestroy(OrbisPthreadAttr*);
int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr*, int);
int32_t scePthreadCreate(OrbisPthread*, const OrbisPthreadAttr*, void*(*)(void*), void*, const char*);
uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal);
void sceSysmoduleIsLoadedInternal();
int32_t sceAppInstUtilInitialize();
int32_t sceAppInstUtilTerminate();
int32_t sceAppInstUtilGetTitleIdFromPkg(const char*, char*, int32_t*);
int32_t sceAppInstUtilAppExists(const char*, int32_t*);
bool sceAppInstUtilAppIsInInstalling(const char*);
int32_t sceBgftServiceDownloadStartTask(int32_t);
int32_t sceBgftServiceDownloadStopTask(int32_t);
int32_t sceBgftServiceIntDownloadUnregisterTask(int32_t);
int32_t sceBgftServiceIntTerm();
}
