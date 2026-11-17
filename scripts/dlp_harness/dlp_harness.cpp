#include "Args.h"
#include "GPU.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "RTC.h"
#include "SPI_Firmware.h"
#include "Savestate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

extern void Harness_InitLocalMP(int recvTimeoutMs, bool verboseLog);
extern void Harness_ShutdownLocalMP();
extern void Harness_RegisterConsoles(int count, melonDS::NDS** ptrs);
extern void Harness_GetMPStats(u64& cmdSent, u64& hostRecv, u64& replySent, u64& replyRecv);
extern void Harness_SetStage2Active(bool active);
extern void Harness_NotifyStop();
extern void Harness_SetPumpGuestsCallback(std::function<bool()> cb);
extern void Harness_CaptureFrameStarts();
extern void Harness_NotifyFrameBegin(int inst);
extern void Harness_NotifyFrameEnd(int inst);

namespace
{

// DS KeyInput bits (SetKeyMask uses 0 = pressed, 1 = released!)
constexpr u32 KEY_A      = (1u << 0);
constexpr u32 KEY_B      = (1u << 1);
constexpr u32 KEY_SELECT = (1u << 2);
constexpr u32 KEY_START  = (1u << 3);
constexpr u32 KEY_RIGHT  = (1u << 4);
constexpr u32 KEY_LEFT   = (1u << 5);
constexpr u32 KEY_UP     = (1u << 6);
constexpr u32 KEY_DOWN   = (1u << 7);
constexpr u32 KEY_R      = (1u << 8);
constexpr u32 KEY_L      = (1u << 9);
constexpr u32 KEY_X      = (1u << 10);
constexpr u32 KEY_Y      = (1u << 11);

static inline u32 PressKeys(u32 pressedMask)
{
    return 0xFFFu & ~pressedMask;
}

static std::string gHostSaveOutPath;
static std::mutex gSaveMutex;

} // namespace

namespace melonDS::Platform
{
void WriteNDSSave(const u8* savedata, u32 savelen, u32 /*writeoffset*/, u32 /*writelen*/, void* userdata)
{
    int inst = userdata ? *static_cast<int*>(userdata) : 0;
    if (inst == 0 && !gHostSaveOutPath.empty() && savedata && savelen > 0)
    {
        std::lock_guard<std::mutex> lk(gSaveMutex);
        std::ofstream ofs(gHostSaveOutPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(savedata), savelen);
    }
}
} // namespace melonDS::Platform

