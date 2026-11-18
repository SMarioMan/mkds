#include "Main/HaxxStation.h"

#include "Main/System/Heap.h"
#include "Main/Unknown/code_02007148.h"
#include "Main/Unknown/code_020071b4.h"
#include "Main/Unknown/code_02007624.h"
#include "Main/Unknown/code_02007ea0.h"
#include "Main/Unknown/code_02008908.h"
#include "Main/Unknown/code_02009928.h"
#include "Main/Unknown/code_02009cc8.h"
#include "Main/Unknown/code_0200ad00.h"
#include "Main/Unknown/code_0204df90.h"
#include "Main/Unknown/code_0204ea14.h"
#include "Main/Unknown/code_02050300.h"
#include "Main/Unknown/code_0211d4a8.h"
#include "Main/Unknown/unk_02154000.h"
#include "Main/Unknown/unk_0216f000.h"
#include "Main/Unknown/unk_02175000.h"
#include "Main/Unknown/unk_0217a000.h"
#include "code_0200fc8c.h"
#include "code_02142eac.h"
#include "code_0214d018.h"
#include "fs_file.h"
#include "wm_etc.h"
#include "wm_system.h"

#pragma thumb on

HaxxStationHost gHaxxStationHost = { 0x58584148u };

static void HaxxStation_Pump(void);
static void HaxxStation_PortRecvCb(Unk021425e8 *cb);

static u32 HaxxStation_Popcount16(u32 mask) {
    u32 count = 0;
    u32 bit;
    for (bit = 1; bit <= 15; bit++) {
        if ((mask & (1u << bit)) != 0) {
            count++;
        }
    }
    return count;
}

BOOL HaxxStation_IsEnabled(void) {
#if MKDS_BOOTSTRAP_MODE == MKDS_BOOTSTRAP_HAXXSTATION
    FSFileID fid;
    if (*(u16 *) 0x027ffc40 == 2) {
        return FALSE;
    }
    if (FS_ConvertPathToFileID(&fid, (const u8 *) "/haxx/station_client.srl") != 0) {
        return TRUE;
    }
    return FALSE;
#else
    return FALSE;
#endif
}

void HaxxStation_AllocSceneBuffers(void *heap) {
    if (!HaxxStation_IsEnabled()) {
        gHaxxStationHost.menuBuf = NULL;
        gHaxxStationHost.cacheBuf = NULL;
        gHaxxStationHost.txBuf = NULL;
        return;
    }
    gHaxxStationHost.menuBuf = (u8 *) HeapAllocAligned(heap, HAXXSTATION_MENU_MAX_BYTES, 0x20);
    gHaxxStationHost.cacheBuf =
        (u8 *) HeapAllocAligned(heap, HAXXSTATION_CACHE_PAGE_SIZE * HAXXSTATION_CACHE_SLOTS, 0x20);
    gHaxxStationHost.txBuf = (u16 *) HeapAllocAligned(heap, 160, 0x20);
}

u32 HaxxStation_GetStage1Ggid(void) {
    if (HaxxStation_IsEnabled()) {
        return HAXXSTATION_STATION_GGID;
    }
    return HAXXSTATION_MKDS_GGID;
}

static void HaxxStation_ClosePayloadFile(void) {
    if (gHaxxStationHost.payloadFileOpen != 0) {
        FS_CloseFile((FSFile *) gHaxxStationHost.payloadFileWords);
        gHaxxStationHost.payloadFileOpen = 0;
    }
}

static void HaxxStation_InstallPortCallbacks(void) {
    s32 r = 0;
    s32 p;
    for (p = 13; p <= 15; p++) {
        r |= WM_SetPortCallback((u16) p, (UnkFunc *) HaxxStation_PortRecvCb, NULL);
    }
    gHaxxStationHost.callbacksInstalled = (r == 0) ? 1 : 0;
}

static void HaxxStation_RestoreNativePortsAndIdentity(void) {
    if (gHaxxStationHost.callbacksInstalled != 0) {
        WM_SetPortCallback(13, NULL, NULL);
        WM_SetPortCallback(14, NULL, NULL);
        WM_SetPortCallback(15, (UnkFunc *) MPDataPortCallback, NULL);
        gHaxxStationHost.callbacksInstalled = 0;
    }
    data_0215472c[5] = (void *) HAXXSTATION_MKDS_GGID;
    gHaxxStationHost.activeGgid = HAXXSTATION_MKDS_GGID;
    HaxxStation_ClosePayloadFile();
}

