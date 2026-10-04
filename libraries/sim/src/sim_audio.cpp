#include <nitro.h>
#include <simulator/sim_audio.h>
#include <simulator/sim.h>
#include <simulator/config/sim_config.h>
#include <SDL2/SDL.h>

#include "blip_buf.h"

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

static SDL_AudioSpec s_requestedAudioSpec, s_actualAudioSpec;
static SDL_AudioDeviceID s_audioDevice;

u32 s_SIM_sndcnt[16] = {0};
u8 * s_SIM_sndsad[16] = {0};
u16 s_SIM_sndtmr[16] = {0};
u16 s_SIM_sndpnt[16] = {0};
u32 s_SIM_sndlen[16] = {0};

static s32 s_SIM_internalSoundTimer[16] = {0};
static s32 s_SIM_internalSoundPos[16] = {0};
static s32 s_SIM_internalSoundSample[16] = {0};
static s32 s_SIM_internalADPCMValLoop[16] = {0};
static s32 s_SIM_internalADPCMIndexLoop[16] = {0};
static s32 s_SIM_internalADPCMVal[16] = {0};
static s32 s_SIM_internalADPCMIndex[16] = {0};
static u8 s_SIM_internalADPCMCurByte[16] = {0};
static s32 s_SIM_internalNextADPCMByte[16] = {0};
static u16 s_SIM_internalNoiseVal[16] = {0};

static int s_blipTimer = 0;
static blip_t* s_BlipLeft;
static blip_t* s_BlipRight;
static s16* s_outputBuffer;
static u32 s_outputBufferWritePos = 0;
static u32 s_outputBufferReadPos = 0;

static int s_outputLastLeftSample;
static int s_outputLastRightSample;

static const u16 s_ADPCMTable[89] =
{
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
    0x0010, 0x0011, 0x0013, 0x0015, 0x0017, 0x0019, 0x001C, 0x001F,
    0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F,
    0x009D, 0x00AD, 0x00BE, 0x00D1, 0x00E6, 0x00FD, 0x0117, 0x0133,
    0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583,
    0x0610, 0x06AB, 0x0756, 0x0812, 0x08E0, 0x09C3, 0x0ABD, 0x0BD0,
    0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B,
    0x3BB9, 0x41B2, 0x4844, 0x4F7E, 0x5771, 0x602F, 0x69CE, 0x7462,
    0x7FFF
};

static const s8 s_ADPCMIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static const s16 s_PSGTable[8][8] =
{
    {-0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF},
    {-0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF},
    {-0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF},
    {-0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF},
    {-0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF},
    {-0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF},
    {-0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF},
    {-0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF}
};

#define INTERNAL_SAMPLE_RATE 16756991.f

#if SIM_AUDIO_DEBUG
#include <atomic>
#include <cstdarg>

// Debug tools for audio glitches. The audio callback copies what it plays into a ring buffer.
// A writer thread saves the ring to audio_debug.wav, and once per second writes counters to
// audio_debug.log. The callback never touches files, so this does not cause glitches itself.

#define DEBUG_RING_SIZE (1 << 21) // in s16 values, about 23 seconds of stereo audio

static s16 s_debugRing[DEBUG_RING_SIZE];
static std::atomic<u32> s_debugRingWrite{0};
static u32 s_debugRingRead = 0;

static std::atomic<int> s_debugInCallback{0};
static std::atomic<u32> s_debugCommandsDuringCallback{0};
static std::atomic<u32> s_debugInvalidateHits{0};
static std::atomic<u32> s_debugCommandCounts[64];

static int s_debugFrequency;
static SDL_mutex *s_debugTextMutex;
static char s_debugText[1 << 16];
static int s_debugTextLength;