namespace
{

// Minimal SHA-256 implementation for byte-exact verification of Guest Cloneboot RAM headers/state
struct Sha256
{
    static std::string Hex(const u8* data, std::size_t len)
    {
        static const u32 k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        auto rotr = [](u32 x, u32 n) { return (x >> n) | (x << (32 - n)); };
        u32 h[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };
        std::vector<u8> msg(data, data + len);
        u64 bitLen = static_cast<u64>(len) * 8ULL;
        msg.push_back(0x80);
        while ((msg.size() % 64) != 56) msg.push_back(0x00);
        for (int i = 7; i >= 0; --i) msg.push_back(static_cast<u8>((bitLen >> (i * 8)) & 0xFF));

        for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64)
        {
            u32 w[64];
            for (int i = 0; i < 16; ++i)
            {
                w[i] = (u32(msg[chunk + i * 4]) << 24) |
                       (u32(msg[chunk + i * 4 + 1]) << 16) |
                       (u32(msg[chunk + i * 4 + 2]) << 8) |
                       u32(msg[chunk + i * 4 + 3]);
            }
            for (int i = 16; i < 64; ++i)
            {
                u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; ++i)
            {
                u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                u32 ch = (e & f) ^ ((~e) & g);
                u32 temp1 = hh + S1 + ch + k[i] + w[i];
                u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                u32 maj = (a & b) ^ (a & c) ^ (b & c);
                u32 temp2 = S0 + maj;
                hh = g; g = f; f = e; e = d + temp1;
                d = c; c = b; b = a; a = temp1 + temp2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) oss << std::setw(8) << h[i];
        return oss.str();
    }
};

// Zero-dependency PNG writer (stored zlib blocks + Adler-32 + CRC-32)
static u32 PngCrc32Update(u32 crc, const u8* buf, std::size_t len)
{
    for (std::size_t i = 0; i < len; ++i)
    {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc;
}

static bool WritePngRGBA(const std::string& path, int width, int height, const std::vector<u8>& rgba)
{
    std::vector<u8> raw;
    raw.reserve(height * (1 + width * 4));
    for (int y = 0; y < height; ++y)
    {
        raw.push_back(0); // filter: None
        const u8* row = &rgba[y * width * 4];
        raw.insert(raw.end(), row, row + width * 4);
    }

    // Wrap raw scanlines in RFC 1950 zlib stream with stored deflate blocks
    std::vector<u8> zstream;
    zstream.reserve(raw.size() + (raw.size() / 65535 + 2) * 5 + 6);
    zstream.push_back(0x78);
    zstream.push_back(0x01);
    std::size_t pos = 0;
    while (pos < raw.size())
    {
        u16 blkLen = static_cast<u16>(std::min<std::size_t>(65535, raw.size() - pos));
        bool last = (pos + blkLen == raw.size());
        zstream.push_back(last ? 0x01 : 0x00);
        zstream.push_back(u8(blkLen & 0xFF));
        zstream.push_back(u8(blkLen >> 8));
        u16 nlen = ~blkLen;
        zstream.push_back(u8(nlen & 0xFF));
        zstream.push_back(u8(nlen >> 8));
        zstream.insert(zstream.end(), raw.begin() + pos, raw.begin() + pos + blkLen);
        pos += blkLen;
    }
    u32 s1 = 1, s2 = 0;
    for (u8 b : raw)
    {
        s1 = (s1 + b) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    u32 adler = (s2 << 16) | s1;
    zstream.push_back(u8(adler >> 24));
    zstream.push_back(u8(adler >> 16));
    zstream.push_back(u8(adler >> 8));
    zstream.push_back(u8(adler));

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;

    auto writeU32BE = [&](u32 v) {
        u8 b[4] = {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)};
        ofs.write(reinterpret_cast<const char*>(b), 4);
    };
    auto writeChunk = [&](const char type[4], const u8* data, std::size_t len) {
        writeU32BE(static_cast<u32>(len));
        ofs.write(type, 4);
        if (len > 0) ofs.write(reinterpret_cast<const char*>(data), len);
        u32 crc = 0xFFFFFFFFu;
        crc = PngCrc32Update(crc, reinterpret_cast<const u8*>(type), 4);
        if (len > 0) crc = PngCrc32Update(crc, data, len);
        writeU32BE(crc ^ 0xFFFFFFFFu);
    };

    const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    ofs.write(reinterpret_cast<const char*>(sig), 8);

    u8 ihdr[13] = {
        u8(width >> 24), u8(width >> 16), u8(width >> 8), u8(width),
        u8(height >> 24), u8(height >> 16), u8(height >> 8), u8(height),
        8, 6, 0, 0, 0 // 8-bit RGBA
    };
    writeChunk("IHDR", ihdr, 13);
    writeChunk("IDAT", zstream.data(), zstream.size());
    writeChunk("IEND", nullptr, 0);
    return true;
}

class HarnessNDS : public melonDS::NDS
{
public:
    using melonDS::NDS::NDS;

    u64 GetFrameStartSys() const noexcept { return FrameStartTimestamp; }
    u64 GetSysTimestamp() const noexcept { return SysTimestamp; }

    void BeginFrameSlice()
    {
        Current = this;
        FrameStartTimestamp = SysTimestamp;
        GPU.TotalScanlines = 0;
        LagFrameFlag = true;
        if (Running && !(CPUStop & melonDS::CPUStop_Sleep))
        {
            if (!(CPUStop & melonDS::CPUStop_Wakeup))
                GPU.StartFrame();
            CPUStop &= ~melonDS::CPUStop_Wakeup;
        }
    }

    bool FrameSliceDone() const noexcept
    {
        return !Running || GPU.TotalScanlines != 0;
    }

    void RunUntilSys(u64 sliceEndSys)
    {
        Current = this;
        while (Running && GPU.TotalScanlines == 0 && SysTimestamp < sliceEndSys)
        {
            if (CPUStop & melonDS::CPUStop_Sleep)
            {
                u64 frametarget = FrameStartTimestamp + 560190;
                u64 target = NextTargetSleep();
                if (target > sliceEndSys) target = sliceEndSys;
                if (target > frametarget) target = frametarget;
                ARM9Timestamp = target << ARM9ClockShift;
                ARM7Timestamp = target;
                TimerTimestamp[0] = target;
                TimerTimestamp[1] = target;
                GPU.GPU3D.Timestamp = target;
                RunSystemSleep(target);
                if (SysTimestamp >= frametarget)
                    GPU.BlankFrame();
                if (!(CPUStop & melonDS::CPUStop_Sleep))
                {
                    if (!(CPUStop & melonDS::CPUStop_Wakeup))
                        GPU.StartFrame();
                    CPUStop &= ~melonDS::CPUStop_Wakeup;
                }
                continue;
            }

            u64 target = NextTarget();
            if (target > sliceEndSys)
                target = sliceEndSys;
            ARM9Target = target << ARM9ClockShift;
            CurCPU = 0;

            if (CPUStop & melonDS::CPUStop_GXStall)
            {
                s32 cycles = GPU.GPU3D.CyclesToRunFor();
                ARM9Timestamp = std::min(ARM9Target, ARM9Timestamp + (static_cast<u64>(cycles) << ARM9ClockShift));
            }
            else if (CPUStop & melonDS::CPUStop_DMA9)
            {
                DMAs[0].Run();
                if (!(CPUStop & melonDS::CPUStop_GXStall)) DMAs[1].Run();
                if (!(CPUStop & melonDS::CPUStop_GXStall)) DMAs[2].Run();
                if (!(CPUStop & melonDS::CPUStop_GXStall)) DMAs[3].Run();
            }
            else
            {
                ARM9.Execute<melonDS::CPUExecuteMode::Interpreter>();
            }

            RunTimers(0);
            GPU.GPU3D.Run();

            target = ARM9Timestamp >> ARM9ClockShift;
            CurCPU = 1;

            while (ARM7Timestamp < target)
            {
                ARM7Target = target;
                if (CPUStop & melonDS::CPUStop_DMA7)
                {
                    DMAs[4].Run();
                    DMAs[5].Run();
                    DMAs[6].Run();
                    DMAs[7].Run();
                }
                else
                {
                    ARM7.Execute<melonDS::CPUExecuteMode::Interpreter>();
                }
                RunTimers(1);
            }

            RunSystem(target);
        }
    }

    void EndFrameSlice()
    {
        Current = this;
        SPU.BufferAudio();
        NumFrames++;
        if (LagFrameFlag)
            NumLagFrames++;
    }
};

template <typename ConsoleVec>
static void SaveCompositeScreenshot(const std::string& path, const ConsoleVec& consoles)
{
    int n = static_cast<int>(consoles.size());
    int width = 256 * n;
    int height = 384;
    std::vector<u8> rgba(width * height * 4, 255);
    for (int c = 0; c < n; ++c)
    {
        void* top = nullptr;
        void* bottom = nullptr;
        if (!consoles[c]->GPU.GetFramebuffers(&top, &bottom) || !top || !bottom)
            continue;
        const u32* screens[2] = {static_cast<const u32*>(top), static_cast<const u32*>(bottom)};
        for (int s = 0; s < 2; ++s)
        {
            for (int y = 0; y < 192; ++y)
            {
                for (int x = 0; x < 256; ++x)
                {
                    u32 px = screens[s][y * 256 + x];
                    int dstX = c * 256 + x;
                    int dstY = s * 192 + y;
                    std::size_t idx = (dstY * width + dstX) * 4;
                    rgba[idx + 0] = u8((px >> 16) & 0xFF); // R
                    rgba[idx + 1] = u8((px >> 8) & 0xFF);  // G
                    rgba[idx + 2] = u8(px & 0xFF);         // B
                    rgba[idx + 3] = 255;
                }
            }
        }
    }
    WritePngRGBA(path, width, height, rgba);
}

// MKDS RAM symbol set (defaults to unmodified USA retail addresses; overridable per symbol file)
struct MkdsSymbols
{
    u32 gTitleScene             = 0x0217b320;
    u32 data_0217b43c           = 0x0217b41c; // MenuScene*
    u32 gSelectGame             = 0x02174e3c;
    u32 gMultiSelectMemberGroup = 0x02174e30;
    u32 gMultiSelectNS          = 0x02174e34;
    u32 gMultiSelectMember      = 0x02174e38;
    u32 gSelectCharacter        = 0x02174df4;
    u32 gSelectKart             = 0x02174df8;
    u32 gSelectRaceRule         = 0x0217d348;
    u32 gSelectCupGrid          = 0x0217d340;
    u32 gSelectCupCourse        = 0x0217d344;
    u32 gSelectChoises          = 0x0217d3d4;
    u32 gRaceSetup              = 0x021759a0;
    u32 gRaceSetupCopy          = 0x02174dfc;
    u32 gRaceProgress           = 0x021755fc;
    u32 gCourseMap              = 0x02175600;
    u32 gKarts                  = 0x0217acf8;
    u32 gKartCount              = 0x0217acf4;
    u32 gResultScene            = 0x02174e04;
    u32 gResultsGp              = 0x02174e00;
    u32 gResultsBattle          = 0x02174e40;
    u32 data_0217aa00           = 0x0217a9e0; // ConnectionWork*
    u32 data_021759c8           = 0x021759a8; // ArchiveSlots*
    u32 data_021759d0           = 0x021759b0; // Simple/Wireless role state*
    u32 gRomArchiveTransfer     = 0x02175ea0;
    u32 OSi_ExContext           = 0x021701c8;
    u32 gCupCourseIdTable       = 0x02154168;
    u32 gSelectBattleStage      = 0x00000000;
    u32 gHaxxStationHost        = 0x00000000;

    void LoadFromSymbolsTxt(const std::string& path)
    {
        std::ifstream ifs(path);
        if (!ifs) return;
        std::map<std::string, u32*> fields = {
            {"gTitleScene", &gTitleScene},
            {"data_0217b43c", &data_0217b43c},
            {"gSelectGame", &gSelectGame},
            {"gMultiSelectMemberGroup", &gMultiSelectMemberGroup},
            {"gMultiSelectNS", &gMultiSelectNS},
            {"gMultiSelectMember", &gMultiSelectMember},
            {"gSelectCharacter", &gSelectCharacter},
            {"gSelectKart", &gSelectKart},
            {"gSelectRaceRule", &gSelectRaceRule},
            {"gSelectCupGrid", &gSelectCupGrid},
            {"gSelectCupCourse", &gSelectCupCourse},
            {"gSelectChoises", &gSelectChoises},
            {"gRaceSetup", &gRaceSetup},
            {"gRaceSetupCopy", &gRaceSetupCopy},
            {"gRaceProgress", &gRaceProgress},
            {"gCourseMap", &gCourseMap},
            {"gKarts", &gKarts},
            {"gKartCount", &gKartCount},
            {"gResultScene", &gResultScene},
            {"gResultsGp", &gResultsGp},
            {"gResultsBattle", &gResultsBattle},
            {"data_0217aa00", &data_0217aa00},
            {"data_021759c8", &data_021759c8},
            {"data_021759d0", &data_021759d0},
            {"gRomArchiveTransfer", &gRomArchiveTransfer},
            {"OSi_ExContext", &OSi_ExContext},
            {"gCupCourseIdTable", &gCupCourseIdTable},
            {"gHaxxStationHost", &gHaxxStationHost},
        };
        std::string line;
        while (std::getline(ifs, line))
        {
            // Supports both "name kind:... addr:0x0217..." and nm "021759a0 B gRaceSetup"
            std::istringstream iss(line);
            std::string tok1, tok2, tok3;
            if (!(iss >> tok1 >> tok2)) continue;
            auto addrPos = line.find("addr:0x");
            if (addrPos != std::string::npos)
            {
                auto it = fields.find(tok1);
                if (it != fields.end())
                {
                    *it->second = static_cast<u32>(std::stoul(line.substr(addrPos + 7, 8), nullptr, 16));
                }
            }
            else if (iss >> tok3)
            {
                auto it = fields.find(tok3);
                if (it != fields.end() && tok1.size() >= 7)
                {
                    *it->second = static_cast<u32>(std::stoul(tok1, nullptr, 16));
                }
            }
        }
    }
};

// Safe MainRAM readers (DS 4 MB MainRAM is mirrored across 0x02000000..0x027FFFFF with mask 0x3FFFFF)
static inline bool IsValidMainRamPtr(u32 addr)
{
    return addr >= 0x02000000u && addr < 0x02400000u;
}

static inline u8 RamRead8(const melonDS::NDS& nds, u32 addr)
{
    return nds.MainRAM[addr & nds.MainRAMMask];
}

static inline u16 RamRead16(const melonDS::NDS& nds, u32 addr)
{
    u16 v;
    std::memcpy(&v, &nds.MainRAM[addr & nds.MainRAMMask], 2);
    return v;
}

static inline s16 RamReadS16(const melonDS::NDS& nds, u32 addr)
{
    s16 v;
    std::memcpy(&v, &nds.MainRAM[addr & nds.MainRAMMask], 2);
    return v;
}

static inline u32 RamRead32(const melonDS::NDS& nds, u32 addr)
{
    u32 v;
    std::memcpy(&v, &nds.MainRAM[addr & nds.MainRAMMask], 4);
    return v;
}

static inline s32 RamReadS32(const melonDS::NDS& nds, u32 addr)
{
    s32 v;
    std::memcpy(&v, &nds.MainRAM[addr & nds.MainRAMMask], 4);
    return v;
}

// Pure controller-input ENPT waypoint autopilot (read-only RAM observation -> SetKeyMask!)
struct KartAutopilot
{
    u32 currentLinkAddr = 0;
    int stuckFrames = 0;
    int framesSinceLinkAdvance = 0;
    int reverseTimer = 0;
    s32 lastSteerDir = 1;

    void Reset()
    {
        currentLinkAddr = 0;
        stuckFrames = 0;
        framesSinceLinkAdvance = 0;
        reverseTimer = 0;
    }

    u32 Step(const melonDS::NDS& nds, const MkdsSymbols& sym)
    {
        u32 raceSetup = RamRead32(nds, sym.gRaceSetup);
        u32 raceProg  = RamRead32(nds, sym.gRaceProgress);
        u32 courseMap = RamRead32(nds, sym.gCourseMap);
        u32 kartsPtr  = RamRead32(nds, sym.gKarts);
        if (!IsValidMainRamPtr(raceSetup) || !IsValidMainRamPtr(raceProg) ||
            !IsValidMainRamPtr(courseMap) || !IsValidMainRamPtr(kartsPtr))
        {
            Reset();
            return PressKeys(0);
        }

        s32 clockEnabled = RamReadS32(nds, raceProg + 0x04);
        if (clockEnabled == 0)
        {
            Reset();
            return PressKeys(0);
        }

        int entryIdx = static_cast<std::int8_t>(RamRead8(nds, raceSetup + 0x62));
        if (entryIdx < 0 || entryIdx >= 8) entryIdx = 0;

        u32 progKart = raceProg + 0x14 + entryIdx * 0x8c;
        s32 status = RamReadS32(nds, progKart + 0x00);
        if (status != 0)
        {
            return PressKeys(0);
        }
        u16 progFlags = RamRead16(nds, progKart + 0x30); // bit 0x4 = facing against checkpoint (wrong-way)
        u16 cpIdx     = RamRead16(nds, progKart + 0x32); // current CourseMapCheckpoint index

        u32 kart = kartsPtr + entryIdx * 0x5a8;
        double kx = RamReadS32(nds, kart + 0x80) / 4096.0;
        double ky = RamReadS32(nds, kart + 0x84) / 4096.0;
        double kz = RamReadS32(nds, kart + 0x88) / 4096.0;

        // Row 0 (+0x120) = local Left, Row 1 (+0x12c) = local Up, Row 2 (+0x138) = local Forward in 3D
        double lx = RamReadS32(nds, kart + 0x120) / 4096.0;
        double ly = RamReadS32(nds, kart + 0x124) / 4096.0;
        double lz = RamReadS32(nds, kart + 0x128) / 4096.0;
        double ux = RamReadS32(nds, kart + 0x12c) / 4096.0;
        double uy = RamReadS32(nds, kart + 0x130) / 4096.0;
        double uz = RamReadS32(nds, kart + 0x134) / 4096.0;
        double fx = RamReadS32(nds, kart + 0x138) / 4096.0;
        double fy = RamReadS32(nds, kart + 0x13c) / 4096.0;
        double fz = RamReadS32(nds, kart + 0x140) / 4096.0;
        double fwdSpeed = RamReadS32(nds, kart + 0x2a8) / 4096.0;
        double velLen   = RamReadS32(nds, kart + 0x2a4) / 4096.0;
        s32 kartState   = RamReadS32(nds, kart + 0x180);
        const bool isInvertedOrClimbing = (uy < 0.42) || (fy > 0.42);

        u32 linksBase   = RamRead32(nds, courseMap + 0xa0);
        u32 route0First = RamRead32(nds, courseMap + 0xa4);
        u16 pointCount  = RamRead16(nds, courseMap + 0x68);
        if (!IsValidMainRamPtr(linksBase) || pointCount == 0 || pointCount > 512)
        {
            return PressKeys(KEY_A);
        }

        auto getLinkPos = [&](u32 linkAddr, double& px, double& py, double& pz, double& rad) -> bool {
            if (!IsValidMainRamPtr(linkAddr)) return false;
            u32 ptAddr = RamRead32(nds, linkAddr + 0x18);
            if (!IsValidMainRamPtr(ptAddr)) return false;
            px = RamReadS32(nds, ptAddr + 0x00) / 4096.0;
            py = RamReadS32(nds, ptAddr + 0x04) / 4096.0;
            pz = RamReadS32(nds, ptAddr + 0x08) / 4096.0;
            rad = std::max(110.0, RamReadS32(nds, ptAddr + 0x0c) / 4096.0);
            return true;
        };

        auto dist3 = [](double x1, double y1, double z1, double x2, double y2, double z2) -> double {
            double dx = x1 - x2, dy = y1 - y2, dz = z1 - z2;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };

        auto segProject3D = [&](double ax, double ay, double az, double bx, double by, double bz,
                                double& outT, double& outDist3D, double& outProjY) {
            double vx = bx - ax, vy = by - ay, vz = bz - az;
            double len2 = vx * vx + vy * vy + vz * vz;
            outT = (len2 > 1.0)
                 ? std::clamp(((kx - ax) * vx + (ky - ay) * vy + (kz - az) * vz) / len2, 0.0, 1.0)
                 : 1.0;
            double px = ax + vx * outT;
            double py = ay + vy * outT;
            double pz = az + vz * outT;
            outProjY = py;
            outDist3D = dist3(kx, ky, kz, px, py, pz);
        };

        // 1. Initialize at race start to MKDS's exact gCourseMap->mRaceRoute0FirstLink (same as InitRacePointLinkCursor)
        double tx = 0, ty = 0, tz = 0, trad = 200.0;
        double ax = 0, ay = 0, az = 0, arad = 200.0;
        if (currentLinkAddr == 0 || !getLinkPos(currentLinkAddr, tx, ty, tz, trad))
        {
            currentLinkAddr = IsValidMainRamPtr(route0First) ? route0First : linksBase;
            if (!getLinkPos(currentLinkAddr, tx, ty, tz, trad))
            {
                return PressKeys(KEY_A);
            }
            framesSinceLinkAdvance = 0;
        }

        // Unstick recovery if wedged against wall/bumper (keeps currentLinkAddr; never globally re-snaps!)
        if (reverseTimer > 0)
        {
            reverseTimer--;
            u32 revKeys = KEY_B | (lastSteerDir > 0 ? KEY_RIGHT : KEY_LEFT);
            return PressKeys(revKeys);
        }

        {
            u32 prev0 = RamRead32(nds, currentLinkAddr + 0x0c);
            if (!getLinkPos(prev0, ax, ay, az, arad))
            {
                ax = tx; ay = ty; az = tz;
            }
            double segT = 0.0, dSeg3D = 0.0, projY = ty;
            segProject3D(ax, ay, az, tx, ty, tz, segT, dSeg3D, projY);

            // Cannon flight immunity (Airship Fortress / Waluigi Pinball / DK Pass): hold straight while airborne at cannon speed
            if (!isInvertedOrClimbing && fwdSpeed > 10.5 && std::abs(ky - projY) > 400.0)
            {
                framesSinceLinkAdvance = 0;
                stuckFrames = 0;
                return PressKeys(KEY_A);
            }

            // 2. Re-acquire ONLY on cannon landing or Lakitu respawn (dSeg3D > 1350), strictly constrained to forward route / checkpoint!
            if (dSeg3D > 1350.0)
            {
                u32 bestLink = 0;
                double bestScore = 1e18;

                // First: walk forward along the RacePointLink chain (handles cannon landings & shortcuts cleanly)
                u32 walk = currentLinkAddr;
                for (int step = 0; step < 56 && IsValidMainRamPtr(walk); ++step)
                {
                    double bx, by, bz, brad;
                    if (!getLinkPos(walk, bx, by, bz, brad)) break;
                    u32 p0 = RamRead32(nds, walk + 0x0c);
                    double pax = bx, pay = by, paz = bz, parad = brad;
                    getLinkPos(p0, pax, pay, paz, parad);
                    double st = 0.0, sd = 0.0, py = by;
                    segProject3D(pax, pay, paz, bx, by, bz, st, sd, py);
                    double dy = std::abs(ky - py);
                    double sc = sd + 1.8 * dy;
                    if (sd < 600.0 && dy < 380.0 && sc < bestScore)
                    {
                        bestScore = sc;
                        bestLink = walk;
                    }
                    walk = RamRead32(nds, walk + 0x00);
                }

                // Second: if Lakitu respawned the kart behind currentLinkAddr, use MKDS's exact CourseMapRespawn for cpIdx!
                if (bestLink == 0)
                {
                    u32 checkpointsBase  = RamRead32(nds, courseMap + 0x44);
                    u16 cpCount          = RamRead16(nds, courseMap + 0x48);
                    u32 respawnsBase     = RamRead32(nds, courseMap + 0x24);
                    u16 respawnCount     = RamRead16(nds, courseMap + 0x28);
                    u32 respawnIdxBySlot = RamRead32(nds, courseMap + 0xf8);

                    double cpDirX = fx, cpDirZ = fz;
                    if (IsValidMainRamPtr(checkpointsBase) && cpIdx < cpCount)
                    {
                        u32 cpAddr = checkpointsBase + cpIdx * 0x24;
                        cpDirX = RamReadS32(nds, cpAddr + 0x10) / 4096.0;
                        cpDirZ = RamReadS32(nds, cpAddr + 0x14) / 4096.0;
                        u8 respawnSlot = RamRead8(nds, cpAddr + 0x22);
                        if (IsValidMainRamPtr(respawnsBase) && IsValidMainRamPtr(respawnIdxBySlot) && respawnCount > 0)
                        {
                            u16 rIdx = RamRead16(nds, respawnIdxBySlot + respawnSlot * 2);
                            if (rIdx < respawnCount)
                            {
                                u16 ptIdx = RamRead16(nds, respawnsBase + rIdx * 0x20 + 0x18);
                                if (ptIdx < pointCount)
                                {
                                    u32 rWalk = linksBase + ptIdx * 0x28;
                                    for (int step = 0; step < 20 && IsValidMainRamPtr(rWalk); ++step)
                                    {
                                        double bx, by, bz, brad;
                                        if (!getLinkPos(rWalk, bx, by, bz, brad)) break;
                                        u32 p0 = RamRead32(nds, rWalk + 0x0c);
                                        double pax = bx, pay = by, paz = bz, parad = brad;
                                        getLinkPos(p0, pax, pay, paz, parad);
                                        double st = 0.0, sd = 0.0, py = by;
                                        segProject3D(pax, pay, paz, bx, by, bz, st, sd, py);
                                        double sc = sd + 1.8 * std::abs(ky - py);
                                        if (sc < bestScore)
                                        {
                                            bestScore = sc;
                                            bestLink = rWalk;
                                        }
                                        rWalk = RamRead32(nds, rWalk + 0x00);
                                    }
                                }
                            }
                        }
                    }

                    // Final safety fallback: match checkpoint forward direction (NEVER kart's spun nose!)
                    if (bestLink == 0)
                    {
                        for (u16 i = 0; i < pointCount; ++i)
                        {
                            u32 cand = linksBase + i * 0x28;
                            double bx, by, bz, brad;
                            if (!getLinkPos(cand, bx, by, bz, brad)) continue;
                            u32 p0 = RamRead32(nds, cand + 0x0c);
                            double pax = bx, pay = by, paz = bz, parad = brad;
                            getLinkPos(p0, pax, pay, paz, parad);
                            double st = 0.0, sd = 0.0, py = by;
                            segProject3D(pax, pay, paz, bx, by, bz, st, sd, py);
                            double svx = bx - pax, svz = bz - paz;
                            double slen2D = std::sqrt(svx * svx + svz * svz);
                            double dotCp = (slen2D > 1.0) ? ((svx * cpDirX + svz * cpDirZ) / slen2D) : 1.0;
                            double penalty = (dotCp < -0.10) ? 5000.0 : 0.0;
                            double sc = sd + 2.2 * std::abs(ky - py) + penalty;
                            if (sc < bestScore)
                            {
                                bestScore = sc;
                                bestLink = cand;
                            }
                        }
                    }
                }

                if (IsValidMainRamPtr(bestLink))
                {
                    currentLinkAddr = bestLink;
                    getLinkPos(currentLinkAddr, tx, ty, tz, trad);
                    framesSinceLinkAdvance = 0;
                }
            }
        }

        // 3. Advance along 3D RacePointLink chain (handles flat corners, overpasses, corkscrews, and Rainbow Road's 360-deg vertical loop!)
        for (int step = 0; step < 3; ++step)
        {
            u32 next0 = RamRead32(nds, currentLinkAddr + 0x00);
            if (!IsValidMainRamPtr(next0)) break;
            double nx, ny, nz, nrad;
            if (!getLinkPos(next0, nx, ny, nz, nrad)) break;

            u32 prev0 = RamRead32(nds, currentLinkAddr + 0x0c);
            if (!getLinkPos(prev0, ax, ay, az, arad))
            {
                ax = kx; ay = ky; az = kz;
            }

            double abX = tx - ax, abY = ty - ay, abZ = tz - az;
            double bcX = nx - tx, bcY = ny - ty, bcZ = nz - tz;
            double abDotU = abX * ux + abY * uy + abZ * uz;
            double bcDotU = bcX * ux + bcY * uy + bcZ * uz;
            double abpX = abX - abDotU * ux, abpY = abY - abDotU * uy, abpZ = abZ - abDotU * uz;
            double bcpX = bcX - bcDotU * ux, bcpY = bcY - bcDotU * uy, bcpZ = bcZ - bcDotU * uz;
            double abLen = std::sqrt(abpX * abpX + abpY * abpY + abpZ * abpZ);
            double bcLen = std::sqrt(bcpX * bcpX + bcpY * bcpY + bcpZ * bcpZ);
            double cosTurn = (abLen > 1.0 && bcLen > 1.0)
                           ? ((abpX * bcpX + abpY * bcpY + abpZ * bcpZ) / (abLen * bcLen))
                           : 1.0;

            double distToB = dist3(kx, ky, kz, tx, ty, tz);
            double passedDot = (kx - tx) * bcX + (ky - ty) * bcY + (kz - tz) * bcZ;
            double nextT = 0.0, distNextSeg = 1e9, nextProjY = ny;
            segProject3D(tx, ty, tz, nx, ny, nz, nextT, distNextSeg, nextProjY);

            double reach = (cosTurn < 0.96 && !isInvertedOrClimbing)
                         ? 92.0
                         : std::clamp(trad * 0.55, 145.0, 260.0);
            if (distToB < reach || (distToB < 340.0 && passedDot > 0.0 && distNextSeg < 140.0))
            {
                currentLinkAddr = next0;
                tx = nx; ty = ny; tz = nz; trad = nrad;
                framesSinceLinkAdvance = 0;
            }
            else
            {
                break;
            }
        }

        // 4. 3D Pure-Pursuit centerline target along A -> B
        u32 prev0 = RamRead32(nds, currentLinkAddr + 0x0c);
        if (!getLinkPos(prev0, ax, ay, az, arad))
        {
            ax = tx; ay = ty; az = tz;
        }
        u32 nextLink = RamRead32(nds, currentLinkAddr + 0x00);
        double nx = tx, ny = ty, nz = tz, nrad = trad;
        double cosCorner = 1.0;
        bool isApproachingClimb = false;
        if (IsValidMainRamPtr(nextLink) && getLinkPos(nextLink, nx, ny, nz, nrad))
        {
            double abX = tx - ax, abY = ty - ay, abZ = tz - az;
            double bcX = nx - tx, bcY = ny - ty, bcZ = nz - tz;
            double bc3DL = std::sqrt(bcX * bcX + bcY * bcY + bcZ * bcZ);
            if (bc3DL > 1.0 && (bcY / bc3DL) > 0.16)
                isApproachingClimb = true;
            u32 next2Link = RamRead32(nds, nextLink + 0x00);
            double n2x = nx, n2y = ny, n2z = nz, n2rad = nrad;
            if (IsValidMainRamPtr(next2Link) && getLinkPos(next2Link, n2x, n2y, n2z, n2rad))
            {
                double cdX = n2x - nx, cdY = n2y - ny, cdZ = n2z - nz;
                double cd3DL = std::sqrt(cdX * cdX + cdY * cdY + cdZ * cdZ);
                if (cd3DL > 1.0 && (cdY / cd3DL) > 0.20)
                    isApproachingClimb = true;
            }
            double abDotU = abX * ux + abY * uy + abZ * uz;
            double bcDotU = bcX * ux + bcY * uy + bcZ * uz;
            double abpX = abX - abDotU * ux, abpY = abY - abDotU * uy, abpZ = abZ - abDotU * uz;
            double bcpX = bcX - bcDotU * ux, bcpY = bcY - bcDotU * uy, bcpZ = bcZ - bcDotU * uz;
            double abL = std::sqrt(abpX * abpX + abpY * abpY + abpZ * abpZ);
            double bcL = std::sqrt(bcpX * bcpX + bcpY * bcpY + bcpZ * bcpZ);
            if (abL > 1.0 && bcL > 1.0)
                cosCorner = (abpX * bcpX + abpY * bcpY + abpZ * bcpZ) / (abL * bcL);
        }

        double segT = 0.0, dSeg3D = 0.0, projY = ty;
        segProject3D(ax, ay, az, tx, ty, tz, segT, dSeg3D, projY);
        double segLen = dist3(ax, ay, az, tx, ty, tz);
        // Tight lookahead on curves/unguarded edges so kart hugs exact road centerline instead of cutting inside chords or drifting wide!
        double lookahead = (!isInvertedOrClimbing && !isApproachingClimb && (cosCorner < 0.95 || dSeg3D > 95.0))
                         ? 90.0
                         : std::clamp(135.0 + std::max(0.0, fwdSpeed) * 7.5, 135.0, 220.0);
        double tAim = (segLen > 1.0) ? std::min(1.0, segT + lookahead / segLen) : 1.0;
        double aimX = ax + (tx - ax) * tAim;
        double aimY = ay + (ty - ay) * tAim;
        double aimZ = az + (tz - az) * tAim;

        double distToB = dist3(kx, ky, kz, tx, ty, tz);
        if (cosCorner > 0.96 && distToB < 150.0)
        {
            double w = std::clamp((1.0 - distToB / 150.0) * 0.15, 0.0, 0.15);
            aimX = aimX * (1.0 - w) + nx * w;
            aimY = aimY * (1.0 - w) + ny * w;
            aimZ = aimZ * (1.0 - w) + nz * w;
        }

        // Project 3D target vector into kart's local wheel tangent plane (orthogonal to local Up u)
        double rx = aimX - kx, ry = aimY - ky, rz = aimZ - kz;
        double dotU = rx * ux + ry * uy + rz * uz;
        double tx3 = rx - dotU * ux;
        double ty3 = ry - dotU * uy;
        double tz3 = rz - dotU * uz;
        double len = std::sqrt(tx3 * tx3 + ty3 * ty3 + tz3 * tz3);
        if (len < 1e-3) return PressKeys(KEY_A);
        tx3 /= len;
        ty3 /= len;
        tz3 /= len;

        double projFwd  = tx3 * fx + ty3 * fy + tz3 * fz;
        double projLeft = tx3 * lx + ty3 * ly + tz3 * lz;

        // 5. Zero-radius in-place spin-turn (TryTurnKartAtLowSpeed: KEY_A | KEY_B | LEFT/RIGHT) if ever spun backwards!
        if (!isInvertedOrClimbing && (projFwd < -0.15 || ((progFlags & 0x4) != 0 && projFwd < 0.30)))
        {
            stuckFrames = 0;
            u32 turnKey = (projLeft >= 0.0) ? KEY_LEFT : KEY_RIGHT;
            lastSteerDir = (turnKey == KEY_LEFT) ? 1 : -1;
            if (velLen >= 1.45 && fwdSpeed > 0.5)
            {
                return PressKeys(KEY_B | turnKey);
            }
            return PressKeys(KEY_A | KEY_B | turnKey);
        }

        if (!isInvertedOrClimbing)
            framesSinceLinkAdvance++;
        if (kartState == 0 && !isInvertedOrClimbing && std::abs(fwdSpeed) < 0.85)
            stuckFrames++;
        else
            stuckFrames = 0;

        if (stuckFrames > 42 || (framesSinceLinkAdvance > 300 && fwdSpeed < 2.0))
        {
            stuckFrames = 0;
            framesSinceLinkAdvance = 0;
            reverseTimer = 26;
            return PressKeys(KEY_B);
        }

        u32 keys = KEY_A;
        if (projLeft > 0.048 || (projFwd < 0.0 && projLeft >= 0.0))
        {
            keys |= KEY_LEFT;
            lastSteerDir = 1;
        }
        else if (projLeft < -0.048 || (projFwd < 0.0 && projLeft < 0.0))
        {
            keys |= KEY_RIGHT;
            lastSteerDir = -1;
        }

        if ((RamReadS32(nds, raceProg + 0x00) % 90) == 45)
        {
            keys |= KEY_X;
        }

        double distToCorner = dist3(kx, ky, kz, tx, ty, tz);
        if (!isInvertedOrClimbing && !isApproachingClimb &&
            ((cosCorner < 0.94 && distToCorner < 360.0 && fwdSpeed > 4.6) ||
             (cosCorner < 0.85 && distToCorner < 460.0 && fwdSpeed > 3.7) ||
             (std::abs(projLeft) > 0.18 && fwdSpeed > 4.1) ||
             (projFwd < 0.70 && fwdSpeed > 3.8)))
        {
            keys &= ~KEY_A;
            if (fwdSpeed > 5.2 || (cosCorner < 0.85 && fwdSpeed > 4.3) || (projFwd < 0.20 && fwdSpeed > 2.8))
                keys |= KEY_B;
        }

        return PressKeys(keys);
    }
};

struct Config
{
    std::string hostRomPath;
    std::string guestRomPath; // Only set for normal-vs multi-card regression tests!
    std::string bios9Path;
    std::string bios7Path;
    std::string firmwarePath;
    std::string hostSymPath;
    std::string guestSymPath;
    std::string saveFilePath;
    std::string saveLobbyPrefix;
    std::string loadLobbyPrefix;
    std::string outDir = "out";
    std::string traceJsonlPath;
    std::string mode = "simple-vs"; // simple-vs, simple-battle, normal-vs, single-gp, single-tt, station-probe, station-mkds-guest
    int numGuests = 1;
    int guestTapDelay = 0;  // Extra per-guest frame stagger before pressing A in Station menu
    int courseListMode = 0; // 0 = Choose, 1 = In Order (Sequential), 2 = Random
    int cpuMode = -1;       // -1 = Keep game default, 0 = Off, 1 = Easy, 2 = Normal, 3 = Hard
    int targetCup = 0;      // 0..7
    int targetCourseInCup = 0; // 0..3
    int targetRaces = 1;    // Number of full races to complete before Next Race check
    int maxFrames = 18000;
    int wallTimeoutSec = 120;
    int mpRecvTimeoutMs = 2;
    bool checkNextRace = true;
    bool verbose = false;
};

static std::vector<u8> ReadBinaryFile(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
    {
        std::cerr << "ERROR: Failed to open file: " << path << "\n";
        std::exit(2);
    }
    std::streamsize sz = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::vector<u8> buf(static_cast<std::size_t>(sz));
    if (sz > 0 && !ifs.read(reinterpret_cast<char*>(buf.data()), sz))
    {
        std::cerr << "ERROR: Failed to read file: " << path << "\n";
        std::exit(2);
    }
    return buf;
}

} // namespace

int main(int argc, char** argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto nextStr = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << arg << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (arg == "--host-rom") cfg.hostRomPath = nextStr();
        else if (arg == "--guest-rom") cfg.guestRomPath = nextStr();
        else if (arg == "--bios9") cfg.bios9Path = nextStr();
        else if (arg == "--bios7") cfg.bios7Path = nextStr();
        else if (arg == "--firmware") cfg.firmwarePath = nextStr();
        else if (arg == "--host-sym") cfg.hostSymPath = nextStr();
        else if (arg == "--guest-sym") cfg.guestSymPath = nextStr();
        else if (arg == "--save-file") cfg.saveFilePath = nextStr();
        else if (arg == "--save-lobby-state") cfg.saveLobbyPrefix = nextStr();
        else if (arg == "--load-lobby-state") cfg.loadLobbyPrefix = nextStr();
        else if (arg == "--out-dir") cfg.outDir = nextStr();
        else if (arg == "--trace-jsonl") cfg.traceJsonlPath = nextStr();
        else if (arg == "--mode") cfg.mode = nextStr();
        else if (arg == "--guests") cfg.numGuests = std::stoi(nextStr());
        else if (arg == "--guest-tap-delay") cfg.guestTapDelay = std::stoi(nextStr());
        else if (arg == "--course-list-mode") cfg.courseListMode = std::stoi(nextStr());
        else if (arg == "--cpu") cfg.cpuMode = std::stoi(nextStr());
        else if (arg == "--cup") cfg.targetCup = std::stoi(nextStr());
        else if (arg == "--course-in-cup") cfg.targetCourseInCup = std::stoi(nextStr());
        else if (arg == "--races") cfg.targetRaces = std::stoi(nextStr());
        else if (arg == "--max-frames") cfg.maxFrames = std::stoi(nextStr());
        else if (arg == "--wall-timeout") cfg.wallTimeoutSec = std::stoi(nextStr());
        else if (arg == "--mp-timeout") cfg.mpRecvTimeoutMs = std::stoi(nextStr());
        else if (arg == "--no-next-race") cfg.checkNextRace = false;
        else if (arg == "--verbose") cfg.verbose = true;
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    if (cfg.hostRomPath.empty() || cfg.bios9Path.empty() || cfg.bios7Path.empty() || cfg.firmwarePath.empty())
    {
        std::cerr << "Usage: dlp_harness --host-rom <rom.nds> --bios9 <bios9> --bios7 <bios7> --firmware <fw> [options]\n";
        return 2;
    }

    std::filesystem::create_directories(cfg.outDir);
    if (!cfg.saveFilePath.empty())
    {
        gHostSaveOutPath = cfg.saveFilePath;
    }

    const bool isSinglePlayer = (cfg.mode == "single-gp" || cfg.mode == "single-tt");
    const bool isMultiCard = (cfg.mode == "normal-vs");
    const bool isBattle = (cfg.mode == "simple-battle");
    const bool isStationProbe = (cfg.mode == "station-probe");
    const bool isStationMkdsGuest = (cfg.mode == "station-mkds-guest");
    const bool isStandaloneStation = isStationProbe || isStationMkdsGuest;
    const int numConsoles = isSinglePlayer ? 1 : (1 + cfg.numGuests);

    MkdsSymbols hostSym;
    MkdsSymbols guestSym;
    if (!cfg.hostSymPath.empty()) hostSym.LoadFromSymbolsTxt(cfg.hostSymPath);
    if (!cfg.guestSymPath.empty()) guestSym.LoadFromSymbolsTxt(cfg.guestSymPath);

    auto bios9Bytes = ReadBinaryFile(cfg.bios9Path);
    auto bios7Bytes = ReadBinaryFile(cfg.bios7Path);
    auto fwBytes    = ReadBinaryFile(cfg.firmwarePath);
    auto hostRom    = ReadBinaryFile(cfg.hostRomPath);
    std::vector<u8> guestRom;
    if (isMultiCard)
    {
        guestRom = ReadBinaryFile(cfg.guestRomPath.empty() ? cfg.hostRomPath : cfg.guestRomPath);
    }

    if (bios9Bytes.size() != melonDS::ARM9BIOSSize || bios7Bytes.size() != melonDS::ARM7BIOSSize)
    {
        std::cerr << "ERROR: Invalid BIOS sizes.\n";
        return 2;
    }

    Harness_InitLocalMP(cfg.mpRecvTimeoutMs, cfg.verbose);

    std::vector<int> instIds(numConsoles);
    std::vector<std::unique_ptr<HarnessNDS>> consoles(numConsoles);

    for (int c = 0; c < numConsoles; ++c)
    {
        instIds[c] = c;
        melonDS::NDSArgs ndsArgs;
        ndsArgs.ARM9BIOS = std::make_unique<melonDS::ARM9BIOSImage>();
        std::memcpy(ndsArgs.ARM9BIOS->data(), bios9Bytes.data(), melonDS::ARM9BIOSSize);
        ndsArgs.ARM7BIOS = std::make_unique<melonDS::ARM7BIOSImage>();
        std::memcpy(ndsArgs.ARM7BIOS->data(), bios7Bytes.data(), melonDS::ARM7BIOSSize);

        melonDS::Firmware fw(fwBytes.data(), static_cast<u32>(fwBytes.size()));
        fw.GetHeader().MacAddr = {0x00, 0x09, 0xBF, 0x11, 0x22, static_cast<u8>(0x33 + c)};
        for (auto& ud : fw.GetUserData())
        {
            std::memset(ud.Nickname, 0, sizeof(ud.Nickname));
            std::string nick = (c == 0) ? "HostDS" : ("Guest" + std::to_string(c));
            for (std::size_t k = 0; k < nick.size(); ++k) ud.Nickname[k] = static_cast<char16_t>(nick[k]);
            ud.NameLength = static_cast<u16>(nick.size());
            // Preserve retail FW600DF0.BIN configured flags (0xFC00), clearing only bit 9 (Settings Lost) & AutoStart
            u16 baseSettings = (ud.Settings & ~0x0247u) |
                               static_cast<u16>(melonDS::Firmware::Language::English);
            if (c == 0 || isMultiCard)
                ud.Settings = baseSettings | static_cast<u16>(melonDS::Firmware::BootMenu::Autostart);
            else
                ud.Settings = baseSettings;
            ud.UpdateChecksum();
        }
        fw.UpdateChecksums();
        ndsArgs.Firmware = std::move(fw);
        ndsArgs.JIT = std::nullopt;

        consoles[c] = std::make_unique<HarnessNDS>(std::move(ndsArgs), &instIds[c]);

        if (c == 0)
        {
            auto cart = melonDS::NDSCart::ParseROM(hostRom.data(), static_cast<u32>(hostRom.size()), &instIds[c]);
            if (!cart) { std::cerr << "ERROR: Failed to parse Host ROM.\n"; return 2; }
            consoles[c]->SetNDSCart(std::move(cart));
            if (!cfg.saveFilePath.empty())
            {
                if (std::filesystem::exists(cfg.saveFilePath))
                {
                    auto sav = ReadBinaryFile(cfg.saveFilePath);
                    if (!sav.empty()) consoles[c]->SetNDSSave(sav.data(), static_cast<u32>(sav.size()));
                }
                else
                {
                    std::vector<u8> blankSav(0x40000, 0xFF); // 256 KiB EEPROM for MKDS save + ghost data
                    consoles[c]->SetNDSSave(blankSav.data(), static_cast<u32>(blankSav.size()));
                }
            }
        }
        else if (isMultiCard)
        {
            auto cart = melonDS::NDSCart::ParseROM(guestRom.data(), static_cast<u32>(guestRom.size()), &instIds[c]);
            if (!cart) { std::cerr << "ERROR: Failed to parse Guest ROM.\n"; return 2; }
            consoles[c]->SetNDSCart(std::move(cart));
        }
        // Note: For Download Play guests (c >= 1 && !isMultiCard), NO cartridge is inserted!
        // They boot pure Nintendo DS BIOS + Firmware -> DS Download Play -> RSA verification!

        consoles[c]->Reset();
        consoles[c]->RTC.SetDateTime(2026, 11, 15, 12, 0, 0);
        consoles[c]->Start();
    }

    std::vector<melonDS::NDS*> rawPtrs(numConsoles);
    for (int c = 0; c < numConsoles; ++c) rawPtrs[c] = consoles[c].get();
    Harness_RegisterConsoles(numConsoles, rawPtrs.data());

    // Optional savestate restore for fast branching from Simple-mode lobby
    bool loadedFromLobbyState = false;
    if (!cfg.loadLobbyPrefix.empty())
    {
        loadedFromLobbyState = true;
        for (int c = 0; c < numConsoles; ++c)
        {
            std::string ssPath = cfg.loadLobbyPrefix + "_inst" + std::to_string(c) + ".mln";
            auto ssBytes = ReadBinaryFile(ssPath);
            melonDS::Savestate ss(ssBytes.data(), static_cast<u32>(ssBytes.size()), false);
            if (!consoles[c]->DoSavestate(&ss) || ss.Error)
            {
                std::cerr << "ERROR: Failed to restore savestate " << ssPath << "\n";
                return 2;
            }
            if (!isSinglePlayer)
            {
                melonDS::Platform::MP_Begin(&instIds[c]);
            }
        }
        if (!isSinglePlayer)
        {
            Harness_SetStage2Active(true);
        }
    }

    // Expected internal course ID from Host ROM's gCupCourseIdTable
    u32 expectedCourseId = 0;
    std::array<std::array<u32, 4>, 8> cupTable {};

    // Telemetry & milestone tracking
    bool firstStageRsaPass = isSinglePlayer || isMultiCard || loadedFromLobbyState;
    bool stage2LaunchPass = isSinglePlayer || isMultiCard || loadedFromLobbyState;
    bool nativeJoinPass = isSinglePlayer || loadedFromLobbyState;
    bool bootDownloadPass = isSinglePlayer || loadedFromLobbyState;
    bool lobbySelectionPass = false;
    bool assetTransferPass = false;
    bool raceCountdownPass = false;
    bool fullRaceResultsPass = false;
    bool nextRaceTransitionPass = !cfg.checkNextRace;

    std::string hostClonebootSha256;
    std::string hostClonebootRsaSha256;
    std::string guestStage1HeaderSha256;
    std::string guestHeaderSha256;
    u16 guestBootType = 0;
    u32 probeMagic = 0;
    u32 probeStage1Ggid = 0;
    u16 probeStage1Tgid = 0;
    u8  probeStage1Chan = 0;
    u32 probeArm7Ticks = 0;
    u32 probeArm9Ticks = 0;
    bool guestDownloadedFlagOk = false;
    bool guestSimpleFlagOk = false;
    u32 guestObservedGgid = 0;
    u16 guestObservedTgid = 0;
    u8  guestObservedChan = 0;
    int mkdsGuestStableFrames = 0;
    std::vector<int> stationStage1StartFrame(numConsoles, -1);
    std::vector<int> firmwareDownloadFrames(numConsoles, 0);

    s32 hostSelectedCourseId = -1;
    s32 hostSetupCourseId = -1;
    s32 hostTransmittedCourseId = -1;
    s32 guestReceivedCourseId = -1;
    std::array<s32, 8> cupsEnabledSnapshot {};
    std::array<s32, 8> guestCupsEnabledSnapshot {};
    std::vector<int> courseListSnapshot;
    std::vector<int> guestCourseListSnapshot;
    std::vector<int> observedRaceCourseIds;
    u32 minGuestCourseHeapFree = 0xFFFFFFFFu;
    u32 guestCourseBuf6Size = 0;
    u32 guestCourseBuf7Size = 0;
    int completedRaces = 0;
    int raceDoneFrames = 0;
    bool inRaceNow = false;
    bool sawResultsForCurrentRace = false;
    bool savedLobbyStateDone = false;
    bool rulesConfigured = false;
    int selectCupCooldown = 0;
    int selectCourseCooldown = 0;
    int ruleCooldown = 0;
    int memberReadyFrames = 0;
    int desyncFrames = -1;
    std::string firstErrorStr;

    std::ofstream telemetry(cfg.outDir + "/telemetry.log");
    const std::string traceOutPath = cfg.traceJsonlPath.empty() ? (cfg.outDir + "/phase_trace.jsonl") : cfg.traceJsonlPath;
    std::ofstream traceOfs(traceOutPath);

    auto emitPhaseTrace = [&](int frame, int cIdx, const std::string& stageName, const std::string& errStr = "") {
        if (!traceOfs) return;
        const auto& nds = *consoles[cIdx];
        const auto& sym = (cIdx == 0) ? hostSym : guestSym;
        std::string role = (cIdx == 0) ? "host" : ("guest_" + std::to_string(cIdx));
        u32 conn = (!isStandaloneStation || cIdx > 0) ? RamRead32(nds, sym.data_0217aa00) : 0;
        u16 bType = RamRead16(nds, 0x027ffc40);
        u32 ggid = 0;
        u16 tgid = 0;
        u8 chan = 0;
        u16 hMpVal = 0;
        int simpleFlag = 0;
        int aidVal = (cIdx == 0) ? 0 : cIdx;
        int assocGen = 0;
        u32 sendOff = 0;
        int sendStat = 0;
        int wmSt = 0;
        int mbSt = 0;
        if (cIdx > 0 && bType == 2)
        {
            ggid = u32(RamRead16(nds, 0x027ffc4e)) | (u32(RamRead16(nds, 0x027ffc50)) << 16);
            tgid = RamRead16(nds, 0x027ffc52);
            chan = RamRead8(nds, 0x027ffc78);
        }
        if (IsValidMainRamPtr(conn))
        {
            hMpVal = RamRead16(nds, conn + 0x1616);
            simpleFlag = (RamRead16(nds, conn + 0x454) & 0x40) ? 1 : 0;
            wmSt = RamReadS32(nds, conn + 0x1e34);
            if (cIdx == 0)
            {
                tgid = RamRead16(nds, conn + 0x1690);
                chan = RamRead8(nds, conn + 0x0088);
                ggid = 0x25;
            }
            else
            {
                u32 scanBlk = RamRead32(nds, conn + 0x24);
                if (IsValidMainRamPtr(scanBlk))
                {
                    u32 bssGgid = RamRead32(nds, scanBlk + 0x44);
                    if (bssGgid != 0) ggid = bssGgid;
                    u16 bssTgid = RamRead16(nds, scanBlk + 0x48);
                    if (bssTgid != 0) tgid = bssTgid;
                }
            }
        }
        if (cIdx == 0 && IsValidMainRamPtr(hostSym.gHaxxStationHost))
        {
            u32 hs = hostSym.gHaxxStationHost;
            u32 hsPhase = RamRead32(nds, hs + 0x04);
            if (hsPhase != 0)
            {
                mbSt = static_cast<int>(hsPhase);
                ggid = RamRead32(nds, hs + 0x08);
                tgid = static_cast<u16>(RamRead32(nds, hs + 0x0c) & 0xFFFFu);
                chan = static_cast<u8>(RamRead32(nds, hs + 0x10) & 0xFFu);
                assocGen = static_cast<int>(RamRead32(nds, hs + 0x18));
                sendOff = RamRead32(nds, hs + 0x1c);
                sendStat = static_cast<int>(RamReadS32(nds, hs + 0x20));
            }
        }
        u8 macBuf[6] = {0x00, 0x09, 0xBF, 0x11, 0x22, static_cast<u8>(0x33 + cIdx)};
        std::string macHash = Sha256::Hex(macBuf, 6).substr(0, 16);
        std::string listSha = "none";
        u32 rs = (!isStandaloneStation || cIdx > 0) ? RamRead32(nds, sym.gRaceSetup) : 0;
        s32 cid = -1;
        if (IsValidMainRamPtr(rs))
        {
            cid = RamReadS32(nds, rs + 0x00);
            u32 lp = RamRead32(nds, rs + 0x3dc);
            if (IsValidMainRamPtr(lp))
                listSha = Sha256::Hex(&nds.MainRAM[lp & nds.MainRAMMask], 33);
        }
        int pendFiles = 0;
        if (!isStandaloneStation || cIdx > 0)
        {
            u32 pFile = RamRead32(nds, sym.gRomArchiveTransfer + 0x1c);
            if (IsValidMainRamPtr(pFile)) pendFiles = 1;
        }
        long long heapFreeOut = (cIdx > 0 && minGuestCourseHeapFree != 0xFFFFFFFFu)
                                    ? static_cast<long long>(minGuestCourseHeapFree)
                                    : -1;
        std::ostringstream hexGgid;
        hexGgid << "0x" << std::hex << std::setw(8) << std::setfill('0') << ggid;
        traceOfs << "{\"timestamp_or_frame\":" << frame
                 << ",\"platform\":\"melonDS-906e9ebb\""
                 << ",\"console_role\":\"" << role << "\""
                 << ",\"stage\":\"" << stageName << "\""
                 << ",\"aid\":" << aidVal
                 << ",\"association_generation\":" << assocGen
                 << ",\"peer_mac_hash\":\"" << macHash << "\""
                 << ",\"ggid\":\"" << hexGgid.str() << "\""
                 << ",\"tgid\":" << tgid
                 << ",\"channel\":" << int(chan)
                 << ",\"wm_state\":" << wmSt
                 << ",\"mb_state\":" << mbSt
                 << ",\"hMp\":" << hMpVal
                 << ",\"boot_type\":" << bType
                 << ",\"simple_mode_flag\":" << simpleFlag
                 << ",\"course_list_mode\":" << cfg.courseListMode
                 << ",\"course_id\":" << cid
                 << ",\"course_list_sha256\":\"" << listSha << "\""
                 << ",\"pending_files\":" << pendFiles
                 << ",\"course_heap_free\":" << heapFreeOut
                 << ",\"send_offset\":" << sendOff
                 << ",\"send_status\":" << sendStat
                 << ",\"first_error\":\"" << (errStr.empty() ? firstErrorStr : errStr) << "\"}\n";
        traceOfs.flush();
    };
    auto logEvent = [&](int frame, const std::string& msg) {
        telemetry << "[frame " << std::setw(6) << frame << "] " << msg << "\n";
        telemetry.flush();
        if (cfg.verbose)
        {
            std::cout << "[frame " << std::setw(6) << frame << "] " << msg << "\n";
        }
    };

    std::vector<KartAutopilot> autopilots(numConsoles);
    std::vector<u32> nextKeys(numConsoles, PressKeys(0));
    std::vector<std::optional<std::pair<u16, u16>>> desiredTouch(numConsoles, std::nullopt);

    auto setTouch = [&](int idx, std::optional<std::pair<u16, u16>> pt) {
        desiredTouch[idx] = pt;
    };

    std::vector<bool> sliceOpen(numConsoles, false);
    auto applyInputsToConsole = [&](int c) {
        consoles[c]->SetKeyMask(nextKeys[c]);
        if (desiredTouch[c].has_value())
            consoles[c]->TouchScreen(desiredTouch[c]->first, desiredTouch[c]->second);
        else
            consoles[c]->ReleaseScreen();
    };
    auto advanceGuestTo = [&](int c, u64 limitSys) {
        while (consoles[c]->GetSysTimestamp() < limitSys)
        {
            if (!sliceOpen[c])
            {
                applyInputsToConsole(c);
                consoles[c]->BeginFrameSlice();
                sliceOpen[c] = true;
            }
            u64 before = consoles[c]->GetSysTimestamp();
            consoles[c]->RunUntilSys(limitSys);
            if (consoles[c]->FrameSliceDone())
            {
                consoles[c]->EndFrameSlice();
                sliceOpen[c] = false;
            }
            else if (consoles[c]->GetSysTimestamp() >= limitSys || consoles[c]->GetSysTimestamp() == before)
            {
                break;
            }
        }
    };

    auto quiesceGuestsAndRun = [&](auto&& fn) {
        for (int c = 1; c < numConsoles; ++c)
        {
            if (sliceOpen[c])
            {
                consoles[c]->RunUntilSys(consoles[c]->GetFrameStartSys() + 600000ULL);
                consoles[c]->EndFrameSlice();
                sliceOpen[c] = false;
            }
        }
        melonDS::NDS::Current = consoles[0].get();
        fn();
    };

    Harness_SetPumpGuestsCallback([&]() -> bool {
        melonDS::NDS* savedCur = melonDS::NDS::Current;
        const u64 leadWindow = 360448ULL + (numConsoles > 2 ? 262144ULL * u64(numConsoles - 2) : 0ULL);
        const u64 maxLeadSys = consoles[0]->GetSysTimestamp() + leadWindow;
        bool progressed = false;
        for (int c = 1; c < numConsoles; ++c)
        {
            u64 cur = consoles[c]->GetSysTimestamp();
            if (cur < maxLeadSys)
            {
                advanceGuestTo(c, std::min<u64>(cur + 1024u, maxLeadSys));
                if (consoles[c]->GetSysTimestamp() > cur)
                    progressed = true;
            }
        }
        melonDS::NDS::Current = savedCur;
        return progressed;
    });

    auto stepHostAndGuests = [&](int /*frame*/) {
        for (int c = 0; c < numConsoles; ++c)
            applyInputsToConsole(c);

        consoles[0]->BeginFrameSlice();
        sliceOpen[0] = true;
        const u64 hostStartSys = consoles[0]->GetFrameStartSys();

        for (u64 relSys = 1024; !consoles[0]->FrameSliceDone(); relSys += 1024)
        {
            const u64 targetSys = hostStartSys + relSys;
            for (int c = 1; c < numConsoles; ++c)
                advanceGuestTo(c, targetSys);
            consoles[0]->RunUntilSys(targetSys);
        }

        consoles[0]->EndFrameSlice();
        sliceOpen[0] = false;
        const u64 hostEndSys = consoles[0]->GetSysTimestamp();
        for (int c = 1; c < numConsoles; ++c)
            advanceGuestTo(c, hostEndSys);

        melonDS::NDS::Current = consoles[0].get();
    };

    melonDS::NDS::Current = consoles[0].get();
    bool allDone = false;
    int finalFrame = 0;
    const auto wallStart = std::chrono::steady_clock::now();

    for (int frame = 0; frame < cfg.maxFrames; ++frame)
    {
        finalFrame = frame;
        if ((frame & 255) == 0)
        {
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - wallStart).count();
            if (elapsedSec >= cfg.wallTimeoutSec)
            {
                logEvent(frame, "Wallclock timeout (" + std::to_string(cfg.wallTimeoutSec) + "s) reached; stopping cleanly.");
                break;
            }
            if (frame > 0 && (frame % 2048 == 0))
            {
                std::cerr << "[dlp_harness] frame=" << frame << " elapsed=" << elapsedSec << "s\n";
            }
        }
        for (int c = 0; c < numConsoles; ++c)
        {
            nextKeys[c] = PressKeys(0);
            desiredTouch[c] = std::nullopt;
        }

        const auto& host = *consoles[0];

        // Read Host's gCupCourseIdTable once MKDS ARM9 is loaded into MainRAM
        if (expectedCourseId == 0 && RamRead32(host, 0x027ffe00) == 0x4952414Du) // "MARI" ("MARIOKARTDS")
        {
            u32 firstEntry = RamRead32(host, hostSym.gCupCourseIdTable);
            if (firstEntry == 20) // COURSE_ID_CROSS == 0x14 == 20
            {
                for (int cu = 0; cu < 8; ++cu)
                    for (int co = 0; co < 4; ++co)
                        cupTable[cu][co] = RamRead32(host, hostSym.gCupCourseIdTable + (cu * 4 + co) * 4);
                expectedCourseId = cupTable[cfg.targetCup][cfg.targetCourseInCup];
                logEvent(frame, "Host MKDS booted in RAM; target cup=" + std::to_string(cfg.targetCup) +
                                " courseInCup=" + std::to_string(cfg.targetCourseInCup) +
                                " expectedCourseId=" + std::to_string(expectedCourseId));
            }
        }

        // Check Stage-1 Station RSA boot ("NTRJ" = 0x4A52544E) and Stage-2 payload boot!
        if (numConsoles > 1 && !isMultiCard)
        {
            bool allInStationStage1 = true;
            for (int g = 1; g < numConsoles; ++g)
            {
                const auto& guest = *consoles[g];
                u16 bootType = RamRead16(guest, 0x027ffc40);
                u32 gameCode = RamRead32(guest, 0x027ffe0c);
                if (bootType == 2 && gameCode == 0x4A52544Eu) // "NTRJ" = signed DS Download Station client!
                {
                    if (stationStage1StartFrame[g] < 0)
                    {
                        stationStage1StartFrame[g] = frame;
                        logEvent(frame, "Guest " + std::to_string(g) + " passed DS Firmware RSA check & booted signed Station Stage-1 (NTRJ)!");
                        emitPhaseTrace(frame, g, "first_stage_RSA");
                    }
                }
                else if (stationStage1StartFrame[g] < 0)
                {
                    allInStationStage1 = false;
                }
            }
            if (allInStationStage1 && !firstStageRsaPass)
            {
                firstStageRsaPass = true;
                Harness_SetStage2Active(true);
                const auto& guest1 = *consoles[1];
                guestStage1HeaderSha256 = Sha256::Hex(&guest1.MainRAM[0x027ffe00 & guest1.MainRAMMask], 0x160);
                {
                    std::ofstream hofs(cfg.outDir + "/guest_stage1_header_027ffe00.bin", std::ios::binary);
                    hofs.write(reinterpret_cast<const char*>(&guest1.MainRAM[0x027ffe00 & guest1.MainRAMMask]), 0x160);
                }
                SaveCompositeScreenshot(cfg.outDir + "/01a_station_stage1_rsa_pass.png", consoles);
                emitPhaseTrace(frame, 0, "first_stage_RSA");
            }
        }

        // Standalone Gate M1 check: marker-probe.nds ("HAXP" magic at 0x023FF000 + ARM7 & ARM9 frame counters advancing)
        if (isStationProbe && numConsoles > 1 && !bootDownloadPass)
        {
            bool allProbesAlive = true;
            for (int g = 1; g < numConsoles; ++g)
            {
                const auto& guest = *consoles[g];
                u32 magic = RamRead32(guest, 0x023ff000);
                u32 a7mag = RamRead32(guest, 0x023ff010);
                u32 a7cnt = RamRead32(guest, 0x023ff014);
                u32 a9cnt = RamRead32(guest, 0x023ff020);
                if (magic != 0x50584148u || a7mag != 0x374D5241u || a7cnt < 15 || a9cnt < 15)
                    allProbesAlive = false;
            }
            if (allProbesAlive)
            {
                const auto& guest1 = *consoles[1];
                probeMagic      = RamRead32(guest1, 0x023ff000);
                guestBootType   = static_cast<u16>(RamRead32(guest1, 0x023ff004) & 0xFFFFu);
                probeStage1Ggid = RamRead32(guest1, 0x023ff008);
                u32 chanTgid    = RamRead32(guest1, 0x023ff00c);
                probeStage1Tgid = static_cast<u16>(chanTgid & 0xFFFFu);
                probeStage1Chan = static_cast<u8>((chanTgid >> 16) & 0xFFu);
                probeArm7Ticks  = RamRead32(guest1, 0x023ff014);
                probeArm9Ticks  = RamRead32(guest1, 0x023ff020);
                guestHeaderSha256 = Sha256::Hex(&guest1.MainRAM[0x027ffe00 & guest1.MainRAMMask], 0x160);

                stage2LaunchPass = true;
                bootDownloadPass = true;
                lobbySelectionPass = true;
                assetTransferPass = true;
                raceCountdownPass = true;
                fullRaceResultsPass = true;
                nextRaceTransitionPass = true;
                SaveCompositeScreenshot(cfg.outDir + "/02_probe_executed.png", consoles);
                logEvent(frame, "Gate M1 MKDS HAXX PROBE executed! bootType=" + std::to_string(guestBootType) +
                                " ggid=" + std::to_string(probeStage1Ggid) +
                                " tgid=" + std::to_string(probeStage1Tgid) +
                                " chan=" + std::to_string(probeStage1Chan) +
                                " arm7Ticks=" + std::to_string(probeArm7Ticks) +
                                " arm9Ticks=" + std::to_string(probeArm9Ticks));
                emitPhaseTrace(frame, 1, "stage2_launch");
                allDone = true;
            }
        }

        // Check if Download Play Guest(s) finished Firmware RSA check & booted MKDS child image!
        if (!isStationProbe && !bootDownloadPass && numConsoles > 1)
        {
            bool allGuestsBooted = true;
            for (int g = 1; g < numConsoles; ++g)
            {
                const auto& guest = *consoles[g];
                u32 hdr0 = RamRead32(guest, 0x027ffe00);
                u16 bootType = RamRead16(guest, 0x027ffc40);
                u32 connWork = RamRead32(guest, guestSym.data_0217aa00);
                if (isMultiCard)
                {
                    if (hdr0 != 0x4952414Du || !IsValidMainRamPtr(connWork))
                        allGuestsBooted = false;
                }
                else
                {
                    if (hdr0 != 0x4952414Du || bootType != 2 || !IsValidMainRamPtr(connWork))
                        allGuestsBooted = false;
                }
            }
            if (allGuestsBooted)
            {
                firstStageRsaPass = true;
                stage2LaunchPass = true;
                bootDownloadPass = true;
                Harness_SetStage2Active(true);
                const auto& guest1 = *consoles[1];
                guestBootType = RamRead16(guest1, 0x027ffc40);
                guestHeaderSha256 = Sha256::Hex(&guest1.MainRAM[0x027ffe00 & guest1.MainRAMMask], 0x160);
                // Save exact 0x160-byte downloaded header from Guest RAM for signed-client audit
                {
                    std::ofstream hofs(cfg.outDir + "/guest_ram_header_027ffe00.bin", std::ios::binary);
                    hofs.write(reinterpret_cast<const char*>(&guest1.MainRAM[0x027ffe00 & guest1.MainRAMMask]), 0x160);
                }
                SaveCompositeScreenshot(cfg.outDir + "/01_guest_dlp_booted.png", consoles);
                logEvent(frame, "Guest Download Play booted MKDS! bootType=" + std::to_string(guestBootType) +
                                " guestHeaderSha256=" + guestHeaderSha256);
                for (int g = 1; g < numConsoles; ++g)
                    emitPhaseTrace(frame, g, "stage2_launch");
            }
        }

        // Standalone Gate M2 check: verify modified MKDS Stage-2 guest runs cleanly for 180+ frames with valid child flags & parent GGID 0x25!
        if (isStationMkdsGuest && bootDownloadPass && !allDone)
        {
            const auto& guest1 = *consoles[1];
            u32 gConn = RamRead32(guest1, guestSym.data_0217aa00);
            u32 gRole = RamRead32(guest1, guestSym.data_021759d0);
            u32 exPc  = IsValidMainRamPtr(guestSym.OSi_ExContext) ? RamRead32(guest1, guestSym.OSi_ExContext + 0x40) : 0;
            if (IsValidMainRamPtr(gConn) && IsValidMainRamPtr(gRole))
            {
                guestDownloadedFlagOk = (RamRead8(guest1, gRole + 0x1c) & 0x04u) != 0;
                guestSimpleFlagOk     = (RamRead16(guest1, gConn + 0x454) & 0x40u) != 0;
                u32 scanBlk           = RamRead32(guest1, gConn + 0x24);
                if (IsValidMainRamPtr(scanBlk) && RamRead32(guest1, scanBlk + 0x44) != 0)
                {
                    guestObservedGgid = RamRead32(guest1, scanBlk + 0x44);
                    guestObservedTgid = RamRead16(guest1, scanBlk + 0x48);
                }
                else
                {
                    guestObservedGgid = u32(RamRead16(guest1, 0x027ffc4e)) | (u32(RamRead16(guest1, 0x027ffc50)) << 16);
                    guestObservedTgid = RamRead16(guest1, 0x027ffc52);
                }
                guestObservedChan     = RamRead8(guest1, 0x027ffc78);
            }
            if (exPc != 0)
            {
                firstErrorStr = "guest_arm9_exception";
                logEvent(frame, "ERROR: Guest ARM9 exception at pc=0x" + std::to_string(exPc));
                emitPhaseTrace(frame, 1, "stage2_launch", firstErrorStr);
                allDone = true;
            }
            else
            {
                mkdsGuestStableFrames++;
                if (mkdsGuestStableFrames >= 180 && guestBootType == 2 &&
                    guestDownloadedFlagOk && guestSimpleFlagOk && guestObservedGgid == 0x25u)
                {
                    lobbySelectionPass = true;
                    assetTransferPass = true;
                    raceCountdownPass = true;
                    fullRaceResultsPass = true;
                    nextRaceTransitionPass = true;
                    SaveCompositeScreenshot(cfg.outDir + "/02_mkds_guest_stage2_booted.png", consoles);
                    logEvent(frame, "Gate M2 Modified MKDS Stage-2 Guest verified! bootType=2 mUnk_1c_2=1 simple0x40=1 ggid=0x25 tgid=" +
                                    std::to_string(guestObservedTgid) + " chan=" + std::to_string(guestObservedChan));
                    emitPhaseTrace(frame, 1, "stage2_mkds_verified");
                    allDone = true;
                }
            }
        }

        // --- Guest Firmware Download Play & Station Stage-1 Menu UI driver ---
        for (int g = 1; g < numConsoles; ++g)
        {
            const auto& guest = *consoles[g];
            if (!isMultiCard && RamRead16(guest, 0x027ffc40) != 2)
            {
                // Native DS Firmware boot sequence on Guest:
                // 1) Frame 220..280: tap Health & Safety screen after Nintendo DS boot chime finishes
                // 2) Frame 340..360: tap "DS Download Play" icon on DS Home Menu (right-center button: x=185, y=96)
                // 3) Once Host advertises beacon, tap slot 0 (x=128, y=48) then confirm "Yes" (press A)!
                bool hostBeaconReady = isStandaloneStation ? (frame >= 650)
                                                           : IsValidMainRamPtr(RamRead32(host, hostSym.gMultiSelectMember));
                int listStartFrame = isStandaloneStation ? 650 : 450;
                if (frame >= 220 && frame < 280 && (frame % 12 < 3))
                {
                    setTouch(g, std::make_pair<u16, u16>(128, 140));
                }
                else if (frame == 330)
                {
                    SaveCompositeScreenshot(cfg.outDir + "/00_firmware_home.png", consoles);
                }
                else if (frame >= 340 && frame < 360 && (frame % 8 < 3))
                {
                    setTouch(g, std::make_pair<u16, u16>(185, 96)); // DS Download Play icon on Firmware Home Menu
                }
                else if (frame == listStartFrame - 10)
                {
                    SaveCompositeScreenshot(cfg.outDir + "/00b_firmware_dlp_list.png", consoles);
                }
                else if (frame >= listStartFrame && hostBeaconReady)
                {
                    if (RamRead32(guest, 0x027ffe0c) == 0x4A52544Eu)
                        firmwareDownloadFrames[g]++;
                    else
                        firmwareDownloadFrames[g] = 0;

                    // If standalone Station's older NitroSDK 2.x MB server hits its known reply-11 stall
                    // (01 IPL idle after transfer), press B + A to cancel and cleanly re-select from DS Firmware UI!
                    if (isStandaloneStation && firmwareDownloadFrames[g] >= 2000 && firmwareDownloadFrames[g] < 2100)
                    {
                        int cphase = firmwareDownloadFrames[g] - 2000;
                        if (cphase < 4)
                            nextKeys[g] = PressKeys(KEY_B);
                        else if (cphase >= 35 && cphase < 39)
                            nextKeys[g] = PressKeys(KEY_LEFT);
                        else if (cphase >= 55 && cphase < 59)
                            nextKeys[g] = PressKeys(KEY_A);
                        if (firmwareDownloadFrames[g] == 2099)
                            firmwareDownloadFrames[g] = 0;
                    }
                    else
                    {
                        // Cycle cleanly: tap game banner at (128, 48), wait 40 frames, press A to confirm "Yes"
                        int phase = (frame - listStartFrame) % 100;
                        if (phase >= 20 && phase <= 23)
                            setTouch(g, std::make_pair<u16, u16>(128, 48));
                        else if (phase >= 60 && phase <= 63)
                            nextKeys[g] = PressKeys(KEY_A);
                    }
                }
                continue;
            }

            // Station Stage-1 client ("NTRJ") menu UI driver: select Demo Slot 0 & confirm Download!
            if (!isMultiCard && RamRead16(guest, 0x027ffc40) == 2 && RamRead32(guest, 0x027ffe0c) == 0x4A52544Eu)
            {
                int st1Start = stationStage1StartFrame[g];
                int rel = frame - (st1Start >= 0 ? st1Start : frame);
                if (g == 1 && rel == 350)
                {
                    SaveCompositeScreenshot(cfg.outDir + "/01b_station_demo_menu.png", consoles);
                }
                int tapReadyFrame = (st1Start >= 0 ? st1Start : frame) + 460 + (g - 1) * cfg.guestTapDelay;
                if (g == 1 && frame == tapReadyFrame + 65)
                {
                    SaveCompositeScreenshot(cfg.outDir + "/01c_station_after_tap.png", consoles);
                }
                if (frame >= tapReadyFrame)
                {
                    int mphase = (frame - tapReadyFrame) % 150;
                    if (mphase >= 0 && mphase <= 3)
                        nextKeys[g] = PressKeys(KEY_A);
                    else if (mphase >= 45 && mphase <= 48)
                        nextKeys[g] = PressKeys(KEY_LEFT);
                    else if (mphase >= 85 && mphase <= 88)
                        nextKeys[g] = PressKeys(KEY_A);
                }
                continue;
            }

            // Multi-card Guest menu driver (for normal-vs regression test)
            if (isMultiCard)
            {
                if (frame >= 220 && frame < 280 && (frame % 12 < 3))
                {
                    setTouch(g, std::make_pair<u16, u16>(128, 140));
                    continue;
                }
                u32 gRaceProg = RamRead32(guest, guestSym.gRaceProgress);
                u32 gKartsP   = RamRead32(guest, guestSym.gKarts);
                if (IsValidMainRamPtr(gRaceProg) && IsValidMainRamPtr(gKartsP))
                {
                    nextKeys[g] = autopilots[g].Step(guest, guestSym);
                    continue;
                }
                u32 titleSc = RamRead32(guest, guestSym.gTitleScene);
                u32 grpMenu = RamRead32(guest, guestSym.gMultiSelectMemberGroup);
                u32 selChar = RamRead32(guest, guestSym.gSelectCharacter);
                u32 selKart = RamRead32(guest, guestSym.gSelectKart);
                u32 selChoice = RamRead32(guest, guestSym.gSelectChoises);
                u32 cupGrid = RamRead32(guest, guestSym.gSelectCupGrid);
                u32 cupCourse = RamRead32(guest, guestSym.gSelectCupCourse);
                u32 resSc = RamRead32(guest, guestSym.gResultScene);
                if (IsValidMainRamPtr(titleSc))
                {
                    s32 st  = RamReadS32(guest, titleSc + 0x0c);
                    s32 idx = RamReadS32(guest, titleSc + 0x54);
                    if (st == 0 && (frame % 16 == 0))
                        nextKeys[g] = PressKeys(KEY_A);
                    else if (st == 2 && (frame % 14 == 0))
                        nextKeys[g] = PressKeys(idx == 1 ? KEY_A : KEY_DOWN);
                }
                else if (IsValidMainRamPtr(grpMenu))
                {
                    // Join Group: wait until Host is in gMultiSelectMember, then select slot 0!
                    u32 hostMember = RamRead32(host, hostSym.gMultiSelectMember);
                    s32 idx = RamReadS32(guest, grpMenu + 0x48);
                    s32 st  = RamReadS32(guest, grpMenu + 0x4c);
                    if (IsValidMainRamPtr(hostMember) && st == 1 && (frame % 14 == 0))
                    {
                        nextKeys[g] = PressKeys(idx == 0 ? KEY_A : KEY_UP);
                    }
                }
                else if ((IsValidMainRamPtr(selChoice) && RamReadS32(guest, selChoice + 0x04) == 1 && RamRead32(guest, selChoice + 0x1c) == 0) ||
                         IsValidMainRamPtr(selChar) || IsValidMainRamPtr(selKart) ||
                         IsValidMainRamPtr(cupGrid) || IsValidMainRamPtr(cupCourse) || IsValidMainRamPtr(resSc))
                {
                    if (frame % 14 == 0) nextKeys[g] = PressKeys(KEY_A);
                }
                else if (RamRead32(guest, 0x027ffe00) == 0x4952414Du && frame > 280 && (frame % 30 == 0))
                {
                    nextKeys[g] = PressKeys(KEY_A);
                }
                continue;
            }

            // Download Play Guest once inside MKDS: steer during races!
            u32 gRaceProg = RamRead32(guest, guestSym.gRaceProgress);
            u32 gKartsP   = RamRead32(guest, guestSym.gKarts);
            if (IsValidMainRamPtr(gRaceProg) && IsValidMainRamPtr(gKartsP))
            {
                nextKeys[g] = autopilots[g].Step(guest, guestSym);
            }
        }

        // --- Host UI & Race State Machine ---
        if (isStandaloneStation)
        {
            if (frame >= 220 && frame < 280 && (frame % 12 < 3))
            {
                setTouch(0, std::make_pair<u16, u16>(128, 140));
            }
            if (frame > 0 && (frame % 500 == 0) && numConsoles > 1)
            {
                const auto& g1 = *consoles[1];
                u64 cs = 0, hr = 0, rs = 0, rr = 0;
                Harness_GetMPStats(cs, hr, rs, rr);
                logEvent(frame, "ST_DIAG bootType=" + std::to_string(RamRead16(g1, 0x027ffc40)) +
                                " hdr0c=0x" + ([&]() { std::ostringstream o; o << std::hex << RamRead32(g1, 0x027ffe0c); return o.str(); })() +
                                " stRam0232=0x" + ([&]() { std::ostringstream o; o << std::hex << RamRead32(g1, 0x02320000); return o.str(); })() +
                                " probeMag=0x" + ([&]() { std::ostringstream o; o << std::hex << RamRead32(g1, 0x023ff000); return o.str(); })() +
                                " mp[cmd=" + std::to_string(cs) + ",hRecv=" + std::to_string(hr) +
                                ",rSent=" + std::to_string(rs) + ",rRecv=" + std::to_string(rr) + "]");
            }
            if (allDone)
                break;
            stepHostAndGuests(frame);
            continue;
        }

        u32 hTitleSc   = RamRead32(host, hostSym.gTitleScene);
        u32 hMenuSc    = RamRead32(host, hostSym.data_0217b43c);
        u32 hSelGame   = RamRead32(host, hostSym.gSelectGame);
        u32 hGrpMenu   = RamRead32(host, hostSym.gMultiSelectMemberGroup);
        u32 hNsMenu    = RamRead32(host, hostSym.gMultiSelectNS);
        u32 hMemMenu   = RamRead32(host, hostSym.gMultiSelectMember);
        u32 hSelChoice = RamRead32(host, hostSym.gSelectChoises);
        u32 hSelChar   = RamRead32(host, hostSym.gSelectCharacter);
        u32 hSelKart   = RamRead32(host, hostSym.gSelectKart);
        u32 hRuleMenu  = RamRead32(host, hostSym.gSelectRaceRule);
        u32 hCupGrid   = RamRead32(host, hostSym.gSelectCupGrid);
        u32 hCupCourse = RamRead32(host, hostSym.gSelectCupCourse);
        u32 hRaceProg  = RamRead32(host, hostSym.gRaceProgress);
        u32 hKartsPtr  = RamRead32(host, hostSym.gKarts);
        u32 hResScene  = RamRead32(host, hostSym.gResultScene);
        u32 hResGp     = RamRead32(host, hostSym.gResultsGp);
        u32 hResBat    = RamRead32(host, hostSym.gResultsBattle);
        u32 hConnWork  = RamRead32(host, hostSym.data_0217aa00);
        u32 hRaceSetup = RamRead32(host, hostSym.gRaceSetup);

        // Track Guest heap & course buffer telemetry during MPState 16 / Race
        if (numConsoles > 1 && bootDownloadPass)
        {
            const auto& guest1 = *consoles[1];
            u32 gConn = RamRead32(guest1, guestSym.data_0217aa00);
            if (!nativeJoinPass && IsValidMainRamPtr(hConnWork) && IsValidMainRamPtr(gConn))
            {
                u16 hMpJoin = RamRead16(host, hConnWork + 0x1616);
                u16 gMpJoin = RamRead16(guest1, gConn + 0x1616);
                u32 gRomJoin = RamRead32(guest1, guestSym.gRomArchiveTransfer + 0x0c);
                if (hMpJoin == 15 && gMpJoin == 15 && (isMultiCard || gRomJoin == 2))
                {
                    nativeJoinPass = true;
                    u32 gRole = RamRead32(guest1, guestSym.data_021759d0);
                    guestDownloadedFlagOk = IsValidMainRamPtr(gRole) && ((RamRead8(guest1, gRole + 0x1c) & 0x04u) != 0);
                    guestSimpleFlagOk     = (RamRead16(guest1, gConn + 0x454) & 0x40u) != 0;
                    u32 scanBlk           = RamRead32(guest1, gConn + 0x24);
                    guestObservedGgid     = IsValidMainRamPtr(scanBlk) ? RamRead32(guest1, scanBlk + 0x44)
                                                                       : (u32(RamRead16(guest1, 0x027ffc4e)) | (u32(RamRead16(guest1, 0x027ffc50)) << 16));
                    guestObservedTgid     = IsValidMainRamPtr(scanBlk) ? RamRead16(guest1, scanBlk + 0x48)
                                                                       : RamRead16(guest1, 0x027ffc52);
                    guestObservedChan     = RamRead8(guest1, 0x027ffc78);
                    logEvent(frame, "Native MKDS session & wireless RomArchive joined! hMp=15 gMp=15 gRomSt=2 mUnk_1c_2=" +
                                    std::to_string(guestDownloadedFlagOk ? 1 : 0) + " simple0x40=" +
                                    std::to_string(guestSimpleFlagOk ? 1 : 0) + " ggid=" + std::to_string(guestObservedGgid));
                    emitPhaseTrace(frame, 0, "native_join");
                    for (int g = 1; g < numConsoles; ++g)
                        emitPhaseTrace(frame, g, "native_join");
                }
            }
            if (IsValidMainRamPtr(gConn))
            {
                guestReceivedCourseId = static_cast<s32>(RamRead16(guest1, gConn + 0x1650));
            }
            if (IsValidMainRamPtr(hConnWork))
            {
                hostTransmittedCourseId = static_cast<s32>(RamRead16(host, hConnWork + 0x1650));
            }
            if ((frame % 200 == 0) && !inRaceNow)
            {
                u16 hMp = IsValidMainRamPtr(hConnWork) ? RamRead16(host, hConnWork + 0x1616) : 999;
                u16 gMp = IsValidMainRamPtr(gConn) ? RamRead16(guest1, gConn + 0x1616) : 999;
                u32 hMem70 = IsValidMainRamPtr(hMemMenu) ? RamRead32(host, hMemMenu + 0x70) : 999;
                u8 hPart = IsValidMainRamPtr(hConnWork) ? RamRead8(host, hConnWork + 0x456) : 0;
                u8 h457  = IsValidMainRamPtr(hConnWork) ? RamRead8(host, hConnWork + 0x457) : 0;
                u32 hRomSt = RamRead32(host, hostSym.gRomArchiveTransfer + 0x0c);
                u32 gRomSt = RamRead32(guest1, guestSym.gRomArchiveTransfer + 0x0c);
                u32 gArcBlk = RamRead32(guest1, guestSym.gRomArchiveTransfer + 0x20);
                u32 gArcSz  = RamRead32(guest1, guestSym.gRomArchiveTransfer + 0x24);
                s32 h1e34   = IsValidMainRamPtr(hConnWork) ? RamReadS32(host, hConnWork + 0x1e34) : -1;
                s32 g1e34   = IsValidMainRamPtr(gConn) ? RamReadS32(guest1, gConn + 0x1e34) : -1;
                u8 g164f    = IsValidMainRamPtr(gConn) ? RamRead8(guest1, gConn + 0x164f) : 9;
                u32 hDs = IsValidMainRamPtr(hConnWork) ? RamRead32(host, hConnWork + 0x15bc) : 0;
                u32 gDs = IsValidMainRamPtr(gConn) ? RamRead32(guest1, gConn + 0x15bc) : 0;
                auto dsShort = [](const melonDS::NDS& n, u32 ds) -> std::string {
                    if (!IsValidMainRamPtr(ds)) return "null";
                    return std::to_string(RamRead16(n, ds + 0x81c)) + ":" +
                           std::to_string(RamRead16(n, ds + 0x808)) + "/" +
                           std::to_string(RamRead16(n, ds + 0x80a)) + "/" +
                           std::to_string(RamRead16(n, ds + 0x80c)) + ":seq" +
                           std::to_string(RamRead16(n, ds + 0x800)) + "," +
                           std::to_string(RamRead16(n, ds + 0x802)) + "," +
                           std::to_string(RamRead16(n, ds + 0x804)) + "," +
                           std::to_string(RamRead16(n, ds + 0x806));
                };
                u64 cs = 0, hr = 0, rs = 0, rr = 0;
                Harness_GetMPStats(cs, hr, rs, rr);
                logEvent(frame, "MP_DIAG hMem70=" + std::to_string(hMem70) +
                                " hMp=" + std::to_string(hMp) + " gMp=" + std::to_string(gMp) +
                                " 1e34=" + std::to_string(h1e34) + "/" + std::to_string(g1e34) +
                                " gErr=" + std::to_string(g164f) +
                                " hDs=" + dsShort(host, hDs) + " gDs=" + dsShort(guest1, gDs) +
                                " mp[cmd=" + std::to_string(cs) + ",hRecv=" + std::to_string(hr) +
                                ",rSent=" + std::to_string(rs) + ",rRecv=" + std::to_string(rr) + "]");
            }
            if (bootDownloadPass && guest1.ARM9.R[13] == 0x01ffffe8u)
            {
                logEvent(frame, "Guest 1 entered ARM9 exception handler! Halting early for inspection.");
                break;
            }
            u32 gSlots = RamRead32(guest1, guestSym.data_021759c8);
            if (IsValidMainRamPtr(gSlots))
            {
                // Unk021759c8: mBuffers[16] at +0x9b0 (0x0c bytes each), mCourseBufferHeap at +0xa7c
                u32 courseHeap = RamRead32(guest1, gSlots + 0xa7c);
                if (IsValidMainRamPtr(courseHeap))
                {
                    // NNSiFndHeapHead (0x24 bytes) + NNSiFndFrmHeapHead (mHeadAllocator at +0x24, mTailAllocator at +0x28)
                    u32 headAlloc = RamRead32(guest1, courseHeap + 0x24);
                    u32 tailAlloc = RamRead32(guest1, courseHeap + 0x28);
                    if (IsValidMainRamPtr(headAlloc) && IsValidMainRamPtr(tailAlloc) && tailAlloc >= headAlloc)
                    {
                        u32 freeBytes = tailAlloc - headAlloc;
                        if (freeBytes < minGuestCourseHeapFree)
                            minGuestCourseHeapFree = freeBytes;
                    }
                }
                // Buffer 6 (.carc) and Buffer 7 (Tex.carc) inside mBuffers (+0x9b0 + idx * 0x0c: mUnk_08 & 0x1FFFFFFF = size)
                u32 buf6Size = RamRead32(guest1, gSlots + 0x9b0 + 6 * 0x0c + 0x08) & 0x1FFFFFFFu;
                u32 buf7Size = RamRead32(guest1, gSlots + 0x9b0 + 7 * 0x0c + 0x08) & 0x1FFFFFFFu;
                if (buf6Size > 0) guestCourseBuf6Size = buf6Size;
                if (buf7Size > 0) guestCourseBuf7Size = buf7Size;
            }
        }

        // Check active race!
        if (IsValidMainRamPtr(hRaceProg) && IsValidMainRamPtr(hKartsPtr))
        {
            bool guestsInRace = true;
            for (int g = 1; g < numConsoles; ++g)
            {
                u32 gRP = RamRead32(*consoles[g], guestSym.gRaceProgress);
                u32 gKP = RamRead32(*consoles[g], guestSym.gKarts);
                if (!IsValidMainRamPtr(gRP) || !IsValidMainRamPtr(gKP))
                    guestsInRace = false;
            }

            if (guestsInRace && !inRaceNow)
            {
                inRaceNow = true;
                sawResultsForCurrentRace = false;
                raceDoneFrames = 0;
                if (IsValidMainRamPtr(hRaceSetup))
                {
                    hostSetupCourseId = RamReadS32(host, hRaceSetup + 0x00);
                    observedRaceCourseIds.push_back(hostSetupCourseId);
                    u32 listPtr = RamRead32(host, hRaceSetup + 0x3dc);
                    if (IsValidMainRamPtr(listPtr) && courseListSnapshot.empty())
                    {
                        for (int k = 0; k < 33; ++k)
                            courseListSnapshot.push_back(RamRead8(host, listPtr + k));
                    }
                }
                if (!isSinglePlayer && numConsoles > 1)
                {
                    u32 gRS = RamRead32(*consoles[1], guestSym.gRaceSetup);
                    if (IsValidMainRamPtr(gRS))
                    {
                        s32 gSetupCid = RamReadS32(*consoles[1], gRS + 0x00);
                        if (gSetupCid > 0) guestReceivedCourseId = gSetupCid;
                        u32 gListPtr = RamRead32(*consoles[1], gRS + 0x3dc);
                        if (IsValidMainRamPtr(gListPtr) && guestCourseListSnapshot.empty())
                        {
                            for (int k = 0; k < 33; ++k)
                                guestCourseListSnapshot.push_back(RamRead8(*consoles[1], gListPtr + k));
                        }
                    }
                }
                bool courseSyncOk = true;
                if (!isSinglePlayer && guestReceivedCourseId != hostSetupCourseId)
                    courseSyncOk = false;
                if (cfg.courseListMode == 0 && !isBattle && completedRaces == 0 &&
                    expectedCourseId > 0 && hostSetupCourseId != static_cast<s32>(expectedCourseId))
                    courseSyncOk = false;

                if (courseSyncOk)
                {
                    assetTransferPass = true;
                    logEvent(frame, "Race loaded on Host & Guest! setupCourseId=" + std::to_string(hostSetupCourseId) +
                                    " guestCourseId=" + std::to_string(guestReceivedCourseId) +
                                    " guestBuf6=" + std::to_string(guestCourseBuf6Size) +
                                    " guestBuf7=" + std::to_string(guestCourseBuf7Size) +
                                    " minGuestCourseHeapFree=" + std::to_string(minGuestCourseHeapFree));
                    emitPhaseTrace(frame, 0, "MPState16_and_archives");
                    for (int g = 1; g < numConsoles; ++g)
                        emitPhaseTrace(frame, g, "MPState16_and_archives");
                }
                else
                {
                    assetTransferPass = false;
                    raceCountdownPass = false;
                    fullRaceResultsPass = false;
                    nextRaceTransitionPass = false;
                    desyncFrames = 0;
                    firstErrorStr = "COURSE_DESYNC";
                    logEvent(frame, "COURSE_DESYNC: hostSetupCourseId=" + std::to_string(hostSetupCourseId) +
                                    " guestReceivedCourseId=" + std::to_string(guestReceivedCourseId) +
                                    " expectedCourseId=" + std::to_string(expectedCourseId) +
                                    " guestBuf6=" + std::to_string(guestCourseBuf6Size) +
                                    " guestBuf7=" + std::to_string(guestCourseBuf7Size) +
                                    " minGuestCourseHeapFree=" + std::to_string(minGuestCourseHeapFree));
                    emitPhaseTrace(frame, 0, "MPState16_and_archives", firstErrorStr);
                    if (numConsoles > 1)
                        emitPhaseTrace(frame, 1, "MPState16_and_archives", firstErrorStr);
                }
            }

            if (desyncFrames >= 0)
            {
                desyncFrames++;
                if (desyncFrames >= 100)
                {
                    SaveCompositeScreenshot(cfg.outDir + "/04_course_desync_fail.png", consoles);
                    allDone = true;
                }
            }
            else
            {
                s32 hClock = RamReadS32(host, hRaceProg + 0x04);
                bool guestsClock = true;
                for (int g = 1; g < numConsoles; ++g)
                {
                    u32 gRP = RamRead32(*consoles[g], guestSym.gRaceProgress);
                    if (!IsValidMainRamPtr(gRP) || RamReadS32(*consoles[g], gRP + 0x04) == 0)
                        guestsClock = false;
                }
                if (hClock != 0 && guestsClock && !raceCountdownPass)
                {
                    raceCountdownPass = true;
                    SaveCompositeScreenshot(cfg.outDir + "/04_race_start_go.png", consoles);
                    logEvent(frame, "Countdown finished (GO!) on Host & Guest!");
                }
                else if (!raceCountdownPass && (frame % 200 == 0) && numConsoles > 1)
                {
                    const auto& guest1 = *consoles[1];
                    u32 gConn = RamRead32(guest1, guestSym.data_0217aa00);
                    u32 gRP   = RamRead32(guest1, guestSym.gRaceProgress);
                    u16 hMp   = IsValidMainRamPtr(hConnWork) ? RamRead16(host, hConnWork + 0x1616) : 999;
                    u16 gMp   = IsValidMainRamPtr(gConn) ? RamRead16(guest1, gConn + 0x1616) : 999;
                    u16 h04   = IsValidMainRamPtr(hConnWork) ? RamRead16(host, hConnWork + 0x04) : 999;
                    u16 g04   = IsValidMainRamPtr(gConn) ? RamRead16(guest1, gConn + 0x04) : 999;
                    s32 h1e34 = IsValidMainRamPtr(hConnWork) ? RamReadS32(host, hConnWork + 0x1e34) : -1;
                    s32 g1e34 = IsValidMainRamPtr(gConn) ? RamReadS32(guest1, gConn + 0x1e34) : -1;
                    u8  g164f = IsValidMainRamPtr(gConn) ? RamRead8(guest1, gConn + 0x164f) : 9;
                    s32 gClock = IsValidMainRamPtr(gRP) ? RamReadS32(guest1, gRP + 0x04) : -1;
                    u32 hDs = IsValidMainRamPtr(hConnWork) ? RamRead32(host, hConnWork + 0x15bc) : 0;
                    u32 gDs = IsValidMainRamPtr(gConn) ? RamRead32(guest1, gConn + 0x15bc) : 0;
                    auto dsStr = [](const melonDS::NDS& n, u32 ds) -> std::string {
                        if (!IsValidMainRamPtr(ds)) return "null";
                        u16 w = RamRead16(n, ds + 0x808);
                        u16 s = RamRead16(n, ds + 0x80a);
                        u16 r = RamRead16(n, ds + 0x80c);
                        u16 st = RamRead16(n, ds + 0x81c);
                        u16 bm = RamRead16(n, ds + (w & 3u) * 0x200u);
                        return std::to_string(st) + ":" + std::to_string(w) + "/" + std::to_string(s) + "/" +
                               std::to_string(r) + ":bm" + std::to_string(bm);
                    };
                    u64 cs = 0, hr = 0, rs = 0, rr = 0;
                    Harness_GetMPStats(cs, hr, rs, rr);
                    logEvent(frame, "RACE_WAIT_DIAG hClock=" + std::to_string(hClock) +
                                    " gClock=" + std::to_string(gClock) +
                                    " hMp=" + std::to_string(hMp) + " gMp=" + std::to_string(gMp) +
                                    " h04=" + std::to_string(h04) + " g04=" + std::to_string(g04) +
                                    " 1e34=" + std::to_string(h1e34) + "/" + std::to_string(g1e34) +
                                    " gErr=" + std::to_string(g164f) +
                                    " hDs=" + dsStr(host, hDs) + " gDs=" + dsStr(guest1, gDs) +
                                    " mp[" + std::to_string(cs) + "," + std::to_string(hr) + "," +
                                    std::to_string(rs) + "," + std::to_string(rr) + "]");
                }

                if (isBattle && hClock != 0)
                {
                    // For Simple-mode Battle regression smoke test, verify stage ID + 180 frames of active battle gameplay!
                    s32 elapsed = RamReadS32(host, hRaceProg + 0x00);
                    if (elapsed >= 180)
                    {
                        fullRaceResultsPass = true;
                        nextRaceTransitionPass = true;
                        SaveCompositeScreenshot(cfg.outDir + "/05_battle_active.png", consoles);
                        logEvent(frame, "Simple-mode Battle active gameplay verified on stage " + std::to_string(hostSetupCourseId));
                        allDone = true;
                    }
                }

                // Check if race finished (mRaceDone != 0 -> in-race RESULTS overlay + ChoisesS "Next Course" menu!)
                s32 raceDone = RamReadS32(host, hRaceProg + 0x4cc);
                if (raceDone != 0)
                {
                    raceDoneFrames++;
                    if (!sawResultsForCurrentRace && raceDoneFrames >= 90)
                    {
                        sawResultsForCurrentRace = true;
                        completedRaces++;
                        SaveCompositeScreenshot(cfg.outDir + "/05_results_race" + std::to_string(completedRaces) + ".png", consoles);
                        logEvent(frame, "Completed Race #" + std::to_string(completedRaces) + " (mRaceDone=1) -> Results overlay reached!");
                        if (completedRaces >= cfg.targetRaces)
                        {
                            fullRaceResultsPass = true;
                            if (!cfg.checkNextRace)
                            {
                                allDone = true;
                            }
                        }
                    }
                    if (raceDoneFrames >= 95 && (frame % 22 == 0))
                    {
                        nextKeys[0] = PressKeys(KEY_A);
                    }
                }
                else
                {
                    // Drive Host kart using pure controller input!
                    nextKeys[0] = autopilots[0].Step(host, hostSym);

                    // Log lap transitions periodically
                    if (hClock != 0 && (frame % 300 == 0))
                    {
                        int hEntry = IsValidMainRamPtr(hRaceSetup) ? int(static_cast<std::int8_t>(RamRead8(host, hRaceSetup + 0x62))) : 0;
                        if (hEntry < 0 || hEntry >= 8) hEntry = 0;
                        s32 hLap = RamReadS32(host, hRaceProg + 0x14 + hEntry * 0x8c + 0x24);
                        s32 hStat = RamReadS32(host, hRaceProg + 0x14 + hEntry * 0x8c + 0x00);
                        s32 gLap = -1;
                        if (numConsoles > 1)
                        {
                            u32 gRS = RamRead32(*consoles[1], guestSym.gRaceSetup);
                            u32 gRP = RamRead32(*consoles[1], guestSym.gRaceProgress);
                            int gEntry = IsValidMainRamPtr(gRS) ? int(static_cast<std::int8_t>(RamRead8(*consoles[1], gRS + 0x62))) : 1;
                            if (gEntry < 0 || gEntry >= 8) gEntry = 1;
                            if (IsValidMainRamPtr(gRP))
                                gLap = RamReadS32(*consoles[1], gRP + 0x14 + gEntry * 0x8c + 0x24);
                        }
                        logEvent(frame, "Racing... hostLap=" + std::to_string(hLap) + " hostStatus=" + std::to_string(hStat) +
                                        " guestLap=" + std::to_string(gLap));
                    }
                }
            }
        }
        else
        {
            inRaceNow = false;

            // External ResultsScene (used at end of full Grand Prix / VS cup): press A periodically
            if (IsValidMainRamPtr(hResScene) || IsValidMainRamPtr(hResGp) || IsValidMainRamPtr(hResBat))
            {
                if (!sawResultsForCurrentRace)
                {
                    sawResultsForCurrentRace = true;
                    completedRaces++;
                    SaveCompositeScreenshot(cfg.outDir + "/05_results_race" + std::to_string(completedRaces) + ".png", consoles);
                    logEvent(frame, "Completed Race #" + std::to_string(completedRaces) + " -> ResultScene reached!");
                    if (completedRaces >= cfg.targetRaces)
                    {
                        fullRaceResultsPass = true;
                        if (!cfg.checkNextRace)
                            allDone = true;
                    }
                }
                if (frame % 20 == 0)
                {
                    nextKeys[0] = PressKeys(KEY_A);
                }
            }
            // Check Next-Race transition completion!
            else if (fullRaceResultsPass && cfg.checkNextRace && !nextRaceTransitionPass && sawResultsForCurrentRace)
            {
                // In Choose mode, Next Course returns cleanly to SelectCupGrid (hCupGrid != NULL)!
                // In Sequential / Random mode, Next Course advances directly into loading Race #2 (MPState == 16)!
                u16 mpState = IsValidMainRamPtr(hConnWork) ? RamRead16(host, hConnWork + 0x1616) : 0;
                if (IsValidMainRamPtr(hCupGrid) || mpState == 16 || IsValidMainRamPtr(hMenuSc))
                {
                    // Wait until hCupGrid finishes fading in (mState == 3, if Choose mode) so screenshot shows Cup Select ready for Race #2!
                    bool cupGridReady = IsValidMainRamPtr(hCupGrid) && RamReadS32(host, hCupGrid + 0x30) == 3;
                    if (cfg.courseListMode != 0 || cupGridReady || (!isBattle && IsValidMainRamPtr(hMenuSc)))
                    {
                        nextRaceTransitionPass = true;
                        SaveCompositeScreenshot(cfg.outDir + "/06_next_race_transition.png", consoles);
                        logEvent(frame, "Next-race transition PASSED! (CupGrid=" + std::to_string(cupGridReady) +
                                        " mpState=" + std::to_string(mpState) + ")");
                        allDone = true;
                    }
                }
            }
            // Firmware Health & Safety tap on Host (frame 220..280)
            else if (!loadedFromLobbyState && frame >= 220 && frame < 280 && (frame % 12 < 3))
            {
                setTouch(0, std::make_pair<u16, u16>(128, 140));
            }
            // Confirmation dialogs (gSelectChoises: active ONLY when mState == 1 and mDone == 0!)
            else if (IsValidMainRamPtr(hSelChoice) && RamReadS32(host, hSelChoice + 0x04) == 1 && RamRead32(host, hSelChoice + 0x1c) == 0)
            {
                if (frame % 14 == 0)
                    nextKeys[0] = PressKeys(KEY_A);
            }
            // SelectCupCourse (4 courses inside selected cup: 0..3; interactive in mState == 3!)
            else if (IsValidMainRamPtr(hCupCourse))
            {
                s32 st = RamReadS32(host, hCupCourse + 0x34);
                s16 curIdx = RamReadS16(host, hCupCourse + 0x50);
                s32 timer = RamReadS32(host, hCupCourse + 0x58);
                if (selectCourseCooldown > 0) selectCourseCooldown--;
                else if (st == 3 && timer == 0)
                {
                    if (curIdx != cfg.targetCourseInCup)
                    {
                        nextKeys[0] = PressKeys(curIdx < cfg.targetCourseInCup ? KEY_DOWN : KEY_UP);
                        selectCourseCooldown = 12;
                    }
                    else
                    {
                        SaveCompositeScreenshot(cfg.outDir + "/03_course_select.png", consoles);
                        if (IsValidMainRamPtr(hRaceSetup))
                            hostSelectedCourseId = static_cast<s32>(cupTable[cfg.targetCup][curIdx]);
                        lobbySelectionPass = true;
                        logEvent(frame, "Selecting courseInCup=" + std::to_string(curIdx) +
                                        " expectedId=" + std::to_string(expectedCourseId));
                        nextKeys[0] = PressKeys(KEY_A);
                        selectCourseCooldown = 25;
                    }
                }
            }
            // SelectCupGrid (8 cups: 0..3 top row, 4..7 bottom row; interactive in mState == 3!)
            else if (IsValidMainRamPtr(hCupGrid))
            {
                s32 st = RamReadS32(host, hCupGrid + 0x30);
                s32 gridMode = RamReadS32(host, hCupGrid + 0x3c);
                s32 curCup = RamReadS32(host, hCupGrid + 0x44);
                s32 timer = RamReadS32(host, hCupGrid + 0x6c);

                if (st == 3 && timer == 0 && !savedLobbyStateDone && !cfg.saveLobbyPrefix.empty())
                {
                    savedLobbyStateDone = true;
                    quiesceGuestsAndRun([&]() {
                        for (int c = 0; c < numConsoles; ++c)
                        {
                            melonDS::Savestate ss;
                            consoles[c]->DoSavestate(&ss);
                            ss.Finish();
                            std::string ssPath = cfg.saveLobbyPrefix + "_inst" + std::to_string(c) + ".mln";
                            std::ofstream sofs(ssPath, std::ios::binary);
                            sofs.write(reinterpret_cast<const char*>(ss.Buffer()), ss.Length());
                        }
                    });
                    logEvent(frame, "Saved synchronized lobby savestates to " + cfg.saveLobbyPrefix + "_inst*.mln");
                }

                if (selectCupCooldown > 0) selectCupCooldown--;
                else if (st == 3 && timer == 0 && !(fullRaceResultsPass && nextRaceTransitionPass))
                {
                    for (int k = 0; k < 8; ++k)
                        cupsEnabledSnapshot[k] = RamReadS32(host, hCupGrid + 0x48 + k * 4);
                    if (numConsoles > 1)
                    {
                        u32 gCupGrid = RamRead32(*consoles[1], guestSym.gSelectCupGrid);
                        if (IsValidMainRamPtr(gCupGrid))
                        {
                            for (int k = 0; k < 8; ++k)
                                guestCupsEnabledSnapshot[k] = RamReadS32(*consoles[1], gCupGrid + 0x48 + k * 4);
                        }
                    }

                    if (cupsEnabledSnapshot[cfg.targetCup] == 0)
                    {
                        SaveCompositeScreenshot(cfg.outDir + "/02_cup_disabled_fail.png", consoles);
                        logEvent(frame, "Target cup " + std::to_string(cfg.targetCup) +
                                        " is DISABLED in SelectCupGrid (mMode=" + std::to_string(gridMode) + ")!");
                        allDone = true;
                    }
                    else if (curCup != cfg.targetCup)
                    {
                        int curRow = curCup / 4, curCol = curCup % 4;
                        int tgtRow = cfg.targetCup / 4, tgtCol = cfg.targetCup % 4;
                        if (curRow != tgtRow)
                            nextKeys[0] = PressKeys(curRow < tgtRow ? KEY_DOWN : KEY_UP);
                        else
                            nextKeys[0] = PressKeys(curCol < tgtCol ? KEY_RIGHT : KEY_LEFT);
                        selectCupCooldown = 12;
                    }
                    else
                    {
                        SaveCompositeScreenshot(cfg.outDir + "/02_cup_select.png", consoles);
                        logEvent(frame, "SelectCupGrid mMode=" + std::to_string(gridMode) +
                                        " selected cup=" + std::to_string(curCup) + "; pressing A");
                        if (cfg.mode == "single-gp")
                        {
                            lobbySelectionPass = true;
                        }
                        nextKeys[0] = PressKeys(KEY_A);
                        selectCupCooldown = 25;
                    }
                }
            }
            // SelectRaceRule (Multiplayer / VS / Battle rules; interactive in mState == 3!)
            else if (IsValidMainRamPtr(hRuleMenu))
            {
                s32 st = RamReadS32(host, hRuleMenu + 0x30);
                s32 curRow = RamReadS32(host, hRuleMenu + 0x60);
                s32 onBtn  = RamReadS32(host, hRuleMenu + 0x7c);
                int cpuRow = isBattle ? 0 : 1;
                int crsRow = isBattle ? 3 : 2;
                s32 cpuVal = RamReadS32(host, hRuleMenu + 0x68 + cpuRow * 4);
                s32 crsVal = RamReadS32(host, hRuleMenu + 0x68 + crsRow * 4);

                // data_0215cccc maps CourseListMode {0=Choose, 1=InOrder, 2=Random} -> mRowValues[crsRow] {2, 0, 1}
                static const int kCourseListModeToRowVal2[3] = {2, 0, 1};
                int clampedListMode = (cfg.courseListMode >= 0 && cfg.courseListMode <= 2) ? cfg.courseListMode : 0;
                int desiredRowVal2 = kCourseListModeToRowVal2[clampedListMode];

                if (ruleCooldown > 0) ruleCooldown--;
                else if (st == 3)
                {
                    if (cfg.cpuMode >= 0 && cpuVal != cfg.cpuMode)
                    {
                        if (onBtn != 0 || curRow != cpuRow)
                            nextKeys[0] = PressKeys(KEY_DOWN);
                        else
                            nextKeys[0] = PressKeys(KEY_RIGHT);
                        ruleCooldown = 14;
                    }
                    else if (crsVal != desiredRowVal2)
                    {
                        if (onBtn != 0 || curRow != crsRow)
                            nextKeys[0] = PressKeys(KEY_DOWN);
                        else
                            nextKeys[0] = PressKeys(KEY_RIGHT);
                        ruleCooldown = 14;
                    }
                    else if (onBtn == 0)
                    {
                        nextKeys[0] = PressKeys(KEY_UP);
                        ruleCooldown = 12;
                    }
                    else
                    {
                        rulesConfigured = true;
                        if (cfg.courseListMode == 2 || isBattle)
                            lobbySelectionPass = true;
                        logEvent(frame, "SelectRaceRule configured (cpu=" + std::to_string(cpuVal) +
                                        " rowVal2=" + std::to_string(crsVal) +
                                        " courseListMode=" + std::to_string(cfg.courseListMode) + "); pressing Start button");
                        nextKeys[0] = PressKeys(KEY_A);
                        ruleCooldown = 20;
                    }
                }
            }
            // SelectCharacter / SelectKart / Battle vs VS mode choice
            else if (IsValidMainRamPtr(hSelChar) || IsValidMainRamPtr(hSelKart))
            {
                if (frame % 15 == 0) nextKeys[0] = PressKeys(KEY_A);
            }
            // MultiSelectMember (Waiting for Download Play / Multi-Card guests to join!)
            else if (IsValidMainRamPtr(hMemMenu))
            {
                if (hostClonebootSha256.empty() && !isMultiCard && IsValidMainRamPtr(hConnWork))
                {
                    u32 mbBuf = RamRead32(host, hConnWork + 0x1658);
                    if (IsValidMainRamPtr(mbBuf) &&
                        RamRead32(host, mbBuf + 0x0c) == 0x4A52544Eu &&
                        RamRead32(host, mbBuf + 0x268) != 0)
                    {
                        u32 arm9Size = RamRead32(host, mbBuf + 0x2c);
                        u32 arm7Size = RamRead32(host, mbBuf + 0x3c);
                        if (0x268 + arm9Size + arm7Size <= 0x80000)
                        {
                            const u8* hdrPtr = &host.MainRAM[mbBuf & host.MainRAMMask];
                            const u8* rsaPtr = &host.MainRAM[(mbBuf + 0x160) & host.MainRAMMask];
                            const u8* segPtr = &host.MainRAM[(mbBuf + 0x268) & host.MainRAMMask];
                            std::vector<u8> fullStationMb;
                            fullStationMb.reserve(0x160 + arm9Size + arm7Size + 0x88);
                            fullStationMb.insert(fullStationMb.end(), hdrPtr, hdrPtr + 0x160);
                            fullStationMb.insert(fullStationMb.end(), segPtr, segPtr + arm9Size + arm7Size);
                            fullStationMb.insert(fullStationMb.end(), rsaPtr, rsaPtr + 0x88);
                            hostClonebootSha256 = Sha256::Hex(fullStationMb.data(), fullStationMb.size());
                            hostClonebootRsaSha256 = Sha256::Hex(rsaPtr, 0x88);
                            logEvent(frame, "Captured Host live signed Station Stage-1 MB image (" +
                                            std::to_string(fullStationMb.size()) + " bytes) sha256=" +
                                            hostClonebootSha256 + " rsaSha256=" + hostClonebootRsaSha256);
                        }
                    }
                    else if (IsValidMainRamPtr(mbBuf) &&
                             RamRead32(host, mbBuf) == 0x4952414Du &&
                             RamRead32(host, mbBuf + 0x268) != 0 &&
                             RamRead32(host, mbBuf + 0x268 + 0x0a58) == 0xe12fff1eu)
                    {
                        u32 arm9Size = RamRead32(host, mbBuf + 0x2c);
                        u32 arm7Size = RamRead32(host, mbBuf + 0x3c);
                        u32 romArm9  = RamRead32(host, mbBuf + 0x25c);
                        u32 romArm7  = RamRead32(host, mbBuf + 0x260);
                        if (arm9Size > 0x4400 && romArm9 + arm9Size <= hostRom.size() && romArm7 + arm7Size <= hostRom.size())
                        {
                            std::vector<u8> fullCloneboot;
                            fullCloneboot.reserve(0x160 + arm9Size + arm7Size + 0x88);
                            const u8* hdrPtr = &host.MainRAM[mbBuf & host.MainRAMMask];
                            const u8* rsaPtr = &host.MainRAM[(mbBuf + 0x160) & host.MainRAMMask];
                            const u8* ramArm9Prefix = &host.MainRAM[(mbBuf + 0x268) & host.MainRAMMask];
                            fullCloneboot.insert(fullCloneboot.end(), hdrPtr, hdrPtr + 0x160);
                            fullCloneboot.insert(fullCloneboot.end(), ramArm9Prefix, ramArm9Prefix + 0x4400);
                            fullCloneboot.insert(fullCloneboot.end(), hostRom.begin() + romArm9 + 0x4400, hostRom.begin() + romArm9 + arm9Size);
                            fullCloneboot.insert(fullCloneboot.end(), hostRom.begin() + romArm7, hostRom.begin() + romArm7 + arm7Size);
                            fullCloneboot.insert(fullCloneboot.end(), rsaPtr, rsaPtr + 0x88);
                            hostClonebootSha256 = Sha256::Hex(fullCloneboot.data(), fullCloneboot.size());
                            hostClonebootRsaSha256 = Sha256::Hex(rsaPtr, 0x88);
                            std::ofstream cbofs(cfg.outDir + "/host_transmitted_cloneboot.bin", std::ios::binary);
                            cbofs.write(reinterpret_cast<const char*>(fullCloneboot.data()), static_cast<std::streamsize>(fullCloneboot.size()));
                            logEvent(frame, "Captured Host live Cloneboot payload (" + std::to_string(fullCloneboot.size()) +
                                            " bytes) sha256=" + hostClonebootSha256 + " rsaSha256=" + hostClonebootRsaSha256);
                        }
                    }
                }
                s32 childCount = RamReadS32(host, hMemMenu + 0x74);
                s32 st = RamReadS32(host, hMemMenu + 0x94);
                if (IsValidMainRamPtr(hostSym.gHaxxStationHost) && (frame % 400 == 0))
                {
                    u32 hs = hostSym.gHaxxStationHost;
                    u32 hsPhase = RamRead32(host, hs + 0x04);
                    if (hsPhase != 0)
                    {
                        logEvent(frame, "HS_DIAG hsPhase=" + std::to_string(hsPhase) +
                                        " exp=" + std::to_string(RamRead32(host, hs + 0x14)) +
                                        " appMask=" + std::to_string(RamRead16(host, hs + 0x2a)) +
                                        " bodyMask=" + std::to_string(RamRead16(host, hs + 0x2c)) +
                                        " execMask=" + std::to_string(RamRead16(host, hs + 0x2e)) +
                                        " off=" + std::to_string(RamRead32(host, hs + 0x1c)) +
                                        " err=" + std::to_string(RamRead32(host, hs + 0x6c)));
                    }
                }
                if (childCount >= cfg.numGuests && st == 1)
                {
                    memberReadyFrames++;
                    int settle = (cfg.numGuests > 1) ? 75 : 0;
                    if (memberReadyFrames >= settle && (frame % 18 == 0))
                    {
                        logEvent(frame, "Host MultiSelectMember sees childCount=" + std::to_string(childCount) + "; pressing A to cut off!");
                        nextKeys[0] = PressKeys(KEY_A);
                    }
                }
                else
                {
                    memberReadyFrames = 0;
                }
            }
            // MultiSelectNS (0 = Normal Mode, 1 = Simple Mode!)
            else if (IsValidMainRamPtr(hNsMenu))
            {
                s32 idx = RamReadS32(host, hNsMenu + 0x28);
                s32 st  = RamReadS32(host, hNsMenu + 0x30);
                int targetNs = isMultiCard ? 0 : 1;
                if (st == 1 && (frame % 14 == 0))
                {
                    nextKeys[0] = PressKeys(idx == targetNs ? KEY_A : (idx < targetNs ? KEY_DOWN : KEY_UP));
                }
            }
            // MultiSelectMemberGroup (0..3 = Join Group slots, 4 = CREATE GROUP button!)
            else if (IsValidMainRamPtr(hGrpMenu))
            {
                s32 idx = RamReadS32(host, hGrpMenu + 0x48);
                s32 st  = RamReadS32(host, hGrpMenu + 0x4c);
                if (st == 1 && (frame % 14 == 0))
                {
                    nextKeys[0] = PressKeys(idx == 4 ? KEY_A : KEY_DOWN);
                }
            }
            // SelectGame (Multiplayer post-cutoff: 0 = VS, 1 = Balloon Battle, 2 = Shine Runners; interactive in mState == 4!)
            else if (IsValidMainRamPtr(hSelGame))
            {
                s32 idx   = RamReadS32(host, hSelGame + 0x44);
                s32 st    = RamReadS32(host, hSelGame + 0x54);
                int targetIdx = isBattle ? 1 : 0;
                if (st == 4 && (frame % 14 == 0))
                {
                    nextKeys[0] = PressKeys(idx == targetIdx ? KEY_A : (idx < targetIdx ? KEY_DOWN : KEY_UP));
                }
            }
            // Simple-mode Battle GAME SETTINGS (gSelectBattleRule OK button at (128, 178)) & SelectBattleStage
            else if (isBattle && IsValidMainRamPtr(hMenuSc))
            {
                lobbySelectionPass = true;
                int ph = frame % 48;
                if (ph >= 10 && ph <= 12)
                    setTouch(0, std::make_pair<u16, u16>(128, 178));
                else if (ph == 32)
                    nextKeys[0] = PressKeys(KEY_A);
            }
            // Single-Player MenuScene (screen 1 = SinglePlaySelect: 0=GP, 1=TT, 2=VS, 3=Battle, 4=Mission; other screens = SelectEngine / Ghost)
            else if (isSinglePlayer && IsValidMainRamPtr(hMenuSc))
            {
                s32 scr = RamReadS32(host, hMenuSc + 0x0c);
                if (scr == 1 && (frame % 14 == 0))
                {
                    s32 spIdx = RamReadS32(host, hMenuSc + 0x930);
                    int targetIdx = (cfg.mode == "single-tt") ? 1 : 0;
                    nextKeys[0] = PressKeys(spIdx == targetIdx ? KEY_A : (spIdx < targetIdx ? KEY_DOWN : KEY_UP));
                }
                else if (scr != 1 && (frame % 16 == 0))
                {
                    nextKeys[0] = PressKeys(KEY_A); // Confirm 50cc class / Time Trial prompt
                }
            }
            // TitleScene (state 0 = Press Start, state 2 = Top Menu: 0 = Single Player, 1 = Multiplayer)
            else if (IsValidMainRamPtr(hTitleSc))
            {
                s32 st  = RamReadS32(host, hTitleSc + 0x0c);
                s32 idx = RamReadS32(host, hTitleSc + 0x54);
                int targetTop = isSinglePlayer ? 0 : 1;
                if (st == 0 && (frame % 16 == 0))
                {
                    nextKeys[0] = PressKeys(KEY_A);
                }
                else if (st == 2 && (frame % 14 == 0))
                {
                    nextKeys[0] = PressKeys(idx == targetTop ? KEY_A : (idx < targetTop ? KEY_DOWN : KEY_UP));
                }
            }
            // Nintendo / Mario Kart logo skip fallback
            else if (RamRead32(host, 0x027ffe00) == 0x4952414Du && frame > 280 && (frame % 30 == 0))
            {
                nextKeys[0] = PressKeys(KEY_A);
            }
        }

        if (allDone)
            break;

        // Step Host and Guests within a +-1 frame sliding window (so cross-VBlank MP transactions never deadlock)!
        stepHostAndGuests(frame);
    }

