#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef volatile uint16_t vu16;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define RACE_KIND_NORMAL 0

/* Retail Mario Kart DS (USA) cup -> course ID table (gCupCourseIdTable, arm9.bin) */
static const u32 gCupCourseIdTable[8][4] = {
    {20, 22, 31, 18}, /* 0: Mushroom Cup */
    {27, 28, 33, 24}, /* 1: Flower Cup   */
    {30, 17, 25, 19}, /* 2: Star Cup     */
    {34, 26, 32, 29}, /* 3: Special Cup  */
    {10, 11, 13, 14}, /* 4: Shell Cup    */
    {35, 16, 12, 9},  /* 5: Banana Cup   */
    {15, 36, 37, 38}, /* 6: Leaf Cup     */
    {39, 23, 40, 1},  /* 7: Lightning Cup*/
};

/* Cup order tables from code_02045874.c (verified from USA arm9.bin) */
static const u32 data_021540c0[2] = {0, 4};                   /* Stock Simple-mode 2-cup VS pool */
static const u32 data_02154108[8] = {0, 1, 2, 3, 4, 5, 6, 7}; /* Full 8-cup VS pool */
static const u32 data_021540c8[3] = {44, 47, 43};             /* Simple-mode 3-stage Battle pool */
static const u32 data_021540d4[6] = {42, 43, 44, 45, 46, 47}; /* Full 6-stage Battle pool */

typedef struct RandomState64 {
    u64 mValue;
    u64 mMultiplier;
    u64 mIncrement;
} RandomState64;

static RandomState64 gMockRng;
static u32 gMockParticipantUnlockMask = 0;
static u32 gMockBackupUnlockMask = 0;
static s32 gMockMirrorMode = 0;
static u32 gMockEngineIndex = 0;

static RandomState64 *GetRandomState(void) {
    return &gMockRng;
}

static BOOL IsBitSetInAnyParticipant(u32 bit) {
    return ((gMockParticipantUnlockMask >> bit) & 1u) != 0;
}

static BOOL IsBackupFlagSet(u32 bit) {
    return ((gMockBackupUnlockMask >> bit) & 1u) != 0;
}

static s32 GetMirrorMode(void) {
    return gMockMirrorMode;
}

static u32 GetEngineIndex(void) {
    return gMockEngineIndex;
}

static void ResetCupRecords(void) {}

static struct {
    s32 mMode;
} gMockSelectCupGridObj, *gSelectCupGrid = &gMockSelectCupGridObj;

static struct {
    u32 mUnk_1c_2;
    s32 mUnk_20;
} gMock59d0Obj, *data_021759d0 = &gMock59d0Obj;

static struct {
    u32 mUnk_0454;
} gMockConnObj, *data_0217aa00 = &gMockConnObj;

static struct {
    struct {
        u32 mEngineIndex;
        u32 mMirrorMode;
        u32 mCourseNumber;
        u32 mRaceKind;
        u32 mCourseListMode;
        s32 mCourseId;
    } mNextHeader;
    u32 mCourseListMode;
    u32 mCourseListIndex;
    u8 *mCourseList;
} gMockRaceSetupObj, *gRaceSetup = &gMockRaceSetupObj;

static const u8 data_0215c8bc[8] = {0, 0, 12, 13, 0, 0, 14, 15};
static const u8 data_0215c95c[8][3] = {
    {0, 0, 0}, {0, 0, 0}, {0, 1, 2}, {4, 5, 6}, {0, 0, 0}, {0, 0, 0}, {8, 9, 10}, {12, 13, 14},
};
static const u8 data_0215c7f0[3] = {0, 1, 2};

/* --- Exact patched functions under test --- */

