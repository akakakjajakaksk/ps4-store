#pragma once
#include <stdint.h>
#define ORBIS_USER_SERVICE_USER_ID_SYSTEM 0xFF
#define ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT 0x8026000Eu
enum OrbisAudioOutPort { ORBIS_AUDIO_OUT_PORT_TYPE_MAIN = 0 };
enum OrbisAudioOutParam { ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO = 1 };
extern "C" {
int32_t sceAudioOutInit();
int32_t sceAudioOutOpen(int32_t, OrbisAudioOutPort, int32_t, uint32_t, uint32_t, uint32_t);
int32_t sceAudioOutOutput(int32_t, const void*);
int32_t sceAudioOutClose(int32_t);
}
