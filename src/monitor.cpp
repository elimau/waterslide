#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <atomic>
#include <stdint.h>
#include <string.h>
#include "uWebSockets/libuwebsockets.h"
#include "globals.h"
// TODO: fix up the generated protobufs code so we can turn pedantic back on
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "protobufs/monitor.pb.h"
#pragma GCC diagnostic pop
#include "config.h"
#include "monitor.h"

// DEBUG: test
// #define SYNC_RECORD_LENGTH 12000 // 10 mins
// #include "syncer.h"

static int audioChannelCount, endpointCount;
// DEBUG: two threads are accessing the members of wsClient (wsThread and statsThread). Make sure these are thread-safe
static std::atomic<uws_ws_t*> wsClient = NULL;

static void openHandler(uws_ws_t *ws) {
  wsClient = ws;
}

static void messageHandler(UNUSED uws_ws_t *ws, UNUSED const char *msg, UNUSED size_t length, UNUSED unsigned char opCode) {
  // printf("message (code: %d, length: %zu): %.*s\n", opCode, length, length, msg);
}

static void closeHandler(UNUSED uws_ws_t *ws, UNUSED int code) {
  wsClient = NULL;
}

static void listenHandler(void *listenSocket) {
  if (listenSocket) {
    int wsPort = globals_get1i(monitor, wsPort);
    printf("Monitor: WebSocket server listening on port %d\n", wsPort);
  }
}

static void *startWsApp (UNUSED void *arg) {
  uws_app_t *app = uws_createApp();
  uws_appWs(app, "/*", openHandler, messageHandler, closeHandler);
  uws_appListen(app, globals_get1i(monitor, wsPort), listenHandler);
  uws_appRun(app);

  return NULL;
}

// map an array of bins (unsigned ints) to an array of uint8 values that can be displayed in a heatmap graph
static void mapStreamMeterBins (unsigned int *rawBins, uint8_t *mappedBins) {
  unsigned int minBinVal = 0, maxBinVal = 0;
  for (int i = 0; i < STATS_STREAM_METER_BINS; i++) {
    // save streamMeterBins so it doesn't change while we are doing the mapping
    rawBins[i] = globals_get1uiv(statsCh1Audio, streamMeterBins, i);
    if (i == 0) {
      minBinVal = maxBinVal = rawBins[i];
    } else if (rawBins[i] < minBinVal && rawBins[i] > 0) {
      minBinVal = rawBins[i];
    } else if (rawBins[i] > maxBinVal) {
      maxBinVal = rawBins[i];
    }
  }

  if (minBinVal == maxBinVal) return; // no dynamic range (probably streamMeterBins is all zeros)

  for (int i = 0; i < STATS_STREAM_METER_BINS; i++) {
    unsigned int binVal = rawBins[i];
    if (binVal < minBinVal) {
      mappedBins[i] = 0;
      continue;
    }

    mappedBins[i] = 255 * (binVal - minBinVal) / (maxBinVal - minBinVal);
    // Make sure streamMeterBins with count > 0 are also > 0 after mapping
    if (mappedBins[i] == 0) mappedBins[i] = 1;
  }
}

// map a ring buffer to a flat buffer, excluding the element at the write head
static void mapBlockTimingRing (uint8_t *dest) {
  unsigned int ringPos = globals_get1ui(statsCh1, blockTimingRingPos);
  if (++ringPos == STATS_BLOCK_TIMING_RING_LEN) ringPos = 0;
  for (int i = 0; i < STATS_BLOCK_TIMING_RING_LEN - 1; i++) {
    unsigned int val = globals_get1uiv(statsCh1, blockTimingRing, ringPos);
    memcpy(&dest[4*i], &val, 4);
    if (++ringPos == STATS_BLOCK_TIMING_RING_LEN) ringPos = 0;
  }
}

