#pragma once
#include <stdint.h>
enum OrbisSysModuleInternal : uint32_t { ORBIS_SYSMODULE_INTERNAL_NET = 0x8000001c };
extern "C" {
uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal);
int32_t sceSysmoduleIsLoadedInternal(OrbisSysModuleInternal);
}
