#include "NDS.h"
#include "Platform.h"
#include "SPI_Firmware.h"
#include "net/LocalMP.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace melonDS::Platform
{

struct FileHandle
{
    FILE* fp = nullptr;
};

struct Thread
{
    std::thread th;
};

struct Semaphore
{
    std::mutex mtx;
    std::condition_variable cv;
    int count = 0;
};

struct Mutex
{
    std::mutex mtx;
};

static LocalMP* gLocalMP = nullptr;
static bool gVerboseLog = false;
static melonDS::NDS* gConsoles[16] {};
static int gNumConsoles = 0;
static std::atomic<bool> gWorkerInFrame[16] {};
static std::atomic<bool> gHostWaitingReplies {false};
static std::atomic<bool> gStopAll {false};
static std::atomic<u64> gFrameStartArm9[16] {};
static std::atomic<bool> gStage2Active {false};
static std::atomic<u16> gRepliedInstMask {0};
static std::atomic<u16> gCurrentAidMask {0};
static std::atomic<u16> gMissedReplyMask {0};
static std::mutex gLateReplyMtx;
static u8 gLateReplyBuf[16][1024] {};
static int gLateReplyLen[16] {};
static std::atomic<u64> gLastCmdTimestamp {0};
static std::atomic<u64> gLastHostUS {0};
static std::atomic<u64> gLastHostArm9 {0};
static std::atomic<u64> gGuestRefUS[16] {};
static std::atomic<u64> gGuestRefArm9[16] {};
static std::atomic<u64> gStatCmdSent {0};
static std::atomic<u64> gStatHostPktRecv {0};
static std::atomic<u64> gStatReplySent {0};
static std::atomic<u64> gStatReplyRecv {0};
struct BufferedRxPkt
{
    int len = 0;
    u64 ts = 0;
    u8 data[2048] {};
};
static std::mutex gRxQueueMtx;
static std::deque<BufferedRxPkt> gRxQueue[16];
static std::function<bool()> gPumpGuestsCb;
static thread_local int tCurrentInst = -1;
static thread_local bool tInRecvReplies = false;

} // namespace melonDS::Platform

void Harness_SetPumpGuestsCallback(std::function<bool()> cb)
{
    melonDS::Platform::gPumpGuestsCb = std::move(cb);
}

bool Harness_HasGuestReplied(int inst)
{
    if (inst < 0 || inst >= 16) return false;
    return (melonDS::Platform::gRepliedInstMask.load(std::memory_order_acquire) & static_cast<melonDS::u16>(1u << inst)) != 0;
}

melonDS::u16 Harness_GetCurrentAidMask()
{
    return melonDS::Platform::gCurrentAidMask.load(std::memory_order_acquire);
}

void Harness_GetMPStats(std::uint64_t& cmdSent, std::uint64_t& hostRecv, std::uint64_t& replySent, std::uint64_t& replyRecv)
{
    cmdSent   = melonDS::Platform::gStatCmdSent.load(std::memory_order_relaxed);
    hostRecv  = melonDS::Platform::gStatHostPktRecv.load(std::memory_order_relaxed);
    replySent = melonDS::Platform::gStatReplySent.load(std::memory_order_relaxed);
    replyRecv = melonDS::Platform::gStatReplyRecv.load(std::memory_order_relaxed);
}

void Harness_SetStage2Active(bool active)
{
    melonDS::Platform::gStage2Active.store(active, std::memory_order_release);
    melonDS::Platform::gMissedReplyMask.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lk(melonDS::Platform::gLateReplyMtx);
    std::memset(melonDS::Platform::gLateReplyLen, 0, sizeof(melonDS::Platform::gLateReplyLen));
}

void Harness_RegisterConsoles(int count, melonDS::NDS** ptrs)
{
    melonDS::Platform::gNumConsoles = count;
    for (int i = 0; i < 16; ++i)
        melonDS::Platform::gConsoles[i] = (i < count) ? ptrs[i] : nullptr;
}

void Harness_InitLocalMP(int recvTimeoutMs, bool verboseLog)
{
    melonDS::Platform::gVerboseLog = verboseLog;
    melonDS::Platform::gStopAll.store(false, std::memory_order_relaxed);
    for (int i = 0; i < 16; ++i)
        melonDS::Platform::gWorkerInFrame[i].store(false, std::memory_order_relaxed);
    if (!melonDS::Platform::gLocalMP)
    {
        melonDS::Platform::gLocalMP = new melonDS::LocalMP();
    }
    // Non-zero sentinel so LocalMP passes timeout > 0 for blocking paths, which Semaphore_TryWait handles in microseconds.
    melonDS::Platform::gLocalMP->SetRecvTimeout(recvTimeoutMs > 0 ? recvTimeoutMs : 1);
}

