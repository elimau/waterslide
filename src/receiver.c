#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "receiver.h"
#include "opus/opus_multistream.h"
#include "ck/ck_ring.h"
#include "globals.h"
#include "demux.h"
#include "endpoint-secure.h"
#include "audio.h"
#include "syncer.h"
#include "utils.h"
#include "pcm.h"

static OpusMSDecoder *decoder = NULL;
static pcm_codec_t pcmDecoder = { 0 };
static demux_channel_t channel1 = { 0 };
static ck_ring_t decodeRing;
static ck_ring_buffer_t *decodeRingBuf;
static int sbnLast = -1;

static unsigned int audioEncoding;
static int networkChannelCount;
static int maxEncodedPacketSize, audioFrameSize, decodeRingMaxSize;
static float *sampleBuf;
static uint8_t *audioEncodedBuf;
static int audioEncodedBufPos = 0;
static bool slipEsc = false;

// returns: number of audio frames added to decodeRing (after resampling), or negative error code
static int decodePacket (const uint8_t *buf, int len) {
  static bool overrun = false;

  int ringCurrentSize = ck_ring_size(&decodeRing);
  globals_set1ui(statsCh1Audio, streamBufferPos, ringCurrentSize / networkChannelCount);

  int result;
  switch (audioEncoding) {
    case AUDIO_ENCODING_OPUS:
      result = opus_multistream_decode_float(decoder, buf, len, sampleBuf, audioFrameSize, 0);
      if (result != audioFrameSize) {
        globals_add1ui(statsCh1AudioOpus, codecErrorCount, 1);
        return -1;
      }
      break;

    case AUDIO_ENCODING_PCM:
      result = pcm_decode(&pcmDecoder, buf, len, sampleBuf);
      if (result != networkChannelCount * audioFrameSize) {
        if (result == -3) globals_add1ui(statsCh1AudioPCM, crcFailCount, 1);
        return -2;
      }
      break;
  }

  if (overrun) {
    // Let the audio callback empty the ring to about half-way before pushing to it again.
    if (ringCurrentSize > decodeRingMaxSize / 2) {
      return 0;
    } else {
      overrun = false;
    }
  }

  result = syncer_enqueueBuf(sampleBuf, audioFrameSize, networkChannelCount);
  if (result == -2) overrun = true;

  return result;
}

static int enqueueSilence (int frameCount) {
  if (frameCount < 0) return -1;

  int totalSamples = networkChannelCount * frameCount;
  // Don't ever let the ring fill completely, that way the channels stay in order
  if ((int)ck_ring_size(&decodeRing) + totalSamples > decodeRingMaxSize) {
    return -2;
  }

  for (int i = 0; i < totalSamples; i++) {
    ck_ring_enqueue_spsc(&decodeRing, decodeRingBuf, (void*)0);
  }

  return frameCount;
}

static int slipDecodeBlock (const uint8_t *buf, int bufLen) {
  for (int i = 0; i < bufLen; i++) {
    if (slipEsc) {
      if (buf[i] == 0xdc) {
        if (audioEncodedBufPos >= maxEncodedPacketSize) return -1;
        audioEncodedBuf[audioEncodedBufPos++] = 0xc0;
      } else if (buf[i] == 0xdd) {
        if (audioEncodedBufPos >= maxEncodedPacketSize) return -2;
        audioEncodedBuf[audioEncodedBufPos++] = 0xdb;
      } else {
        return -3;
      }
      slipEsc = false;
      continue;
    }

    if (buf[i] == 0xc0) {
      if (audioEncodedBufPos == 0) continue;
      decodePacket(audioEncodedBuf, audioEncodedBufPos);
      audioEncodedBufPos = 0;
      continue;
    }

    if (buf[i] == 0xdb) {
      slipEsc = true;
    } else {
      if (audioEncodedBufPos >= maxEncodedPacketSize) return -4;
      audioEncodedBuf[audioEncodedBufPos++] = buf[i];
    }
  }

  return bufLen;
}

// The channel lock in the demux module protects the static variables accessed here
static void onBlockCh1 (const uint8_t *buf, int sbn) {
  bool tryDecode = true;

  if (sbnLast != -1) {
    int sbnDiff;
    if (sbnLast - sbn > 128) {
      // Overflow
      sbnDiff = 256 - sbnLast + sbn;
    } else {
      sbnDiff = sbn - sbnLast;
    }

    if (sbnDiff == 0) {
      // Duplicate block, ignore
      globals_add1ui(statsCh1, dupBlockCount, 1);
      tryDecode = false;
    } else if (sbnDiff < 0) {
      // Out-of-order old block, ignore
      globals_add1ui(statsCh1, oooBlockCount, 1);
      tryDecode = false;
    } else if (sbnDiff > 1) {
      int blockDroppedCount = sbnDiff - 1;
      // Out-of-order, previous block(s) were dropped
      globals_add1ui(statsCh1, oooBlockCount, blockDroppedCount);
      audioEncodedBufPos = 0;
      slipEsc = false;
      tryDecode = false;
    }
  }

  sbnLast = sbn;

  // Don't bother SLIP decoding duplicate or out-of-order blocks
  if (!tryDecode) return;

  int result = slipDecodeBlock(buf, channel1.symbolsPerBlock * channel1.symbolLen);
  if (result < 0) {
    audioEncodedBufPos = 0;
    slipEsc = false;
  }
}

