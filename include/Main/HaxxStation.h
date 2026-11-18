#pragma once

#include "types.h"
#include "code_02142eac.h"

#define MKDS_BOOTSTRAP_STOCK_CLONEBOOT 0
#define MKDS_BOOTSTRAP_HAXXSTATION     1

#ifndef MKDS_BOOTSTRAP_MODE
#define MKDS_BOOTSTRAP_MODE MKDS_BOOTSTRAP_HAXXSTATION
#endif

#define HAXXSTATION_STATION_GGID 0x00400120u
#define HAXXSTATION_MKDS_GGID    0x00000025u

#define HAXXSTATION_MENU_MAX_BYTES  0x1000u
#define HAXXSTATION_CACHE_PAGE_SIZE 0x2000u
#define HAXXSTATION_CACHE_SLOTS     16

typedef enum {
    HS_PHASE_IDLE                    = 0,
    HS_PHASE_STAGE1_LOBBY            = 1,
    HS_PHASE_STAGE1_CLOSE_ENTRY      = 2,
    HS_PHASE_STAGE1_BOOT_WAIT        = 3,
    HS_PHASE_STAGE1_MB_RESET         = 4,
    HS_PHASE_STATION_APP_START       = 5,
    HS_PHASE_STATION_APP_SERVE       = 6,
    HS_PHASE_STATION_EXECUTE_BARRIER = 7,
    HS_PHASE_STATION_SHUTDOWN        = 8
} HaxxStationPhase;

typedef enum {
    HS_PEER_IDLE             = 0,
    HS_PEER_MENU_LEN_SEND    = 1,
    HS_PEER_MENU_LEN_WAIT    = 2,
    HS_PEER_MENU_BODY        = 3,
    HS_PEER_MENU_DONE        = 4,
    HS_PEER_PAYLOAD_LEN_SEND = 5,
    HS_PEER_PAYLOAD_LEN_WAIT = 6,
    HS_PEER_PAYLOAD_START    = 7,
    HS_PEER_PAYLOAD_BODY     = 8,
    HS_PEER_BODY_COMPLETE    = 9,
    HS_PEER_EXECUTED         = 10
} HaxxStationPeerPhase;

typedef struct HaxxStationPeer {
    u8 associated;
    u8 phase;
    u16 chunk;
    u32 offset;
    u32 generation;
    u16 cooldown;
    u16 reserved;
} HaxxStationPeer;

typedef struct HaxxStationCacheSlot {
    u32 pageIndex;
    u32 validBytes;
    u32 lastUsedTick;
} HaxxStationCacheSlot;

typedef struct HaxxStationHost {
    /* 0x00 */ u32 magic;
    /* 0x04 */ u32 phase;
    /* 0x08 */ u32 activeGgid;
    /* 0x0c */ u32 savedTgid;
    /* 0x10 */ u32 savedChannel;
    /* 0x14 */ u32 expectedGuests;
    /* 0x18 */ u32 lastGeneration;
    /* 0x1c */ u32 lastSendOffset;
    /* 0x20 */ s32 lastSendStatus;
    /* 0x24 */ u32 originalMbGgid;
    /* 0x28 */ u16 savedBootMask;
    /* 0x2a */ u16 stationAppMask;
    /* 0x2c */ u16 bodyDoneMask;
    /* 0x2e */ u16 executedMask;
    /* 0x30 */ u16 executedDisconnectedMask;
    /* 0x32 */ u8 entryCloseState;
    /* 0x33 */ u8 callbacksInstalled;
    /* 0x34 */ u8 internalReset;
    /* 0x35 */ u8 payloadFileOpen;
    /* 0x36 */ u16 handoffGraceFrames;
    /* 0x38 */ u32 menuSize;
    /* 0x3c */ u32 payloadSize;
    /* 0x40 */ u8 *menuBuf;
    /* 0x44 */ u8 *cacheBuf;
    /* 0x48 */ u16 *txBuf;
    /* 0x4c */ volatile u32 txBusy;
    /* 0x50 */ u32 txAid;
    /* 0x54 */ u32 txPhase;
    /* 0x58 */ u32 txOffset;
    /* 0x5c */ u32 txBytes;
    /* 0x60 */ u32 txGen;
    /* 0x64 */ u32 nextAid;
    /* 0x68 */ u32 cacheTick;
    /* 0x6c */ u32 lastError;
    /* 0x70 */ HaxxStationCacheSlot cacheSlots[HAXXSTATION_CACHE_SLOTS];
    /* 0x130 */ HaxxStationPeer peers[16];
    /* 0x230 */ u32 payloadFileWords[18];
    /* 0x278 */ u16 txMask;
    /* 0x27a */ u16 txPad;
    /* 0x27c */ u32 txGenByAid[16];
} HaxxStationHost;

extern HaxxStationHost gHaxxStationHost;

BOOL HaxxStation_IsEnabled(void);
void HaxxStation_AllocSceneBuffers(void *heap);
u32 HaxxStation_GetStage1Ggid(void);
void HaxxStation_OnStage1ParentInit(void);
void HaxxStation_PreTick(void);
BOOL HaxxStation_StepBeforeStage1Boot(void);
BOOL HaxxStation_StepBootstrap(void);
BOOL HaxxStation_HandleMbCallback(u32 index, u32 code, void *arg);
BOOL HaxxStation_OnMbParentEvent(s32 eventCode, Unk021425e8 *eventData);
BOOL HaxxStation_ShouldIgnoreMbChildMsg(u32 child);
int HaxxStation_SendParentControl(u8 kind, u32 childMask, BOOL *handled);
void HaxxStation_OnMbEndCalled(void);
