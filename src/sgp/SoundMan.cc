/*********************************************************************************
* SGP Digital Sound Module
*
*		This module handles the playing of digital samples, preloaded or streamed.
*
* Derek Beland, May 28, 1997
*********************************************************************************/

#include "Buffer.h"
#include "Debug.h"
#include "FileMan.h"
#include "Random.h"
#include "SoundMan.h"
#include "Timer.h"
#include <SDL.h>
#include <assert.h>
#include <stdexcept>

#include "ContentManager.h"
#include "GameInstance.h"
#include "Logger.h"


// Uncomment this to disable the startup of sound hardware
//#define SOUND_DISABLE

/*
 * from\to FREE PLAY STOP DEAD
 *    FREE       M
 *    PLAY  2         M    C
 *    STOP  2              C
 *    DEAD  M         1
 *
 * M = Regular state transition done by main thread
 * C = Regular state transition done by sound callback
 * 1 = Unimportant race, dead channel can be marked stopped by main thread
 *     Gets marked as dead again in the next sound callback run
 * 2 = Only when stopping all sounds, sound callback is deactivated when this
 *     happens
 */
enum
{
	CHANNEL_FREE,
	CHANNEL_PLAY,
	CHANNEL_STOP,
	CHANNEL_DEAD
};


// Sample status flags
enum
{
	SAMPLE_ALLOCATED = 1U << 0,
	SAMPLE_LOCKED    = 1U << 1,
	SAMPLE_RANDOM    = 1U << 2,
	SAMPLE_STEREO    = 1U << 3
};


#define SOUND_MAX_CACHED 128 // number of cache slots
#define SOUND_MAX_CHANNELS 16 // number of mixer channels

#define SOUND_DEFAULT_MEMORY (32 * 1024 * 1024) // default memory limit
#define SOUND_DEFAULT_THRESH ( 2 * 1024 * 1024) // size for sample to be double-buffered
#define SOUND_DEFAULT_STREAM (64 * 1024)        // double-buffered buffer size

// The audio device will be opened with the following values
#define SOUND_FREQ      44100
#define SOUND_FORMAT    AUDIO_S16SYS
#define SOUND_CHANNELS  2
#define SOUND_SAMPLES   1024

// Struct definition for sample slots in the cache
// Holds the regular sample data, as well as the data for the random samples
struct SAMPLETAG
{
	CHAR8   pName[128];  // Path to sample data
	UINT32  n_samples;
	UINT32  uiFlags;     // Status flags
	PTR     pData;       // pointer to sample data memory
	UINT32  uiCacheHits;

	// Random sound data
	UINT32  uiTimeNext;
	UINT32  uiTimeMin;
	UINT32  uiTimeMax;
	UINT32  uiVolMin;
	UINT32  uiVolMax;
	UINT32  uiPanMin;
	UINT32  uiPanMax;
	UINT32  uiInstances;
	UINT32  uiMaxInstances;
};


// Structure definition for slots in the sound output
// These are used for both the cached and double-buffered streams
struct SOUNDTAG
{
	volatile UINT State;
	SAMPLETAG*    pSample;
	UINT32        uiSoundID;
	void          (*EOSCallback)(void*);
	void*         pCallbackData;
	UINT32        uiTimeStamp;
	HWFILE        hFile;
	UINT32        uiFadeVolume;
	UINT32        uiFadeRate;
	UINT32        uiFadeTime;
	UINT32        pos;
	UINT32        Loops;
	UINT32        Pan;
};

static size_t GetSampleSize(const SAMPLETAG* const s);
static const UINT32 guiSoundMemoryLimit    = SOUND_DEFAULT_MEMORY; // Maximum memory used for sounds
static       UINT32 guiSoundMemoryUsed     = 0;                    // Memory currently in use
//static const UINT32 guiSoundCacheThreshold = SOUND_DEFAULT_THRESH; // Double-buffered threshold
static void IncreaseSoundMemoryUsedBySample(SAMPLETAG *sample) { guiSoundMemoryUsed += sample->n_samples * GetSampleSize(sample); }
static void DecreaseSoundMemoryUsedBySample(SAMPLETAG *sample) { guiSoundMemoryUsed -= sample->n_samples * GetSampleSize(sample); }