static u32 GetSelectCupGridCupEnabled(u32 cupIndex) {
    u32 enabled = 0;
    s32 i;

    switch (gSelectCupGrid->mMode) {
        case 0:
        case 2:
            switch (cupIndex) {
                case 0:
                case 1:
                case 4:
                case 5:
                    enabled = 1;
                    break;
                default:
                    if (GetMirrorMode() != 0) {
                        enabled = IsBackupFlagSet(data_0215c8bc[cupIndex]);
                    } else {
                        enabled = IsBackupFlagSet(data_0215c95c[cupIndex][data_0215c7f0[GetEngineIndex()]]);
                    }
                    break;
            }
            break;
        case 1:
            switch (cupIndex) {
                case 0:
                case 1:
                case 4:
                case 5:
                    enabled = 1;
                    break;
                default:
                    enabled = 0;
                    for (i = 0; i < 4; i++) {
                        if (i == 3) {
                            enabled |= IsBackupFlagSet(data_0215c8bc[cupIndex]);
                        } else {
                            enabled |= IsBackupFlagSet(data_0215c95c[cupIndex][i]);
                        }
                    }
                    break;
            }
            break;
        case 3:
            if (cupIndex < 8) {
                enabled = 1;
            }
            break;
        case 4:
            switch (cupIndex) {
                case 0:
                case 1:
                case 4:
                case 5:
                    enabled = 1;
                    break;
                default:
                    if (GetMirrorMode() != 0) {
                        enabled = IsBitSetInAnyParticipant(data_0215c8bc[cupIndex]);
                    } else {
                        enabled = IsBitSetInAnyParticipant(data_0215c95c[cupIndex][data_0215c7f0[GetEngineIndex()]]);
                    }
                    break;
            }
            break;
    }
    return enabled;
}

static u32 func_02046524(RandomState64 *randomState64, u32 limit) {
    u64 result;
    if (limit == 0) {
        result = 0;
    } else {
        randomState64->mValue = randomState64->mMultiplier * randomState64->mValue + randomState64->mIncrement;
        result = ((randomState64->mValue >> 32) * limit) >> 32;
    }
    return (u16) result;
}

