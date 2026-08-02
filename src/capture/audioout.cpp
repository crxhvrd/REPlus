// This file is part of RockstarEditorPlus.
// Copyright (C) 2026 CoreFX (crxhvrd@proton.me)
// SPDX-License-Identifier: GPL-3.0-only
// RockstarEditorPlus is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License v3 as published by the Free
// Software Foundation. See the LICENSE file for details.

#include "main.h"
#include "capture/audioout.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <cstdio>

#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

namespace audioout
{
	namespace
	{
		// 48 kHz stereo float. Process loopback does NOT support GetMixFormat -
		// the format is ours to state - and float32 avoids a conversion step on
		// the way in. ffmpeg reads IEEE-float wav without complaint.
		constexpr int      kRate     = 48000;
		constexpr int      kChannels = 2;
		constexpr int      kBits     = 32;
		constexpr uint16_t kFmtFloat = 3;   // WAVE_FORMAT_IEEE_FLOAT

		constexpr int kMaxRestarts = 8;

		IAudioClient*        s_client  = nullptr;
		IAudioCaptureClient* s_capture = nullptr;
		HANDLE s_event  = nullptr;
		HANDLE s_thread = nullptr;

		FILE*  s_file = nullptr;
		char   s_path[MAX_PATH]{};
		uint32_t s_dataBytes = 0;
		int      s_restarts  = 0;

		// A pump thread that would not stop still owns s_client, s_capture and
		// s_file, so end() deliberately leaks them rather than freeing memory the
		// thread is about to touch. Once that has happened there is no safe way to
		// hand a second pass a fresh FILE* - the old thread may still be writing
		// through the same static - so recording stays off for the session.
		bool s_abandoned = false;

		volatile bool s_run  = false;
		volatile bool s_gate = false;
		volatile bool s_live = false;

		// A 44-byte canonical header, written up front with zero sizes and
		// patched in end(). Streaming to a pipe would avoid the patch, but a
		// real file on disk is what the encoder wants as its second input and
		// what survives a cancelled render.
		void writeHeader(FILE* f)
		{
			const uint32_t byteRate   = kRate * kChannels * (kBits / 8);
			const uint16_t blockAlign = (uint16_t)(kChannels * (kBits / 8));
			const uint32_t fmtSize    = 16;
			const uint16_t ch         = kChannels;
			const uint32_t rate       = kRate;
			const uint16_t bits       = kBits;
			const uint32_t zero       = 0;

			fwrite("RIFF", 1, 4, f); fwrite(&zero, 4, 1, f);
			fwrite("WAVE", 1, 4, f);
			fwrite("fmt ", 1, 4, f); fwrite(&fmtSize, 4, 1, f);
			fwrite(&kFmtFloat, 2, 1, f); fwrite(&ch, 2, 1, f);
			fwrite(&rate, 4, 1, f);      fwrite(&byteRate, 4, 1, f);
			fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
			fwrite("data", 1, 4, f);     fwrite(&zero, 4, 1, f);
		}

		// ActivateAudioInterfaceAsync hands the client back on another thread, so
		// this exists only to signal that it has arrived. IAgileObject because
		// the call may complete in a different apartment than the one asking.
		struct Handler : IActivateAudioInterfaceCompletionHandler, IAgileObject
		{
			HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
			HRESULT result = E_FAIL;
			IAudioClient* client = nullptr;

			~Handler() { if (done) CloseHandle(done); }

			HRESULT STDMETHODCALLTYPE ActivateCompleted(
				IActivateAudioInterfaceAsyncOperation* op) override
			{
				IUnknown* unk = nullptr;
				HRESULT hr = op->GetActivateResult(&result, &unk);
				if (SUCCEEDED(hr) && SUCCEEDED(result) && unk)
					unk->QueryInterface(__uuidof(IAudioClient), (void**)&client);
				if (unk) unk->Release();
				SetEvent(done);
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (!out) return E_POINTER;
				if (riid == __uuidof(IUnknown) ||
				    riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
				    riid == __uuidof(IAgileObject))
				{
					*out = this; return S_OK;
				}
				*out = nullptr; return E_NOINTERFACE;
			}
			// Stack-allocated and outlived by the wait on `done`, so lifetime is
			// not reference counted.
			ULONG STDMETHODCALLTYPE AddRef()  override { return 2; }
			ULONG STDMETHODCALLTYPE Release() override { return 1; }
		};