void Harness_NotifyStop()
{
    melonDS::Platform::gStopAll.store(true, std::memory_order_release);
}

void Harness_ShutdownLocalMP()
{
    {
        std::lock_guard<std::mutex> lk(melonDS::Platform::gRxQueueMtx);
        for (int i = 0; i < 16; ++i)
            melonDS::Platform::gRxQueue[i].clear();
    }
    delete melonDS::Platform::gLocalMP;
    melonDS::Platform::gLocalMP = nullptr;
}

void Harness_CaptureFrameStarts()
{
    for (int i = 0; i < melonDS::Platform::gNumConsoles; ++i)
    {
        if (melonDS::Platform::gConsoles[i])
        {
            melonDS::Platform::gFrameStartArm9[i].store(
                melonDS::Platform::gConsoles[i]->ARM9Timestamp, std::memory_order_relaxed);
        }
    }
    std::atomic_thread_fence(std::memory_order_release);
}

void Harness_NotifyFrameBegin(int inst)
{
    melonDS::Platform::tCurrentInst = inst;
    if (inst >= 0 && inst < 16)
        melonDS::Platform::gWorkerInFrame[inst].store(true, std::memory_order_release);
}

void Harness_NotifyFrameEnd(int inst)
{
    if (inst >= 0 && inst < 16)
        melonDS::Platform::gWorkerInFrame[inst].store(false, std::memory_order_release);
}

namespace melonDS::Platform
{

void SignalStop(StopReason /*reason*/, void* /*userdata*/) {}

std::string GetLocalFilePath(const std::string& filename) { return filename; }

static const char* ModeToFopen(FileMode mode)
{
    const bool r = (mode & FileMode::Read) != 0;
    const bool w = (mode & FileMode::Write) != 0;
    const bool p = (mode & FileMode::Preserve) != 0;
    const bool a = (mode & FileMode::Append) != 0;
    const bool t = (mode & FileMode::Text) != 0;
    if (a) return t ? "a+" : "ab+";
    if (r && w) return p ? (t ? "r+" : "rb+") : (t ? "w+" : "wb+");
    if (w) return p ? (t ? "r+" : "rb+") : (t ? "w" : "wb");
    return t ? "r" : "rb";
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & FileMode::NoCreate) && !std::filesystem::exists(path))
        return nullptr;
    if ((mode & FileMode::Preserve) && (mode & FileMode::Write) && !std::filesystem::exists(path))
    {
        FILE* createFp = std::fopen(path.c_str(), "wb");
        if (createFp) std::fclose(createFp);
    }
    FILE* fp = std::fopen(path.c_str(), ModeToFopen(mode));
    if (!fp) return nullptr;
    auto* fh = new FileHandle();
    fh->fp = fp;
    return fh;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(path, mode);
}

bool FileExists(const std::string& name) { return std::filesystem::exists(name); }
bool LocalFileExists(const std::string& name) { return std::filesystem::exists(name); }
bool CheckFileWritable(const std::string& /*filepath*/) { return true; }
bool CheckLocalFileWritable(const std::string& /*filepath*/) { return true; }

bool CloseFile(FileHandle* file)
{
    if (!file) return false;
    int rc = file->fp ? std::fclose(file->fp) : 0;
    delete file;
    return rc == 0;
}

bool IsEndOfFile(FileHandle* file)
{
    return (!file || !file->fp) ? true : (std::feof(file->fp) != 0);
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    if (!file || !file->fp) return false;
    return std::fgets(str, count, file->fp) != nullptr;
}

u64 FilePosition(FileHandle* file)
{
    if (!file || !file->fp) return 0;
    long pos = std::ftell(file->fp);
    return pos < 0 ? 0 : static_cast<u64>(pos);
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    if (!file || !file->fp) return false;
    int whence = SEEK_SET;
    if (origin == FileSeekOrigin::Current) whence = SEEK_CUR;
    else if (origin == FileSeekOrigin::End) whence = SEEK_END;
    return std::fseek(file->fp, static_cast<long>(offset), whence) == 0;
}