static BOOLEAN fSoundSystemInit = FALSE; // Startup called
static BOOLEAN gfEnableStartup  = TRUE;  // Allow hardware to start up
static INT32*  gMixBuffer = NULL;
static UINT32  guiMixLength = 0;

SDL_AudioSpec gTargetAudioSpec;

// Sample cache list for files loaded
static SAMPLETAG pSampleList[SOUND_MAX_CACHED];
// Sound channel list for output channels
static SOUNDTAG pSoundList[SOUND_MAX_CHANNELS];


void SoundEnableSound(BOOLEAN fEnable)
{
	gfEnableStartup = fEnable;
}


static void    SoundInitCache(void);
static BOOLEAN SoundInitHardware(void);


void InitializeSoundManager(void)
{
	if (fSoundSystemInit) ShutdownSoundManager();

	memset(pSoundList, 0, sizeof(pSoundList));

#ifndef SOUND_DISABLE
	if (gfEnableStartup && SoundInitHardware()) fSoundSystemInit = TRUE;
#endif

	SoundInitCache();

	guiSoundMemoryUsed = 0;
}


static void SoundEmptyCache(void);
static void SoundShutdownHardware(void);


void ShutdownSoundManager(void)
{
	SoundStopAll();
	SoundEmptyCache();
	SoundShutdownHardware();
	fSoundSystemInit = FALSE;
	if (gMixBuffer != NULL)
	{
		MemFree(gMixBuffer);
		gMixBuffer = NULL;
		guiMixLength = 0;
	}
}


static SOUNDTAG*  SoundGetFreeChannel(void);
static SAMPLETAG* SoundLoadSample(const char* pFilename);
static UINT32     SoundStartSample(SAMPLETAG* sample, SOUNDTAG* channel, UINT32 volume, UINT32 pan, UINT32 loop, void (*end_callback)(void*), void* data);


UINT32 SoundPlay(const char* pFilename, UINT32 volume, UINT32 pan, UINT32 loop, void (*end_callback)(void*), void* data)
{
	if (!fSoundSystemInit) return SOUND_ERROR;

#if 0 // TODO0003 implement streaming
	if (SoundPlayStreamed(pFilename))
	{
		//Trying to play a sound which is bigger then the 'guiSoundCacheThreshold'
		SLOGE("Trying to play %s sound is too large to load into cache, use SoundPlayStreamedFile() instead", pFilename));
		return SOUND_ERROR;
	}
#endif

	SAMPLETAG* const sample = SoundLoadSample(pFilename);
	if (sample == NULL) return SOUND_ERROR;

	SOUNDTAG* const channel = SoundGetFreeChannel();
	if (channel == NULL) return SOUND_ERROR;

	return SoundStartSample(sample, channel, volume, pan, loop, end_callback, data);
}

static SAMPLETAG* SoundLoadBuffer(SDL_AudioFormat format, UINT8 channels, int freq, UINT8* pbuffer, UINT32 size);
static BOOLEAN    SoundCleanCache(void);
static SAMPLETAG* SoundGetEmptySample(void);

UINT32 SoundPlayFromSmackBuff(UINT8 channels, UINT8 depth, UINT32 rate, UINT8* pbuffer, UINT32 size, UINT32 volume, UINT32 pan, UINT32 loop, void (*end_callback)(void*), void* data)
{
	SDL_AudioFormat format;

	if (pbuffer == NULL) return SOUND_ERROR;
	if (size == 0) return SOUND_ERROR;

	//Originaly Sound Blaster could only play mono unsigned 8-bit PCM data.
	//Later it became capable of playing 16-bit audio data, but needed to be signed and LSB.
	//They were the de facto standard so I'm assuming smacker uses the same.
	if (depth == 8) format = AUDIO_U8;
	else if (depth == 16) format = AUDIO_S16LSB;
	else return SOUND_ERROR;

	SAMPLETAG* s = SoundLoadBuffer(format, channels, rate, pbuffer, size);
	if (s == NULL) return SOUND_ERROR;

	sprintf(s->pName, "SmackBuff %p - SampleSize %u", pbuffer, size);
	s->uiPanMax        = 64;
	s->uiMaxInstances  = 1;

	SOUNDTAG* const channel = SoundGetFreeChannel();
	if (channel == NULL) return SOUND_ERROR;

	return SoundStartSample(s, channel, volume, pan, loop, end_callback, data);
}

