/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2018 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 * Copyright (c) 2018 Andrew Eikum for CodeWeavers
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
 *
 */

#define COBJMACROS
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include "FAudio_internal.h"

/* Internal Types */

typedef struct FAudioPlatformDevice
{
	const char *name;
	uint32_t bufferSize;
	wchar_t *mmDevID;
	IAudioClient *client;
	IAudioRenderClient *render;
	HANDLE deviceEvent;
	HANDLE deviceThread;
	BOOL exitThread;
	FAudioWaveFormatExtensible format;
	LinkedList *engineList;
	FAudioMutex engineLock;
} FAudioPlatformDevice;

/* Globals */

static LONG platformRef;

static CRITICAL_SECTION platformLock;
static CRITICAL_SECTION_DEBUG platformLockDebug =
{
	0,
	0,
	&platformLock,
	{
		&platformLockDebug.ProcessLocksList,
		&platformLockDebug.ProcessLocksList
	},
	0,
	0,
	{
		(DWORD_PTR) (__FILE__ ": platformLock")
	}
};
static CRITICAL_SECTION platformLock =
{
	&platformLockDebug,
	-1,
	0,
	0,
	0,
	0
};

static IMMDeviceEnumerator *mmDevEnum;
static UINT mmDevCount;
static WCHAR **mmDevIds;
static FAudioDeviceDetails *mmDevDetails;

LinkedList *devlist = NULL;
FAudioMutex devlock = &platformLock;

/* Mixer Thread */

static DWORD WINAPI FAudio_INTERNAL_MixCallback(void *userdata)
{
	FAudioPlatformDevice *device = (FAudioPlatformDevice*) userdata;
	LinkedList *audio;
	UINT32 pad, nframes, offs;
	BYTE *buf;
	HRESULT hr;

	while (1)
	{
		WaitForSingleObject(device->deviceEvent, INFINITE);

		if (device->exitThread)
		{
			return 0;
		}

		hr = IAudioClient_GetCurrentPadding(device->client, &pad);
		if (FAILED(hr))
		{
			FAudio_assert(0 && "GetCurrentPadding failed");
			continue;
		}

		nframes = device->bufferSize * 3 - pad;
		nframes -= nframes & device->bufferSize;

		hr = IAudioRenderClient_GetBuffer(device->render, nframes, &buf);
		if (FAILED(hr))
		{
			FAudio_assert(0 && "GetBuffer failed");
			continue;
		}

		audio = device->engineList;
		while (audio != NULL)
		{
			offs = 0;
			while (offs < nframes)
			{
				FAudio_INTERNAL_UpdateEngine(
					(FAudio*) audio->entry,
					(float*) (buf + offs * device->format.Format.nBlockAlign)
				);
				offs += device->bufferSize;
			}
			audio = audio->next;
		}
	}
}

/* Platform Functions */

