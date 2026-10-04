// GoldHEN Plugin SDK bridge (MIT, Copyright (c) 2022 GoldHEN).
// Extracted version/jailbreak/restore calls and syscall bridge from:
// https://github.com/GoldHEN/GoldHEN_Plugins_SDK/tree/main/source
// Full permission notice: assets/GOLDHEN_SDK_LICENSE.txt.
// SDK credits: OSM, jocover, bucanero, OpenOrbis Team and SiSTRo.
#include <stdint.h>
#include <stddef.h>
#include "third_party/goldhen/GoldHEN.h"

static_assert(sizeof(jailbreak_backup) == 72, "GoldHEN backup ABI");
static_assert(offsetof(jailbreak_backup, cr_paid) == 16, "GoldHEN paid ABI");
static_assert(offsetof(jailbreak_backup, cr_caps) == 24, "GoldHEN caps ABI");
static_assert(offsetof(jailbreak_backup, cr_prison) == 40, "GoldHEN prison ABI");
static_assert(offsetof(jailbreak_backup, fd_rdir) == 64, "GoldHEN root ABI");

extern "C" int orbis_syscall(int num, ...);

// Preserve the official FreeBSD indirect syscall ABI and error handling.
__asm__(
    ".att_syntax prefix\n"
    ".globl orbis_syscall\n"
    "orbis_syscall:\n"
    "  movq $0, %rax\n"
    "  movq %rcx, %r10\n"
    "  syscall\n"
    "  jb err\n"
    "  retq\n"
    "err:\n"
    "  pushq %rax\n"
    "  callq __error\n"
    "  popq %rcx\n"
    "  movl %ecx, 0(%rax)\n"
    "  movq $0xFFFFFFFFFFFFFFFF, %rax\n"
    "  movq $0xFFFFFFFFFFFFFFFF, %rdx\n"
    "  retq\n"
);

extern "C" int sys_sdk_cmd(uint64_t cmd, void* data) {
    return orbis_syscall(500, cmd, data);
}
extern "C" uint32_t sys_sdk_version() {
    return sys_sdk_cmd(GOLDHEN_SDK_CMD_VERSION, NULL);
}
extern "C" int sys_sdk_jailbreak(struct jailbreak_backup* backup) {
    return sys_sdk_cmd(GOLDHEN_SDK_CMD_JAILBREAK, backup);
}
extern "C" int sys_sdk_unjailbreak(struct jailbreak_backup* backup) {
    if (!backup) return -1;
    return sys_sdk_cmd(GOLDHEN_SDK_CMD_UNJAILBREAK, backup);
}