// Adds a line to audio_debug.log. Only buffers text; the writer thread does the file write.
void SIM_AudioDebug_Printf(const char *format, ...)
{
    va_list args;

    if (s_debugTextMutex == NULL) {
        return;
    }
    SDL_LockMutex(s_debugTextMutex);
    int room = (int)sizeof(s_debugText) - s_debugTextLength;
    if (room > 1) {
        int written = snprintf(s_debugText + s_debugTextLength, room, "%.3f ", SDL_GetTicks() / 1000.0);
        s_debugTextLength += (written < room) ? written : room - 1;
        room = (int)sizeof(s_debugText) - s_debugTextLength;
        va_start(args, format);
        written = vsnprintf(s_debugText + s_debugTextLength, room, format, args);
        va_end(args);
        s_debugTextLength += (written < room) ? written : room - 1;
    }
    SDL_UnlockMutex(s_debugTextMutex);
}

static void DebugLogPlayers(FILE *log)
{
    for (int playerNo = 0; playerNo < SND_PLAYER_NUM; playerNo++) {
        SNDPlayer *player = &SNDi_Work.player[playerNo];
        if (!player->active_flag) {
            continue;
        }
        fprintf(log, "  player %d: prepared %d pause %d prio %d volume %d extFader %d tracks",
            playerNo, player->prepared_flag, player->pause_flag, player->prio, player->volume, player->extFader);
        for (int t = 0; t < SND_TRACK_NUM_PER_PLAYER; t++) {
            if (player->tracks[t] != 0xFF) {
                SNDTrack *track = &SNDi_Work.track[player->tracks[t]];
                fprintf(log, " [%d mask %04x/%d vol %d ext %d mute %d]", t, track->channel_mask,
                    track->channel_mask_flag, track->volume, track->extFader, track->mute_flag);
            }
        }
        fprintf(log, "\n");
    }
    fprintf(log, "  locked %04x weak %04x active channels", SND_GetLockedChannel(0),
        SND_GetLockedChannel(SND_LOCK_IMPLIED_ALLOC_CHANNEL));
    for (int ch = 0; ch < SND_CHANNEL_NUM; ch++) {
        if (SNDi_Work.channel[ch].active_flag) {
            fprintf(log, " %d", ch);
        }
    }
    fprintf(log, "\n");
}

static void WriteWavHeader(FILE *file, u32 dataBytes)
{
    u32 u;
    u16 h;

    fseek(file, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, file);
    u = 36 + dataBytes; fwrite(&u, 4, 1, file);
    fwrite("WAVEfmt ", 1, 8, file);
    u = 16; fwrite(&u, 4, 1, file);
    h = 1; fwrite(&h, 2, 1, file); // PCM
    h = 2; fwrite(&h, 2, 1, file); // stereo
    u = s_debugFrequency; fwrite(&u, 4, 1, file);
    u = s_debugFrequency * 4; fwrite(&u, 4, 1, file);
    h = 4; fwrite(&h, 2, 1, file);
    h = 16; fwrite(&h, 2, 1, file);
    fwrite("data", 1, 4, file);
    fwrite(&dataBytes, 4, 1, file);
    fseek(file, 0, SEEK_END);
}

static int DebugWriterThread(void *arg)
{
    FILE *wav = fopen("audio_debug.wav", "wb");
    FILE *log = fopen("audio_debug.log", "w");
    u32 dataBytes = 0;
    u32 lastLogTicks = SDL_GetTicks();
    u32 lastCommandCounts[64] = {0};

    if (wav == NULL || log == NULL) {
        printf("Audio debug: could not open output files\n");
        return 0;
    }
    WriteWavHeader(wav, 0);
    fprintf(log, "time_s wav_s cmds_during_callback invalidate_hits commands(id:count)\n");

    while (1) {
        SDL_Delay(100);

        u32 write = s_debugRingWrite.load(std::memory_order_acquire);
        while (s_debugRingRead != write) {
            u32 end = (write > s_debugRingRead) ? write : DEBUG_RING_SIZE;
            fwrite(&s_debugRing[s_debugRingRead], 2, end - s_debugRingRead, wav);
            dataBytes += (end - s_debugRingRead) * 2;
            s_debugRingRead = end % DEBUG_RING_SIZE;
        }
        WriteWavHeader(wav, dataBytes);
        fflush(wav);

        SDL_LockMutex(s_debugTextMutex);
        fwrite(s_debugText, 1, s_debugTextLength, log);
        s_debugTextLength = 0;
        SDL_UnlockMutex(s_debugTextMutex);

        u32 now = SDL_GetTicks();
        if (now - lastLogTicks >= 1000) {
            lastLogTicks = now;
            fprintf(log, "%.1f %.2f %u %u",
                now / 1000.0, dataBytes / 4.0 / s_debugFrequency,
                s_debugCommandsDuringCallback.exchange(0),
                s_debugInvalidateHits.exchange(0));
            for (int i = 0; i < 64; i++) {
                u32 count = s_debugCommandCounts[i].load();
                if (count != lastCommandCounts[i]) {
                    fprintf(log, " %d:%u", i, count - lastCommandCounts[i]);
                    lastCommandCounts[i] = count;
                }
            }
            fprintf(log, "\n");
            DebugLogPlayers(log);
            fflush(log);
        }
    }
    return 0;
}

