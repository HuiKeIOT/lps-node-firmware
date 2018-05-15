/*
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * LPS node firmware.
 *
 * Copyright 2018, Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * uwb_tdoa_anchor3.c is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with uwb_tdoa_anchor3.c. If not, see <http://www.gnu.org/licenses/>.
 */
/* uwb_tdoa_anchor3.c: Uwb TDOA anchor V3, randomized tx times and ids */

/*

The anchor is part of a static system of other anchors, anchors are not moving.
New anchors may be added or removed during the lifetime of the system, but this
type of event is rare and adaptation to a new setup may take a few seconds.
Packets are transmitted at random times by all anchors and by listening to surrounding
anchors one anchor can calculate the distance to the other anchors.
An anchor must not make any assumptions of the ids of surrounding anchors.
It is assumed that the anchor density is evenly distributed in the covered
volume and that 5-15 anchors are visible in every point.

The implementation must handle
1. Transmit at random times
2. Any anchor ids
3. Added or removed anchors
4. Around 20 concurrent visible anchors

*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "uwb.h"
#include "libdw1000.h"
#include "mac.h"

#include "cfg.h"
#include "lpp.h"

#define debug(...) printf(__VA_ARGS__)

// Time length of the preamble
#define PREAMBLE_LENGTH_S ( 128 * 1017.63e-9 )
#define PREAMBLE_LENGTH (uint64_t)( PREAMBLE_LENGTH_S * 499.2e6 * 128 )

// Guard length to account for clock drift and time of flight
#define TDMA_GUARD_LENGTH_S ( 1e-6 )
#define TDMA_GUARD_LENGTH (uint64_t)( TDMA_GUARD_LENGTH_S * 499.2e6 * 128 )

#define TDMA_EXTRA_LENGTH_S ( 300e-6 )
#define TDMA_EXTRA_LENGTH (uint64_t)( TDMA_EXTRA_LENGTH_S * 499.2e6 * 128 )

#define TDMA_HIGH_RES_RAND_S ( 1e-3 )
#define TDMA_HIGH_RES_RAND (uint64_t)( TDMA_HIGH_RES_RAND_S * 499.2e6 * 128 )

#define ANCHOR_LIST_UPDATE_INTERVAL 1000;

#define ANCHOR_STORAGE_COUNT 8
#define REMOTE_TX_MAX_COUNT 8
#if REMOTE_TX_MAX_COUNT > ANCHOR_STORAGE_COUNT
  #error "Invalid settings"
#endif

#define ID_COUNT 256
#define ID_WITHOUT_CONTEXT 0xff
#define ID_INVALID 0xff

// Useful constants
static const uint8_t base_address[] = {0,0,0,0,0,0,0xcf,0xbc};

// Anchor context
typedef struct {
  uint8_t id;
  bool isUsed;
  uint8_t seqNr;
  uint32_t rxTimeStamp;
  uint32_t txTimeStamp;
  uint16_t distance;
} anchorContext_t;

// This context struct contains all the required global values of the algorithm
static struct ctx_s {
  int anchorId;

  // Information about latest transmitted packet
  uint8_t seqNr;
  uint32_t txTime; // In UWB clock ticks

  // Next transmit time in system clock ticks
  uint32_t nextTxTick;

  // List of ids to transmit in remote data section
  uint8_t remoteTxId[REMOTE_TX_MAX_COUNT];
  uint8_t remoteTxIdCount;

  // The list of anchors to transmit and store is updated at regular intervals
  uint32_t nextAnchorListUpdate;

  // Remote anchor data
  uint8_t anchorCtxLookup[ID_COUNT];
  anchorContext_t anchorCtx[ANCHOR_STORAGE_COUNT];
  uint8_t anchorRxCount[ID_COUNT];
} ctx;

// Packet formats
#define PACKET_TYPE_TDOA3 0x30

typedef struct {
  uint8_t type;
  uint8_t seq;
  uint32_t txTimeStamp;
  uint8_t remoteCount;
} __attribute__((packed)) rangePacketHeader3_t;

typedef struct {
  uint8_t id;
  uint8_t seq;
  uint32_t rxTimeStamp;
  uint16_t distance;
} __attribute__((packed)) remoteAnchorDataFull_t;

typedef struct {
  uint8_t id;
  uint8_t seq;
  uint32_t rxTimeStamp;
} __attribute__((packed)) remoteAnchorDataShort_t;

typedef struct {
  rangePacketHeader3_t header;
  uint8_t remoteAnchorData;
} __attribute__((packed)) rangePacket3_t;


#define LPP_HEADER 0
#define LPP_TYPE (LPP_HEADER + 1)
#define LPP_PAYLOAD (LPP_HEADER + 2)


static anchorContext_t* getContext(uint8_t anchorId) {
  uint8_t slot = ctx.anchorCtxLookup[anchorId];

  if (slot == ID_WITHOUT_CONTEXT) {
    return 0;
  }

  return &ctx.anchorCtx[slot];
}

static void clearAnchorRxCount() {
  memset(&ctx.anchorRxCount, 0, ID_COUNT);
}

static void removeAnchorContextsNotInList(const uint8_t* id, const uint8_t count) {
  for (int i = 0; i < ANCHOR_STORAGE_COUNT; i++) {
    anchorContext_t* anchorCtx = &ctx.anchorCtx[i];
    if (anchorCtx->isUsed) {
      const uint8_t ctxId = anchorCtx->id;
      bool found = false;
      for (int j = 0; j < count; j++) {
        if (id[j] == ctxId) {
          found = true;
          break;
        }
      }

      if (!found) {
        ctx.anchorCtxLookup[ctxId] = ID_WITHOUT_CONTEXT;
        anchorCtx->isUsed = false;
      }
    }
  }
}

static void createAnchorContext(const uint8_t id) {
  if (ctx.anchorCtxLookup[id] != ID_WITHOUT_CONTEXT) {
    // Already has a contex, we're done
    return;
  }

  for (uint8_t i = 0; i < ANCHOR_STORAGE_COUNT; i++) {
    anchorContext_t* anchorCtx = &ctx.anchorCtx[i];
    if (!anchorCtx->isUsed) {
      ctx.anchorCtxLookup[id] = i;

      memset(anchorCtx, 0, sizeof(anchorContext_t));
      anchorCtx->id = id;
      anchorCtx->isUsed = true;

      debug("+ctx %i %i\r\n", id, i);
      break;
    }
  }
}

static void createAnchorContextsInList(const uint8_t* id, const uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    createAnchorContext(id[i]);
  }
}


// This function is called at regular intervals to update lists containing data
// about which anchors to store and add to outgoing messages. This
// update might take some time but this should not be a problem since the TX
// times are randomized anyway. The intention is that we could plug in clever
// algorithms here that optimizes which anchors to use.
static void updateAnchorLists() {
  // First naive approach is to use the first anchors that we have seen since
  // last update.

  // TODO krri Do something clever

  static uint8_t id[ANCHOR_STORAGE_COUNT];
  static uint8_t count[ANCHOR_STORAGE_COUNT];

  memset(id, 0, sizeof(id));
  memset(count, 0, sizeof(count));

  uint8_t index = 0;

  for (int anchor = 0; anchor < ID_COUNT; anchor++) {
    uint8_t rxCount = ctx.anchorRxCount[anchor];

    if (rxCount > 0) {
      count[index] = rxCount;
      id[index++] = anchor;

      if (index >= ANCHOR_STORAGE_COUNT) {
        break;
      }
    }
  }

  removeAnchorContextsNotInList(id, index);
  createAnchorContextsInList(id, index);

  uint8_t remoteTXIdIndex = 0;
  for (int i = 0; i < index; i++) {
    if (remoteTXIdIndex < REMOTE_TX_MAX_COUNT) {
      ctx.remoteTxId[remoteTXIdIndex++] = id[i];
    }
  }
  ctx.remoteTxIdCount = remoteTXIdIndex;

  clearAnchorRxCount();
}

/* Adjust time for schedule transfer by DW1000 radio. Set 9 LSB to 0 */
static uint32_t adjustTxRxTime(dwTime_t *time)
{
  uint32_t added = (1<<9) - (time->low32 & ((1<<9)-1));

  time->low32 = (time->low32 & ~((1<<9)-1)) + (1<<9);

  return added;
}

