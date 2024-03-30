#pragma once
#pragma once

#ifndef AUDIO_RING_BUFFER_H
#define AUDIO_RING_BUFFER_H

#include <wdm.h>
#include <definitions.h>

class AudioRingBuffer
{
public:

	AudioRingBuffer() = default;
	~AudioRingBuffer() = default;

	void Clear() {
		m_Buffer = nullptr;
		m_BufferSize = 0;
		m_NumFrames = 0;
		m_PartialFrameIn = nullptr;
		m_PartialFrameBytesIn = 0;
		m_PartialFrameOut = nullptr;
		m_PartialFrameBytesOut = 0;
		m_FrameSizeIn = 0;
		m_FrameSizeOut = 0;
		m_WriteFrameId = 0;
		m_ReadFrameId = 0;
		m_waveFormat = nullptr;
		m_ChannelCountIn = 0;
		m_BitsPerSampleIn = 0;
		m_SamplesPerSecondIn = 0;
		m_ChannelCountOut = 0;
		m_BitsPerSampleOut = 0;
		m_SamplesPerSecondOut = 0;
	};

	BOOLEAN InitializeInput(PKSDATAFORMAT  pDataFormat);
	BOOLEAN InitializeOutput(_In_    PWAVEFORMATEXTENSIBLE   WfExt);
	void Reset();

	// Writes data to the internal buffer
	BOOLEAN WriteData(_In_reads_bytes_(ulByteCount)   PBYTE   pBuffer,
		_In_    ULONG   ulByteCount);

	// Reads data from the internal buffer, filling with silence if necessary
	BOOLEAN ReadData(_Out_writes_bytes_(size) BYTE* data, _In_ ULONG size);

private:
	BYTE* m_Buffer;                   // Pointer to the buffer memory
	ULONG m_BufferSize;               // Size of the buffer
	ULONG m_NumFrames;
	WORD            m_ChannelCountIn;
	WORD            m_BitsPerSampleIn;
	DWORD           m_SamplesPerSecondIn;
	BYTE* m_PartialFrameIn;
	DWORD m_PartialFrameBytesIn;
	BYTE* m_PartialFrameOut;
	DWORD m_PartialFrameBytesOut;
	WORD            m_ChannelCountOut;
	WORD            m_BitsPerSampleOut;
	DWORD           m_SamplesPerSecondOut;
	ULONG m_FrameSizeIn;
	ULONG m_FrameSizeOut;
	ULONG m_WriteFrameId;
	ULONG m_ReadFrameId;
	KSPIN_LOCK m_Lock;                // Spin lock for synchronization
	// output format
	PWAVEFORMATEX               m_waveFormat;

};



#endif // AUDIO_RING_BUFFER_H#pragma once