static void DebugPushSamples(const s16 *samples, int count)
{
    u32 write = s_debugRingWrite.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        s_debugRing[write] = samples[i];
        write = (write + 1) % DEBUG_RING_SIZE;
    }
    s_debugRingWrite.store(write, std::memory_order_release);
}

void SIM_AudioDebug_OnCommand(int commandId)
{
    if (commandId >= 0 && commandId < 64) {
        s_debugCommandCounts[commandId]++;
    }
    if (s_debugInCallback.load()) {
        s_debugCommandsDuringCallback++;
    }
}

void SIM_AudioDebug_OnInvalidateWave(const void *start, const void *end)
{
    for (int chNo = 0; chNo < 16; chNo++) {
        const u8 *data = s_SIM_sndsad[chNo];
        if ((s_SIM_sndcnt[chNo] & (1u << 31)) && (const u8 *)start <= data && data <= (const u8 *)end) {
            s_debugInvalidateHits++;
        }
    }
}
#endif


void SIM_Audio_Init(int aAudioFrequency)
{
    s_BlipLeft = blip_new(512*64);
    s_BlipRight = blip_new(512*64);

    blip_set_rates(s_BlipLeft, INTERNAL_SAMPLE_RATE * 1.0f, aAudioFrequency);
    blip_set_rates(s_BlipRight, INTERNAL_SAMPLE_RATE * 1.0f, aAudioFrequency);

    memset(&s_requestedAudioSpec, 0, sizeof(SDL_AudioSpec));
    s_requestedAudioSpec.freq = aAudioFrequency;
    s_requestedAudioSpec.format = AUDIO_S16LSB;
    s_requestedAudioSpec.channels = 2;

    // Set up the number of samples so NitroComposer runs at the exact right interval
    s_requestedAudioSpec.samples = (int)((double)aAudioFrequency * 0.005215419);
    if(s_requestedAudioSpec.samples & 1) {
        s_requestedAudioSpec.samples += 1;
    }
    s_requestedAudioSpec.callback = SIM_Audio_Callback;
    if(SDL_OpenAudio(&s_requestedAudioSpec, NULL) < 0) {
        printf("SDL Error %s\n", SDL_GetError());
    }
    SDL_PauseAudio(0);

#if SIM_AUDIO_DEBUG
    s_debugFrequency = aAudioFrequency;
    s_debugTextMutex = SDL_CreateMutex();
    SDL_CreateThread(DebugWriterThread, "AudioDebug", NULL);
#endif
}

static u8 GetNextADPCMByte(int chNo);
static void PanOutput(s32 in, s32 * left, s32 * right, int chNo);