void FAudio_PlatformAddRef()
{
	IMMDeviceCollection *devCollection;
	IMMDevice *dev, *defaultDev;
	IPropertyStore *pProp;
	PROPVARIANT prop;
	UINT i, count, idx;
	HRESULT hr;

	/* TODO: SSE2 detection */
	FAudio_INTERNAL_InitSIMDFunctions(
		1,
		0
	);

	FAudio_PlatformLockMutex(devlock);

	InterlockedIncrement(&platformRef);

	if (mmDevIds == NULL)
	{
		if (mmDevEnum == NULL)
		{
			hr = CoCreateInstance(
				&CLSID_MMDeviceEnumerator,
				NULL,
				CLSCTX_INPROC_SERVER,
				&IID_IMMDeviceEnumerator,
				(void**) &mmDevEnum
			);
			if (FAILED(hr))
			{
				FAudio_assert(0 && "Failed to create MMDeviceEnumerator");
				goto end;
			}
		}

		hr = IMMDeviceEnumerator_EnumAudioEndpoints(
			mmDevEnum,
			eRender,
			DEVICE_STATE_ACTIVE,
			&devCollection
		);
		if (FAILED(hr))
		{
			FAudio_assert(0 && "Failed to get audio endpoints");
			goto end;
		}

		hr = IMMDeviceCollection_GetCount(devCollection, &mmDevCount);
		if (FAILED(hr))
		{
			IMMDeviceCollection_Release(devCollection);
			goto end;
		}

		if (mmDevCount == 0)
		{
			/* Nothing to do... */
			goto end;
		}

		IMMDeviceEnumerator_GetDefaultAudioEndpoint(
			mmDevEnum,
			eRender,
			eConsole,
			&defaultDev
		);

		count = 1;
		mmDevIds = (WCHAR**) FAudio_malloc(sizeof(WCHAR*) * mmDevCount);
		mmDevDetails = (FAudioDeviceDetails*) FAudio_malloc(sizeof(FAudioDeviceDetails) * mmDevCount);
		FAudio_zero(mmDevDetails, sizeof(FAudioDeviceDetails) * mmDevCount);
		for (i = 0; i < mmDevCount; i += 1)
		{
			#define DEVICE_FAIL(msg) \
				if (FAILED(hr)) \
				{ \
					FAudio_assert(0 && msg); \
					FAudio_free(mmDevIds); \
					FAudio_free(mmDevDetails); \
					mmDevIds = NULL; \
					mmDevDetails = NULL; \
					IMMDeviceCollection_Release(devCollection); \
					goto end; \
				}

			hr = IMMDeviceCollection_Item(devCollection, i, &dev);
			DEVICE_FAIL("Failed to get audio endpoint")

			if (dev == defaultDev)
			{
				idx = 0;
			}
			else
			{
				idx = count++;
			}

			hr = IMMDevice_GetId(dev, &mmDevIds[idx]);
			DEVICE_FAIL("GetId failed")

			IMMDevice_OpenPropertyStore(dev, STGM_READ, &pProp);
			DEVICE_FAIL("OpenPropertyStore failed")

			PropVariantInit(&prop);
			IPropertyStore_GetValue(
				pProp,
				&PKEY_Device_FriendlyName,
				&prop
			);
			DEVICE_FAIL("FriendlyName get failed")

			#undef DEVICE_FAIL

			/* FIXME: I have this dumb thing where I use the ID
			 * as a basic index value, since SDL doesn't have an ID string.
			 * In a perfect world, mmDevIds could go into Details.DeviceID.
			 * -flibit
			 */
			mmDevDetails[idx].DeviceID[0] = L'0' + idx;
			lstrcpynW(
				mmDevDetails[idx].DisplayName,
				prop.pwszVal,
				0xFF
			);

			IMMDevice_Release(dev);
		}
	}

end:
	FAudio_PlatformUnlockMutex(devlock);
}

void FAudio_PlatformRelease()
{
	LONG r;
	UINT i;
	FAudio_PlatformLockMutex(devlock);

	r = InterlockedDecrement(&platformRef);
	if (r == 0)
	{
		FAudio_free(mmDevIds);
		mmDevIds = NULL;

		for (i = 0; i < mmDevCount; i += 1)
		{
			FAudio_free(mmDevIds[i]);
		}
		FAudio_free(mmDevIds);
		IMMDeviceEnumerator_Release(mmDevEnum);
		mmDevEnum = NULL;
	}

	FAudio_PlatformUnlockMutex(devlock);
}

static inline DWORD get_channel_mask(unsigned int channels)
{
	switch (channels)
	{
	case 0:
		return 0;
	case 1:
		return KSAUDIO_SPEAKER_MONO;
	case 2:
		return KSAUDIO_SPEAKER_STEREO;
	case 3:
		return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
	case 4:
		return KSAUDIO_SPEAKER_QUAD; /* not _SURROUND */
	case 5:
		return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
	case 6:
		return KSAUDIO_SPEAKER_5POINT1; /* not 5POINT1_SURROUND */
	case 7:
		return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
	case 8:
		return KSAUDIO_SPEAKER_7POINT1_SURROUND; /* Vista deprecates 7POINT1 */
	}
	FAudio_assert(0 && "Unknown speaker configuration!");
	return 0;
}

