#include "Main/Unknown/code_02008880.h"
#include "Main/HaxxStation.h"
#include "code_02003024.h"
#include "code_02003be0.h"
#include "fs_file.h"

#pragma thumb on

u16 ComputeU16Checksum(const u16 *data, s32 byteCount) {
    u32 sum;
    u32 folded;
    s32 halfwordCount;
    halfwordCount = byteCount >> 1;
    sum = 0;
    while (halfwordCount > 0) {
        sum += *data++;
        halfwordCount--;
    }
    folded = (sum >> 16) + (sum & 0xffff);
    return (folded + (folded >> 16)) ^ 0xffff;
}

u32 func_02008880(FSFile *a) {
    Unk02008880 header;
    Unk02008880 *headerPtr;
    u32 size;
    s32 v4;
    headerPtr = NULL;
    size = 0;
    if (a != NULL) {
        v4 = a->mUnk_2c - a->mProp.w.mUnk_04;
        if ((u32) FS_ReadFile(a, &header, 0x60) >= 0x60) {
            headerPtr = &header;
        }
        FS_SeekFile(a, v4, 0);
    } else {
        FSFile childFile;
        const u8 *childPath = HaxxStation_IsEnabled() ? (const u8 *) "/haxx/station_client.srl"
                                                      : (const u8 *) "/haxx/mb_child_usa.bin";
        FS_InitFile(&childFile);
        if (FS_OpenFile(&childFile, childPath) != 0 ||
            (!HaxxStation_IsEnabled() && FS_OpenFile(&childFile, (const u8 *) "/data/Boot/mb_child_usa.bin") != 0)) {
            if ((u32) FS_ReadFile(&childFile, &header, 0x60) >= 0x60) {
                headerPtr = &header;
            }
            FS_CloseFile(&childFile);
        } else {
            headerPtr = (Unk02008880 *) 0x027ffe00;
        }
    }
    if (headerPtr != NULL) {
        size = headerPtr->mUnk_2c + 0x268 + headerPtr->mUnk_3c;
        if (size < 0x10000) {
            size = 0x10000;
        }
    }
    return size;
}

#pragma thumb off