void SIM_Audio_Callback(void *userdata, Uint8 *stream, int len)
{
    #ifdef SDK_TRACY_ENABLE
    TracyCZone(ctx, 1);
    #endif
#if SIM_AUDIO_DEBUG
    s_debugInCallback = 1;
#endif
    // Run NitroComposer
    SND_UpdateExChannel();
    SND_SeqMain(TRUE);
    SND_ExChannelMain(TRUE);

    // get samples from each channel and do the thing
    

    while(blip_samples_avail(s_BlipLeft) < len / 4) {
        s32 left = 0;
        s32 right = 0;
        s32 leftOutput = 0;
        s32 rightOutput = 0;

        // Mix all 16 channels, each with its own pan, into left and right.
        for(int chNo = 0; chNo < 16; chNo++) {
            s32 channel = SIM_Audio_RunChannel(512, chNo);
            PanOutput(channel, &left, &right, chNo);
        }

        s_blipTimer += 512;

        // blip_buf works on changes, so every change must be added, also a change back to 0.
        // Skipping those leaves the output off by the last value, which sounds like static.
        if(left != s_outputLastLeftSample) {
            blip_add_delta(s_BlipLeft, s_blipTimer, left - s_outputLastLeftSample);
        }
        if(right != s_outputLastRightSample) {
            blip_add_delta(s_BlipRight, s_blipTimer, right - s_outputLastRightSample);
        }

        s_outputLastLeftSample = left;
        s_outputLastRightSample = right;

        if(s_blipTimer >= 512 * 128) {
            blip_end_frame(s_BlipLeft, s_blipTimer);
            blip_end_frame(s_BlipRight, s_blipTimer);
            s_blipTimer = 0;
        }
    }

    int avail = blip_samples_avail(s_BlipLeft);
    if(avail > len / 4) {
        avail = len / 4;
    }
    s16 * tempbuf = (s16*)stream;
    blip_read_samples(s_BlipLeft, tempbuf, avail, TRUE);
    blip_read_samples(s_BlipRight, tempbuf+1, avail, TRUE);
#if SIM_AUDIO_DEBUG
    DebugPushSamples(tempbuf, avail * 2);
    s_debugInCallback = 0;
#endif
    #ifdef SDK_TRACY_ENABLE
    TracyCZoneEnd(ctx);
    #endif
}

void SIM_Audio_StartChannel(int chNo)
{
    if((s_SIM_sndcnt[chNo]>>29)&0x3 == 3) {
        s_SIM_internalSoundPos[chNo] = -1;
    } else {
        s_SIM_internalSoundPos[chNo] = -3;
    }
    s_SIM_internalSoundSample[chNo] = 0;
    s_SIM_internalSoundTimer[chNo] = s_SIM_sndtmr[chNo];
    s_SIM_internalNoiseVal[chNo] = 0x7FFF;
}

s32 SIM_Audio_RunChannel(u32 cycles, int chNo)
{
    if(!(s_SIM_sndcnt[chNo] & (1<<31))) {
        return 0;
    }

    int type = (s_SIM_sndcnt[chNo] >> 29) & 0x3;
    if(type == 3) {
        if(chNo >= 14) {
            type = 4;
        } else if (chNo >= 8) {
            type = 3;
        } else {
            // these channels cant do psg or noise
            return 0;
        }
    }

    if((type < 3) && 
       (s_SIM_sndlen[chNo] + s_SIM_sndpnt[chNo] < 16)) {
        return 0;
    }

    s_SIM_internalSoundTimer[chNo] += cycles;

    while(s_SIM_internalSoundTimer[chNo] >> 16) {
        s_SIM_internalSoundTimer[chNo] = s_SIM_sndtmr[chNo] + (s_SIM_internalSoundTimer[chNo] - 0x10000);

        switch(type) {
            case 0: SIM_Audio_NextSamplePCM8(chNo); break;
            case 1: SIM_Audio_NextSamplePCM16(chNo); break;
            case 2: SIM_Audio_NextSampleADPCM(chNo); break;
            case 3: SIM_Audio_NextSamplePSG(chNo); break;
            case 4: SIM_Audio_NextSampleNoise(chNo); break;
        }
    }

    s32 ret = s_SIM_internalSoundSample[chNo];
    int volumeShift = (s_SIM_sndcnt[chNo] & (0x3 << 8)) >> 8;
    int volume = s_SIM_sndcnt[chNo] & 0b1111111;

    //ret <<= volumeShift;
    ret = ret >> volumeShift;
    //ret *= volume;
    //ret = ret / 128;
    ret = (s16)((double)ret * ((double)volume/128.0));

    return ret;
}