static inline int format_is_float32(WAVEFORMATEX *fmt)
{
	if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (
		fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
		IsEqualGUID(&((FAudioWaveFormatExtensible *)fmt)->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)))
	{
		return (fmt->wBitsPerSample == 32);
	}
	return 0;
}

void FAudio_PlatformInit(FAudio *audio, uint32_t deviceIndex)
{
	FAudioPlatformDevice *device;
	REFERENCE_TIME period, bufDur;
	LinkedList *deviceList;
	WAVEFORMATEX *fmt;
	IMMDevice *dev;
	HRESULT hr;

	if (mmDevIds == NULL)
	{
		return; /* How did we get here? */
	}

	hr = IMMDeviceEnumerator_GetDevice(mmDevEnum, mmDevIds[deviceIndex], &dev);
	if (FAILED(hr))
	{
		FAudio_assert(0 && "GetDevice failed!");
		return;
	}

	/* Add to existing device if it already exists! */
	deviceList = devlist;
	while (deviceList != NULL)
	{
		if (((FAudioPlatformDevice*) deviceList->entry)->mmDevID == mmDevIds[deviceIndex])
		{
			break;
		}
		deviceList = deviceList->next;
	}
	if (deviceList != NULL)
	{
		device = (FAudioPlatformDevice*) deviceList->entry;

		/* But give us the output format first! */
		audio->updateSize = device->bufferSize;
		audio->mixFormat = &device->format;

		/* Someone else was here first, you get their format! */
		audio->master->master.inputChannels =
			device->format.Format.nChannels;
		audio->master->master.inputSampleRate =
			device->format.Format.nSamplesPerSec;

		LinkedList_AddEntry(
			&device->engineList,
			audio,
			device->engineLock
		);
		return;
	}

	/* Store the ID, for supporting multiple engines */
	device = (FAudioPlatformDevice*) FAudio_malloc(sizeof(FAudioPlatformDevice));
	FAudio_zero(device, sizeof(FAudioPlatformDevice));
	device->mmDevID = mmDevIds[deviceIndex];

	/* We're making a new device, activate it! */
	hr = IMMDevice_Activate(
		dev,
		&IID_IAudioClient,
		CLSCTX_INPROC_SERVER,
		NULL,
		(void**) &device->client
	);
	IMMDevice_Release(dev);
	if (FAILED(hr))
	{
		FAudio_assert(0 && "ActivateClient failed!");
		FAudio_free(device);
		return;
	}

	/* Write up the format */
	device->format.Samples.wValidBitsPerSample = 32;
	device->format.Format.wBitsPerSample = 32;
	device->format.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
	device->format.Format.nChannels = audio->master->master.inputChannels;
	device->format.Format.nSamplesPerSec = audio->master->master.inputSampleRate;
	device->format.Format.nBlockAlign = (
		device->format.Format.nChannels *
		(device->format.Format.wBitsPerSample / 8)
		);
	device->format.Format.nAvgBytesPerSec = (
		device->format.Format.nSamplesPerSec *
		device->format.Format.nBlockAlign
		);
	device->format.Format.cbSize = sizeof(FAudioWaveFormatExtensible) - sizeof(FAudioWaveFormatEx);
	device->format.dwChannelMask = get_channel_mask(device->format.Format.nChannels);

	/* Verify the FAudio format with WASAPI */
	hr = IAudioClient_IsFormatSupported(
		device->client,
		AUDCLNT_SHAREMODE_SHARED,
		(WAVEFORMATEX*)&device->format.Format,
		&fmt
	);
	if (hr == S_FALSE)
	{
		if (!format_is_float32(fmt))
		{
			FAudio_assert(0 && "Mix format must be float32!");
			IAudioClient_Release(device->client);
			FAudio_free(device);
			return;
		}
		if (sizeof(WAVEFORMATEX) + fmt->cbSize > sizeof(WAVEFORMATEXTENSIBLE))
		{
			FAudio_assert("Mix format doesn't fit into WAVEFORMATEXTENSIBLE!");
			IAudioClient_Release(device->client);
			FAudio_free(device);
			return;
		}
		FAudio_memcpy(
			&device->format,
			fmt,
			sizeof(WAVEFORMATEX) + fmt->cbSize
		);
	}

	/* Get the period size, eventually becomes update size */
	hr = IAudioClient_GetDevicePeriod(device->client, &period, NULL);
	if (FAILED(hr))
	{
		FAudio_assert(0 && "GetDevicePeriod failed!");
		IAudioClient_Release(device->client);
		FAudio_free(device);
		return;
	}

	/* 3 period minimum, for safety */
	bufDur = FAudio_max(3 * period, 1000000); /* FIXME: Hardcoded size... */
	hr = IAudioClient_Initialize(
		device->client,
		AUDCLNT_SHAREMODE_SHARED,
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
		bufDur,
		0,
		(WAVEFORMATEX*) &device->format.Format,
		NULL
	);
	if (FAILED(hr))
	{
		FAudio_assert(0 && "Initialize failed!");
		IAudioClient_Release(device->client);
		FAudio_free(device);
		return;
	}

	/* WASAPI event handle */
	device->deviceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	hr = IAudioClient_SetEventHandle(device->client, device->deviceEvent);
	if (FAILED(hr))
	{
		FAudio_assert(0 && "SetEventHandle failed!");
		IAudioClient_Release(device->client);
		CloseHandle(device->deviceEvent);
		FAudio_free(device);
		return;
	}

	/* Initialize the render client */
	hr = IAudioClient_GetService(
		device->client,
		&IID_IAudioRenderClient,
		(void**) &device->render
	);
	if (FAILED(hr))
	{
		FAudio_assert(0 && "GetService failed!");
		IAudioRenderClient_Release(device->render);
		IAudioClient_Release(device->client);
		CloseHandle(device->deviceEvent);
		FAudio_free(device);
		return;
	}

	/* Okay, _now_ we assign our properties to the engine */
	audio->updateSize = MulDiv(
		(int) period,
		device->format.Format.nSamplesPerSec,
		10000000 /* FIXME: Hardcoded size... */
	);
	audio->mixFormat = &device->format;
	audio->master->master.inputChannels = device->format.Format.nChannels;
	audio->master->master.inputSampleRate = device->format.Format.nSamplesPerSec;

	/* Add the engine and device, finally.*/
	LinkedList_AddEntry(
		&device->engineList,
		audio,
		device->engineLock
	);
	LinkedList_AddEntry(&devlist, device, devlock);

	/* Create the thread, start the renderer! */
	device->deviceThread = CreateThread(NULL, 0, &FAudio_INTERNAL_MixCallback, device, 0, NULL);
	hr = IAudioClient_Start(device->client);
	if (FAILED(hr))
	{
		FAudio_assert(0 && "AudioClient Start failed!");
	}
}