UINT32 SoundPlayStreamedFile(const char* pFilename, UINT32 volume, UINT32 pan, UINT32 loop, void (*end_callback)(void*), void* data)
try
{
#if 1
	// TODO0003 implement streaming
	return SoundPlay(pFilename, volume, pan, loop, end_callback, data);
#else
	if (!fSoundSystemInit) return SOUND_ERROR;

	SOUNDTAG* const channel = SoundGetFreeChannel();
	if (channel == NULL) return SOUND_ERROR;

	AutoSGPFile hFile(GCM->openForReadingSmart(pFilename, true));

	// MSS cannot determine which provider to play if you don't give it a real filename
	// so if the file isn't in a library, play it normally
	if (DB_EXTRACT_LIBRARY(hFile) == REAL_FILE_LIBRARY_ID)
	{
		return SoundStartStream(pFilename, channel, volume, pan, loop, end_callback, data);
	}

	//Get the real file handle of the file
	FILE* hRealFileHandle = GetRealFileHandleFromFileManFileHandle(hFile);
	if (hRealFileHandle == NULL)
	{
		SLOGE("SoundPlayStreamedFile(): Couldnt get a real file handle for '%s' in SoundPlayStreamedFile()", pFilename );
		return SOUND_ERROR;
	}

	//Convert the file handle into a 'name'
	char pFileHandlefileName[128];
	sprintf(pFileHandlefileName, "\\\\\\\\%d", hRealFileHandle);

	//Start the sound stream
	UINT32 uiRetVal = SoundStartStream(pFileHandlefileName, channel, volume, pan, loop, end_callback, data);

	//if it succeeded, record the file handle
	if (uiRetVal != SOUND_ERROR)
	{
		channel->hFile = hFile.Release();
	}

	return uiRetVal;
#endif
}
catch (...)
{
	SLOGE("SoundPlayStreamedFile(): Failed to play '%s'", pFilename);
	return SOUND_ERROR;
}


UINT32 SoundPlayRandom(const char* pFilename, UINT32 time_min, UINT32 time_max, UINT32 vol_min, UINT32 vol_max, UINT32 pan_min, UINT32 pan_max, UINT32 max_instances)
{
	SLOGD("playing random Sound: \"%s\"", pFilename);

	if (!fSoundSystemInit) return SOUND_ERROR;

	SAMPLETAG* const s = SoundLoadSample(pFilename);
	if (s == NULL) return SOUND_ERROR;

	s->uiFlags        |= SAMPLE_RANDOM | SAMPLE_LOCKED;
	s->uiTimeMin       = time_min;
	s->uiTimeMax       = time_max;
	s->uiVolMin        = vol_min;
	s->uiVolMax        = vol_max;
	s->uiPanMin        = pan_min;
	s->uiPanMax        = pan_max;
	s->uiMaxInstances  = max_instances;

	s->uiTimeNext =
		GetClock() +
		s->uiTimeMin +
		Random(s->uiTimeMax - s->uiTimeMin);

	return (UINT32)(s - pSampleList);
}


static SOUNDTAG* SoundGetChannelByID(UINT32 uiSoundID);


BOOLEAN SoundIsPlaying(UINT32 uiSoundID)
{
	if (!fSoundSystemInit) return FALSE;

	const SOUNDTAG* const channel = SoundGetChannelByID(uiSoundID);
	return channel != NULL &&  channel->State != CHANNEL_FREE;
}


static BOOLEAN SoundStopChannel(SOUNDTAG* channel);


BOOLEAN SoundStop(UINT32 uiSoundID)
{
	if (!fSoundSystemInit) return FALSE;
	if (!SoundIsPlaying(uiSoundID)) return FALSE;

	SOUNDTAG* const channel = SoundGetChannelByID(uiSoundID);
	if (channel == NULL) return FALSE;

	SoundStopChannel(channel);
	return TRUE;
}


