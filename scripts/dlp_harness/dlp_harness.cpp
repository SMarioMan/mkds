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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

static void SaveCompositeScreenshot(const std::string& path, const std::vector<std::unique_ptr<melonDS::NDS>>& consoles)
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
    int reverseTimer = 0;
    s32 lastSteerDir = 1;

    void Reset()
    {
        currentLinkAddr = 0;
        stuckFrames = 0;
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
            // During countdown, hold A after ~120 elapsed frames or keep neutral so we don't spin out!
            Reset();
            return PressKeys(0);
        }

        int entryIdx = static_cast<std::int8_t>(RamRead8(nds, raceSetup + 0x62));
        if (entryIdx < 0 || entryIdx >= 8) entryIdx = 0;

        u32 progKart = raceProg + 0x14 + entryIdx * 0x8c;
        s32 status = RamReadS32(nds, progKart + 0x00);
        if (status != 0)
        {
            // Kart already finished race! Game takes over post-goal driving automatically.
            return PressKeys(0);
        }

        u32 kart = kartsPtr + entryIdx * 0x5a8;
        double kx = RamReadS32(nds, kart + 0x80) / 4096.0;
        double ky = RamReadS32(nds, kart + 0x84) / 4096.0;
        double kz = RamReadS32(nds, kart + 0x88) / 4096.0;

        // Row 0 of mUnk_120_m (+0x120) is local Left (+X), Row 2 (+0x138) is local Forward (+Z)
        double lx = RamReadS32(nds, kart + 0x120) / 4096.0;
        double lz = RamReadS32(nds, kart + 0x128) / 4096.0;
        double fx = RamReadS32(nds, kart + 0x138) / 4096.0;
        double fz = RamReadS32(nds, kart + 0x140) / 4096.0;
        double fwdSpeed = RamReadS32(nds, kart + 0x2a8) / 4096.0;
        s32 kartState = RamReadS32(nds, kart + 0x180);

        u32 linksBase = RamRead32(nds, courseMap + 0xa0);
        u16 pointCount = RamRead16(nds, courseMap + 0x68);
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
            rad = std::max(120.0, RamReadS32(nds, ptAddr + 0x0c) / 4096.0);
            return true;
        };

        double tx = 0, ty = 0, tz = 0, trad = 200.0;
        bool needSnap = !getLinkPos(currentLinkAddr, tx, ty, tz, trad);
        if (!needSnap)
        {
            double dx = tx - kx, dz = tz - kz;
            double distXZ = std::hypot(dx, dz);
            if (distXZ > 2600.0 || std::abs(ty - ky) > 1200.0)
                needSnap = true;
        }

        if (needSnap)
        {
            double bestScore = 1e18;
            u32 bestLink = linksBase;
            for (u16 i = 0; i < pointCount; ++i)
            {
                u32 cand = linksBase + i * 0x28;
                double px, py, pz, prad;
                if (!getLinkPos(cand, px, py, pz, prad)) continue;
                double dx = px - kx, dz = pz - kz, dy = std::abs(py - ky);
                double distXZ = std::hypot(dx, dz);
                double dotF = (distXZ > 1.0) ? ((dx * fx + dz * fz) / distXZ) : 1.0;
                double penalty = (dotF < -0.2) ? 1200.0 : 0.0;
                double score = distXZ + 3.0 * dy + penalty;
                if (score < bestScore)
                {
                    bestScore = score;
                    bestLink = cand;
                }
            }
            // Advance 1 link forward so we aim ahead of the kart rather than slightly behind it
            u32 next0 = RamRead32(nds, bestLink + 0x00);
            currentLinkAddr = IsValidMainRamPtr(next0) ? next0 : bestLink;
            getLinkPos(currentLinkAddr, tx, ty, tz, trad);
        }

        // Advance along the RacePointLink chain when within reach or already past the waypoint plane
        for (int step = 0; step < 3; ++step)
        {
            u32 next0 = RamRead32(nds, currentLinkAddr + 0x00);
            if (!IsValidMainRamPtr(next0)) break;
            double nx, ny, nz, nrad;
            if (!getLinkPos(next0, nx, ny, nz, nrad)) break;

            double dx = tx - kx, dz = tz - kz;
            double distXZ = std::hypot(dx, dz);
            double segX = nx - tx, segZ = nz - tz;
            double passedDot = (kx - tx) * segX + (kz - tz) * segZ;
            double reach = std::clamp(trad * 0.85, 220.0, 520.0);
            if (distXZ < reach || (distXZ < 750.0 && passedDot > 0.0))
            {
                currentLinkAddr = next0;
                tx = nx; ty = ny; tz = nz; trad = nrad;
            }
            else
            {
                break;
            }
        }

        // Blend slightly toward next waypoint when close for smooth apex entry
        u32 nextLink = RamRead32(nds, currentLinkAddr + 0x00);
        double nx, ny, nz, nrad;
        double aimX = tx, aimZ = tz;
        if (IsValidMainRamPtr(nextLink) && getLinkPos(nextLink, nx, ny, nz, nrad))
        {
            double dCur = std::hypot(tx - kx, tz - kz);
            if (dCur < 500.0)
            {
                double w = std::clamp(1.0 - dCur / 500.0, 0.0, 0.55);
                aimX = tx * (1.0 - w) + nx * w;
                aimZ = tz * (1.0 - w) + nz * w;
            }
        }

        // Unstick recovery if wedged against wall/obstacle
        if (reverseTimer > 0)
        {
            reverseTimer--;
            u32 revKeys = KEY_B | (lastSteerDir > 0 ? KEY_RIGHT : KEY_LEFT);
            return PressKeys(revKeys);
        }
        if (kartState == 0 && std::abs(fwdSpeed) < 0.6)
        {
            stuckFrames++;
            if (stuckFrames > 50)
            {
                stuckFrames = 0;
                reverseTimer = 28;
                return PressKeys(KEY_B);
            }
        }
        else
        {
            stuckFrames = 0;
        }

        double dx = aimX - kx;
        double dz = aimZ - kz;
        double len = std::hypot(dx, dz);
        if (len < 1e-3) return PressKeys(KEY_A);
        dx /= len;
        dz /= len;

        // Project target direction onto kart's local Forward (+Z) and Left (+X)
        double projFwd  = dx * fx + dz * fz;
        double projLeft = dx * lx + dz * lz;

        u32 keys = KEY_A;
        if (projLeft > 0.065 || (projFwd < 0.0 && projLeft >= 0.0))
        {
            keys |= KEY_LEFT;
            lastSteerDir = 1;
        }
        else if (projLeft < -0.065 || (projFwd < 0.0 && projLeft < 0.0))
        {
            keys |= KEY_RIGHT;
            lastSteerDir = -1;
        }

        // Use items automatically if held (press X periodically) so item boxes/shells get exercised too!
        if ((RamReadS32(nds, raceProg + 0x00) % 90) == 45)
        {
            keys |= KEY_X;
        }

        // Ease throttle on very sharp hairpins so kart stays cleanly on narrow bridges/edges
        if (projFwd < 0.35 && fwdSpeed > 5.5)
        {
            keys &= ~KEY_A;
            if (projFwd < -0.15 && fwdSpeed > 3.5)
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
    std::string mode = "simple-vs"; // simple-vs, simple-battle, normal-vs, single-gp, single-tt
    int numGuests = 1;
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
        else if (arg == "--mode") cfg.mode = nextStr();
        else if (arg == "--guests") cfg.numGuests = std::stoi(nextStr());
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
    std::vector<std::unique_ptr<melonDS::NDS>> consoles(numConsoles);

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

        consoles[c] = std::make_unique<melonDS::NDS>(std::move(ndsArgs), &instIds[c]);

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
    bool bootDownloadPass = isSinglePlayer || loadedFromLobbyState;
    bool lobbySelectionPass = false;
    bool assetTransferPass = false;
    bool raceCountdownPass = false;
    bool fullRaceResultsPass = false;
    bool nextRaceTransitionPass = !cfg.checkNextRace;

    std::string hostClonebootSha256;
    std::string hostClonebootRsaSha256;
    std::string guestHeaderSha256;
    u16 guestBootType = 0;
    s32 hostSelectedCourseId = -1;
    s32 hostSetupCourseId = -1;
    s32 hostTransmittedCourseId = -1;
    s32 guestReceivedCourseId = -1;
    std::array<s32, 8> cupsEnabledSnapshot {};
    std::vector<int> courseListSnapshot;
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

    std::ofstream telemetry(cfg.outDir + "/telemetry.log");
    auto logEvent = [&](int frame, const std::string& msg) {
        telemetry << "[frame " << std::setw(6) << frame << "] " << msg << "\n";
        telemetry.flush();
        if (cfg.verbose)
        {
            std::cout << "[frame " << std::setw(6) << frame << "] " << msg << "\n";
        }
    };

    std::vector<KartAutopilot> autopilots(numConsoles);
    std::vector<std::atomic<u32>> nextKeys(numConsoles);
    for (int c = 0; c < numConsoles; ++c) nextKeys[c].store(PressKeys(0));
    std::mutex touchMutex;
    std::vector<std::optional<std::pair<u16, u16>>> nextTouch(numConsoles, std::nullopt);

    std::atomic<bool> stopWorkers{false};
    std::barrier syncStart(numConsoles + 1);
    std::barrier syncEnd(numConsoles + 1);

    auto setTouch = [&](int idx, std::optional<std::pair<u16, u16>> pt) {
        std::lock_guard<std::mutex> lk(touchMutex);
        nextTouch[idx] = pt;
    };

    std::vector<std::thread> workers;
    workers.reserve(numConsoles);
    for (int c = 0; c < numConsoles; ++c)
    {
        workers.emplace_back([&, c]() {
            melonDS::NDS::Current = consoles[c].get();
            for (;;)
            {
                syncStart.arrive_and_wait();
                if (stopWorkers.load(std::memory_order_acquire))
                    break;
                consoles[c]->SetKeyMask(nextKeys[c].load(std::memory_order_relaxed));
                if (nextTouch[c].has_value())
                    consoles[c]->TouchScreen(nextTouch[c]->first, nextTouch[c]->second);
                else
                    consoles[c]->ReleaseScreen();
                Harness_NotifyFrameBegin(c);
                consoles[c]->RunFrame();
                Harness_NotifyFrameEnd(c);
                syncEnd.arrive_and_wait();
            }
        });
    }

    auto quiesceGuestsAndRun = [&](auto&& fn) {
        fn();
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
            nextKeys[c].store(PressKeys(0), std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lk(touchMutex);
            for (int c = 0; c < numConsoles; ++c) nextTouch[c] = std::nullopt;
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

        // Check if Download Play Guest(s) finished Firmware RSA check & booted MKDS child image!
        if (!bootDownloadPass && numConsoles > 1)
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
            }
        }

        // --- Guest Firmware Download Play UI driver (before Guest boots MKDS) ---
        for (int g = 1; g < numConsoles; ++g)
        {
            const auto& guest = *consoles[g];
            if (!isMultiCard && RamRead16(guest, 0x027ffc40) != 2)
            {
                // Native DS Firmware boot sequence on Guest:
                // 1) Frame 220..280: tap Health & Safety screen after Nintendo DS boot chime finishes
                // 2) Frame 340..360: tap "DS Download Play" icon on DS Home Menu (right-center button: x=185, y=96)
                // 3) Once Host advertises MKDS beacon (Host gMultiSelectMember != NULL), tap slot 0 (x=128, y=48)
                //    then confirm "Yes" to download (press A)!
                u32 hostMember = RamRead32(host, hostSym.gMultiSelectMember);
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
                else if (frame == 440)
                {
                    SaveCompositeScreenshot(cfg.outDir + "/00b_firmware_dlp_list.png", consoles);
                }
                else if (frame >= 450 && IsValidMainRamPtr(hostMember))
                {
                    // Cycle cleanly: tap game banner at (128, 48), wait 40 frames, press A to confirm "Yes"
                    int phase = (frame - 450) % 100;
                    if (phase >= 20 && phase <= 23)
                        setTouch(g, std::make_pair<u16, u16>(128, 48));
                    else if (phase >= 60 && phase <= 63)
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
                u64 cs = 0, hr = 0, rs = 0, rr = 0;
                Harness_GetMPStats(cs, hr, rs, rr);
                logEvent(frame, "MP_DIAG hMem70=" + std::to_string(hMem70) +
                                " hMp=" + std::to_string(hMp) + " gMp=" + std::to_string(gMp) +
                                " 1e34=" + std::to_string(h1e34) + "/" + std::to_string(g1e34) +
                                " gErr=" + std::to_string(g164f) +
                                " hRomSt=" + std::to_string(hRomSt) + " gRomSt=" + std::to_string(gRomSt) +
                                " gArcSz=" + std::to_string(gArcSz) +
                                " mp[cmd=" + std::to_string(cs) + ",hRecv=" + std::to_string(hr) +
                                ",rSent=" + std::to_string(rs) + ",rRecv=" + std::to_string(rr) + "]");
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
                }
                else
                {
                    assetTransferPass = false;
                    raceCountdownPass = false;
                    fullRaceResultsPass = false;
                    nextRaceTransitionPass = false;
                    desyncFrames = 0;
                    logEvent(frame, "COURSE_DESYNC: hostSetupCourseId=" + std::to_string(hostSetupCourseId) +
                                    " guestReceivedCourseId=" + std::to_string(guestReceivedCourseId) +
                                    " expectedCourseId=" + std::to_string(expectedCourseId) +
                                    " guestBuf6=" + std::to_string(guestCourseBuf6Size) +
                                    " guestBuf7=" + std::to_string(guestCourseBuf7Size) +
                                    " minGuestCourseHeapFree=" + std::to_string(minGuestCourseHeapFree));
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
                        s32 hLap = RamReadS32(host, hRaceProg + 0x14 + 0x24);
                        s32 hStat = RamReadS32(host, hRaceProg + 0x14 + 0x00);
                        s32 gLap = (numConsoles > 1) ? RamReadS32(*consoles[1], RamRead32(*consoles[1], guestSym.gRaceProgress) + 0x14 + 0x8c + 0x24) : -1;
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

        // Step all consoles 1 frame simultaneously in lockstep!
        Harness_CaptureFrameStarts();
        syncStart.arrive_and_wait();
        syncEnd.arrive_and_wait();
    }

    stopWorkers.store(true, std::memory_order_release);
    syncStart.arrive_and_wait();
    for (auto& th : workers) th.join();

    if (!allDone)
    {
        SaveCompositeScreenshot(cfg.outDir + "/99_timeout_state.png", consoles);
        std::ostringstream pcs;
        pcs << std::hex;
        for (int c = 0; c < numConsoles; ++c)
        {
            u32 exBase = (c == 0) ? hostSym.OSi_ExContext : guestSym.OSi_ExContext;
            pcs << " inst" << c << "[pc=0x" << consoles[c]->ARM9.R[15]
                << ",lr=0x" << consoles[c]->ARM9.R[14]
                << ",sp=0x" << consoles[c]->ARM9.R[13];
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
             << "  \"host_cloneboot_sha256\": \"" << hostClonebootSha256 << "\",\n"
             << "  \"host_cloneboot_rsa_sha256\": \"" << hostClonebootRsaSha256 << "\",\n"
             << "  \"guest_header_sha256\": \"" << guestHeaderSha256 << "\",\n"
             << "  \"guest_course_buf6_size\": " << guestCourseBuf6Size << ",\n"
             << "  \"guest_course_buf7_size\": " << guestCourseBuf7Size << ",\n"
             << "  \"min_guest_course_heap_free\": " << (minGuestCourseHeapFree == 0xFFFFFFFFu ? -1 : static_cast<long long>(minGuestCourseHeapFree)) << ",\n"
             << "  \"completed_races\": " << completedRaces << ",\n"
             << "  \"frames_elapsed\": " << finalFrame << ",\n"
             << "  \"cups_enabled\": [";
        for (int k = 0; k < 8; ++k) jofs << (k ? ", " : "") << cupsEnabledSnapshot[k];
        jofs << "],\n  \"observed_race_course_ids\": [";
        for (std::size_t k = 0; k < observedRaceCourseIds.size(); ++k) jofs << (k ? ", " : "") << observedRaceCourseIds[k];
        jofs << "],\n  \"course_list_snapshot\": [";
        for (std::size_t k = 0; k < courseListSnapshot.size(); ++k) jofs << (k ? ", " : "") << courseListSnapshot[k];
        jofs << "],\n"
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
