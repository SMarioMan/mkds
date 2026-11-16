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

    printf("ALL simple32 logic unit tests PASSED (32 sequential starts, 64x wrap-around, 100 random seeds, Battle & Normal-VS invariants).\n");
    return 0;
}