void SoundStopAll(void)
{
	if (!fSoundSystemInit) return;

	SDL_PauseAudio(1);
	FOR_EACH(SOUNDTAG, i, pSoundList)
	{
		if (SoundStopChannel(i))
		{
			assert(i->pSample->uiInstances != 0);
			i->pSample->uiInstances -= 1;
			i->pSample               = NULL;
			i->uiSoundID             = SOUND_ERROR;
			i->State                 = CHANNEL_FREE;
		}
	}
	SDL_PauseAudio(0);
}


BOOLEAN SoundSetVolume(UINT32 uiSoundID, UINT32 uiVolume)
{
	if (!fSoundSystemInit) return FALSE;

	SOUNDTAG* const channel = SoundGetChannelByID(uiSoundID);
	if (channel == NULL) return FALSE;

	channel->uiFadeVolume = __min(uiVolume, MAXVOLUME);
	return TRUE;
}


BOOLEAN SoundSetPan(UINT32 uiSoundID, UINT32 uiPan)
{
	if (!fSoundSystemInit) return FALSE;

	SOUNDTAG* const channel = SoundGetChannelByID(uiSoundID);
	if (channel == NULL) return FALSE;

	channel->Pan = __min(uiPan, 127);
	return TRUE;
}


UINT32 SoundGetVolume(UINT32 uiSoundID)
{
	if (!fSoundSystemInit) return SOUND_ERROR;

	const SOUNDTAG* const channel = SoundGetChannelByID(uiSoundID);
	if (channel == NULL) return SOUND_ERROR;

	return channel->uiFadeVolume;
}


static BOOLEAN SoundRandomShouldPlay(const SAMPLETAG* s);
static UINT32 SoundStartRandom(SAMPLETAG* s);


void SoundServiceRandom(void)
{
	FOR_EACH(SAMPLETAG, i, pSampleList)
	{
		if (SoundRandomShouldPlay(i)) SoundStartRandom(i);
	}
}


/* Determines whether a random sound is ready for playing or not.
 *
 * Returns: TRUE if a the sample should be played. */
static BOOLEAN SoundRandomShouldPlay(const SAMPLETAG* s)
{
	return
		s->uiFlags & SAMPLE_RANDOM &&
		s->uiTimeNext <= GetClock() &&
		s->uiInstances < s->uiMaxInstances;
}


/* Starts an instance of a random sample.
 *
 * Returns: TRUE if a new random sound was created, FALSE if nothing was done. */
static UINT32 SoundStartRandom(SAMPLETAG* s)
{
	SOUNDTAG* const channel = SoundGetFreeChannel();
	if (channel == NULL) return NO_SAMPLE;

	const UINT32 volume = s->uiVolMin + Random(s->uiVolMax - s->uiVolMin);
	const UINT32 pan    = s->uiPanMin + Random(s->uiPanMax - s->uiPanMin);

	const UINT32 uiSoundID = SoundStartSample(s, channel, volume, pan, 1, NULL, NULL);
	if (uiSoundID == SOUND_ERROR) return NO_SAMPLE;

	s->uiTimeNext =
		GetClock() +
		s->uiTimeMin +
		Random(s->uiTimeMax - s->uiTimeMin);
	return uiSoundID;
}


void SoundStopAllRandom(void)
{
	// Stop all currently playing random sounds
	FOR_EACH(SOUNDTAG, i, pSoundList)
	{
		if (i->State == CHANNEL_PLAY && i->pSample->uiFlags & SAMPLE_RANDOM)
		{
			SoundStopChannel(i);
		}
	}

	// Unlock all random sounds so they can be dumped from the cache, and
	// take the random flag off so they won't be serviced/played
	FOR_EACH(SAMPLETAG, i, pSampleList)
	{
		if (i->uiFlags & SAMPLE_RANDOM)
		{
			i->uiFlags &= ~(SAMPLE_RANDOM | SAMPLE_LOCKED);
		}
	}
}


