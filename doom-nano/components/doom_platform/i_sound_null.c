/**
 * @file i_sound_null.c
 * @brief Engine sound interface (i_sound.h) as silent stubs. Sound over I2S is a roadmap item.
 */
#include <stdbool.h>
#include <stddef.h>

/* Engine header. */
#include "i_sound.h"

#define NO_HANDLE (-1)
#define NO_DEVICE (0)
#define SAMPLE_RATE_HZ (11025) /* Doom's native sample rate; unused while silent */

/* Engine globals that the reference port defines in its platform layer. 0 = no device. */
int snd_card = NO_DEVICE;
int mus_card = NO_DEVICE;
int snd_samplerate = SAMPLE_RATE_HZ;

void I_InitSound(void)
{
}

void I_ShutdownSound(void)
{
}

void I_SetChannels(void)
{
}

int I_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    (void)sfxinfo;
    return NO_HANDLE;
}

int I_StartSound(int id, int channel, int vol, int sep, int pitch, int priority)
{
    (void)id;
    (void)channel;
    (void)vol;
    (void)sep;
    (void)pitch;
    (void)priority;
    return NO_HANDLE;
}

void I_StopSound(int handle)
{
    (void)handle;
}

bool I_SoundIsPlaying(int handle)
{
    (void)handle;
    return false;
}

bool I_AnySoundStillPlaying(void)
{
    return false;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    (void)handle;
    (void)vol;
    (void)sep;
    (void)pitch;
}

void I_InitMusic(void)
{
}

void I_ShutdownMusic(void)
{
}

void I_UpdateMusic(void)
{
}

void I_SetMusicVolume(int volume)
{
    (void)volume;
}

void I_PauseSong(int handle)
{
    (void)handle;
}

void I_ResumeSong(int handle)
{
    (void)handle;
}

int I_RegisterSong(const void *data, size_t len)
{
    (void)data;
    (void)len;
    return NO_HANDLE;
}

void I_PlaySong(int handle, int looping)
{
    (void)handle;
    (void)looping;
}

void I_StopSong(int handle)
{
    (void)handle;
}

void I_UnRegisterSong(int handle)
{
    (void)handle;
}