    Harness_NotifyStop();

    if (!allDone)
    {
        SaveCompositeScreenshot(cfg.outDir + "/99_timeout_state.png", consoles);
        if (cfg.verbose)
        {
            std::ofstream r0(cfg.outDir + "/99_host_ram.bin", std::ios::binary);
            r0.write(reinterpret_cast<const char*>(consoles[0]->MainRAM), 0x400000);
            if (numConsoles > 1)
            {
                std::ofstream r1(cfg.outDir + "/99_guest1_ram.bin", std::ios::binary);
                r1.write(reinterpret_cast<const char*>(consoles[1]->MainRAM), 0x400000);
                std::ofstream d1(cfg.outDir + "/99_guest1_dtcm.bin", std::ios::binary);
                d1.write(reinterpret_cast<const char*>(consoles[1]->ARM9.DTCM), 0x4000);
            }
        }
        std::ostringstream pcs;
        pcs << std::hex;
        for (int c = 0; c < numConsoles; ++c)
        {
            u32 exBase = (c == 0) ? hostSym.OSi_ExContext : guestSym.OSi_ExContext;
            pcs << " inst" << c << "[pc=0x" << consoles[c]->ARM9.R[15]
                << ",lr=0x" << consoles[c]->ARM9.R[14]
                << ",sp=0x" << consoles[c]->ARM9.R[13]
                << ",a7pc=0x" << consoles[c]->ARM7.R[15]
                << ",a7lr=0x" << consoles[c]->ARM7.R[14]
                << ",a7sp=0x" << consoles[c]->ARM7.R[13];
            if (IsValidMainRamPtr(exBase) && RamRead32(*consoles[c], exBase + 0x40) != 0)
            {
                pcs << ",ex_pc=0x" << RamRead32(*consoles[c], exBase + 0x40)
                    << ",ex_lr=0x" << RamRead32(*consoles[c], exBase + 0x3c)
                    << ",ex_sp=0x" << RamRead32(*consoles[c], exBase + 0x38)
                    << ",ex_r0=0x" << RamRead32(*consoles[c], exBase + 0x04)
                    << ",ex_r1=0x" << RamRead32(*consoles[c], exBase + 0x08);
            }
            pcs << "]";
        }
        logEvent(finalFrame, "Reached maxFrames (" + std::to_string(cfg.maxFrames) + ") before allDone." + pcs.str());
    }