void SoundServiceStreams(void)
{
	if (!fSoundSystemInit) return;

	for (UINT32 i = 0; i < lengthof(pSoundList); i++)
	{
		SOUNDTAG* Sound = &pSoundList[i];
		if (Sound->State == CHANNEL_DEAD)
		{
			SLOGD("DEAD channel %u file \"%s\" (refcount %u)", i, Sound->pSample->pName, Sound->pSample->uiInstances);
			if (Sound->EOSCallback != NULL) Sound->EOSCallback(Sound->pCallbackData);
			assert(Sound->pSample->uiInstances != 0);
			Sound->pSample->uiInstances--;
			Sound->pSample   = NULL;
			Sound->uiSoundID = SOUND_ERROR;
			Sound->State     = CHANNEL_FREE;
		}
	}
}


UINT32 SoundGetPosition(UINT32 uiSoundID)
{
	if (!fSoundSystemInit) return 0;

	const SOUNDTAG* const channel = SoundGetChannelByID(uiSoundID);
	if (channel == NULL) return 0;

	const UINT32 now = GetClock();
	return now - channel->uiTimeStamp;
}


// Zeros out the structures of the sample list.
static void SoundInitCache(void)
{
	memset(pSampleList, 0, sizeof(pSampleList));
}


static void SoundFreeSample(SAMPLETAG* s);


// Frees up all samples in the cache.
static void SoundEmptyCache(void)
{
	SoundStopAll();

	FOR_EACH(SAMPLETAG, i, pSampleList)
	{
		SoundFreeSample(i);
	}
}


static SAMPLETAG* SoundGetCached(const char* pFilename);
static SAMPLETAG* SoundLoadDisk(const char* pFilename);


static SAMPLETAG* SoundLoadSample(const char* pFilename)
{
	SAMPLETAG* const s = SoundGetCached(pFilename);
	if (s != NULL) return s;

	return SoundLoadDisk(pFilename);
}


/* Tries to locate a sound by looking at what is currently loaded in the cache.
 *
 * Returns: The sample index if successful, NO_SAMPLE if the file wasn't found
 *          in the cache. */
static SAMPLETAG* SoundGetCached(const char* pFilename)
{
	if (pFilename[0] == '\0') return NULL; // XXX HACK0009

	FOR_EACH(SAMPLETAG, i, pSampleList)
	{
		if (strcasecmp(i->pName, pFilename) == 0) return i;
	}

	return NULL;
}

static size_t GetSampleSize(const SAMPLETAG* const s)
{
	return 2u * (s->uiFlags & SAMPLE_STEREO ? 2 : 1);
}

/* Loads a sound from a buffer into the cache, allocating memory and a slot for storage.
 *
 * Returns: The sample if successful, NULL otherwise. */