void HaxxStation_OnStage1ParentInit(void) {
    u32 i;
    if (!HaxxStation_IsEnabled()) {
        gHaxxStationHost.phase = HS_PHASE_IDLE;
        return;
    }
    HaxxStation_ClosePayloadFile();
    gHaxxStationHost.originalMbGgid = HAXXSTATION_MKDS_GGID;
    data_0215472c[5] = (void *) HAXXSTATION_STATION_GGID;

    gHaxxStationHost.phase = HS_PHASE_STAGE1_LOBBY;
    gHaxxStationHost.activeGgid = HAXXSTATION_STATION_GGID;
    if (data_0217aa00->mUnk_0088 != 1 && data_0217aa00->mUnk_0088 != 7 && data_0217aa00->mUnk_0088 != 13) {
        data_0217aa00->mUnk_0088 = 1;
    }
    gHaxxStationHost.savedTgid = data_0217aa00->mUnk_1690;
    gHaxxStationHost.savedChannel = (u8) data_0217aa00->mUnk_0088;
    gHaxxStationHost.expectedGuests = 0;
    gHaxxStationHost.lastGeneration = 0;
    gHaxxStationHost.lastSendOffset = 0;
    gHaxxStationHost.lastSendStatus = 0;
    gHaxxStationHost.savedBootMask = 0;
    gHaxxStationHost.stationAppMask = 0;
    gHaxxStationHost.bodyDoneMask = 0;
    gHaxxStationHost.executedMask = 0;
    gHaxxStationHost.executedDisconnectedMask = 0;
    gHaxxStationHost.entryCloseState = 0;
    gHaxxStationHost.callbacksInstalled = 0;
    gHaxxStationHost.internalReset = 0;
    gHaxxStationHost.handoffGraceFrames = 0;
    gHaxxStationHost.menuSize = 0;
    gHaxxStationHost.payloadSize = 0;
    gHaxxStationHost.txBusy = 0;
    gHaxxStationHost.txAid = 0;
    gHaxxStationHost.txMask = 0;
    gHaxxStationHost.txPad = 0;
    gHaxxStationHost.txPhase = 0;
    gHaxxStationHost.txOffset = 0;
    gHaxxStationHost.txBytes = 0;
    gHaxxStationHost.txGen = 0;
    gHaxxStationHost.nextAid = 1;
    gHaxxStationHost.cacheTick = 1;
    gHaxxStationHost.lastError = 0;

    for (i = 0; i < HAXXSTATION_CACHE_SLOTS; i++) {
        gHaxxStationHost.cacheSlots[i].pageIndex = 0xffffffffu;
        gHaxxStationHost.cacheSlots[i].validBytes = 0;
        gHaxxStationHost.cacheSlots[i].lastUsedTick = 0;
    }
    for (i = 0; i < 16; i++) {
        gHaxxStationHost.peers[i].associated = 0;
        gHaxxStationHost.peers[i].phase = HS_PEER_IDLE;
        gHaxxStationHost.peers[i].chunk = 0;
        gHaxxStationHost.peers[i].offset = 0;
        gHaxxStationHost.peers[i].generation = 1;
        gHaxxStationHost.peers[i].cooldown = 0;
        gHaxxStationHost.peers[i].reserved = 0;
        gHaxxStationHost.txGenByAid[i] = 0;
    }
}

static void HaxxStation_OnEntryClosedCb(Unk021425e8 *cb) {
    WMi_SetCallbackTable(0x21, ParentCallback);
    if (cb != NULL && cb->mResultCode == 0) {
        gHaxxStationHost.entryCloseState = 3;
    } else {
        gHaxxStationHost.entryCloseState = 1;
    }
}

static void HaxxStation_OnBeaconClosedCb(Unk021425e8 *cb) {
    WMi_SetCallbackTable(0x18, NULL);
    if (cb != NULL && cb->mResultCode == 0) {
        gHaxxStationHost.entryCloseState = 5;
    } else {
        gHaxxStationHost.entryCloseState = 3;
    }
}

static BOOL HaxxStation_AllPeersPastMenu(void) {
    u32 aid;
    u32 pastMenuCount = 0;
    if (gHaxxStationHost.expectedGuests == 0) {
        return FALSE;
    }
    for (aid = 1; aid <= 15; aid++) {
        if (gHaxxStationHost.peers[aid].associated != 0 &&
            gHaxxStationHost.peers[aid].phase >= HS_PEER_PAYLOAD_LEN_SEND) {
            pastMenuCount++;
        }
    }
    return pastMenuCount >= gHaxxStationHost.expectedGuests;
}

void HaxxStation_PreTick(void) {
    u32 savedIrq;
    if (gHaxxStationHost.phase != HS_PHASE_STAGE1_BOOT_WAIT &&
        gHaxxStationHost.phase != HS_PHASE_STATION_APP_SERVE) {
        return;
    }
    savedIrq = OS_DisableInterrupts();
    if (data_0216fdac != NULL && gWirelessWork != NULL && gWirelessWork->mMPStarted != 0 &&
        gWirelessWork->mResetting == 0 && gWirelessWork->mDataQueued == 0 &&
        gWirelessWork->mConnectedAids != 0) {
        if (gHaxxStationHost.phase == HS_PHASE_STAGE1_BOOT_WAIT || !HaxxStation_AllPeersPastMenu()) {
            func_020071b4();
        }
    }
    OS_RestoreInterrupts(savedIrq);

    if (gHaxxStationHost.phase == HS_PHASE_STAGE1_BOOT_WAIT && data_0217aa00 != NULL &&
        data_0217aa00->mUnk_000c == 25 && data_0217aa00->mUnk_1696 != 0 &&
        data_0217aa00->mUnk_1696 == (data_0217aa00->mUnk_1696 & data_0217aa00->mUnk_1695)) {
        gHaxxStationHost.handoffGraceFrames++;
        if (gHaxxStationHost.handoffGraceFrames >= 30) {
            data_0217aa00->mRequestPending = 0;
        }
    }
}

BOOL HaxxStation_StepBeforeStage1Boot(void) {
    if (!HaxxStation_IsEnabled() || gHaxxStationHost.phase == HS_PHASE_IDLE) {
        return TRUE;
    }
    if (gHaxxStationHost.entryCloseState == 0) {
        gHaxxStationHost.phase = HS_PHASE_STAGE1_CLOSE_ENTRY;
        if (gWirelessWork != NULL) {
            gWirelessWork->mUnk_00e = 0;
        }
        gHaxxStationHost.entryCloseState = 1;
    }
    if (gHaxxStationHost.entryCloseState == 1) {
        gHaxxStationHost.entryCloseState = 2;
        if (WM_SetEntry(HaxxStation_OnEntryClosedCb, FALSE) != 2) {
            gHaxxStationHost.entryCloseState = 1;
        }
        return FALSE;
    }
    if (gHaxxStationHost.entryCloseState == 3) {
        gHaxxStationHost.entryCloseState = 4;
        if (WM_SetGameInfo(HaxxStation_OnBeaconClosedCb, data_0217f960, 0x70, GetGgid(), GetTgid(),
                           GetGameInfoAttr()) != 2) {
            gHaxxStationHost.entryCloseState = 3;
        }
        return FALSE;
    }
    if (gHaxxStationHost.entryCloseState == 5) {
        gHaxxStationHost.phase = HS_PHASE_STAGE1_BOOT_WAIT;
        return TRUE;
    }
    return FALSE;
}