    if (!cfg.saveFilePath.empty() && consoles[0]->GetNDSSave() != nullptr && consoles[0]->GetNDSSaveLength() > 0)
    {
        std::ofstream savOut(cfg.saveFilePath, std::ios::binary);
        savOut.write(reinterpret_cast<const char*>(consoles[0]->GetNDSSave()),
                     static_cast<std::streamsize>(consoles[0]->GetNDSSaveLength()));
    }

    // Emit structured JSON report
    {
        if (raceCountdownPass) emitPhaseTrace(finalFrame, 0, "countdown");
        if (fullRaceResultsPass) emitPhaseTrace(finalFrame, 0, "3_laps_and_results");
        if (nextRaceTransitionPass) emitPhaseTrace(finalFrame, 0, "next_race");

        std::ofstream jofs(cfg.outDir + "/summary.json");
        jofs << "{\n"
             << "  \"mode\": \"" << cfg.mode << "\",\n"
             << "  \"guests\": " << (isSinglePlayer ? 0 : cfg.numGuests) << ",\n"
             << "  \"course_list_mode\": " << cfg.courseListMode << ",\n"
             << "  \"target_cup\": " << cfg.targetCup << ",\n"
             << "  \"target_course_in_cup\": " << cfg.targetCourseInCup << ",\n"
             << "  \"expected_course_id\": " << expectedCourseId << ",\n"
             << "  \"host_selected_course_id\": " << hostSelectedCourseId << ",\n"
             << "  \"host_setup_course_id\": " << hostSetupCourseId << ",\n"
             << "  \"host_transmitted_course_id\": " << hostTransmittedCourseId << ",\n"
             << "  \"guest_received_course_id\": " << guestReceivedCourseId << ",\n"
             << "  \"guest_boot_type\": " << guestBootType << ",\n"
             << "  \"guest_downloaded_flag_ok\": " << (guestDownloadedFlagOk ? "true" : "false") << ",\n"
             << "  \"guest_simple_flag_ok\": " << (guestSimpleFlagOk ? "true" : "false") << ",\n"
             << "  \"guest_observed_ggid\": " << guestObservedGgid << ",\n"
             << "  \"guest_observed_tgid\": " << guestObservedTgid << ",\n"
             << "  \"guest_observed_channel\": " << int(guestObservedChan) << ",\n"
             << "  \"probe_magic\": " << probeMagic << ",\n"
             << "  \"probe_stage1_ggid\": " << probeStage1Ggid << ",\n"
             << "  \"probe_stage1_tgid\": " << probeStage1Tgid << ",\n"
             << "  \"probe_stage1_channel\": " << int(probeStage1Chan) << ",\n"
             << "  \"probe_arm7_ticks\": " << probeArm7Ticks << ",\n"
             << "  \"probe_arm9_ticks\": " << probeArm9Ticks << ",\n"
             << "  \"host_cloneboot_sha256\": \"" << hostClonebootSha256 << "\",\n"
             << "  \"host_cloneboot_rsa_sha256\": \"" << hostClonebootRsaSha256 << "\",\n"
             << "  \"guest_stage1_header_sha256\": \"" << guestStage1HeaderSha256 << "\",\n"
             << "  \"guest_header_sha256\": \"" << guestHeaderSha256 << "\",\n"
             << "  \"guest_course_buf6_size\": " << guestCourseBuf6Size << ",\n"
             << "  \"guest_course_buf7_size\": " << guestCourseBuf7Size << ",\n"
             << "  \"min_guest_course_heap_free\": " << (minGuestCourseHeapFree == 0xFFFFFFFFu ? -1 : static_cast<long long>(minGuestCourseHeapFree)) << ",\n"
             << "  \"completed_races\": " << completedRaces << ",\n"
             << "  \"frames_elapsed\": " << finalFrame << ",\n"
             << "  \"cups_enabled\": [";
        for (int k = 0; k < 8; ++k) jofs << (k ? ", " : "") << cupsEnabledSnapshot[k];
        jofs << "],\n  \"guest_cups_enabled\": [";
        for (int k = 0; k < 8; ++k) jofs << (k ? ", " : "") << guestCupsEnabledSnapshot[k];
        jofs << "],\n  \"observed_race_course_ids\": [";
        for (std::size_t k = 0; k < observedRaceCourseIds.size(); ++k) jofs << (k ? ", " : "") << observedRaceCourseIds[k];
        jofs << "],\n  \"course_list_snapshot\": [";
        for (std::size_t k = 0; k < courseListSnapshot.size(); ++k) jofs << (k ? ", " : "") << courseListSnapshot[k];
        jofs << "],\n  \"guest_course_list_snapshot\": [";
        for (std::size_t k = 0; k < guestCourseListSnapshot.size(); ++k) jofs << (k ? ", " : "") << guestCourseListSnapshot[k];
        jofs << "],\n"
             << "  \"first_stage_RSA\": \"" << (firstStageRsaPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"stage2_launch\": \"" << (stage2LaunchPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"native_join\": \"" << (nativeJoinPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"menu_course_sync\": \"" << (lobbySelectionPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"MPState16_and_archives\": \"" << (assetTransferPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"countdown\": \"" << (raceCountdownPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"3_laps_and_results\": \"" << (fullRaceResultsPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"next_race\": \"" << (nextRaceTransitionPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"boot_download\": \"" << (bootDownloadPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"lobby_selection\": \"" << (lobbySelectionPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"asset_transfer\": \"" << (assetTransferPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"race_countdown\": \"" << (raceCountdownPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"full_race_results\": \"" << (fullRaceResultsPass ? "PASS" : "FAIL") << "\",\n"
             << "  \"next_race_transition\": \"" << (nextRaceTransitionPass ? "PASS" : "FAIL") << "\"\n"
             << "}\n";
    }

    Harness_ShutdownLocalMP();

    const bool overallPass = bootDownloadPass && lobbySelectionPass && assetTransferPass &&
                             raceCountdownPass && fullRaceResultsPass && nextRaceTransitionPass;
    return overallPass ? 0 : 1;
}