static dwTime_t findTransmitTimeAsSoonAsPossible(dwDevice_t *dev)
{
  dwTime_t transmitTime = { .full = 0 };
  dwGetSystemTimestamp(dev, &transmitTime);

  // Add guard and preamble time
  transmitTime.full += TDMA_GUARD_LENGTH;
  transmitTime.full += PREAMBLE_LENGTH;

  // And some extra
  transmitTime.full += TDMA_EXTRA_LENGTH;

  // TODO krri Adding randomization on this level adds a long delay, is it worth it?
  // The randomization on OS level is quantized to 1 ms (tick time of the system)
  // Add a high res random to smooth it out
  // uint32_t r = random();
  // uint32_t delay = r % TDMA_HIGH_RES_RAND;
  // transmitTime.full += delay;

  // DW1000 can only schedule time with 9 LSB at 0, adjust for it
  adjustTxRxTime(&transmitTime);

  return transmitTime;
}


static uint16_t calculateDistance(anchorContext_t* anchorCtx, int remoteTxSeqNr, int remoteRxSeqNr, uint32_t remoteTx, uint32_t remoteRx, uint32_t rx)
{
  // Check that the 2 last packets are consecutive packets and that our last packet is in between
  if ((anchorCtx->seqNr == ((remoteTxSeqNr - 1) & 0x07f)) && remoteRxSeqNr == ctx.seqNr) {
    double tround1 = remoteRx - anchorCtx->txTimeStamp;
    double treply1 = ctx.txTime - anchorCtx->rxTimeStamp;
    double tround2 = rx - ctx.txTime;
    double treply2 = remoteTx - remoteRx;

    uint32_t distance = ((tround2 * tround1)-(treply1 * treply2)) / (2 * (treply1 + tround2));
    return distance & 0xfffful;
  } else {
    return 0;
  }
}