void FAudio_PlatformQuit(FAudio *audio)
{
	FAudioPlatformDevice *device;
	LinkedList *dev, *entry;
	uint8_t found = 0;

	dev = devlist;
	while (dev != NULL)
	{
		device = (FAudioPlatformDevice*) dev->entry;
		entry = device->engineList;
		while (entry != NULL)
		{
			if (entry->entry == audio)
			{
				found = 1;
				break;
			}
			entry = entry->next;
		}

		if (found)
		{
			LinkedList_RemoveEntry(
				&device->engineList,
				audio,
				device->engineLock
			);

			if (device->engineList == NULL)
			{
				device->exitThread = TRUE;
				SetEvent(device->deviceEvent);
				WaitForSingleObject(device->deviceThread, INFINITE);
				CloseHandle(device->deviceThread);

				LinkedList_RemoveEntry(
					&devlist,
					device,
					devlock
				);

				IAudioRenderClient_Release(device->render);
				IAudioClient_Release(device->client);
				CloseHandle(device->deviceEvent);
				FAudio_PlatformDestroyMutex(device->engineLock);
				FAudio_free(device);
			}

			return;
		}
		dev = dev->next;
	}
}

void FAudio_PlatformStart(FAudio *audio)
{
	LinkedList *dev, *entry;

	dev = devlist;
	while (dev != NULL)
	{
		entry = ((FAudioPlatformDevice*) dev->entry)->engineList;
		while (entry != NULL)
		{
			if (((FAudio*) entry->entry) == audio)
			{
				IAudioClient_Start(
					((FAudioPlatformDevice*) dev->entry)->client
				);
				return;
			}
			entry = entry->next;
		}
		dev = dev->next;
	}
}