void SIM_Audio_NextSamplePCM8(int chNo)
{
    s_SIM_internalSoundPos[chNo]++;
    if(s_SIM_internalSoundPos[chNo] < 0) {
        return;
    }
    
    if(s_SIM_internalSoundPos[chNo] >= s_SIM_sndpnt[chNo] + s_SIM_sndlen[chNo]) {
        u32 repeat = (s_SIM_sndcnt[chNo] >> 27) & 0x3;
        if(repeat & 1) {
            s_SIM_internalSoundPos[chNo] = s_SIM_sndpnt[chNo];
        } else if(repeat & 2) {
            s_SIM_internalSoundSample[chNo] = 0;
            s_SIM_sndcnt[chNo] &= ~(1<<31);
            return;
        }
    }
    s8 * ptr = (s8*)s_SIM_sndsad[chNo];
    if(ptr) {
        s8 val = *(ptr+s_SIM_internalSoundPos[chNo]);
        s_SIM_internalSoundSample[chNo] = val << 8;
    }
}

void SIM_Audio_NextSamplePCM16(int chNo)
{
    s_SIM_internalSoundPos[chNo]++;
    if(s_SIM_internalSoundPos[chNo] < 0) {
        return;
    }

    if((s_SIM_internalSoundPos[chNo]<<1) >= s_SIM_sndpnt[chNo] + s_SIM_sndlen[chNo]) {
        u32 repeat = (s_SIM_sndcnt[chNo] >> 27) & 0x3;
        if(repeat & 1) {
            s_SIM_internalSoundPos[chNo] = s_SIM_sndpnt[chNo]>>1;
        } else if(repeat & 2) {
            s_SIM_internalSoundSample[chNo] = 0;
            s_SIM_sndcnt[chNo] &= ~(1<<31);
            return;
        }
    }
    s16 * ptr = (s16*)s_SIM_sndsad[chNo];
    if(ptr) {
        s16 val = *(ptr+s_SIM_internalSoundPos[chNo]);
        s_SIM_internalSoundSample[chNo] = val;
    }
}