static SAMPLETAG* SoundLoadBuffer(SDL_AudioFormat format, UINT8 channels, int freq, UINT8* buffer, UINT32 size)
{
	SDL_AudioCVT cvt;
	int ret;
	UINT8* sampledata = NULL;
	UINT32 samplesize = 0;
	UINT8  samplechannels;

	if (buffer == NULL || size == 0)
	{
		SLOGE("SoundLoadBuffer Error: buffer is empty - Buffer: %p, Size: %u", buffer, size);
		return NULL;
	}

	samplechannels = __min(channels, gTargetAudioSpec.channels);
	ret = SDL_BuildAudioCVT(&cvt, format, channels, freq, gTargetAudioSpec.format, samplechannels, gTargetAudioSpec.freq);
	if (ret == -1)
	{
		SLOGE("SoundLoadBuffer Error: unsupported audio conversion - %s", SDL_GetError());
		return NULL;
	}

	if (cvt.needed)
	{
		UINT32 bufsize = size * cvt.len_mult;
		UINT32 cvtsize = size * cvt.len_ratio;

		Assert(bufsize >= size);
		cvt.len = size;
		cvt.buf = MALLOCN(UINT8, bufsize);
		memcpy(cvt.buf, buffer, size);

		if (SDL_ConvertAudio(&cvt) == -1) {
			SLOGE("SoundLoadBuffer Error: error converting audio - %s", SDL_GetError());
			MemFree(cvt.buf);
			return NULL;
		}

		if (cvtsize == bufsize)
		{
			Assert(cvtsize == cvt.len_cvt);
			sampledata = cvt.buf;
			samplesize = cvtsize;
		}
		else// if (cvtsize < bufsize)
		{
			Assert(cvtsize < bufsize);
			sampledata = MALLOCN(UINT8, cvtsize);
			memcpy(sampledata, cvt.buf, cvtsize);
			samplesize = cvtsize;
			MemFree(cvt.buf);
		}
	}
	else// if (!cvt.needed)
	{
		sampledata = MALLOCN(UINT8, size);
		memcpy(sampledata, buffer, size);
		samplesize = size;
	}
	// cvt is invalid from this point forward

	// if insufficient memory, start unloading old samples until either
	// there's nothing left to unload, or we fit
	while (samplesize + guiSoundMemoryUsed > guiSoundMemoryLimit)
	{
		if (!SoundCleanCache())
		{
			SLOGE("SoundLoadBuffer Error: not enough memory - Size: %u, Used: %u, Max: %u", samplesize, guiSoundMemoryUsed, guiSoundMemoryLimit);
			MemFree(sampledata);
			return NULL;
		}
	}

	// if all the sample slots are full, unloading one
	SAMPLETAG* s = SoundGetEmptySample();
	if (s == NULL)
	{
		SoundCleanCache();
		s = SoundGetEmptySample();
	}

	// if we still don't have a sample slot
	if (s == NULL)
	{
		SLOGE("SoundLoadBuffer Error: sound channels are full");
		MemFree(sampledata);
		return NULL;
	}

	s->pData = sampledata;
	s->uiFlags |= SAMPLE_ALLOCATED;
	if (samplechannels != 1) {
		Assert(samplechannels == 2);
		s->uiFlags |= SAMPLE_STEREO;
	}
	s->n_samples = UINT32(samplesize / GetSampleSize(s));

	IncreaseSoundMemoryUsedBySample(s);

	return s;
}

/* Loads a sound file from disk into the cache, allocating memory and a slot
 * for storage.
 *
 * Returns: The sample index if successful, NO_SAMPLE if the file wasn't found
 *          in the cache. */
static SAMPLETAG* SoundLoadDisk(const char* pFilename)
{
	Assert(pFilename != NULL);

	if(pFilename[0] == '\0') {
		SLOGA("SoundLoadDisk Error: pFilename is an empty string.");
		return NULL;
	}

	AutoSGPFile hFile;

	try
	{
		hFile = GCM->openGameResForReading(pFilename);
	}
	catch (const std::runtime_error& err)
	{
		SLOGA("SoundLoadDisk Error: %s", err.what());
		return NULL;
	}

	SDL_RWops* rwOps = FileGetRWOps(hFile);
	SDL_AudioSpec wavSpec;
	Uint32 wavLength;
	Uint8 *wavBuffer;

	if (SDL_LoadWAV_RW(rwOps, 0,  &wavSpec, &wavBuffer, &wavLength) == NULL) {
		SLOGE("SoundLoadDisk Error: Error loading file \"%s\"- %s", pFilename, SDL_GetError());
		return NULL;
	}

	SAMPLETAG* s = SoundLoadBuffer(wavSpec.format, wavSpec.channels, wavSpec.freq, wavBuffer, wavLength);

	SDL_FreeWAV(wavBuffer);
	SDL_FreeRW(rwOps);

	if (s == NULL)
	{
		SLOGE("SoundLoadDisk: Error converting sound file \"%s\"", pFilename);
		return NULL;
	}

	strcpy(s->pName, pFilename);

	return s;
}


// Returns TRUE/FALSE that a sample is currently in use for playing a sound.
static BOOLEAN SoundSampleIsPlaying(const SAMPLETAG* s)
{
	return s->uiInstances > 0;
}


/* Removes the least-used sound from the cache to make room.
 *
 * Returns: TRUE if a sample was freed, FALSE if none */