		void deactivate()
		{
			if (s_client)  s_client->Stop();
			if (s_capture) { s_capture->Release(); s_capture = nullptr; }
			if (s_client)  { s_client->Release();  s_client  = nullptr; }
			if (s_event)   { CloseHandle(s_event); s_event   = nullptr; }
		}

		// Opens a fresh loopback client. Split out of begin() because the pump
		// calls it again mid-recording when the stream dies under it.
		bool activate()
		{
			AUDIOCLIENT_ACTIVATION_PARAMS ap{};
			ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
			ap.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
			// The TREE variant, not the bare process: the audio engine may render
			// on a child of ours, and including the tree costs nothing when there
			// is none. Aiming at our own pid is what keeps other applications out.
			ap.ProcessLoopbackParams.ProcessLoopbackMode =
				PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

			PROPVARIANT pv{};
			pv.vt             = VT_BLOB;
			pv.blob.cbSize    = sizeof(ap);
			pv.blob.pBlobData = (BYTE*)&ap;

			Handler handler;
			IActivateAudioInterfaceAsyncOperation* op = nullptr;
			HRESULT hr = ActivateAudioInterfaceAsync(
				VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
				__uuidof(IAudioClient), &pv, &handler, &op);
			if (op) op->Release();

			if (FAILED(hr) ||
			    WaitForSingleObject(handler.done, 3000) != WAIT_OBJECT_0 ||
			    !handler.client)
			{
				logger::write("info", "audio: process loopback unavailable (hr 0x%08X)",
					(unsigned)(FAILED(hr) ? hr : handler.result));
				return false;
			}
			s_client = handler.client;

			WAVEFORMATEX wf{};
			wf.wFormatTag      = kFmtFloat;
			wf.nChannels       = kChannels;
			wf.nSamplesPerSec  = kRate;
			wf.wBitsPerSample  = kBits;
			wf.nBlockAlign     = (WORD)(kChannels * (kBits / 8));
			wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;

			// Loopback plus event callback, and a period of 0 - process loopback
			// rejects an explicit period.
			hr = s_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				0, 0, &wf, nullptr);
			if (FAILED(hr))
			{
				logger::write("info", "audio: Initialize failed (hr 0x%08X)", (unsigned)hr);
				deactivate();
				return false;
			}

			s_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
			if (FAILED(s_client->SetEventHandle(s_event)) ||
			    FAILED(s_client->GetService(__uuidof(IAudioCaptureClient), (void**)&s_capture)) ||
			    FAILED(s_client->Start()))
			{
				logger::write("info", "audio: could not start the capture stream");
				deactivate();
				return false;
			}
			return true;
		}

		DWORD WINAPI pump(LPVOID)
		{
			// Our own apartment: this thread outlives any single game callback
			// and must not depend on how the game initialised COM elsewhere.
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);

			while (s_run)
			{
				// Not gated on the wait result. A loopback stream produces no
				// events at all while the target is silent, and a clip load is
				// exactly that - so a timeout is normal and the packet query below
				// is what actually reports whether the stream is still alive.
				WaitForSingleObject(s_event, 200);

				UINT32 packet = 0;
				HRESULT hr = s_capture ? s_capture->GetNextPacketSize(&packet) : E_POINTER;

				if (FAILED(hr))
				{
					// The game rebuilds its audio graph across a clip load, which
					// leaves this client attached to a stream that no longer
					// exists. Nothing errors visibly - recording simply stops, so
					// the first clip lands in the file and nothing after it does.
					// Re-activating is the only way to hear the rest.
					if (++s_restarts > kMaxRestarts)
					{
						logger::write("info",
							"audio: stream failed %d times (hr 0x%08X) - giving up, "
							"the rest of this pass is silent", s_restarts, (unsigned)hr);
						s_run = false;
						break;
					}
					logger::write("info",
						"audio: capture stream lost (hr 0x%08X) - restarting (%d)",
						(unsigned)hr, s_restarts);
					deactivate();
					if (!activate()) Sleep(100);
					continue;
				}

				while (packet)
				{
					BYTE*  data  = nullptr;
					UINT32 count = 0;
					DWORD  flags = 0;
					if (FAILED(s_capture->GetBuffer(&data, &count, &flags, nullptr, nullptr)))
						break;

					if (s_gate && s_file && count)
					{
						const uint32_t bytes = count * kChannels * (kBits / 8);

						// SILENT is not "no data" - it means the buffer contents
						// are undefined, so write real zeros rather than passing
						// whatever was left in the shared buffer through.
						if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
						{
							static const uint8_t quiet[4096]{};
							uint32_t left = bytes;
							while (left)
							{
								const uint32_t n = left < sizeof(quiet) ? left : (uint32_t)sizeof(quiet);
								fwrite(quiet, 1, n, s_file);
								left -= n;
							}
						}
						else
						{
							fwrite(data, 1, bytes, s_file);
						}
						s_dataBytes += bytes;
					}

					s_capture->ReleaseBuffer(count);
					packet = 0;
					if (FAILED(s_capture->GetNextPacketSize(&packet))) break;
				}
			}

