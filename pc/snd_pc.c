/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

snd_pc.c -- PulseX sound output: Win32 waveOut, id's own waveOut design from WinQuake's snd_win.c without DirectSound.

One buffer of WAV_BUFFERS blocks of WAV_BUFFER_SIZE bytes is snd_dma.c's ring (shm->buffer). SNDDMA_GetDMAPos returns
the position of the next block to SEND (id's trick: snd_dma mixes _snd_mixahead ahead of that); SNDDMA_Submit retires
the blocks waveOut has played and keeps WAV_AHEAD blocks queued (22050 Hz stereo 16-bit: 1 KB = 11.6 ms, 6 blocks =
70 ms). No thread of our own (CALLBACK_NULL, polled every frame from S_Update). The shutdown is bounded: waveOutReset
returns every queued block at once, then the headers are unprepared and the device closed; it is logged.
-sndspeed <hz> picks the rate (11025 / 22050 / 44100); -nosound turns it all off.
*/

#include "quakedef.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#define	WAV_BUFFERS		64
#define	WAV_MASK		0x3F
#define	WAV_BUFFER_SIZE	0x0400
#define	WAV_AHEAD		6

static HWAVEOUT		hwo;
static WAVEHDR		*hdr;
static byte			*data;
static int			sent, completed, wav_init;
static long			overruns;

static void free_sound (void)
{
	int		i;
	double	t0 = Sys_FloatTime ();

	if (hwo)
	{
		waveOutReset (hwo);		// every queued block comes back at once
		if (hdr)
			for (i = 0 ; i < WAV_BUFFERS ; i++)
				if (hdr[i].dwFlags & WHDR_PREPARED)
					waveOutUnprepareHeader (hwo, hdr + i, sizeof(WAVEHDR));
		waveOutClose (hwo);
		hwo = NULL;
		Sys_Printf ("PCSND shutdown %.1f ms, blocks sent %d, overruns %ld\n", (Sys_FloatTime () - t0) * 1000.0, sent, overruns);
	}
	if (hdr)
		free (hdr);
	if (data)
		free (data);
	hdr = NULL;
	data = NULL;
	wav_init = 0;
}

qboolean SNDDMA_Init (void)
{
	WAVEFORMATEX	format;
	MMRESULT		r;
	int				i, t, speed = 22050;

	t = COM_CheckParm ("-sndspeed");
	if (t && t < com_argc-1)
		speed = Q_atoi (com_argv[t+1]);
	if (speed != 11025 && speed != 22050 && speed != 44100)
		speed = 22050;

	sent = completed = 0;
	shm = &sn;
	shm->channels = 2;
	shm->samplebits = 16;
	shm->speed = speed;

	memset (&format, 0, sizeof(format));
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = shm->channels;
	format.wBitsPerSample = shm->samplebits;
	format.nSamplesPerSec = shm->speed;
	format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
	format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

	r = waveOutOpen (&hwo, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
	if (r != MMSYSERR_NOERROR)
	{
		hwo = NULL;
		Sys_Printf ("PCSND waveOutOpen failed (%u): no sound\n", (unsigned)r);	// e.g. no device: the game runs silent
		return false;
	}
	data = (byte *)calloc (WAV_BUFFERS, WAV_BUFFER_SIZE);
	hdr = (WAVEHDR *)calloc (WAV_BUFFERS, sizeof(WAVEHDR));
	if (!data || !hdr)
	{
		free_sound ();
		return false;
	}
	for (i = 0 ; i < WAV_BUFFERS ; i++)
	{
		hdr[i].dwBufferLength = WAV_BUFFER_SIZE;
		hdr[i].lpData = (LPSTR)(data + i * WAV_BUFFER_SIZE);
		if (waveOutPrepareHeader (hwo, hdr + i, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
		{
			free_sound ();
			return false;
		}
	}
	shm->soundalive = true;
	shm->splitbuffer = false;
	shm->samples = WAV_BUFFERS * WAV_BUFFER_SIZE / (shm->samplebits / 8);
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	shm->buffer = data;
	wav_init = 1;
	Sys_Printf ("PCSND waveOut %d Hz stereo 16-bit, %d x %d bytes, %d queued\n", shm->speed, WAV_BUFFERS, WAV_BUFFER_SIZE, WAV_AHEAD);
	return true;
}

// the next block to send, in mono samples
int SNDDMA_GetDMAPos (void)
{
	int		s;

	if (!wav_init)
		return 0;
	s = sent * WAV_BUFFER_SIZE;
	s >>= 1;		// 16-bit samples
	s &= (shm->samples - 1);
	return s;
}

void SNDDMA_Submit (void)
{
	if (!wav_init)
		return;
	// the blocks waveOut has played
	while (completed != sent && (hdr[completed & WAV_MASK].dwFlags & WHDR_DONE))
		completed++;
	if (completed == sent)
		overruns++;		// everything played: the mixer fell behind (a long frame)
	// keep WAV_AHEAD blocks queued
	while (sent - completed < WAV_AHEAD)
	{
		WAVEHDR	*h = hdr + (sent & WAV_MASK);

		h->dwFlags &= ~WHDR_DONE;
		sent++;
		if (waveOutWrite (hwo, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
		{
			Sys_Printf ("PCSND waveOutWrite failed: sound off\n");
			free_sound ();
			return;
		}
	}
}

void SNDDMA_Shutdown (void)
{
	free_sound ();
}