int receiver_init () {
  networkChannelCount = globals_get1i(audio, networkChannelCount);
  audioEncoding = globals_get1ui(audio, encoding);
  int decodeRingLength;
  double encodedSampleRate; // Hz

  switch (audioEncoding) {
    case AUDIO_ENCODING_OPUS:
      encodedSampleRate = AUDIO_OPUS_SAMPLE_RATE;
      audioFrameSize = globals_get1i(opus, frameSize);
      maxEncodedPacketSize = globals_get1i(opus, maxPacketSize);
      decodeRingLength = globals_get1i(opus, decodeRingLength);
      break;
    case AUDIO_ENCODING_PCM:
      encodedSampleRate = globals_get1i(pcm, sampleRate);
      audioFrameSize = globals_get1i(pcm, frameSize);
      // 24-bit samples plus 2 bytes for CRC
      maxEncodedPacketSize = 3 * networkChannelCount * audioFrameSize + 2;
      decodeRingLength = globals_get1i(pcm, decodeRingLength);
      break;
    default:
      printf("Error: Audio encoding %d not implemented.\n", audioEncoding);
      return -1;
  }

  decodeRingMaxSize = networkChannelCount * decodeRingLength;
  // ck requires the size to be a power of two but we will pretend decodeRing contains
  // decodeRingMaxSize number of float values, and ignore the rest.
  int decodeRingAllocSize = utils_roundUpPowerOfTwo(decodeRingMaxSize);

  sampleBuf = (float*)malloc(4 * networkChannelCount * audioFrameSize);
  if (sampleBuf == NULL) return -1;
  audioEncodedBuf = (uint8_t *)malloc(maxEncodedPacketSize);
  if (audioEncodedBuf == NULL) return -2;
  decodeRingBuf = (ck_ring_buffer_t*)malloc(sizeof(ck_ring_buffer_t) * decodeRingAllocSize);
  if (decodeRingBuf == NULL) return -3;
  memset(decodeRingBuf, 0, sizeof(ck_ring_buffer_t) * decodeRingAllocSize);

  if (demux_init() < 0) return -4;

  channel1.chId = 1;
  channel1.symbolsPerBlock = globals_get1i(fec, sourceSymbolsPerBlock);
  channel1.symbolLen = globals_get1i(fec, symbolLen);
  channel1.onBlock = onBlockCh1;
  demux_addChannel(&channel1);

  int err;
  if (audioEncoding == AUDIO_ENCODING_OPUS) {
    unsigned char mapping[networkChannelCount];
    for (int i = 0; i < networkChannelCount; i++) mapping[i] = i;
    decoder = opus_multistream_decoder_create(AUDIO_OPUS_SAMPLE_RATE, networkChannelCount, networkChannelCount, 0, mapping, &err);
    if (err < 0) {
      printf("opus_multistream_decoder_create failed: %s\n", opus_strerror(err));
      return -5;
    }
  }

  ck_ring_init(&decodeRing, decodeRingAllocSize);
  // half-fill ring buffer
  enqueueSilence(decodeRingLength / 2);

  if (audio_init(&decodeRing, decodeRingBuf, decodeRingMaxSize) < 0) return -6;

  char audioDeviceName[MAX_DEVICE_NAME_LEN + 1] = { 0 };
  globals_get1s(audio, deviceName, audioDeviceName, sizeof(audioDeviceName));
  if (audio_start(audioDeviceName) < 0) return -7;

  double ioSampleRate = globals_get1i(audio, ioSampleRate);
  err = syncer_init(encodedSampleRate, ioSampleRate, audioFrameSize, &decodeRing, decodeRingBuf, decodeRingMaxSize);
  if (err < 0) {
    printf("syncer_init error: %d\n", err);
    return -8;
  }

  char privateKey[SEC_KEY_LENGTH + 1] = { 0 };
  char peerPublicKey[SEC_KEY_LENGTH + 1] = { 0 };
  globals_get1s(root, privateKey, privateKey, sizeof(privateKey));
  globals_get1s(root, peerPublicKey, peerPublicKey, sizeof(peerPublicKey));

  if (strlen(privateKey) != SEC_KEY_LENGTH || strlen(peerPublicKey) != SEC_KEY_LENGTH) {
    printf("Expected privateKey and peerPublicKey to be base64 x25519 keys.\n");
    return -9;
  }

  err = endpointsec_init(demux_readPacket);
  if (err < 0) {
    printf("endpointsec_init error: %d\n", err);
    return -10;
  }

  return 0;
}