static BOOL HaxxStation_LoadCachePage(u32 pageIndex) {
    u32 i;
    u32 victim = 0;
    u32 oldestTick = 0xffffffffu;
    u32 fileOffset;
    u32 remain;
    u32 toRead;
    u8 *slotPtr;
    FSFile *pf;

    if (gHaxxStationHost.payloadFileOpen == 0 || gHaxxStationHost.cacheBuf == NULL) {
        return FALSE;
    }
    fileOffset = pageIndex * HAXXSTATION_CACHE_PAGE_SIZE;
    if (fileOffset >= gHaxxStationHost.payloadSize) {
        return TRUE;
    }
    for (i = 0; i < HAXXSTATION_CACHE_SLOTS; i++) {
        if (gHaxxStationHost.cacheSlots[i].pageIndex == pageIndex &&
            gHaxxStationHost.cacheSlots[i].validBytes > 0) {
            gHaxxStationHost.cacheSlots[i].lastUsedTick = ++gHaxxStationHost.cacheTick;
            return TRUE;
        }
    }
    for (i = 0; i < HAXXSTATION_CACHE_SLOTS; i++) {
        if (gHaxxStationHost.cacheSlots[i].validBytes == 0) {
            victim = i;
            break;
        }
        if (gHaxxStationHost.cacheSlots[i].lastUsedTick < oldestTick) {
            oldestTick = gHaxxStationHost.cacheSlots[i].lastUsedTick;
            victim = i;
        }
    }

    remain = gHaxxStationHost.payloadSize - fileOffset;
    toRead = remain < HAXXSTATION_CACHE_PAGE_SIZE ? remain : HAXXSTATION_CACHE_PAGE_SIZE;
    slotPtr = gHaxxStationHost.cacheBuf + victim * HAXXSTATION_CACHE_PAGE_SIZE;
    pf = (FSFile *) gHaxxStationHost.payloadFileWords;

    gHaxxStationHost.cacheSlots[victim].validBytes = 0;
    FS_SeekFile(pf, (s32) fileOffset, 0);
    if ((u32) FS_ReadFile(pf, slotPtr, (s32) toRead) != toRead) {
        gHaxxStationHost.lastError = 0x101u;
        return FALSE;
    }
    DC_FlushRange(slotPtr, (toRead + 0x1fu) & ~0x1fu);
    gHaxxStationHost.cacheSlots[victim].pageIndex = pageIndex;
    gHaxxStationHost.cacheSlots[victim].validBytes = toRead;
    gHaxxStationHost.cacheSlots[victim].lastUsedTick = ++gHaxxStationHost.cacheTick;
    return TRUE;
}

static void HaxxStation_RefillCacheForPeers(void) {
    u32 aid;
    for (aid = 1; aid <= 15; aid++) {
        HaxxStationPeer *peer = &gHaxxStationHost.peers[aid];
        if (peer->associated != 0 &&
            (peer->phase == HS_PEER_PAYLOAD_LEN_SEND || peer->phase == HS_PEER_PAYLOAD_LEN_WAIT ||
             peer->phase == HS_PEER_PAYLOAD_START || peer->phase == HS_PEER_PAYLOAD_BODY) &&
            peer->offset < gHaxxStationHost.payloadSize) {
            u32 page0 = peer->offset / HAXXSTATION_CACHE_PAGE_SIZE;
            u32 pageOff = peer->offset % HAXXSTATION_CACHE_PAGE_SIZE;
            HaxxStation_LoadCachePage(page0);
            if (pageOff + 126u > HAXXSTATION_CACHE_PAGE_SIZE &&
                (page0 + 1u) * HAXXSTATION_CACHE_PAGE_SIZE < gHaxxStationHost.payloadSize) {
                HaxxStation_LoadCachePage(page0 + 1u);
            }
        }
    }
}

static BOOL HaxxStation_CopyFromCache(u32 offset, u8 *dst, u32 length) {
    u32 copied = 0;
    while (copied < length) {
        u32 curOff = offset + copied;
        u32 pageIdx = curOff / HAXXSTATION_CACHE_PAGE_SIZE;
        u32 pageOff = curOff % HAXXSTATION_CACHE_PAGE_SIZE;
        u32 slotIdx = HAXXSTATION_CACHE_SLOTS;
        u32 i;
        u32 avail;
        u32 step;
        for (i = 0; i < HAXXSTATION_CACHE_SLOTS; i++) {
            if (gHaxxStationHost.cacheSlots[i].pageIndex == pageIdx &&
                pageOff < gHaxxStationHost.cacheSlots[i].validBytes) {
                slotIdx = i;
                break;
            }
        }
        if (slotIdx == HAXXSTATION_CACHE_SLOTS) {
            return FALSE;
        }
        avail = gHaxxStationHost.cacheSlots[slotIdx].validBytes - pageOff;
        step = (length - copied) < avail ? (length - copied) : avail;
        MI_CpuCopy8(gHaxxStationHost.cacheBuf + slotIdx * HAXXSTATION_CACHE_PAGE_SIZE + pageOff, dst + copied, step);
        copied += step;
    }
    return TRUE;
}

static BOOL HaxxStation_CanExecuteStage2(void) {
    u32 readyMask;
    if (gHaxxStationHost.expectedGuests == 0 || gWirelessWork == NULL) {
        return FALSE;
    }
    if (gHaxxStationHost.txBusy != 0) {
        return FALSE;
    }
    readyMask = (gHaxxStationHost.bodyDoneMask | gHaxxStationHost.executedMask);
    if (HaxxStation_Popcount16(readyMask) < gHaxxStationHost.expectedGuests) {
        return FALSE;
    }
    return TRUE;
}