void FileRewind(FileHandle* file)
{
    if (file && file->fp) std::rewind(file->fp);
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    if (!file || !file->fp) return 0;
    return std::fread(data, size, count, file->fp);
}

bool FileFlush(FileHandle* file)
{
    if (!file || !file->fp) return false;
    return std::fflush(file->fp) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    if (!file || !file->fp) return 0;
    return std::fwrite(data, size, count, file->fp);
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    if (!file || !file->fp) return 0;
    va_list args;
    va_start(args, fmt);
    int n = std::vfprintf(file->fp, fmt, args);
    va_end(args);
    return n < 0 ? 0 : static_cast<u64>(n);
}

u64 FileLength(FileHandle* file)
{
    if (!file || !file->fp) return 0;
    long cur = std::ftell(file->fp);
    if (std::fseek(file->fp, 0, SEEK_END) != 0) return 0;
    long end = std::ftell(file->fp);
    std::fseek(file->fp, cur, SEEK_SET);
    return end < 0 ? 0 : static_cast<u64>(end);
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (!gVerboseLog && level < LogLevel::Warn)
        return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

Thread* Thread_Create(std::function<void()> func)
{
    auto* t = new Thread();
    t->th = std::thread(std::move(func));
    return t;
}

void Thread_Free(Thread* thread)
{
    if (!thread) return;
    if (thread->th.joinable()) thread->th.join();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread && thread->th.joinable()) thread->th.join();
}

Semaphore* Semaphore_Create() { return new Semaphore(); }
void Semaphore_Free(Semaphore* sema) { delete sema; }

void Semaphore_Reset(Semaphore* sema)
{
    if (!sema) return;
    std::lock_guard<std::mutex> lk(sema->mtx);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    if (!sema) return;
    std::unique_lock<std::mutex> lk(sema->mtx);
    sema->cv.wait(lk, [&] { return sema->count > 0; });
    sema->count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    if (!sema) return false;
    {
        std::lock_guard<std::mutex> lk(sema->mtx);
        if (sema->count > 0)
        {
            sema->count--;
            return true;
        }
    }
    if (timeout_ms > 0 && tInRecvReplies && gPumpGuestsCb)
    {
        bool savedInRecv = tInRecvReplies;
        tInRecvReplies = false;
        for (int attempt = 0; attempt < 400; ++attempt)
        {
            bool progressed = gPumpGuestsCb();
            std::lock_guard<std::mutex> lk(sema->mtx);
            if (sema->count > 0)
            {
                sema->count--;
                tInRecvReplies = savedInRecv;
                return true;
            }
            if (!progressed)
                break;
        }
        tInRecvReplies = savedInRecv;
    }
    return false;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    if (!sema || count <= 0) return;
    {
        std::lock_guard<std::mutex> lk(sema->mtx);
        sema->count += count;
    }
    if (count == 1)
        sema->cv.notify_one();
    else
        sema->cv.notify_all();
}

Mutex* Mutex_Create() { return new Mutex(); }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { if (mutex) mutex->mtx.lock(); }
void Mutex_Unlock(Mutex* mutex) { if (mutex) mutex->mtx.unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return mutex ? mutex->mtx.try_lock() : false; }

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

u64 GetUSCount()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static inline int InstId(void* userdata)
{
    return userdata ? *static_cast<int*>(userdata) : 0;
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32 /*writeoffset*/, u32 /*writelen*/, void* userdata);
void WriteGBASave(const u8* /*savedata*/, u32 /*savelen*/, u32 /*writeoffset*/, u32 /*writelen*/, void* /*userdata*/) {}
void WriteFirmware(const Firmware& /*firmware*/, u32 /*writeoffset*/, u32 /*writelen*/, void* /*userdata*/) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void* userdata)
{
    int inst = InstId(userdata);
    if (inst >= 0 && inst < 16)
    {
        std::lock_guard<std::mutex> lk(gRxQueueMtx);
        gRxQueue[inst].clear();
    }
    if (gLocalMP) gLocalMP->Begin(inst);
}

void MP_End(void* userdata)
{
    int inst = InstId(userdata);
    if (inst >= 0 && inst < 16)
    {
        std::lock_guard<std::mutex> lk(gRxQueueMtx);
        gRxQueue[inst].clear();
    }
    if (gLocalMP) gLocalMP->End(inst);
}

static inline void RecordHostPacketClock(int inst, u64 hostUS)
{
    if (inst == 0 && gConsoles[0])
    {
        gLastHostUS.store(hostUS, std::memory_order_relaxed);
        gLastHostArm9.store(gConsoles[0]->ARM9Timestamp, std::memory_order_release);
    }
}