static void *statsLoop (UNUSED void *arg) {
  MonitorProto proto;
  MonitorProto_MuxChannelStats *protoCh1 = proto.add_muxchannel();
  MonitorProto_AudioChannel **protoAudioChannels = new MonitorProto_AudioChannel*[audioChannelCount];
  MonitorProto_EndpointStats **protoEndpoints = new MonitorProto_EndpointStats*[endpointCount];
  unsigned int *streamMeterBinsRaw = new unsigned int[STATS_STREAM_METER_BINS];
  uint8_t *streamMeterBinsMapped = new uint8_t[STATS_STREAM_METER_BINS];
  uint8_t *blockTimingRingMapped = new uint8_t[4 * (STATS_BLOCK_TIMING_RING_LEN-1)];

  memset(streamMeterBinsRaw, 0, sizeof(unsigned int) * STATS_STREAM_METER_BINS);
  memset(streamMeterBinsMapped, 0, STATS_STREAM_METER_BINS);

  for (int i = 0; i < audioChannelCount; i++) {
    protoAudioChannels[i] = protoCh1->mutable_audiostats()->add_audiochannel();
  }
  for (int i = 0; i < endpointCount; i++) {
    protoEndpoints[i] = protoCh1->add_endpoint();
    std::string ifName(MAX_NET_IF_NAME_LEN + 1, '\0');
    int ifLen = globals_get1sv(endpoints, interface, i, &ifName[0], MAX_NET_IF_NAME_LEN + 1);
    if (ifLen <= 0) {
      ifName = "any";
    } else {
      ifName.resize(ifLen);
    }
    protoEndpoints[i]->set_interfacename(ifName);
  }

  // DEBUG: test
  // FILE *syncDataFile = fopen("receiver-sync.data", "w+b");
  // uint8_t *syncDataBuf = (uint8_t *)malloc(8 * SYNC_RECORD_LENGTH);
  // int debugCounter = 0;
  // printf("writing to receiver-sync.data...\n");
  // TODO: need a flag here to break out of the while loop and deinit properly
  while (true) {
    usleep(50000);
    if (wsClient == NULL) continue;

    // DEBUG: test
    // if (debugCounter < SYNC_RECORD_LENGTH) {
    //   double receiverSync;
    //   globals_get1ff(statsCh1Audio, receiverSyncFilt, &receiverSync);
    //   memcpy(&syncDataBuf[8*debugCounter], &receiverSync, 8);

    //   if (debugCounter == 6000) { // 10 min
    //     printf("syncer_changeRate returned %d\n", syncer_changeRate(47999.7));
    //   }

    // } else if (debugCounter == SYNC_RECORD_LENGTH) {
    //   fwrite(syncDataBuf, 8 * SYNC_RECORD_LENGTH, 1, syncDataFile);
    //   fclose(syncDataFile);
    //   printf("done!\n");
    // }
    // debugCounter++;

    for (int i = 0; i < audioChannelCount; i++) {
      protoAudioChannels[i]->set_clippingcount(globals_get1uiv(statsCh1Audio, clippingCounts, i));
      double levelFast, levelSlow;
      globals_get1ffv(statsCh1Audio, levelsFast, i, &levelFast);
      globals_get1ffv(statsCh1Audio, levelsSlow, i, &levelSlow);
      protoAudioChannels[i]->set_levelfast(levelFast);
      protoAudioChannels[i]->set_levelslow(levelSlow);
    }

    protoCh1->set_dupblockcount(globals_get1ui(statsCh1, dupBlockCount));
    protoCh1->set_oooblockcount(globals_get1ui(statsCh1, oooBlockCount));
    mapBlockTimingRing(blockTimingRingMapped);
    protoCh1->set_blocktiming(blockTimingRingMapped, 4 * (STATS_BLOCK_TIMING_RING_LEN-1));

    int lastSbn0 = globals_get1iv(statsCh1Endpoints, lastSbn, 0);
    for (int i = 0; i < endpointCount; i++) {
      int relSbn = globals_get1iv(statsCh1Endpoints, lastSbn, i) - lastSbn0;
      if (relSbn > 127) relSbn -= 256;
      if (relSbn < -128) relSbn += 256;
      protoEndpoints[i]->set_lastrelativesbn(relSbn);
      protoEndpoints[i]->set_open(globals_get1uiv(statsEndpoints, open, i));
      protoEndpoints[i]->set_bytesout(globals_get1uiv(statsEndpoints, bytesOut, i));
      protoEndpoints[i]->set_bytesin(globals_get1uiv(statsEndpoints, bytesIn, i));
      protoEndpoints[i]->set_sendcongestion(globals_get1uiv(statsEndpoints, sendCongestion, i));
    }
    protoCh1->mutable_audiostats()->set_streambuffersize(globals_get1i(statsCh1Audio, streamBufferSize));
    protoCh1->mutable_audiostats()->set_bufferoverruncount(globals_get1ui(statsCh1Audio, bufferOverrunCount));
    protoCh1->mutable_audiostats()->set_bufferunderruncount(globals_get1ui(statsCh1Audio, bufferUnderrunCount));
    protoCh1->mutable_audiostats()->set_encodethreadjittercount(globals_get1ui(statsCh1Audio, encodeThreadJitterCount));
    protoCh1->mutable_audiostats()->set_audioloopxruncount(globals_get1ui(statsCh1Audio, audioLoopXrunCount));
    double receiverSyncFilt;
    globals_get1ff(statsCh1Audio, receiverSyncFilt, &receiverSyncFilt);
    protoCh1->mutable_audiostats()->set_receiversync(receiverSyncFilt);

    mapStreamMeterBins(streamMeterBinsRaw, streamMeterBinsMapped);
    protoCh1->mutable_audiostats()->set_streammeterbins(streamMeterBinsMapped, STATS_STREAM_METER_BINS);

    switch (globals_get1ui(audio, encoding)) {
      case AUDIO_ENCODING_OPUS:
        protoCh1->mutable_audiostats()->mutable_opusstats()->set_codecerrorcount(globals_get1ui(statsCh1AudioOpus, codecErrorCount));
        break;
      case AUDIO_ENCODING_PCM:
        protoCh1->mutable_audiostats()->mutable_pcmstats()->set_crcfailcount(globals_get1ui(statsCh1AudioPCM, crcFailCount));
        break;
    }

    std::string protoData;
    proto.SerializeToString(&protoData);
    int error = uws_wsSend(wsClient, protoData.c_str(), protoData.length(), UWS_OPCODE_BINARY);
    if (error < 0) printf("uws_wsSend error: %d\n", error); // DEBUG: log
  }

  delete[] protoAudioChannels;
  delete[] protoEndpoints;
  delete[] streamMeterBinsRaw;
  delete[] streamMeterBinsMapped;
  delete[] blockTimingRingMapped;
  return NULL;
}

int monitor_init (void) {
  audioChannelCount = globals_get1i(audio, networkChannelCount);
  endpointCount = globals_get1i(endpoints, endpointCount);

  pthread_t wsThread, statsThread;
  int err = pthread_create(&wsThread, NULL, startWsApp, NULL);
  if (err != 0) return -2;
  err = pthread_create(&statsThread, NULL, statsLoop, NULL);
  if (err != 0) return -3;

  return 0;
}
