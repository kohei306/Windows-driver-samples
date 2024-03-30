#include "audio_ring_buffer.h"

#define INPUTAUDIOSAMPLESPERSECOND 44100
#define INPUTWAVEFORMATPOOLTAG 'wfm'
#define PARTIALFRAMEINPOOLTAG 'pfi'
#define PARTIALFRAMEOUTPOOLTAG 'pfo'
#define AUDIORINGBUFFERINTERNALBUFFERPOOLTAG 'sab'


BOOLEAN AudioRingBuffer::InitializeInput(PKSDATAFORMAT  pDataFormat)
{
	if (m_Buffer != nullptr) {
		return FALSE;

	}

	PAGED_CODE();


	ASSERT(pDataFormat);

	PWAVEFORMATEX pwfx = nullptr;

	if (IsEqualGUIDAligned(pDataFormat->Specifier,
		KSDATAFORMAT_SPECIFIER_DSOUND))
	{
		pwfx =
			&(((PKSDATAFORMAT_DSOUND)pDataFormat)->BufferDesc.WaveFormatEx);
	}
	else if (IsEqualGUIDAligned(pDataFormat->Specifier,
		KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
	{
		pwfx = &((PKSDATAFORMAT_WAVEFORMATEX)pDataFormat)->WaveFormatEx;
	}

	if (pwfx)
	{
		if (m_waveFormat == nullptr) {
			m_waveFormat = (PWAVEFORMATEX)
				ExAllocatePool2
				(
					POOL_FLAG_NON_PAGED,
					(pwfx->wFormatTag == WAVE_FORMAT_PCM) ?
					sizeof(PCMWAVEFORMAT) :
					sizeof(WAVEFORMATEX) + pwfx->cbSize,
					INPUTWAVEFORMATPOOLTAG
				);
		}

		if (m_waveFormat)
		{
			RtlCopyMemory(m_waveFormat,
				pwfx,
				(pwfx->wFormatTag == WAVE_FORMAT_PCM) ?
				sizeof(PCMWAVEFORMAT) :
				sizeof(WAVEFORMATEX) + pwfx->cbSize);

			m_ChannelCountIn = m_waveFormat->nChannels;      // # channels.
			m_BitsPerSampleIn = m_waveFormat->wBitsPerSample; // bits per sample.
			m_SamplesPerSecondIn = m_waveFormat->nSamplesPerSec; // samples per sec.
			m_FrameSizeIn = m_ChannelCountIn * m_BitsPerSampleIn / 8;
		}
		else
		{
			return FALSE;
		}
	}



	DbgPrint("Render Channel count : %d, Bits per sample : %d, Samples per second : %d\n", m_ChannelCountIn, m_BitsPerSampleIn, m_SamplesPerSecondIn);
	m_BufferSize = m_SamplesPerSecondIn * (m_BitsPerSampleIn / 8) * m_ChannelCountIn * INPUTAUDIOSAMPLESPERSECOND;
	m_NumFrames = m_BufferSize / m_FrameSizeIn;

	if (m_Buffer == nullptr) {
		m_Buffer = (BYTE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, m_BufferSize, AUDIORINGBUFFERINTERNALBUFFERPOOLTAG);
		if (m_Buffer == nullptr) {
			return FALSE;
		}
	}

	if (m_PartialFrameIn == nullptr) {
		m_PartialFrameIn = (BYTE*)ExAllocatePool2(
			POOL_FLAG_NON_PAGED,
			m_FrameSizeIn,
			PARTIALFRAMEINPOOLTAG);
	}

	m_WriteFrameId = 0;
	m_ReadFrameId = 0;
	m_PartialFrameBytesIn = 0;


	// Initialize the spin lock.
	KeInitializeSpinLock(&m_Lock);
	return TRUE;
}

BOOLEAN AudioRingBuffer::InitializeOutput(_In_    PWAVEFORMATEXTENSIBLE   WfExt)
{
	// check if WfExt is not null

	if (WfExt == nullptr)
	{
		return false;
	}
	if ((WfExt->Format.wFormatTag != WAVE_FORMAT_PCM &&
		!(WfExt->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
			IsEqualGUIDAligned(WfExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))))
	{
		return false;
	}
	KIRQL oldIrql;
	KeAcquireSpinLock(&m_Lock, &oldIrql);
	m_ChannelCountOut = WfExt->Format.nChannels;      // # channels.
	m_BitsPerSampleOut = WfExt->Format.wBitsPerSample; // bits per sample.
	m_SamplesPerSecondOut = WfExt->Format.nSamplesPerSec; // samples per sec.
	m_FrameSizeOut = m_ChannelCountOut * m_BitsPerSampleOut / 8;

	DbgPrint("Capture Channel Count: %d, Bits Per Sample: %d, Samples Per Second: %d", m_ChannelCountOut, m_BitsPerSampleOut, m_SamplesPerSecondOut);
	if (m_PartialFrameOut == nullptr) {
		m_PartialFrameOut = (BYTE*)ExAllocatePool2(
			POOL_FLAG_NON_PAGED,
			m_FrameSizeOut,
			PARTIALFRAMEOUTPOOLTAG);
	}
	m_PartialFrameBytesOut = 0;
	m_WriteFrameId = 0;
	m_ReadFrameId = 0;
	KeReleaseSpinLock(&m_Lock, oldIrql);
	return true;
}


void AudioRingBuffer::Reset()
{
	KIRQL oldIrql;
	KeAcquireSpinLock(&m_Lock, &oldIrql);

	// Free the buffer memory.
	if (m_Buffer != nullptr) {
		ExFreePoolWithTag(m_Buffer, AUDIORINGBUFFERINTERNALBUFFERPOOLTAG);
		m_Buffer = nullptr;
	}

	if (m_waveFormat)
	{
		ExFreePoolWithTag(m_waveFormat, INPUTWAVEFORMATPOOLTAG);
		m_waveFormat = nullptr;
	}

	if (m_PartialFrameIn) {
		ExFreePoolWithTag(m_PartialFrameIn, PARTIALFRAMEINPOOLTAG);
		m_PartialFrameIn = nullptr;
	}

	if (m_PartialFrameOut) {
		ExFreePoolWithTag(m_PartialFrameOut, PARTIALFRAMEOUTPOOLTAG);
		m_PartialFrameOut = nullptr;
	}

	KeReleaseSpinLock(&m_Lock, oldIrql);
}

BOOLEAN AudioRingBuffer::WriteData(
	_In_reads_bytes_(ulByteCount)   PBYTE   pBuffer,
	_In_    ULONG   ulByteCount)
{
	if (m_Buffer == nullptr || ulByteCount == 0 || pBuffer == nullptr) {
		return FALSE;
	}

	ULONG length = ulByteCount;
	BYTE* buffer = pBuffer;

	KIRQL oldIrql;
	KeAcquireSpinLock(&m_Lock, &oldIrql);

	if (m_PartialFrameBytesIn > 0) {
		ULONG partialFrameBytes = min(length, m_PartialFrameBytesIn);
		DWORD offset = m_FrameSizeIn - m_PartialFrameBytesIn;
		RtlCopyMemory(m_PartialFrameIn + offset, buffer, partialFrameBytes);
		ULONG currentWritePosition = m_WriteFrameId * m_FrameSizeIn;
		RtlCopyMemory(m_Buffer + currentWritePosition, m_PartialFrameIn, m_FrameSizeIn);
		RtlZeroMemory(m_PartialFrameIn, m_FrameSizeIn);
		buffer += partialFrameBytes;
		length -= partialFrameBytes;
		if (m_WriteFrameId + 1 == m_NumFrames) {
			if (m_ReadFrameId == 0) {
				m_ReadFrameId = (m_ReadFrameId + 1) % m_NumFrames;
			}
			m_WriteFrameId = 0;
		}
		else {
			if (m_WriteFrameId + 1 == m_ReadFrameId) {
				m_ReadFrameId = (m_ReadFrameId + 1) % m_NumFrames;
			}
			m_WriteFrameId = (m_WriteFrameId + 1) % m_NumFrames;
		}
		m_PartialFrameBytesIn = 0;
	}

	if (length == 0) {
		KeReleaseSpinLock(&m_Lock, oldIrql);
		return TRUE;
	}

	ULONG numFrames = length / m_FrameSizeIn;

	for (ULONG i = 0; i < numFrames; ++i) {
		ULONG currentWritePosition = m_WriteFrameId * m_FrameSizeIn;
		RtlCopyMemory(m_Buffer + currentWritePosition, buffer, m_FrameSizeIn);
		buffer += m_FrameSizeIn;
		length -= m_FrameSizeIn;
		if (m_WriteFrameId + 1 == m_NumFrames) {
			if (m_ReadFrameId == 0) {
				m_ReadFrameId = (m_ReadFrameId + 1) % m_NumFrames;
			}
			m_WriteFrameId = 0;
		}
		else {
			if (m_WriteFrameId + 1 == m_ReadFrameId) {
				m_ReadFrameId = (m_ReadFrameId + 1) % m_NumFrames;
			}
			m_WriteFrameId = (m_WriteFrameId + 1) % m_NumFrames;
		}
	}

	if (length > 0) {
		ASSERT(m_FrameSizeIn > length);
		RtlCopyMemory(m_PartialFrameIn, buffer, length);
		m_PartialFrameBytesIn = m_FrameSizeIn - length;
	}

	KeReleaseSpinLock(&m_Lock, oldIrql);
	return TRUE;
}



BOOLEAN AudioRingBuffer::ReadData(
	_Out_writes_bytes_(size) BYTE* data,
	_In_ ULONG size)
{
	if (size == 0 || m_Buffer == nullptr)
	{
		return FALSE;
	}

	ULONG length = size;
	BYTE* buffer = data;
	// Acquire lock for thread safety.
	KIRQL oldIrql;
	KeAcquireSpinLock(&m_Lock, &oldIrql);

	if (m_PartialFrameBytesOut > 0) {
		ULONG partialFrameBytes = min(length, m_PartialFrameBytesOut);
		DWORD offset = m_FrameSizeOut - m_PartialFrameBytesOut;
		RtlCopyMemory(buffer, m_PartialFrameOut + offset, partialFrameBytes);
		m_PartialFrameBytesOut -= partialFrameBytes;
		length -= partialFrameBytes;
		buffer += partialFrameBytes;
		RtlZeroMemory(m_PartialFrameOut, m_FrameSizeOut);
		m_PartialFrameBytesOut = 0;
	}

	if (m_WriteFrameId == m_ReadFrameId) {
		// buffer is empty, fill with silence
		RtlZeroMemory(buffer, length);
		KeReleaseSpinLock(&m_Lock, oldIrql);
		return TRUE;
	}

	if (length == 0) {
		// Release lock.
		KeReleaseSpinLock(&m_Lock, oldIrql);
		return TRUE;
	}

	ULONG num_frames = length / m_FrameSizeOut;
	BOOLEAN empty = false;

	for (ULONG i = 0; i < num_frames; ++i)
	{
		ULONG currentReadPosition = m_ReadFrameId * m_FrameSizeIn;
		short* dataBuffer = reinterpret_cast<short*>(m_Buffer + currentReadPosition);
		long* dataOut = reinterpret_cast<long*>(buffer);
		for (ULONG channel_idx = 0; channel_idx < m_ChannelCountOut; ++channel_idx)
		{
			dataOut[channel_idx] = static_cast<long>(dataBuffer[channel_idx] << 16);
		}
		length -= m_FrameSizeOut;
		buffer += m_FrameSizeOut;
		m_ReadFrameId = (m_ReadFrameId + 1) % m_NumFrames;
		if (m_ReadFrameId == m_WriteFrameId)
		{
			// emptry buffer
			empty = true;
			break;
		}
	}

	if (length > 0) {
		long* partialFrameOut = reinterpret_cast<long*>(m_PartialFrameOut);
		if (empty)
		{
			while (length >= m_FrameSizeOut) {
				long* dataOut = reinterpret_cast<long*>(buffer);
				for (ULONG channel_idx = 0; channel_idx < m_ChannelCountOut; ++channel_idx)
				{
					dataOut[channel_idx] = static_cast<long>(0);
				}
				buffer += m_FrameSizeOut;
				length -= m_FrameSizeOut;
			}
			if (length > 0) {
				ASSERT(m_FrameSizeOut > length);
				for (ULONG channel_idx = 0; channel_idx < m_ChannelCountOut; ++channel_idx)
				{
					partialFrameOut[channel_idx] = static_cast<long>(0);
				}
				RtlCopyMemory(buffer, m_PartialFrameOut, length);
				buffer += length;
				RtlZeroMemory(m_PartialFrameOut, length);
				m_PartialFrameBytesOut = m_FrameSizeOut - (DWORD)length;
			}
		}
		else {
			ASSERT(m_FrameSizeOut > length);
			ULONG currentReadPosition = m_ReadFrameId * m_FrameSizeIn;
			short* dataBuffer = reinterpret_cast<short*>(m_Buffer + currentReadPosition);
			for (ULONG channel_idx = 0; channel_idx < m_ChannelCountOut; ++channel_idx)
			{
				partialFrameOut[channel_idx] = static_cast<long>(dataBuffer[channel_idx] << 16);
			}
			m_ReadFrameId = (m_ReadFrameId + 1) % m_NumFrames;

			RtlCopyMemory(buffer, m_PartialFrameOut, length);
			buffer += length;
			RtlZeroMemory(m_PartialFrameOut, length);
			m_PartialFrameBytesOut = m_FrameSizeOut - (DWORD)length;
		}
	}
	KeReleaseSpinLock(&m_Lock, oldIrql);
	return TRUE;
}