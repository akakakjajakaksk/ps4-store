#pragma once
#include <stdint.h>
enum OrbisSysModuleInternal : uint32_t {
    ORBIS_SYSMODULE_INTERNAL_NET = 0x8000001c,
    ORBIS_SYSMODULE_INTERNAL_NETCTL = 0x80000009
};
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal);