static inline void PaceHostToGuests(int inst)
{
    if (inst != 0 || !gConsoles[0])
        return;
    const u64 hostTs = gConsoles[0]->ARM9Timestamp;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(15000);
    for (int g = 1; g < gNumConsoles; ++g)
    {
        if (!gConsoles[g])
            continue;
        while (!gStopAll.load(std::memory_order_acquire))
        {
            const u64 guestTs = gConsoles[g]->ARM9Timestamp;
            if (hostTs + 24576ULL <= guestTs || std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::yield();
        }
    }
}

int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata)
{
    int inst = InstId(userdata);
    RecordHostPacketClock(inst, timestamp);
    return gLocalMP ? gLocalMP->SendPacket(inst, data, len, timestamp) : 0;
}

static inline void DrainAllPacketFifos()
{
    if (!gLocalMP) return;
    const int maxConsole = (gNumConsoles > 0 && gNumConsoles <= 16) ? gNumConsoles : 16;
    std::lock_guard<std::mutex> lk(gRxQueueMtx);
    for (int c = 0; c < maxConsole; ++c)
    {
        for (;;)
        {
            BufferedRxPkt pkt;
            int rxlen = gLocalMP->RecvPacket(c, pkt.data, &pkt.ts);
            if (rxlen <= 0)
                break;
            pkt.len = rxlen;
            gRxQueue[c].push_back(pkt);
            while (gRxQueue[c].size() > 2048)
                gRxQueue[c].pop_front();
        }
    }
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata)
{
    if (!gLocalMP) return 0;
    int inst = InstId(userdata);
    int rxlen = 0;
    bool fromQueue = false;
    if (inst >= 0 && inst < 16)
    {
        std::lock_guard<std::mutex> lk(gRxQueueMtx);
        if (!gRxQueue[inst].empty())
        {
            const auto& front = gRxQueue[inst].front();
            rxlen = front.len;
            std::memcpy(data, front.data, rxlen);
            if (timestamp) *timestamp = front.ts;
            gRxQueue[inst].pop_front();
            fromQueue = true;
        }
    }
    if (!fromQueue)
    {
        rxlen = gLocalMP->RecvPacket(inst, data, timestamp);
    }
    if (rxlen >= 14 && inst > 0 && inst < 16 && timestamp && *timestamp && data &&
        (data[0xC] & 0xFF) == 0x10 && (data[0xD] & 0x03) == 0x00 && gConsoles[inst])
    {
        // Align Association Response (0x0010) sync timestamp to Guest's current ARM9Timestamp so
        // Host and all 1..7 Guests share an identical USTimestamp(SysTimestamp) curve without skew.
        const u64 hostUS = gLastHostUS.load(std::memory_order_acquire);
        const u64 hostArm9 = gLastHostArm9.load(std::memory_order_acquire);
        if (hostUS > 0 && hostArm9 > 0)
        {
            const s64 deltaArm9 = static_cast<s64>(gConsoles[inst]->ARM9Timestamp) - static_cast<s64>(hostArm9);
            const s64 deltaUS = (deltaArm9 * 1000000LL) / 67027964LL;
            const s64 aligned = static_cast<s64>(hostUS) + deltaUS;
            if (aligned > 0)
                *timestamp = static_cast<u64>(aligned) & ~7ULL;
        }
        gGuestRefUS[inst].store(*timestamp, std::memory_order_relaxed);
        gGuestRefArm9[inst].store(gConsoles[inst]->ARM9Timestamp, std::memory_order_release);
    }
    return rxlen;
}

int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata)
{
    int inst = InstId(userdata);
    RecordHostPacketClock(inst, timestamp);
    u64 n = gStatCmdSent.fetch_add(1, std::memory_order_relaxed) + 1;
    if (inst == 0 && (n & 7ULL) == 0)
    {
        // Prevent continuous MP_SendCmd/MP_SendAck writes from lapping PacketReadOffset[0..N] in LocalMP's 64 KiB ring!
        DrainAllPacketFifos();
    }
    gLastCmdTimestamp.store(timestamp, std::memory_order_release);
    gRepliedInstMask.store(0, std::memory_order_release);
    gMissedReplyMask.store(0, std::memory_order_release);
    return gLocalMP ? gLocalMP->SendCmd(inst, data, len, timestamp) : 0;
}