void FAudio_PlatformStop(FAudio *audio)
{
	LinkedList *dev, *entry;

	dev = devlist;
	while (dev != NULL)
	{
		entry = ((FAudioPlatformDevice*) dev->entry)->engineList;
		while (entry != NULL)
		{
			if (((FAudio*) entry->entry) == audio)
			{
				IAudioClient_Stop(
					((FAudioPlatformDevice*)dev->entry)->client
				);
				return;
			}
			entry = entry->next;
		}
		dev = dev->next;
	}
}

uint32_t FAudio_PlatformGetDeviceCount()
{
	return mmDevCount;
}

void FAudio_UTF8_To_UTF16(const char *src, uint16_t *dst, size_t len);

void FAudio_PlatformGetDeviceDetails(
	uint32_t index,
	FAudioDeviceDetails *details
) {
	if (index > FAudio_PlatformGetDeviceCount())
	{
		FAudio_zero(details, sizeof(FAudioDeviceDetails));
		return;
	}

	/* We got details at init, just copy it over */
	FAudio_memcpy(
		details,
		&mmDevDetails[index],
		sizeof(FAudioDeviceDetails)
	);

	/* TODO: SDL_GetAudioDeviceSpec! */
	details->OutputFormat.Format.nSamplesPerSec = 48000;
	details->OutputFormat.Format.nChannels = 2;

	if (details->OutputFormat.Format.nChannels == 1)
	{
		details->OutputFormat.dwChannelMask = SPEAKER_MONO;
	}
	else if (details->OutputFormat.Format.nChannels == 2)
	{
		details->OutputFormat.dwChannelMask = SPEAKER_STEREO;
	}
	else if (details->OutputFormat.Format.nChannels == 3)
	{
		details->OutputFormat.dwChannelMask = SPEAKER_2POINT1;
	}
	else if (details->OutputFormat.Format.nChannels == 4)
	{
		details->OutputFormat.dwChannelMask = SPEAKER_QUAD;
	}
	else if (details->OutputFormat.Format.nChannels == 5)
	{
		details->OutputFormat.dwChannelMask = SPEAKER_4POINT1;
	}
	else if (details->OutputFormat.Format.nChannels == 6)
	{
		details->OutputFormat.dwChannelMask = SPEAKER_5POINT1;
	}
	else if (details->OutputFormat.Format.nChannels == 8)
	{
		details->OutputFormat.dwChannelMask = SPEAKER_7POINT1;
	}
	else
	{
		FAudio_assert(0 && "Unrecognized speaker layout!");
	}
	details->OutputFormat.Samples.wValidBitsPerSample = 32;
	details->OutputFormat.Format.wBitsPerSample = 32;
	details->OutputFormat.Format.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
	details->OutputFormat.Format.nBlockAlign = (
		details->OutputFormat.Format.nChannels *
		(details->OutputFormat.Format.wBitsPerSample / 8)
	);
	details->OutputFormat.Format.nAvgBytesPerSec = (
		details->OutputFormat.Format.nSamplesPerSec *
		details->OutputFormat.Format.nBlockAlign
	);
}

FAudioPlatformFixedRateSRC FAudio_PlatformInitFixedRateSRC(
	uint32_t channels,
	uint32_t inputRate,
	uint32_t outputRate
) {
	/* TODO: Reuse linear resampler! */
	return NULL;
}