static u16 HaxxStation_GetReadyAidsMask(void) {
    u16 mask;
    Unk02142988 *status;
    if (gWirelessWork == NULL) {
        return 0;
    }
    mask = (u16) (gWirelessWork->mConnectedAids & 0xfffeu);
    status = WMi_GetStatusAddress();
    if (status != NULL) {
        DC_InvalidateRange(&status->mMpReadyBitmap, 2);
        mask &= status->mMpReadyBitmap;
    }
    return mask;
}

static void HaxxStation_PortSentCb(Unk021425e8 *cb) {
    u32 savedIrq = OS_DisableInterrupts();
    u16 err = cb != NULL ? cb->mResultCode : 1;
    /* In NitroSDK ARM7 WM (0x81 / WM_APIID_SET_MP_DATA), sentBitmap is at +0x10 (cb->mUnk_10); +0x0a is port! */
    u16 sentBitmap = cb != NULL ? cb->mUnk_10 : 0;
    u16 txMask = gHaxxStationHost.txMask;
    u16 deliveredMask = (err == 0 || err == 15) ? (u16) (sentBitmap & txMask) : 0;
    u32 aid;

    gHaxxStationHost.txBusy = 0;
    gHaxxStationHost.txMask = 0;
    gHaxxStationHost.lastSendStatus = (s32) err;

    if (deliveredMask != 0) {
        for (aid = 1; aid <= 15; aid++) {
            HaxxStationPeer *peer;
            if ((deliveredMask & (1u << aid)) == 0) {
                continue;
            }
            peer = &gHaxxStationHost.peers[aid];
            if (peer->associated == 0 || peer->generation != gHaxxStationHost.txGenByAid[aid] ||
                peer->phase != gHaxxStationHost.txPhase) {
                continue;
            }
            switch (peer->phase) {
                case HS_PEER_MENU_LEN_SEND:
                    peer->phase = HS_PEER_MENU_LEN_WAIT;
                    break;
                case HS_PEER_PAYLOAD_LEN_SEND:
                    peer->phase = HS_PEER_PAYLOAD_LEN_WAIT;
                    break;
                case HS_PEER_PAYLOAD_START:
                    peer->phase = HS_PEER_PAYLOAD_BODY;
                    peer->cooldown = 45;
                    break;
                case HS_PEER_MENU_BODY:
                    if (gHaxxStationHost.txOffset == gHaxxStationHost.menuSize) {
                        peer->phase = HS_PEER_MENU_DONE;
                    } else if (peer->offset == gHaxxStationHost.txOffset) {
                        peer->offset += gHaxxStationHost.txBytes;
                        peer->chunk++;
                        gHaxxStationHost.lastSendOffset = peer->offset;
                    }
                    break;
                case HS_PEER_PAYLOAD_BODY:
                    if (peer->offset == gHaxxStationHost.txOffset) {
                        peer->offset += gHaxxStationHost.txBytes;
                        peer->chunk++;
                        gHaxxStationHost.lastSendOffset = peer->offset;
                        if (peer->offset >= gHaxxStationHost.payloadSize) {
                            peer->phase = HS_PEER_BODY_COMPLETE;
                            gHaxxStationHost.bodyDoneMask |= (u16) (1u << aid);
                        }
                    }
                    break;
                case HS_PEER_BODY_COMPLETE:
                    if (gHaxxStationHost.txOffset == 0xffffffffu) {
                        peer->phase = HS_PEER_EXECUTED;
                        gHaxxStationHost.executedMask |= (u16) (1u << aid);
                    }
                    break;
            }
        }
        OS_RestoreInterrupts(savedIrq);
        HaxxStation_Pump();
        return;
    }
    OS_RestoreInterrupts(savedIrq);
}

static void HaxxStation_PortRecvCb(Unk021425e8 *cb) {
    u32 port;
    u32 length;
    u32 aid;
    const u8 *data;
    HaxxStationPeer *peer;
    u32 i;

    if (cb == NULL || cb->mResultCode != 0 || cb->mUnk_04 != 0x15) {
        return;
    }
    if (gHaxxStationHost.phase != HS_PHASE_STATION_APP_SERVE &&
        gHaxxStationHost.phase != HS_PHASE_STATION_EXECUTE_BARRIER) {
        return;
    }
    port = cb->mUnk_06;
    length = cb->mUnk_10;
    aid = cb->mUnk_12;
    data = (const u8 *) cb->mUnk_0c;
    if (aid < 1 || aid > 15 || data == NULL) {
        return;
    }

    peer = &gHaxxStationHost.peers[aid];
    peer->associated = 1;
    gHaxxStationHost.lastGeneration = peer->generation;

    if (port == 13 && data_0216fdac != NULL) {
        gHaxxStationHost.stationAppMask |= (u16) (1u << aid);
        data_0216fdac->mUnk_14e8[aid - 1] = 0;
        data_0216fdac->mUnk_1754[aid - 1] = 0;
        for (i = 0; i < 16; i++) {
            (OFFSET_PTR(Unk0216fdac, data_0216fdac, i * 0x5d4))->mUnk_1d4c &= ~(u16) (1u << aid);
        }
    }

    if (port == 13 && length >= 8) {
        static const char sDemoMenuName[8] = { 'd', 'e', 'm', 'o', 'm', 'e', 'n', 'u' };
        BOOL isMenu = TRUE;
        for (i = 0; i < 8; i++) {
            if (data[i] != (u8) sDemoMenuName[i]) {
                isMenu = FALSE;
                break;
            }
        }
        peer->phase = isMenu ? HS_PEER_MENU_LEN_SEND : HS_PEER_PAYLOAD_LEN_SEND;
        peer->offset = 0;
        peer->chunk = 0;
        peer->cooldown = 0;
        gHaxxStationHost.bodyDoneMask &= ~(u16) (1u << aid);
    } else if (port == 14 && (peer->phase == HS_PEER_MENU_LEN_SEND || peer->phase == HS_PEER_MENU_LEN_WAIT)) {
        peer->phase = HS_PEER_MENU_BODY;
        peer->offset = 0;
        peer->chunk = 0;
    } else if (port == 15 && (peer->phase == HS_PEER_PAYLOAD_LEN_SEND || peer->phase == HS_PEER_PAYLOAD_LEN_WAIT)) {
        peer->phase = HS_PEER_PAYLOAD_START;
        peer->offset = 0;
        peer->chunk = 0;
    }
    HaxxStation_Pump();
}