void SIM_Audio_NextSampleADPCM(int chNo)
{
    s_SIM_internalSoundPos[chNo]++;
    if(s_SIM_internalSoundPos[chNo] < 8) {
        if(s_SIM_internalSoundPos[chNo] == 0) {
            //setup adpcm
            u32 header = *(u32*)(s_SIM_sndsad[chNo]);
            s_SIM_internalADPCMVal[chNo] = (s32)(s16)(header & 0xFFFF);
            s_SIM_internalADPCMIndex[chNo] = (header >> 16) & 0x7F;
            s_SIM_internalNextADPCMByte[chNo] = 4;
            if(s_SIM_internalADPCMIndex[chNo] > 88) {
                s_SIM_internalADPCMIndex[chNo] = 88;
            }

            s_SIM_internalADPCMValLoop[chNo] = s_SIM_internalADPCMVal[chNo];
            s_SIM_internalADPCMIndexLoop[chNo] = s_SIM_internalADPCMIndex[chNo];
        }

        return;
    }

    if((s_SIM_internalSoundPos[chNo] >> 1) >= (s_SIM_sndpnt[chNo] + s_SIM_sndlen[chNo])) {
        u32 repeat = (s_SIM_sndcnt[chNo]>>27) & 0x3;
        if(repeat & 1) {
            s_SIM_internalSoundPos[chNo] = s_SIM_sndpnt[chNo]<<1;
            s_SIM_internalNextADPCMByte[chNo] = s_SIM_sndpnt[chNo];
            s_SIM_internalADPCMVal[chNo] = s_SIM_internalADPCMValLoop[chNo];
            s_SIM_internalADPCMIndex[chNo] = s_SIM_internalADPCMIndexLoop[chNo];
            u8 * ptr = (u8*)s_SIM_sndsad[chNo];
            s_SIM_internalADPCMCurByte[chNo] = GetNextADPCMByte(chNo);
            //s_SIM_internalADPCMCurByte[chNo] = ptr[s_SIM_internalSoundPos[chNo]>>1];
        } else if (repeat & 2) {
            s_SIM_internalSoundSample[chNo] = 0;
            s_SIM_sndcnt[chNo] &= ~(1<<31);
            return;
        }
    } else {
        if(!(s_SIM_internalSoundPos[chNo] & 0x1)) {
            //u8 * ptr = (u8*)s_SIM_sndsad[chNo];
            //s_SIM_internalADPCMCurByte[chNo] = ptr[s_SIM_internalSoundPos[chNo]>>1];
            s_SIM_internalADPCMCurByte[chNo] = GetNextADPCMByte(chNo);
        } else {
            s_SIM_internalADPCMCurByte[chNo] >>= 4;
        }

        u16 val = s_ADPCMTable[s_SIM_internalADPCMIndex[chNo]];
        u16 diff = val >> 3;
        if (s_SIM_internalADPCMCurByte[chNo] & 0x1) diff += (val >> 2);
        if (s_SIM_internalADPCMCurByte[chNo] & 0x2) diff += (val >> 1);
        if (s_SIM_internalADPCMCurByte[chNo] & 0x4) diff += val;

        if (s_SIM_internalADPCMCurByte[chNo] & 0x8)
        {
            s_SIM_internalADPCMVal[chNo] -= diff;
            if (s_SIM_internalADPCMVal[chNo] < -0x7FFF) s_SIM_internalADPCMVal[chNo] = -0x7FFF;
        }
        else
        {
            s_SIM_internalADPCMVal[chNo] += diff;
            if (s_SIM_internalADPCMVal[chNo] > 0x7FFF) s_SIM_internalADPCMVal[chNo] = 0x7FFF;
        }

        s_SIM_internalADPCMIndex[chNo] += s_ADPCMIndexTable[s_SIM_internalADPCMCurByte[chNo] & 0x7];
        if      (s_SIM_internalADPCMIndex[chNo] < 0)  s_SIM_internalADPCMIndex[chNo] = 0;
        else if (s_SIM_internalADPCMIndex[chNo] > 88) s_SIM_internalADPCMIndex[chNo] = 88;

        if (s_SIM_internalSoundPos[chNo] == (s_SIM_sndpnt[chNo]<<1))
        {
            s_SIM_internalADPCMValLoop[chNo] = s_SIM_internalADPCMVal[chNo];
            s_SIM_internalADPCMIndexLoop[chNo] = s_SIM_internalADPCMIndex[chNo];
        }
    }

    s_SIM_internalSoundSample[chNo] = s_SIM_internalADPCMVal[chNo];
}

void SIM_Audio_NextSamplePSG(int chNo)
{
    s_SIM_internalSoundPos[chNo]++;
    s_SIM_internalSoundSample[chNo] = s_PSGTable[(s_SIM_sndcnt[chNo] >> 24) & 0x7][s_SIM_internalSoundPos[chNo] & 0x7];
}

void SIM_Audio_NextSampleNoise(int chNo)
{
    if(s_SIM_internalNoiseVal[chNo] & 0x1) {
        s_SIM_internalNoiseVal[chNo] = (s_SIM_internalNoiseVal[chNo] >> 1) ^ 0x6000;
        s_SIM_internalSoundSample[chNo] = -0x7FFF;
    } else {
        s_SIM_internalNoiseVal[chNo] >>= 1;
        s_SIM_internalSoundSample[chNo] = 0x7FFF;
    }
}

static u8 GetNextADPCMByte(int chNo)
{
    u8 ret = s_SIM_sndsad[chNo][s_SIM_internalNextADPCMByte[chNo]];
    s_SIM_internalNextADPCMByte[chNo]++;

    return ret;
}

static void PanOutput(s32 in, s32 * left, s32 * right, int chNo)
{
    int pan = (s_SIM_sndcnt[chNo] & (0b1111111 << 16)) >> 16;
    *left += ((s64)in * (128-pan)) >> 10;
    *right += ((s64)in * pan) >> 10;
}