int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata)
{
    int inst = InstId(userdata);
    if (inst > 0 && inst < 16 && len > 0 && aid > 0)
    {
        gRepliedInstMask.fetch_or(static_cast<u16>(1u << inst), std::memory_order_acq_rel);
        gStatReplySent.fetch_add(1, std::memory_order_relaxed);
    }
    u64 cmdTs = gLastCmdTimestamp.load(std::memory_order_acquire);
    u64 replyTs = (timestamp < cmdTs) ? cmdTs : timestamp;
    return gLocalMP ? gLocalMP->SendReply(inst, data, len, replyTs, aid) : 0;
}

int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata)
{
    int inst = InstId(userdata);
    RecordHostPacketClock(inst, timestamp);
    return gLocalMP ? gLocalMP->SendAck(inst, data, len, timestamp) : 0;
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata)
{
    if (!gLocalMP) return 0;
    int inst = InstId(userdata);
    int len = 0;
    bool fromQueue = false;
    if (inst >= 0 && inst < 16)
    {
        std::lock_guard<std::mutex> lk(gRxQueueMtx);
        if (!gRxQueue[inst].empty())
        {
            const auto& front = gRxQueue[inst].front();
            len = front.len;
            std::memcpy(data, front.data, len);
            if (timestamp) *timestamp = front.ts;
            gRxQueue[inst].pop_front();
            fromQueue = true;
        }
    }
    if (!fromQueue)
    {
        len = gLocalMP->RecvHostPacket(inst, data, timestamp);
    }
    if (len > 0)
    {
        gStatHostPktRecv.fetch_add(1, std::memory_order_relaxed);
    }
    return len;
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata)
{
    if (!gLocalMP) return 0;
    tInRecvReplies = true;
    gCurrentAidMask.store(aidmask, std::memory_order_release);
    gHostWaitingReplies.store(true, std::memory_order_release);
    // Note: Wifi.cpp calls MP_SendCmd at T_start (CurPhase 0), then calls MP_RecvReplies at T_end = T_start + framelen*4 (CurPhase 1).
    // Passing T_start (gLastCmdTimestamp) prevents LocalMP::RecvReplies from falsely discarding replies to >8-byte CMDs via `< timestamp - 32`!
    u64 cmdTs = gLastCmdTimestamp.load(std::memory_order_acquire);
    u16 res = gLocalMP->RecvReplies(InstId(userdata), data, cmdTs ? cmdTs : timestamp, aidmask);
    gHostWaitingReplies.store(false, std::memory_order_release);
    gCurrentAidMask.store(0, std::memory_order_release);
    tInRecvReplies = false;
    if (res != 0) gStatReplyRecv.fetch_add(1, std::memory_order_relaxed);
    return res;
}

int Net_SendPacket(u8* /*data*/, int /*len*/, void* /*userdata*/) { return 0; }
int Net_RecvPacket(u8* /*data*/, void* /*userdata*/) { return 0; }

void Camera_Start(int /*num*/, void* /*userdata*/) {}
void Camera_Stop(int /*num*/, void* /*userdata*/) {}
void Camera_CaptureFrame(int /*num*/, u32* /*frame*/, int /*width*/, int /*height*/, bool /*yuv*/, void* /*userdata*/) {}

void Mic_Start(void* /*userdata*/) {}
void Mic_Stop(void* /*userdata*/) {}
int Mic_ReadInput(s16* data, int maxlength, void* /*userdata*/)
{
    if (data && maxlength > 0) std::memset(data, 0, sizeof(s16) * maxlength);
    return maxlength;
}

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder* /*dec*/) {}
bool AAC_Configure(AACDecoder* /*dec*/, int /*frequency*/, int /*channels*/) { return false; }
bool AAC_DecodeFrame(AACDecoder* /*dec*/, const void* /*input*/, int /*inputlen*/, void* /*output*/, int /*outputlen*/) { return false; }

bool Addon_KeyDown(KeyType /*type*/, void* /*userdata*/) { return false; }
void Addon_RumbleStart(u32 /*len*/, void* /*userdata*/) {}
void Addon_RumbleStop(void* /*userdata*/) {}
float Addon_MotionQuery(MotionQueryType /*type*/, void* /*userdata*/) { return 0.0f; }

DynamicLibrary* DynamicLibrary_Load(const char* /*lib*/) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary* /*lib*/) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary* /*lib*/, const char* /*name*/) { return nullptr; }

} // namespace melonDS::Platform