static void HaxxStation_Pump(void) {
    u32 savedIrq = OS_DisableInterrupts();
    u32 attempt;
    BOOL canExec;
    u16 readyMask;
    u32 selectedAid = 0;
    u16 sendMask = 0;
    u32 port = 0;
    u32 sendLen = 0;
    u32 bodyBytes = 0;
    u32 reqOffset = 0;
    u32 selectedPhase = 0;
    s32 wmErr;

    if ((gHaxxStationHost.phase != HS_PHASE_STATION_APP_SERVE &&
         gHaxxStationHost.phase != HS_PHASE_STATION_EXECUTE_BARRIER) ||
        gHaxxStationHost.txBusy != 0 || gHaxxStationHost.txBuf == NULL || gWirelessWork == NULL ||
        gWirelessWork->mUnk_50d == 0 || gWirelessWork->mMPStarted == 0 || gWirelessWork->mResetting != 0) {
        OS_RestoreInterrupts(savedIrq);
        return;
    }

    readyMask = HaxxStation_GetReadyAidsMask();
    if (readyMask == 0) {
        OS_RestoreInterrupts(savedIrq);
        return;
    }

    /* Promote any completed payload peers before evaluating the party-wide barrier */
    for (attempt = 1; attempt <= 15; attempt++) {
        HaxxStationPeer *p = &gHaxxStationHost.peers[attempt];
        if (p->associated != 0 && p->phase == HS_PEER_PAYLOAD_BODY && p->offset >= gHaxxStationHost.payloadSize) {
            p->phase = HS_PEER_BODY_COMPLETE;
            gHaxxStationHost.bodyDoneMask |= (u16) (1u << attempt);
        }
    }

    canExec = HaxxStation_CanExecuteStage2();
    if (canExec) {
        gHaxxStationHost.phase = HS_PHASE_STATION_EXECUTE_BARRIER;
    }

    /* Pass 1: Prioritize tiny control & barrier phases so staggered joiners never wait behind bulk chunks */
    for (attempt = 0; attempt < 15; attempt++) {
        u32 aid = gHaxxStationHost.nextAid;
        HaxxStationPeer *peer;

        gHaxxStationHost.nextAid = (aid >= 15) ? 1 : (aid + 1);
        if ((readyMask & (1u << aid)) == 0) {
            continue;
        }
        peer = &gHaxxStationHost.peers[aid];
        if (peer->associated == 0 || peer->cooldown != 0) {
            continue;
        }

        if (peer->phase == HS_PEER_MENU_LEN_SEND) {
            *(u32 *) gHaxxStationHost.txBuf = gHaxxStationHost.menuSize;
            port = 13;
            sendLen = 4;
            sendMask = (u16) (1u << aid);
        } else if (peer->phase == HS_PEER_PAYLOAD_LEN_SEND) {
            *(u32 *) gHaxxStationHost.txBuf = gHaxxStationHost.payloadSize;
            port = 13;
            sendLen = 4;
            sendMask = (u16) (1u << aid);
        } else if (peer->phase == HS_PEER_PAYLOAD_START) {
            u32 w;
            gHaxxStationHost.txBuf[0] = 0xfffeu;
            for (w = 1; w < 16; w++) {
                gHaxxStationHost.txBuf[w] = 0;
            }
            port = 15;
            sendLen = 32;
            sendMask = (u16) (1u << aid);
        } else if (peer->phase == HS_PEER_MENU_BODY) {
            port = 14;
            reqOffset = peer->offset;
            if (peer->offset >= gHaxxStationHost.menuSize) {
                gHaxxStationHost.txBuf[0] = 0xffffu;
                sendLen = 2;
            } else {
                u32 rem = gHaxxStationHost.menuSize - peer->offset;
                bodyBytes = rem < 30u ? rem : 30u;
                gHaxxStationHost.txBuf[0] = peer->chunk;
                MI_CpuCopy8(gHaxxStationHost.menuBuf + peer->offset, (u8 *) (gHaxxStationHost.txBuf + 1), bodyBytes);
                sendLen = 2 + bodyBytes;
            }
            sendMask = (u16) (1u << aid);
        } else if (peer->phase == HS_PEER_BODY_COMPLETE && canExec) {
            u32 other;
            gHaxxStationHost.txBuf[0] = 0xffffu;
            port = 15;
            reqOffset = 0xffffffffu;
            sendLen = 2;
            sendMask = 0;
            for (other = 1; other <= 15; other++) {
                HaxxStationPeer *op = &gHaxxStationHost.peers[other];
                if ((readyMask & (1u << other)) != 0 && op->associated != 0 && op->cooldown == 0 &&
                    op->phase == HS_PEER_BODY_COMPLETE) {
                    sendMask |= (u16) (1u << other);
                }
            }
        } else {
            continue;
        }

        selectedAid = aid;
        selectedPhase = peer->phase;
        break;
    }

    /* Pass 2: Minimum-offset catch-up + cohort multicast for bulk Stage-2 payload (Port 15) */
    if (sendMask == 0) {
        u32 bestAid = 0;
        u32 bestOffset = 0xffffffffu;
        u32 scanAid = gHaxxStationHost.nextAid;

        for (attempt = 0; attempt < 15; attempt++) {
            u32 aid = scanAid;
            HaxxStationPeer *peer = &gHaxxStationHost.peers[aid];
            scanAid = (aid >= 15) ? 1 : (aid + 1);
            if ((readyMask & (1u << aid)) == 0 || peer->associated == 0 || peer->cooldown != 0 ||
                peer->phase != HS_PEER_PAYLOAD_BODY) {
                continue;
            }
            if (peer->offset < bestOffset) {
                bestOffset = peer->offset;
                bestAid = aid;
            }
        }

        if (bestAid != 0) {
            HaxxStationPeer *bestPeer = &gHaxxStationHost.peers[bestAid];
            u32 rem = gHaxxStationHost.payloadSize - bestPeer->offset;
            bodyBytes = rem < 126u ? rem : 126u;
            if (HaxxStation_CopyFromCache(bestPeer->offset, (u8 *) (gHaxxStationHost.txBuf + 1), bodyBytes)) {
                u32 other;
                gHaxxStationHost.txBuf[0] = bestPeer->chunk;
                port = 15;
                reqOffset = bestPeer->offset;
                sendLen = 2 + bodyBytes;
                selectedAid = bestAid;
                selectedPhase = HS_PEER_PAYLOAD_BODY;
                gHaxxStationHost.nextAid = (bestAid >= 15) ? 1 : (bestAid + 1);
                for (other = 1; other <= 15; other++) {
                    HaxxStationPeer *op = &gHaxxStationHost.peers[other];
                    if ((readyMask & (1u << other)) != 0 && op->associated != 0 && op->cooldown == 0 &&
                        op->phase == HS_PEER_PAYLOAD_BODY && op->offset == bestPeer->offset &&
                        op->chunk == bestPeer->chunk) {
                        sendMask |= (u16) (1u << other);
                    }
                }
            }
        }
    }

    if (sendMask != 0 && selectedAid != 0) {
        u32 a;
        gHaxxStationHost.txBusy = 1;
        gHaxxStationHost.txAid = selectedAid;
        gHaxxStationHost.txMask = sendMask;
        gHaxxStationHost.txPhase = selectedPhase;
        gHaxxStationHost.txOffset = reqOffset;
        gHaxxStationHost.txBytes = bodyBytes;
        gHaxxStationHost.txGen = gHaxxStationHost.peers[selectedAid].generation;
        for (a = 1; a <= 15; a++) {
            if ((sendMask & (1u << a)) != 0) {
                gHaxxStationHost.txGenByAid[a] = gHaxxStationHost.peers[a].generation;
            }
        }

        wmErr = WM_SetMPDataToPortEx((UnkFunc *) HaxxStation_PortSentCb, NULL, gHaxxStationHost.txBuf, (s32) sendLen,
                                     sendMask, (u16) port, 0);
        if (wmErr != 2) {
            gHaxxStationHost.txBusy = 0;
            gHaxxStationHost.txMask = 0;
            gHaxxStationHost.lastSendStatus = wmErr;
        }
    }

    OS_RestoreInterrupts(savedIrq);
}