void FAudio_PlatformCloseFixedRateSRC(FAudioPlatformFixedRateSRC resampler)
{
	/* TODO: Reuse linear resampler! */
}

uint32_t FAudio_PlatformResample(
	FAudioPlatformFixedRateSRC resampler,
	float *input,
	uint32_t inLen,
	float *output,
	uint32_t outLen
) {
	/* TODO: Reuse linear resampler! */
	return 0;
}

/* Threading */

typedef struct FAudioWin32ThreadData
{
	FAudioThreadFunc func;
	void* userdata;
} FAudioWin32ThreadData;

static DWORD WINAPI FAudio_Win32_TheadWrapper(void* userdata)
{
	FAudioWin32ThreadData *data = (FAudioWin32ThreadData*) userdata;
	int32_t res = data->func(data->userdata);
	FAudio_free(data);
	return (DWORD) res;
}

FAudioThread FAudio_PlatformCreateThread(
	FAudioThreadFunc func,
	const char *name,
	void* data
) {
	FAudioWin32ThreadData *threadData = (FAudioWin32ThreadData*) FAudio_malloc(sizeof(FAudioWin32ThreadData));
	threadData->func = func;
	threadData->userdata = data;
	return CreateThread(
		NULL,
		0,
		FAudio_Win32_TheadWrapper,
		threadData,
		0,
		NULL
	);
}

void FAudio_PlatformWaitThread(FAudioThread thread, int32_t *retval)
{
	WaitForSingleObjectEx((HANDLE) thread, INFINITE, FALSE);
	CloseHandle((HANDLE) thread);
}

void FAudio_PlatformThreadPriority(FAudioThreadPriority priority)
{
	if (priority == FAUDIO_THREAD_PRIORITY_LOW)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
	}
	else if (priority == FAUDIO_THREAD_PRIORITY_NORMAL)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
	}
	else if (priority == FAUDIO_THREAD_PRIORITY_HIGH)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	}
}

FAudioMutex FAudio_PlatformCreateMutex()
{
	CRITICAL_SECTION *r = FAudio_malloc(sizeof(CRITICAL_SECTION));
	InitializeCriticalSection(r);
	return (FAudioMutex) r;
}

void FAudio_PlatformDestroyMutex(FAudioMutex mutex)
{
	DeleteCriticalSection((CRITICAL_SECTION*) mutex);
	FAudio_free(mutex);
}

void FAudio_PlatformLockMutex(FAudioMutex mutex)
{
	EnterCriticalSection((CRITICAL_SECTION*) mutex);
}

void FAudio_PlatformUnlockMutex(FAudioMutex mutex)
{
	LeaveCriticalSection((CRITICAL_SECTION*) mutex);
}

void FAudio_sleep(uint32_t ms)
{
	Sleep(ms);
}

/* Time */

uint32_t FAudio_timems()
{
	return GetTickCount();
}

/* FAudio I/O */

typedef enum FAudioWin32RWtype
{
	FAUDIO_WIN32_IO_FILE,
	FAUDIO_WIN32_IO_MEMORY
} FAudioWin32RWtype;

typedef struct FAudioWin32RWops
{
	FAudioWin32RWtype type;
	union
	{
		HANDLE file;
		struct
		{
			uint8_t *base;
			uint8_t *cur;
			uint8_t *end;
		} mem;
	};
} FAudioWin32RWops;

size_t FAudio_win32_read(void* data, void* dst, size_t size, size_t count)
{
	DWORD byte_read;
	FAudioWin32RWops *rwops = (FAudioWin32RWops*) data;
	if (!ReadFile(rwops->file, dst, size * count, &byte_read, NULL))
	{
		return 0;
	}
	return byte_read;
}