static bool extractFromPacket(const rangePacket3_t* rangePacket, uint32_t* remoteRx, uint8_t* remoteRxSeqNr) {
  const void* anchorDataPtr = &rangePacket->remoteAnchorData;
  for (uint8_t i = 0; i < rangePacket->header.remoteCount; i++) {
    remoteAnchorDataFull_t* anchorData = (remoteAnchorDataFull_t*)anchorDataPtr;

    const uint8_t id = anchorData->id;
    if (id == ctx.anchorId) {
      *remoteRxSeqNr = anchorData->seq & 0x7f;
      *remoteRx = anchorData->rxTimeStamp;
      return true;
    }

    bool hasDistance = ((anchorData->seq & 0x80) != 0);
    if (hasDistance) {
      anchorDataPtr += sizeof(remoteAnchorDataFull_t);
    } else {
      anchorDataPtr += sizeof(remoteAnchorDataShort_t);
    }
  }

  return false;
}


static void handleRangePacket(const uint32_t rxTime, const packet_t* rxPacket)
{
  const uint8_t remoteAnchorId = rxPacket->sourceAddress[0];
  ctx.anchorRxCount[remoteAnchorId]++;
  anchorContext_t* anchorCtx = getContext(remoteAnchorId);
  if (anchorCtx) {
    const rangePacket3_t* rangePacket = (rangePacket3_t *)rxPacket->payload;

    uint32_t remoteTx = rangePacket->header.txTimeStamp;
    uint8_t remoteTxSeqNr = rangePacket->header.seq;

    uint32_t remoteRx = 0;
    uint8_t remoteRxSeqNr = 0;
    bool dataFound = extractFromPacket(rangePacket, &remoteRx, &remoteRxSeqNr);

    if (dataFound) {
      uint16_t distance = calculateDistance(anchorCtx, remoteTxSeqNr, remoteRxSeqNr, remoteTx, remoteRx, rxTime);

      if (distance > 0) {
        anchorCtx->distance = distance;
      }

      anchorCtx->rxTimeStamp = rxTime;
      anchorCtx->seqNr = remoteTxSeqNr;
      anchorCtx->txTimeStamp = remoteTx;
    }
  }
}

static void handleRxPacket(dwDevice_t *dev)
{
  static packet_t rxPacket;
  dwTime_t rxTime = { .full = 0 };

  dwGetRawReceiveTimestamp(dev, &rxTime);
  dwCorrectTimestamp(dev, &rxTime);

  int dataLength = dwGetDataLength(dev);
  rxPacket.payload[0] = 0;
  dwGetData(dev, (uint8_t*)&rxPacket, dataLength);

  if (dataLength == 0) {
    return;
  }

  switch(rxPacket.payload[0]) {
  case PACKET_TYPE_TDOA3:
    handleRangePacket(rxTime.low32, &rxPacket);
    break;
  case SHORT_LPP:
    if (rxPacket.destAddress[0] == ctx.anchorId) {
      lppHandleShortPacket(&rxPacket.payload[1], dataLength - MAC802154_HEADER_LENGTH - 1);
    }
    break;
  default:
    // Do nothing
    break;
  }
}

static void setupRx(dwDevice_t *dev)
{
  dwNewReceive(dev);
  dwSetDefaults(dev);
  dwStartReceive(dev);
}