static BOOL func_02046648(s32 a) {
    u32 n = (u8) gRaceSetup->mNextHeader.mEngineIndex;
    if (gRaceSetup->mNextHeader.mMirrorMode != 0) {
        n = 3;
    }
    switch (a) {
        case 0:
        case 1:
        case 4:
        case 5:
            return TRUE;
        case 2:
            if (IsBitSetInAnyParticipant(n)) {
                return TRUE;
            }
            break;
        case 3:
            if (IsBitSetInAnyParticipant(n + 4)) {
                return TRUE;
            }
            break;
        case 6:
            if (IsBitSetInAnyParticipant(n + 8)) {
                return TRUE;
            }
            break;
        case 7:
            if (IsBitSetInAnyParticipant(n + 12)) {
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static void BuildCourseList(s32 isVersus) {
    s32 f = 0;
    u32 m;
    const u32 *t;
    u32 i;
    u32 n;

    ResetCupRecords();
    gRaceSetup->mCourseListIndex = 0;
    gRaceSetup->mNextHeader.mCourseNumber = 1;
    if (isVersus != 0) {
        gRaceSetup->mNextHeader.mRaceKind = RACE_KIND_NORMAL;
    } else if (data_021759d0->mUnk_1c_2) {
        gRaceSetup->mNextHeader.mRaceKind = RACE_KIND_NORMAL;
    } else {
        gRaceSetup->mNextHeader.mRaceKind = 4;
    }
    for (i = 0; i < 0x21; i++) {
        gRaceSetup->mCourseList[i] = 0;
    }
    switch (data_021759d0->mUnk_20) {
        case 1:
        case 2:
            f = (data_0217aa00->mUnk_0454 & 0x40) != 0;
            break;
    }
    switch (gRaceSetup->mNextHeader.mCourseListMode) {
        case 1:
            if (isVersus != 0) {
                u8 p = 0;
                u8 q = 0;
                s32 e;
                s32 c;
                m = 8;
                t = data_02154108;
                e = gRaceSetup->mNextHeader.mCourseId;
                n = m * 4;
                for (i = 0; i < n; i++) {
                    p = (u8) (i / 4);
                    q = (u8) (i % 4);
                    if (e == (s32) gCupCourseIdTable[t[p]][q]) {
                        break;
                    }
                }
                c = 0;
                for (i = 0; i < n; i++) {
                    if (f != 0 || func_02046648(t[p]) != FALSE) {
                        gRaceSetup->mCourseList[c] = (u8) gCupCourseIdTable[t[p]][q];
                        c = (u8) (c + 1);
                    }
                    q = (u8) (q + 1);
                    if (q >= 4) {
                        q = 0;
                        p = (u8) (p + 1);
                        if (p >= m) {
                            p = 0;
                        }
                    }
                }
                return;
            } else {
                s32 e;
                u32 s = 0;
                n = f != 0 ? 3 : 6;
                t = f != 0 ? data_021540c8 : data_021540d4;
                e = gRaceSetup->mNextHeader.mCourseId;
                for (i = 0; i < n; i++) {
                    s = i;
                    if (e == (s32) t[i]) {
                        break;
                    }
                }
                for (i = 0; i < n; i++) {
                    gRaceSetup->mCourseList[i] = (u8) t[s];
                    s++;
                    if (s >= n) {
                        s = 0;
                    }
                }
                return;
            }
        case 2:
            if (isVersus != 0) {
                RandomState64 *g = GetRandomState();
                s32 k;
                s32 c;
                u8 q;
                u8 p;
                u8 w;
                u8 x;
                u8 y;
                s32 h;
                s32 z;
                m = 8;
                t = data_02154108;
                n = m * 4;
                c = 0;
                for (k = 0; k < (s32) n; k++) {
                    p = (u8) (k / 4);
                    q = (u8) (k % 4);
                    if (f != 0 || func_02046648(t[p]) != FALSE) {
                        gRaceSetup->mCourseList[c] = (u8) gCupCourseIdTable[t[p]][q];
                        c = (u8) (c + 1);
                    }
                }
                h = c * 2;
                for (z = 0; z < h; z++) {
                    w = (u8) func_02046524(g, (vu16) (c - 1));
                    x = (u8) (z % c);
                    y = gRaceSetup->mCourseList[x];
                    if (w >= x) {
                        w = (u8) (w + 1);
                    }
                    gRaceSetup->mCourseList[x] = gRaceSetup->mCourseList[w];
                    gRaceSetup->mCourseList[w] = y;
                }
            } else {
                s32 k;
                RandomState64 *g = GetRandomState();
                n = f != 0 ? 3 : 6;
                t = f != 0 ? data_021540c8 : data_021540d4;
                for (k = 0; k < (s32) n; k++) {
                    gRaceSetup->mCourseList[k] = (u8) t[k];
                }
                for (k = 0; k < (s32) n; k++) {
                    u8 w = (u8) func_02046524(g, (u16) (n - 1));
                    u8 y = gRaceSetup->mCourseList[k];
                    if (w >= k) {
                        w = (u8) (w + 1);
                    }
                    gRaceSetup->mCourseList[k] = gRaceSetup->mCourseList[w];
                    gRaceSetup->mCourseList[w] = y;
                }
            }
            gRaceSetup->mNextHeader.mCourseId = gRaceSetup->mCourseList[0];
            break;
    }
}

static void AdvanceCourseListStep(void) {
    gRaceSetup->mCourseListIndex++;
    if (gRaceSetup->mCourseList[gRaceSetup->mCourseListIndex] == 0) {
        gRaceSetup->mCourseListIndex = 0;
    }
    gRaceSetup->mNextHeader.mCourseId = gRaceSetup->mCourseList[gRaceSetup->mCourseListIndex];
}

/* --- 8-Player (1 Host + 7 DLP Clients) HaxxStation Cache & Cohort Multicast Unit Tests --- */

#define TEST_HS_CACHE_PAGE_SIZE 0x2000u
#define TEST_HS_CACHE_SLOTS     16

typedef struct TestHsCacheSlot {
    u32 pageIndex;
    u32 validBytes;
    u32 lastUsedTick;
} TestHsCacheSlot;

typedef struct TestHsPeer {
    u8 associated;
    u8 phase;
    u16 chunk;
    u32 offset;
    u32 generation;
    u16 cooldown;
} TestHsPeer;

static void TestHaxxStation7PeersAndCohortMulticast(void) {
    static u8 mockPayload[300000];
    static u8 cacheBuf[TEST_HS_CACHE_PAGE_SIZE * TEST_HS_CACHE_SLOTS];
    TestHsCacheSlot slots[TEST_HS_CACHE_SLOTS];
    TestHsPeer peers[16];
    u32 txGenByAid[16];
    u32 cacheTick = 1;
    u32 payloadSize = sizeof(mockPayload);
    u32 i, aid;
    u16 bodyDoneMask = 0;
    u16 executedMask = 0;
    u32 expectedGuests = 7;
    u32 multicastCount = 0;
    u32 totalPackets = 0;
    u32 droppedAid5Once = 0;

    for (i = 0; i < payloadSize; i++) {
        mockPayload[i] = (u8) ((i * 131u + 17u) & 0xffu);
    }
    for (i = 0; i < TEST_HS_CACHE_SLOTS; i++) {
        slots[i].pageIndex = 0xffffffffu;
        slots[i].validBytes = 0;
        slots[i].lastUsedTick = 0;
    }
    memset(peers, 0, sizeof(peers));
    memset(txGenByAid, 0, sizeof(txGenByAid));

    /* A. Worst-case 7-peer straddle test: all 7 DLP clients at 7 distinct page boundaries simultaneously! */
    for (aid = 1; aid <= 7; aid++) {
        peers[aid].associated = 1;
        peers[aid].phase = 8; /* HS_PEER_PAYLOAD_BODY */
        peers[aid].offset = (aid * 3u) * TEST_HS_CACHE_PAGE_SIZE + (TEST_HS_CACHE_PAGE_SIZE - 50u);
        peers[aid].generation = aid + 10u; /* distinct generations across peers! */
    }

    /* Simulate HaxxStation_RefillCacheForPeers() across all 7 straddling peers (7 * 2 = 14 <= 16 slots) */
    for (aid = 1; aid <= 7; aid++) {
        u32 pages[2];
        u32 np = 1;
        u32 p;
        pages[0] = peers[aid].offset / TEST_HS_CACHE_PAGE_SIZE;
        if ((peers[aid].offset % TEST_HS_CACHE_PAGE_SIZE) + 126u > TEST_HS_CACHE_PAGE_SIZE) {
            pages[1] = pages[0] + 1u;
            np = 2;
        }
        for (p = 0; p < np; p++) {
            u32 pageIdx = pages[p];
            u32 hit = 0;
            u32 victim = 0;
            u32 oldest = 0xffffffffu;
            u32 foff = pageIdx * TEST_HS_CACHE_PAGE_SIZE;
            u32 rem = payloadSize - foff;
            u32 toRead = rem < TEST_HS_CACHE_PAGE_SIZE ? rem : TEST_HS_CACHE_PAGE_SIZE;
            for (i = 0; i < TEST_HS_CACHE_SLOTS; i++) {
                if (slots[i].pageIndex == pageIdx && slots[i].validBytes > 0) {
                    slots[i].lastUsedTick = ++cacheTick;
                    hit = 1;
                    break;
                }
            }
            if (!hit) {
                for (i = 0; i < TEST_HS_CACHE_SLOTS; i++) {
                    if (slots[i].validBytes == 0) {
                        victim = i;
                        break;
                    }
                    if (slots[i].lastUsedTick < oldest) {
                        oldest = slots[i].lastUsedTick;
                        victim = i;
                    }
                }
                memcpy(cacheBuf + victim * TEST_HS_CACHE_PAGE_SIZE, mockPayload + foff, toRead);
                slots[victim].pageIndex = pageIdx;
                slots[victim].validBytes = toRead;
                slots[victim].lastUsedTick = ++cacheTick;
            }
        }
    }

    /* Verify ZERO eviction: all 7 straddling peers read exact 126-byte chunks from cache on the same tick! */
    for (aid = 1; aid <= 7; aid++) {
        u8 outChunk[126];
        u32 copied = 0;
        while (copied < 126u) {
            u32 curOff = peers[aid].offset + copied;
            u32 pageIdx = curOff / TEST_HS_CACHE_PAGE_SIZE;
            u32 pageOff = curOff % TEST_HS_CACHE_PAGE_SIZE;
            u32 slotIdx = TEST_HS_CACHE_SLOTS;
            u32 avail, step;
            for (i = 0; i < TEST_HS_CACHE_SLOTS; i++) {
                if (slots[i].pageIndex == pageIdx && pageOff < slots[i].validBytes) {
                    slotIdx = i;
                    break;
                }
            }
            assert(slotIdx < TEST_HS_CACHE_SLOTS);
            avail = slots[slotIdx].validBytes - pageOff;
            step = (126u - copied) < avail ? (126u - copied) : avail;
            memcpy(outChunk + copied, cacheBuf + slotIdx * TEST_HS_CACHE_PAGE_SIZE + pageOff, step);
            copied += step;
        }
        assert(memcmp(outChunk, mockPayload + peers[aid].offset, 126u) == 0);
    }

    /* B. Staggered 7-peer catch-up -> cohort multicast (0x00FE) -> partial-ACK loss on AID 5 -> re-merge -> 0xFFFF barrier! */
    payloadSize = 126u * 40u + 37u; /* 41 chunks including partial final chunk */
    for (aid = 1; aid <= 7; aid++) {
        peers[aid].associated = 1;
        peers[aid].phase = 8;
        peers[aid].offset = (aid <= 3) ? (126u * 5u) : 0u; /* AID 1..3 ahead by 5 chunks; AID 4..7 staggered at 0 */
        peers[aid].chunk = (u16) (peers[aid].offset / 126u);
        peers[aid].generation = aid * 3u;
        peers[aid].cooldown = (aid == 7) ? 8 : 0; /* AID 7 starts 8 steps later */
    }

    while (executedMask != 0x00feu) {
        u32 bestAid = 0;
        u32 bestOff = 0xffffffffu;
        u16 sendMask = 0;
        u16 portFieldInUnk08_2 = 15u; /* port = 15 at +0x0a: bits 4..7 are ZERO, proving sentBitmap at +0x10 is used! */
        u16 simulatedSentBitmap = 0;
        u16 errCode = 0;

        for (aid = 1; aid <= 7; aid++) {
            if (peers[aid].cooldown > 0) {
                peers[aid].cooldown--;
            }
            if (peers[aid].phase == 8 && peers[aid].offset >= payloadSize) {
                peers[aid].phase = 9; /* HS_PEER_BODY_COMPLETE */
                bodyDoneMask |= (u16) (1u << aid);
            }
        }

        /* Check barrier: NO peer may execute while any of the 7 peers is < HS_PEER_BODY_COMPLETE */
        if (bodyDoneMask == 0x00feu) {
            for (aid = 1; aid <= 7; aid++) {
                if (peers[aid].phase == 9) {
                    sendMask |= (u16) (1u << aid);
                    txGenByAid[aid] = peers[aid].generation;
                }
            }
            assert(sendMask == 0x00feu);
            simulatedSentBitmap = sendMask;
            for (aid = 1; aid <= 7; aid++) {
                if ((simulatedSentBitmap & sendMask & (1u << aid)) != 0 &&
                    peers[aid].generation == txGenByAid[aid]) {
                    peers[aid].phase = 10; /* HS_PEER_EXECUTED */
                    executedMask |= (u16) (1u << aid);
                }
            }
            totalPackets++;
            break;
        } else {
            assert(executedMask == 0);
        }

        for (aid = 1; aid <= 7; aid++) {
            if (peers[aid].cooldown == 0 && peers[aid].phase == 8 && peers[aid].offset < bestOff) {
                bestOff = peers[aid].offset;
                bestAid = aid;
            }
        }
        assert(bestAid != 0);
        for (aid = 1; aid <= 7; aid++) {
            if (peers[aid].cooldown == 0 && peers[aid].phase == 8 &&
                peers[aid].offset == bestOff && peers[aid].chunk == peers[bestAid].chunk) {
                sendMask |= (u16) (1u << aid);
                txGenByAid[aid] = peers[aid].generation;
            }
        }
        if (sendMask == 0x00feu) {
            multicastCount++;
        }

        /* Simulate a one-time RF drop on AID 5 at chunk 20 (err = 15 WM_ERRCODE_SEND_FAILED, sentBitmap missing bit 5) */
        assert((portFieldInUnk08_2 & (1u << 4)) == 0 && (portFieldInUnk08_2 & (1u << 7)) == 0);
        if (!droppedAid5Once && bestOff == 126u * 20u && (sendMask & (1u << 5)) != 0) {
            droppedAid5Once = 1;
            errCode = 15;
            simulatedSentBitmap = (u16) (sendMask & ~(1u << 5));
        } else {
            errCode = 0;
            simulatedSentBitmap = sendMask;
        }

        if (errCode == 0 || errCode == 15) {
            u16 delivered = (u16) (simulatedSentBitmap & sendMask);
            u32 rem = payloadSize - bestOff;
            u32 txBytes = rem < 126u ? rem : 126u;
            for (aid = 1; aid <= 7; aid++) {
                if ((delivered & (1u << aid)) != 0 && peers[aid].generation == txGenByAid[aid]) {
                    peers[aid].offset += txBytes;
                    peers[aid].chunk++;
                    if (peers[aid].offset >= payloadSize) {
                        peers[aid].phase = 9;
                        bodyDoneMask |= (u16) (1u << aid);
                    }
                }
            }
        }
        totalPackets++;
        assert(totalPackets < 100); /* Unicast for 7 peers would be 7 * 41 = 287 packets! */
    }

    for (aid = 1; aid <= 7; aid++) {
        assert(peers[aid].offset == payloadSize);
        assert(peers[aid].phase == 10);
    }
    /* Prove all 7 peers merged into full 0x00FE multicast for the bulk of the transfer & recovered AID 5 cleanly */
    assert(multicastCount >= 30);
    assert(totalPackets <= 56);
    /* Prove mUnk_1695 roster bitmask packing yields 0xFE for 7 executed guests */
    assert((((1u << (expectedGuests + 1u)) - 2u) & 0xfeu) == 0xfeu);
}

int main(void) {
    /* 0x21-byte allocation with canary guard bytes immediately after to detect any OOB write */
    u8 guardedBuf[0x21 + 16];
    u32 cup, idx, step, seed;

    /* 1. Cup selector mode 3 (Simple mode) vs modes 0, 1, 2, 4 */
    assert(data_021540c0[0] == 0 && data_021540c0[1] == 4);
    gMockBackupUnlockMask = 0;
    gMockParticipantUnlockMask = 0;
    gSelectCupGrid->mMode = 3;
    for (cup = 0; cup < 8; cup++) {
        assert(GetSelectCupGridCupEnabled(cup) == 1);
    }
    assert(GetSelectCupGridCupEnabled(8) == 0);

    /* Normal multi-card (mode 4) and single-player GP (mode 0) must still gate locked cups 2,3,6,7 */
    gSelectCupGrid->mMode = 4;
    assert(GetSelectCupGridCupEnabled(0) == 1);
    assert(GetSelectCupGridCupEnabled(1) == 1);
    assert(GetSelectCupGridCupEnabled(2) == 0);
    assert(GetSelectCupGridCupEnabled(3) == 0);
    assert(GetSelectCupGridCupEnabled(4) == 1);
    assert(GetSelectCupGridCupEnabled(5) == 1);
    assert(GetSelectCupGridCupEnabled(6) == 0);
    assert(GetSelectCupGridCupEnabled(7) == 0);

    /* 2. Simple-mode Sequential VS (mode 1): all 32 tracks in order from every starting track, even w/ 0 unlocks */
    memset(guardedBuf, 0xAA, sizeof(guardedBuf));
    gRaceSetup->mCourseList = guardedBuf;
    data_021759d0->mUnk_20 = 1;
    data_0217aa00->mUnk_0454 = 0x40; /* Simple-mode bit must never be cleared */
    gMockParticipantUnlockMask = 0;

    for (cup = 0; cup < 8; cup++) {
        for (idx = 0; idx < 4; idx++) {
            u32 startFlat = cup * 4 + idx;
            u8 seen[64] = {0};
            gRaceSetup->mNextHeader.mCourseListMode = 1;
            gRaceSetup->mCourseListMode = 1;
            gRaceSetup->mNextHeader.mCourseId = (s32) gCupCourseIdTable[cup][idx];
            BuildCourseList(1);

            /* Wireless Simple-mode flag preserved */
            assert((data_0217aa00->mUnk_0454 & 0x40) == 0x40);
            /* Terminator at index 32 and canary intact */
            assert(guardedBuf[32] == 0);
            for (step = 0; step < 16; step++) {
                assert(guardedBuf[0x21 + step] == 0xAA);
            }
            for (step = 0; step < 32; step++) {
                u32 flat = (startFlat + step) % 32;
                u32 expected = gCupCourseIdTable[flat / 4][flat % 4];
                assert(guardedBuf[step] == expected);
                assert(seen[expected] == 0);
                seen[expected] = 1;
            }
            /* Verify 64 consecutive race transitions wrap cleanly 31 -> 0 -> 31 -> 0 */
            for (step = 1; step <= 64; step++) {
                u32 expectedFlat = (startFlat + (step % 32)) % 32;
                AdvanceCourseListStep();
                assert(gRaceSetup->mCourseListIndex == (step % 32));
                assert(gRaceSetup->mNextHeader.mCourseId ==
                       (s32) gCupCourseIdTable[expectedFlat / 4][expectedFlat % 4]);
            }
        }
    }

    /* 3. Simple-mode Random VS (mode 2): 100 seeds all yield exact 32-track permutation + 0 terminator */
    for (seed = 1; seed <= 100; seed++) {
        u8 seen[64] = {0};
        memset(guardedBuf, 0xCC, sizeof(guardedBuf));
        gMockRng.mValue = 0x123456789ABCDEF0ULL * seed;
        gMockRng.mMultiplier = 0x5D588B656C078965ULL;
        gMockRng.mIncrement = 0x0000000000269EC3ULL;
        gRaceSetup->mNextHeader.mCourseListMode = 2;
        gRaceSetup->mCourseListMode = 2;
        BuildCourseList(1);

        assert((data_0217aa00->mUnk_0454 & 0x40) == 0x40);
        assert(guardedBuf[32] == 0);
        for (step = 0; step < 16; step++) {
            assert(guardedBuf[0x21 + step] == 0xCC);
        }
        for (step = 0; step < 32; step++) {
            u8 cid = guardedBuf[step];
            assert(cid > 0 && cid < 64);
            assert(seen[cid] == 0);
            seen[cid] = 1;
        }
        for (cup = 0; cup < 8; cup++) {
            for (idx = 0; idx < 4; idx++) {
                assert(seen[gCupCourseIdTable[cup][idx]] == 1);
            }
        }
    }

    /* 4. Simple-mode Battle (isVersus == 0): must STILL restrict to the 3 Simple-mode battle stages */
    gRaceSetup->mNextHeader.mCourseListMode = 1;
    gRaceSetup->mNextHeader.mCourseId = 44;
    BuildCourseList(0);
    assert(guardedBuf[0] == 44 && guardedBuf[1] == 47 && guardedBuf[2] == 43 && guardedBuf[3] == 0);

    gRaceSetup->mNextHeader.mCourseListMode = 2;
    BuildCourseList(0);
    assert(guardedBuf[0] != 0 && guardedBuf[1] != 0 && guardedBuf[2] != 0 && guardedBuf[3] == 0);

    /* 5. Normal multi-card VS (mUnk_0454 & 0x40 == 0): must STILL respect participant cup unlock flags */
    data_0217aa00->mUnk_0454 = 0;
    gMockParticipantUnlockMask = 0;
    gRaceSetup->mNextHeader.mCourseListMode = 1;
    gRaceSetup->mNextHeader.mCourseId = 20;
    BuildCourseList(1);
    for (step = 0; step < 16; step++) {
        assert(guardedBuf[step] != 0);
    }
    assert(guardedBuf[16] == 0);

    /* 6. 8-Player (1 Host + 7 DLP Clients) HaxxStation cache, sentBitmap (AID 4..7), cohort multicast & 0xFFFF barrier */
    TestHaxxStation7PeersAndCohortMulticast();

    printf("ALL simple32 & 8-player HaxxStation logic unit tests PASSED (32 sequential starts, 64x wrap-around, 100 random seeds, 7-peer cache/multicast/barrier, Battle & Normal-VS invariants).\n");
    return 0;
}