int64_t FAudio_win32_seek(void* data, int64_t offset, int whence)
{
	DWORD windowswhence;
	LARGE_INTEGER windowsoffset;
	FAudioWin32RWops *rwops = (FAudioWin32RWops*) data;

	switch (whence)
	{
	case FAUDIO_SEEK_SET:
		windowswhence = FILE_BEGIN;
		break;
	case FAUDIO_SEEK_CUR:
		windowswhence = FILE_CURRENT;
		break;
	case FAUDIO_SEEK_END:
		windowswhence = FILE_END;
		break;
	}

	windowsoffset.QuadPart = offset;
	if (!SetFilePointerEx(rwops->file, windowsoffset, &windowsoffset, windowswhence))
	{
		return -1;
	}
	return windowsoffset.QuadPart;
}

int FAudio_win32_close(void* data)
{
	FAudioWin32RWops *rwops = (FAudioWin32RWops*) data;
	CloseHandle(rwops->file);
	FAudio_free(rwops);
	return 0;
}

size_t FAudio_memory_read(void* data, void* dst, size_t size, size_t count)
{
	FAudioWin32RWops *rwops = (FAudioWin32RWops*) data;
	size_t total, max;
	if (size == 0 || count == 0)
	{
		return 0;
	}

	total = size * count;
	max = rwops->mem.end - rwops->mem.cur;
	total = FAudio_min(total, max);

	FAudio_memcpy(dst, rwops->mem.cur, total);
	rwops->mem.cur += total;
	return total / size;
}

int64_t FAudio_memory_seek(void* data, int64_t offset, int whence)
{
	FAudioWin32RWops *rwops = (FAudioWin32RWops*) data;
	uint8_t *next;

	switch (whence)
	{
	case FAUDIO_SEEK_SET:
		next = rwops->mem.base + offset;
		break;
	case FAUDIO_SEEK_CUR:
		next = rwops->mem.cur + offset;
		break;
	case FAUDIO_SEEK_END:
		next = rwops->mem.end + offset;
		break;
	}
	rwops->mem.cur = FAudio_clamp(next, rwops->mem.base, rwops->mem.end);
	return (int64_t) (rwops->mem.cur - rwops->mem.base);
}

int FAudio_memory_close(void* data)
{
	FAudio_free(data);
	return 0;
}

FAudioIOStream* FAudio_fopen(const char *path)
{
	FAudioIOStream *io = (FAudioIOStream*) FAudio_malloc(
		sizeof(FAudioIOStream)
	);
	FAudioWin32RWops *rwops = (FAudioWin32RWops*) FAudio_malloc(sizeof(FAudioWin32RWops));
	rwops->type = FAUDIO_WIN32_IO_FILE;
	rwops->file = CreateFile(
		path,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);
	io->data = rwops;
	io->read = (FAudio_readfunc) FAudio_win32_read;
	io->seek = (FAudio_seekfunc) FAudio_win32_seek;
	io->close = (FAudio_closefunc) FAudio_win32_close;
	return io;
}

FAudioIOStream* FAudio_memopen(void *mem, int len)
{
	FAudioIOStream *io = (FAudioIOStream*) FAudio_malloc(
		sizeof(FAudioIOStream)
	);
	FAudioWin32RWops *rwops = (FAudioWin32RWops*) FAudio_malloc(sizeof(FAudioWin32RWops));
	rwops->type = FAUDIO_WIN32_IO_MEMORY;
	rwops->mem.base = (uint8_t*) mem;
	rwops->mem.cur = rwops->mem.base;
	rwops->mem.end = rwops->mem.base + len;
	io->data = rwops;
	io->read = (FAudio_readfunc) FAudio_memory_read;
	io->seek = (FAudio_seekfunc) FAudio_memory_seek;
	io->close = (FAudio_closefunc) FAudio_memory_close;
	return io;
}

uint8_t* FAudio_memptr(FAudioIOStream *io, size_t offset)
{
	FAudioWin32RWops *rwops = (FAudioWin32RWops*) io->data;
	FAudio_assert(rwops->type == FAUDIO_WIN32_IO_MEMORY);
	return rwops->mem.cur;
}

void FAudio_close(FAudioIOStream *io)
{
	io->close(io->data);
	FAudio_free(io);
}