			CoUninitialize();
			return 0;
		}
	}

	bool begin(const char* folder)
	{
		if (s_live) return true;
		if (s_abandoned)
		{
			logger::write("info",
				"audio: an earlier capture thread never stopped - not starting another. "
				"Renders stay silent until the game is restarted.");
			return false;
		}
		s_dataBytes = 0;
		s_restarts  = 0;
		s_path[0]   = '\0';

		snprintf(s_path, sizeof(s_path), "%s\\audio.wav", folder);
		if (fopen_s(&s_file, s_path, "wb") != 0 || !s_file)
		{
			logger::write("info", "audio: could not open %s", s_path);
			s_path[0] = '\0';
			return false;
		}
		writeHeader(s_file);

		if (!activate())
		{
			fclose(s_file); s_file = nullptr;
			DeleteFileA(s_path); s_path[0] = '\0';
			return false;
		}

		s_run  = true;
		s_gate = false;   // opened by the render once playback is actually rolling
		s_live = true;
		s_thread = CreateThread(nullptr, 0, pump, nullptr, 0, nullptr);

		logger::write("info", "audio: recording %d Hz %d-ch float -> %s",
			kRate, kChannels, s_path);
		return true;
	}

	void gate(bool open) { s_gate = open; }
	bool active()        { return s_live; }

	double seconds()
	{
		return (double)s_dataBytes / (kRate * kChannels * (kBits / 8));
	}

	const char* path() { return s_path; }

	void end()
	{
		if (!s_live) return;
		s_run  = false;
		s_gate = false;

		// Join, and mean it.
		//
		// The pump can be inside activate(), which waits up to 3s on the
		// activation handler - so the old 2s bound could return while the thread
		// was still running, and everything below then tore down state it was
		// about to use. Two real consequences: the freshly Start()ed IAudioClient
		// the thread assigns on its way out was never released, leaking a LIVE
		// capture stream for the rest of the process, and fclose(s_file) raced its
		// next fwrite. Stream loss is what happens at a clip boundary, so that is
		// an ordinary multi-clip audio pass, not a hypothetical.
		//
		// 5s comfortably dominates the worst legitimate case (a 3s activate, the
		// 100ms backoff, a 200ms event wait). Bounded rather than INFINITE because
		// this runs on the game's main thread - a wedged pump must not take the
		// editor down with it.
		bool joined = true;
		if (s_thread)
		{
			joined   = WaitForSingleObject(s_thread, 5000) == WAIT_OBJECT_0;
			CloseHandle(s_thread);
			s_thread = nullptr;
		}

		// It did not stop. The thread still owns the capture client and the FILE*,
		// so releasing either here is precisely the bug this join exists to
		// prevent - leak them on purpose and take the pass as silent.
		if (!joined)
		{
			s_abandoned = true;
			s_live      = false;
			s_path[0]   = '\0';   // so path() reports nothing usable was recorded
			logger::write("info",
				"audio: the capture thread did not stop - leaving the stream and the wav "
				"to it rather than freeing them underneath it. This render is silent.");
			return;
		}

		deactivate();

		if (s_file)
		{
			const uint32_t riff = 36 + s_dataBytes;
			fseek(s_file, 4, SEEK_SET);  fwrite(&riff, 4, 1, s_file);
			fseek(s_file, 40, SEEK_SET); fwrite(&s_dataBytes, 4, 1, s_file);
			fclose(s_file);
			s_file = nullptr;
		}

		if (s_dataBytes == 0)
		{
			// An empty wav would make the encoder fail on a stream with no
			// samples, which is a worse outcome than a silent video.
			logger::write("info", "audio: nothing recorded - rendering silent");
			DeleteFileA(s_path);
			s_path[0] = '\0';
		}
		else
		{
			logger::write("info", "audio: %.2fs recorded (%u bytes, %d restart(s))",
				seconds(), s_dataBytes, s_restarts);
		}
		s_live = false;
	}
}