static int populateTxData(rangePacket3_t *rangePacket)
{
  // rangePacket->header.type already populated
  rangePacket->header.seq = ctx.seqNr;
  rangePacket->header.txTimeStamp = ctx.txTime;
  rangePacket->header.remoteCount = ctx.remoteTxIdCount;

  remoteAnchorDataFull_t* anchorData = (remoteAnchorDataFull_t*) &rangePacket->remoteAnchorData;
  for (uint8_t i = 0; i < ctx.remoteTxIdCount; i++) {
    uint8_t id = ctx.remoteTxId[i];
    anchorContext_t* anchorCtx = getContext(id);

    anchorData->id = id;
    anchorData->seq = anchorCtx->seqNr | 0x80;
    anchorData->rxTimeStamp = anchorCtx->rxTimeStamp;
    anchorData->distance = anchorCtx->distance;

    anchorData++;
  }

  return (void*)anchorData - (void*)rangePacket;
}

// Set TX data in the radio TX buffer
static void setTxData(dwDevice_t *dev)
{
  static packet_t txPacket;
  static bool firstEntry = true;
  static int lppLength = 0;

  if (firstEntry) {

    memcpy(txPacket.sourceAddress, base_address, 8);
    txPacket.sourceAddress[0] = ctx.anchorId;
    memcpy(txPacket.destAddress, base_address, 8);
    txPacket.destAddress[0] = 0xff;

    txPacket.payload[0] = PACKET_TYPE_TDOA3;

    firstEntry = false;
  }

  uwbConfig_t *uwbConfig = uwbGetConfig();

  int rangePacketSize = populateTxData((rangePacket3_t *)txPacket.payload);

  // LPP anchor position is currently sent in all packets
  if (uwbConfig->positionEnabled) {
    txPacket.payload[rangePacketSize + LPP_HEADER] = SHORT_LPP;
    txPacket.payload[rangePacketSize + LPP_TYPE] = LPP_SHORT_ANCHOR_POSITION;

    struct lppShortAnchorPosition_s *pos = (struct lppShortAnchorPosition_s*) &txPacket.payload[rangePacketSize + LPP_PAYLOAD];
    memcpy(pos->position, uwbConfig->position, 3 * sizeof(float));

    lppLength = 2 + sizeof(struct lppShortAnchorPosition_s);
  }

  dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH + rangePacketSize + lppLength);
}

// Setup the radio to send a packet
static void setupTx(dwDevice_t *dev)
{
  dwTime_t txTime = findTransmitTimeAsSoonAsPossible(dev);
  ctx.txTime = txTime.low32;
  ctx.seqNr = (ctx.seqNr + 1) & 0x7f;

  setTxData(dev);

  dwNewTransmit(dev);
  dwSetDefaults(dev);
  dwSetTxRxTime(dev, txTime);

  dwStartTransmit(dev);
}

static uint32_t randomizeDelayToNextTx()
{
  const uint32_t average = 20;
  const uint32_t interval = 6;

  uint32_t r = random();
  uint32_t delay = average + r % interval - interval / 2;

  return delay;
}

static uint32_t startNextEvent(dwDevice_t *dev)
{
  dwIdle(dev);

  uint32_t now = xTaskGetTickCount();
  int32_t timeToNextTx = ctx.nextTxTick - now;
  if (timeToNextTx < 0) {
    uint32_t newDelay = randomizeDelayToNextTx();
    ctx.nextTxTick = now + newDelay;

    setupTx(dev);
  } else {
    setupRx(dev);
  }

  return ctx.nextTxTick - now;
}

// Initialize/reset the agorithm
static void tdoa3Init(uwbConfig_t * config, dwDevice_t *dev)
{
  ctx.anchorId = config->address[0];
  ctx.seqNr = 0;
  ctx.txTime = 0;
  ctx.nextTxTick = 0;
  ctx.remoteTxIdCount = 0;
  ctx.nextAnchorListUpdate = 0;

  memset(&ctx.anchorCtxLookup, ID_WITHOUT_CONTEXT, ID_COUNT);
  for (int i = 0; i < ANCHOR_STORAGE_COUNT; i++) {
    ctx.anchorCtx[i].isUsed = false;
  }

  clearAnchorRxCount();
}

// Called for each DW radio event
static uint32_t tdoa3UwbEvent(dwDevice_t *dev, uwbEvent_t event)
{
  switch (event) {
    case eventPacketReceived: {
        handleRxPacket(dev);
      }
      break;
    default:
      // Nothing here
      break;
  }

  uint32_t now = xTaskGetTickCount();
  if (now > ctx.nextAnchorListUpdate) {
    updateAnchorLists();
    ctx.nextAnchorListUpdate = now + ANCHOR_LIST_UPDATE_INTERVAL;
  }

  uint32_t timeout_ms = startNextEvent(dev);
  return timeout_ms;
}


uwbAlgorithm_t uwbTdoa3Algorithm = {
  .init = tdoa3Init,
  .onEvent = tdoa3UwbEvent,
};