static BOOL HaxxStation_StartStationAppParent(void) {
    FSFile menuFile;
    FSFile *pf;
    Unk0204e4d8 b;
    u32 i;

    if (gHaxxStationHost.menuBuf == NULL || gHaxxStationHost.cacheBuf == NULL || gHaxxStationHost.txBuf == NULL) {
        gHaxxStationHost.lastError = 0x201u;
        return FALSE;
    }

    FS_InitFile(&menuFile);
    if (FS_OpenFile(&menuFile, (const u8 *) "/haxx/station_demomenu.bin") == 0) {
        gHaxxStationHost.lastError = 0x202u;
        return FALSE;
    }
    gHaxxStationHost.menuSize = menuFile.mProp.w.mUnk_08 - menuFile.mProp.w.mUnk_04;
    if (gHaxxStationHost.menuSize == 0 || gHaxxStationHost.menuSize > HAXXSTATION_MENU_MAX_BYTES ||
        (u32) FS_ReadFile(&menuFile, gHaxxStationHost.menuBuf, (s32) gHaxxStationHost.menuSize) !=
            gHaxxStationHost.menuSize) {
        FS_CloseFile(&menuFile);
        gHaxxStationHost.lastError = 0x203u;
        return FALSE;
    }
    FS_CloseFile(&menuFile);
    DC_FlushRange(gHaxxStationHost.menuBuf, (gHaxxStationHost.menuSize + 0x1fu) & ~0x1fu);

    pf = (FSFile *) gHaxxStationHost.payloadFileWords;
    FS_InitFile(pf);
    if (FS_OpenFile(pf, (const u8 *) "/haxx/station_rom0000d.bin") == 0) {
        gHaxxStationHost.lastError = 0x204u;
        return FALSE;
    }
    gHaxxStationHost.payloadFileOpen = 1;
    gHaxxStationHost.payloadSize = pf->mProp.w.mUnk_08 - pf->mProp.w.mUnk_04;

    for (i = 0; i < HAXXSTATION_CACHE_SLOTS; i++) {
        gHaxxStationHost.cacheSlots[i].pageIndex = 0xffffffffu;
        gHaxxStationHost.cacheSlots[i].validBytes = 0;
        gHaxxStationHost.cacheSlots[i].lastUsedTick = 0;
    }
    for (i = 0; i < HAXXSTATION_CACHE_SLOTS; i++) {
        if (i * HAXXSTATION_CACHE_PAGE_SIZE < gHaxxStationHost.payloadSize) {
            HaxxStation_LoadCachePage(i);
        }
    }

    /* Reset MB parent work cleanly on the retained TGID and RF channel */
    data_0217aa00->mUnk_1694 = 0;
    data_0217aa00->mUnk_1690 = (u16) gHaxxStationHost.savedTgid;
    data_0217aa00->mUnk_0088 = (s8) gHaxxStationHost.savedChannel;
    data_0215472c[5] = (void *) HAXXSTATION_STATION_GGID;
    gHaxxStationHost.activeGgid = HAXXSTATION_STATION_GGID;

    func_0204e65c(&data_0217aa00->mUnk_1658_view);
    b.mUnk_01 = 10;
    b.mUnk_00_0 = data_021759d0->mUnk_40;
    b.mUnk_00_4 = 8;
    MI_CpuCopy8(data_0217aa08->mFirstBlock->mPlayerName, b.mUnk_02, 0x14);

    if (InitWirelessWork(data_0217aa00->mUnk_165c, (Unk02009ff4 *) &b, HAXXSTATION_STATION_GGID,
                         gHaxxStationHost.savedTgid, 2) != 0) {
        gHaxxStationHost.lastError = 0x205u;
        return FALSE;
    }
    SetMPLimits(0x1fe, 7);
    SetCallback((UnkFunc *) func_0204e6e8);
    data_0217aa00->mRequestPending = 1;
    if (((s32 (*)(u32)) func_02009cc8)((u16) gHaxxStationHost.savedChannel) != 0) {
        gHaxxStationHost.lastError = 0x206u;
        return FALSE;
    }
    if (func_0204ea3c() == 0) {
        gHaxxStationHost.lastError = 0x207u;
        return FALSE;
    }
    data_0217aa00->mUnk_008b &= ~4;
    HaxxStation_InstallPortCallbacks();
    return TRUE;
}