static BOOLEAN SoundCleanCache(void)
{
	SAMPLETAG* candidate = NULL;

	FOR_EACH(SAMPLETAG, i, pSampleList)
	{
		if (i->uiFlags & SAMPLE_ALLOCATED &&
				!(i->uiFlags & SAMPLE_LOCKED) &&
				(candidate == NULL || candidate->uiCacheHits > i->uiCacheHits))
		{
			if (!SoundSampleIsPlaying(i)) candidate = i;
		}
	}

	if (candidate != NULL)
	{
		SLOGD("freeing sample %u \"%s\" with %u hits", candidate - pSampleList, candidate->pName, candidate->uiCacheHits);
		SoundFreeSample(candidate);
		return TRUE;
	}

	return FALSE;
}


/* Returns an available sample.
 *
 * Returns: A free sample or NULL if none are left. */
static SAMPLETAG* SoundGetEmptySample(void)
{
	FOR_EACH(SAMPLETAG, i, pSampleList)
	{
		if (!(i->uiFlags & SAMPLE_ALLOCATED)) return i;
	}

	return NULL;
}


// Frees up a sample referred to by its index slot number.
static void SoundFreeSample(SAMPLETAG* s)
{
	if (!(s->uiFlags & SAMPLE_ALLOCATED)) return;

	assert(s->uiInstances == 0);

	DecreaseSoundMemoryUsedBySample(s);
	MemFree(s->pData);
	memset(s, 0, sizeof(*s));
}


/* Searches out a sound instance referred to by its ID number.
 *
 * Returns: If the instance was found, the pointer to the channel.  NULL
 *          otherwise. */
static SOUNDTAG* SoundGetChannelByID(UINT32 uiSoundID)
{
	FOR_EACH(SOUNDTAG, i, pSoundList)
	{
		if (i->uiSoundID == uiSoundID) return i;
	}

	return NULL;
}


static void SoundCallback(void* userdata, Uint8* stream, int len)
{
	// INT16 data is being mixed as INT32, so it needs to be double the length of the stream
	if ( guiMixLength < len * 2 )
	{
		guiMixLength = len * 2;
		gMixBuffer = REALLOC(gMixBuffer, INT32, guiMixLength);
	}

	SDL_memset(gMixBuffer, 0, guiMixLength);

	// Mix sounds
	for (UINT32 i = 0; i < lengthof(pSoundList); i++)
	{
		SOUNDTAG* Sound = &pSoundList[i];

		switch (Sound->State)
		{
			default:
			case CHANNEL_FREE:
			case CHANNEL_DEAD:
				continue;

			case CHANNEL_STOP:
				Sound->State = CHANNEL_DEAD;
				continue;

			case CHANNEL_PLAY:
			{
				const SAMPLETAG* const s = Sound->pSample;
				const INT vol_l   = Sound->uiFadeVolume * (127 - Sound->Pan) / MAXVOLUME;
				const INT vol_r   = Sound->uiFadeVolume * (  0 + Sound->Pan) / MAXVOLUME;
				size_t    samples = len / 4;
				size_t    amount;

mixing:
				amount = MIN(samples, s->n_samples - Sound->pos);
				if (s->uiFlags & SAMPLE_STEREO)
				{
					const INT16* const src = (const INT16*)s->pData + Sound->pos * 2;
					for (UINT32 i = 0; i < amount; ++i)
					{
						gMixBuffer[2 * i + 0] += src[2 * i + 0] * vol_l >> 7;
						gMixBuffer[2 * i + 1] += src[2 * i + 1] * vol_r >> 7;
					}
				}
				else
				{
					const INT16* const src = (const INT16*)s->pData + Sound->pos;
					for (UINT32 i = 0; i < amount; i++)
					{
						const INT data = src[i];
						gMixBuffer[2 * i + 0] += data * vol_l >> 7;
						gMixBuffer[2 * i + 1] += data * vol_r >> 7;
					}
				}

				Sound->pos += amount;
				if (Sound->pos == s->n_samples)
				{
					if (Sound->Loops != 1)
					{
						if (Sound->Loops != 0) --Sound->Loops;
						Sound->pos = 0;
						samples -= amount;
						if (samples != 0) goto mixing;
					}
					else
					{
						Sound->State = CHANNEL_DEAD;
					}
				}
			}
		}
	}

	// Clip sounds and fill the stream
	INT16* Stream = (INT16*)stream;
	UINT32 uiEnd = len / sizeof(Stream[0]);
	for (UINT32 i = 0; i < uiEnd; ++i)
	{
		if (gMixBuffer[i] >= INT16_MAX)     Stream[i] = INT16_MAX;
		else if(gMixBuffer[i] <= INT16_MIN) Stream[i] = INT16_MIN;
		else                                Stream[i] = (INT16)gMixBuffer[i];
	}
}


