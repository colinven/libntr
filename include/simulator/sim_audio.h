#ifndef SIM_AUDIO_H
#define SIM_AUDIO_H

#include <nitro.h>
#include <nitro/types.h>
#include <simulator/sim.h>
#include <SDL2/SDL.h>

// Set to 1 to write audio_debug.wav and audio_debug.log in the game folder.
// They help find where audio glitches come from.
#ifndef SIM_AUDIO_DEBUG
#define SIM_AUDIO_DEBUG 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern u32 s_SIM_sndcnt[16];
extern u8 * s_SIM_sndsad[16];
extern u16 s_SIM_sndtmr[16];
extern u16 s_SIM_sndpnt[16];
extern u32 s_SIM_sndlen[16];

void SIM_Audio_Init(int aAudioFrequency);
void SIM_Audio_Callback(void *userdata, Uint8 *stream, int len);

void SIM_Audio_StartChannel(int chNo);

s32 SIM_Audio_RunChannel(u32 cycles, int chNo);

void SIM_Audio_NextSamplePCM8(int chNo); 
void SIM_Audio_NextSamplePCM16(int chNo);
void SIM_Audio_NextSampleADPCM(int chNo);
void SIM_Audio_NextSamplePSG(int chNo);
void SIM_Audio_NextSampleNoise(int chNo);

#if SIM_AUDIO_DEBUG
void SIM_AudioDebug_OnCommand(int commandId);
void SIM_AudioDebug_OnInvalidateWave(const void *start, const void *end);
void SIM_AudioDebug_Printf(const char *format, ...);
#endif

#ifdef __cplusplus
}
#endif

#endif
