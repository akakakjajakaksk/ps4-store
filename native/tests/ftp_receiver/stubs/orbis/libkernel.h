#pragma once
#include <stdint.h>
#include <pthread.h>
typedef pthread_t OrbisPthread;
typedef pthread_attr_t OrbisPthreadAttr;
extern "C" {
uint64_t sceKernelGetProcessTime();
int32_t sceKernelUsleep(uint32_t);
int32_t scePthreadCreate(OrbisPthread*, const OrbisPthreadAttr*, void* (*)(void*), void*, const char*);
int32_t scePthreadJoin(OrbisPthread, void**);
}