BOOL HaxxStation_StepBootstrap(void) {
    u32 aid;
    u32 savedIrq;

    if (!HaxxStation_IsEnabled() || gHaxxStationHost.phase == HS_PHASE_IDLE) {
        return TRUE;
    }

    /* Step 1: Wait for all Stage-1 firmware children to finish BOOT and disconnect */
    if (gHaxxStationHost.phase == HS_PHASE_STAGE1_BOOT_WAIT) {
        if (gWirelessWork != NULL && gWirelessWork->mConnectedAids != 0 &&
            gHaxxStationHost.handoffGraceFrames < 30) {
            savedIrq = OS_DisableInterrupts();
            if (data_0216fdac != NULL && gWirelessWork->mMPStarted != 0 && gWirelessWork->mResetting == 0 &&
                gWirelessWork->mDataQueued == 0) {
                func_020071b4();
            }
            OS_RestoreInterrupts(savedIrq);
            return FALSE;
        }
        gHaxxStationHost.handoffGraceFrames = 0;
        gHaxxStationHost.savedBootMask = data_0217aa00->mUnk_1695 | data_0217aa00->mUnk_1697;
        gHaxxStationHost.expectedGuests = HaxxStation_Popcount16(gHaxxStationHost.savedBootMask);
        if (gHaxxStationHost.expectedGuests == 0) {
            gHaxxStationHost.savedBootMask = data_0217aa00->mUnk_1696;
            gHaxxStationHost.expectedGuests = HaxxStation_Popcount16(gHaxxStationHost.savedBootMask);
        }
        if (gHaxxStationHost.expectedGuests == 0) {
            gHaxxStationHost.expectedGuests = 1;
            gHaxxStationHost.savedBootMask = 2;
        }
        gHaxxStationHost.phase = HS_PHASE_STAGE1_MB_RESET;
        gHaxxStationHost.internalReset = 1;
        data_0217aa00->mRequestPending = 1;
        data_0217aa00->mUnk_1694 &= ~2;
        func_02009c04();
        return FALSE;
    }

    /* Step 2: Wait for Stage-1 MB parent WM_Reset completion, then start Station App parent */
    if (gHaxxStationHost.phase == HS_PHASE_STAGE1_MB_RESET) {
        if (data_0216fdac != NULL) {
            return FALSE;
        }
        gHaxxStationHost.internalReset = 0;
        gHaxxStationHost.phase = HS_PHASE_STATION_APP_START;
        if (!HaxxStation_StartStationAppParent()) {
            func_02050e60(5);
            return FALSE;
        }
        gHaxxStationHost.phase = HS_PHASE_STATION_APP_SERVE;
        return FALSE;
    }

    /* Step 3: Serve Station ports 13 / 14 / 15 + port 1 heartbeat until party-wide Stage-2 execution */
    if (gHaxxStationHost.phase == HS_PHASE_STATION_APP_SERVE ||
        gHaxxStationHost.phase == HS_PHASE_STATION_EXECUTE_BARRIER) {
        if (gHaxxStationHost.callbacksInstalled == 0 && gWirelessWork != NULL && gWirelessWork->mMPStarted != 0 &&
            gWirelessWork->mResetting == 0) {
            HaxxStation_InstallPortCallbacks();
        }

        for (aid = 1; aid <= 15; aid++) {
            if (gHaxxStationHost.peers[aid].cooldown > 0) {
                gHaxxStationHost.peers[aid].cooldown--;
            }
        }

        HaxxStation_RefillCacheForPeers();

        /* Keep port-1 control/heartbeat sender alive until all peers select Stage-2 from the menu */
        savedIrq = OS_DisableInterrupts();
        if (data_0216fdac != NULL && gWirelessWork != NULL && gWirelessWork->mMPStarted != 0 &&
            gWirelessWork->mResetting == 0 && gWirelessWork->mDataQueued == 0 &&
            gWirelessWork->mConnectedAids != 0 && !HaxxStation_AllPeersPastMenu()) {
            func_020071b4();
        }
        OS_RestoreInterrupts(savedIrq);

        HaxxStation_Pump();

        if (HaxxStation_Popcount16(gHaxxStationHost.executedMask) >= gHaxxStationHost.expectedGuests &&
            gHaxxStationHost.txBusy == 0) {
            gHaxxStationHost.handoffGraceFrames++;
            if (gHaxxStationHost.handoffGraceFrames >= 4 ||
                (gWirelessWork != NULL && gWirelessWork->mConnectedAids == 0)) {
                gHaxxStationHost.phase = HS_PHASE_STATION_SHUTDOWN;
            }
        }
        return FALSE;
    }

    /* Step 4: Retire Station callbacks & buffers, restore MKDS identity & roster, hand off to func_0204e090() */
    if (gHaxxStationHost.phase == HS_PHASE_STATION_SHUTDOWN) {
        u32 execCount;
        if (gHaxxStationHost.txBusy != 0) {
            return FALSE;
        }
        HaxxStation_RestoreNativePortsAndIdentity();
        data_0217aa00->mUnk_1690 = (u16) gHaxxStationHost.savedTgid;
        data_0217aa00->mUnk_0088 = (s8) gHaxxStationHost.savedChannel;
        execCount = HaxxStation_Popcount16(gHaxxStationHost.executedMask);
        if (execCount > 7u) {
            execCount = 7u;
        }
        data_0217aa00->mUnk_1695 = (u8) (((1u << (execCount + 1u)) - 2u) & 0xfeu);
        gHaxxStationHost.phase = HS_PHASE_IDLE;
        return TRUE;
    }

    return TRUE;
}

