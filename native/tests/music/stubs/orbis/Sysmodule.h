#pragma once
#include <stdint.h>
enum OrbisSysModuleInternal : uint32_t { ORBIS_SYSMODULE_INTERNAL_AUDIOOUT = 0x80000001u };
extern "C" {
uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal);
int32_t sceSysmoduleIsLoadedInternal(OrbisSysModuleInternal);
}