static BOOLEAN SoundInitHardware(void)
{
	SDL_InitSubSystem(SDL_INIT_AUDIO);

	gTargetAudioSpec.freq     = SOUND_FREQ;
	gTargetAudioSpec.format   = SOUND_FORMAT;
	gTargetAudioSpec.channels = SOUND_CHANNELS;
	gTargetAudioSpec.samples  = SOUND_SAMPLES;
	gTargetAudioSpec.callback = SoundCallback;
	gTargetAudioSpec.userdata = NULL;

	if (SDL_OpenAudio(&gTargetAudioSpec, NULL) != 0) return FALSE;

	memset(pSoundList, 0, sizeof(pSoundList));
	SDL_PauseAudio(0);
	return TRUE;
}


static void SoundShutdownHardware(void)
{
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}


/* Finds an unused sound channel in the channel list.
 *
 * Returns: Pointer to a sound channel if one was found, NULL if not. */
static SOUNDTAG* SoundGetFreeChannel(void)
{
	FOR_EACH(SOUNDTAG, i, pSoundList)
	{
		if (i->State == CHANNEL_FREE) return i;
	}

	return NULL;
}


static UINT32 SoundGetUniqueID(void);


/* Starts up a sample on the specified channel. Override parameters are passed
 * in through the structure pointer pParms. Any entry with a value of 0xffffffff
 * will be filled in by the system.
 *
 * Returns: Unique sound ID if successful, SOUND_ERROR if not. */
static UINT32 SoundStartSample(SAMPLETAG* sample, SOUNDTAG* channel, UINT32 volume, UINT32 pan, UINT32 loop, void (*end_callback)(void*), void* data)
{
	SLOGD("playing channel %u sample %u file \"%s\"", channel - pSoundList, sample - pSampleList, sample->pName);

	if (!fSoundSystemInit) return SOUND_ERROR;

	channel->uiFadeVolume  = volume;
	channel->Loops         = loop;
	channel->Pan           = pan;
	channel->EOSCallback   = end_callback;
	channel->pCallbackData = data;

	UINT32 uiSoundID = SoundGetUniqueID();
	channel->uiSoundID    = uiSoundID;
	channel->pSample      = sample;
	channel->uiTimeStamp  = GetClock();
	channel->pos          = 0;
	channel->State        = CHANNEL_PLAY;

	sample->uiInstances++;
	sample->uiCacheHits++;

	return uiSoundID;
}

/* Returns a unique ID number with every call. Basically it's just a 32-bit
 * static value that is incremented each time. */
static UINT32 SoundGetUniqueID(void)
{
	static UINT32 uiNextID = 0;

	if (uiNextID == SOUND_ERROR) uiNextID++;

	return uiNextID++;
}

/* Stops a sound referred to by its channel.  This function is the only one
 * that should be deallocating sample handles. The random sounds have to have
 * their counters maintained, and using this as the central function ensures
 * that they stay in sync.
 *
 * Returns: TRUE if the sample was stopped, FALSE if it could not be found. */
static BOOLEAN SoundStopChannel(SOUNDTAG* channel)
{
	if (!fSoundSystemInit) return FALSE;

	if (channel->pSample == NULL) return FALSE;

	SLOGD("stopping channel channel %u", channel - pSoundList);
	channel->State = CHANNEL_STOP;
	return TRUE;
}


void SoundStopRandom(UINT32 uiSample)
{
	// CHECK FOR VALID SAMPLE
	SAMPLETAG* const s = &pSampleList[uiSample];
	if (s->uiFlags & SAMPLE_ALLOCATED)
	{
		s->uiFlags &= ~SAMPLE_RANDOM;
	}
}