BOOL HaxxStation_HandleMbCallback(u32 index, u32 code, void *arg) {
    (void) arg;
    if (gHaxxStationHost.phase == HS_PHASE_STAGE1_MB_RESET) {
        if (code == 12) {
            data_0217aa00->mRequestPending = 0;
        }
        return TRUE;
    }
    if (gHaxxStationHost.phase == HS_PHASE_STATION_APP_START ||
        gHaxxStationHost.phase == HS_PHASE_STATION_APP_SERVE ||
        gHaxxStationHost.phase == HS_PHASE_STATION_EXECUTE_BARRIER ||
        gHaxxStationHost.phase == HS_PHASE_STATION_SHUTDOWN) {
        switch (code) {
            case 10:
                func_02007d50(index, 1);
                break;
            case 1:
            case 12:
                data_0217aa00->mRequestPending = 0;
                break;
            default:
                break;
        }
        return TRUE;
    }
    return FALSE;
}

BOOL HaxxStation_OnMbParentEvent(s32 eventCode, Unk021425e8 *eventData) {
    if (gHaxxStationHost.phase == HS_PHASE_IDLE) {
        return FALSE;
    }
    /* Suppress native BEACON_SENT SetNextGameInfo while awaiting our closed-entry beacon callback */
    if (eventCode == 0x1c &&
        (gHaxxStationHost.entryCloseState == 3 || gHaxxStationHost.entryCloseState == 4)) {
        return TRUE;
    }
    if (gHaxxStationHost.phase == HS_PHASE_STATION_APP_SERVE ||
        gHaxxStationHost.phase == HS_PHASE_STATION_EXECUTE_BARRIER) {
        if (eventCode == 0) {
            HaxxStation_InstallPortCallbacks();
            if (eventData != NULL) {
                u32 aid = eventData->mUnk_10;
                if (aid >= 1 && aid <= 15) {
                    HaxxStationPeer *peer = &gHaxxStationHost.peers[aid];
                    peer->generation++;
                    peer->associated = 1;
                    peer->phase = HS_PEER_IDLE;
                    peer->offset = 0;
                    peer->chunk = 0;
                    peer->cooldown = 0;
                    gHaxxStationHost.lastGeneration = peer->generation;
                    gHaxxStationHost.txMask &= ~(u16) (1u << aid);
                    gHaxxStationHost.stationAppMask &= ~(u16) (1u << aid);
                    gHaxxStationHost.bodyDoneMask &= ~(u16) (1u << aid);
                }
            }
        } else if (eventCode == 1 && eventData != NULL) {
            u32 aid = eventData->mUnk_10;
            if (aid >= 1 && aid <= 15) {
                HaxxStationPeer *peer = &gHaxxStationHost.peers[aid];
                peer->generation++;
                peer->associated = 0;
                gHaxxStationHost.txMask &= ~(u16) (1u << aid);
                if (peer->phase == HS_PEER_EXECUTED) {
                    gHaxxStationHost.executedDisconnectedMask |= (u16) (1u << aid);
                } else {
                    peer->phase = HS_PEER_IDLE;
                    peer->offset = 0;
                    peer->chunk = 0;
                    peer->cooldown = 0;
                    gHaxxStationHost.stationAppMask &= ~(u16) (1u << aid);
                    gHaxxStationHost.bodyDoneMask &= ~(u16) (1u << aid);
                }
            }
        }
    }
    return FALSE;
}

BOOL HaxxStation_ShouldIgnoreMbChildMsg(u32 child) {
    if ((gHaxxStationHost.phase == HS_PHASE_STATION_APP_SERVE ||
         gHaxxStationHost.phase == HS_PHASE_STATION_EXECUTE_BARRIER) &&
        child >= 1 && child <= 15) {
        if (gHaxxStationHost.peers[child].phase != HS_PEER_IDLE) {
            return TRUE;
        }
    }
    return FALSE;
}

int HaxxStation_SendParentControl(u8 kind, u32 childMask, BOOL *handled) {
    u8 *buf;
    if (gHaxxStationHost.phase != HS_PHASE_STATION_APP_SERVE &&
        gHaxxStationHost.phase != HS_PHASE_STATION_EXECUTE_BARRIER) {
        *handled = FALSE;
        return 0;
    }
    if (HaxxStation_AllPeersPastMenu()) {
        *handled = TRUE;
        return 0;
    }
    if (kind == 0 && data_0216fdac != NULL) {
        buf = (u8 *) data_0216fdac;
        buf[0] = 0;
        buf[1] = 0;
        *(u16 *) (buf + 2) = 0x0048u;
        *(u16 *) (buf + 4) = 0x0002u;
        *handled = TRUE;
        return FlushAndSendMPData(6, childMask, (u32) buf);
    }
    *handled = FALSE;
    return 0;
}

void HaxxStation_OnMbEndCalled(void) {
    if (gHaxxStationHost.phase != HS_PHASE_IDLE && gHaxxStationHost.internalReset == 0) {
        HaxxStation_RestoreNativePortsAndIdentity();
        gHaxxStationHost.phase = HS_PHASE_IDLE;
    }
}

#pragma thumb off
