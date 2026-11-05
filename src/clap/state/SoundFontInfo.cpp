#include "state/SoundFontInfo.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "platform.hpp"

namespace {

uint32_t le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t le16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// INFO strings are zero-terminated (and zero-padded) ASCII; also strips
// trailing whitespace/CRs that some editors leave in comments.
std::string chunkString(const std::vector<unsigned char>& data) {
    size_t n = 0;
    while (n < data.size() && data[n] != 0) ++n;
    std::string s(reinterpret_cast<const char*>(data.data()), n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

void parseInfoList(std::ifstream& f, uint32_t listSize, SoundFontInfo& out) {
    uint32_t consumed = 4; // "INFO" itself
    while (consumed + 8 <= listSize) {
        unsigned char hdr[8];
        if (!f.read(reinterpret_cast<char*>(hdr), 8)) return;
        const uint32_t size = le32(hdr + 4);
        const uint32_t padded = size + (size & 1);
        consumed += 8;
        // Guard against corrupt sizes - INFO entries are tiny in practice
        // (the SF2 spec caps comments at 64 KiB).
        if (size > (1u << 20) || consumed + padded > listSize + 1) return;
        std::vector<unsigned char> data(padded);
        if (padded && !f.read(reinterpret_cast<char*>(data.data()), padded)) return;
        consumed += padded;

        const char* id = reinterpret_cast<const char*>(hdr);
        if (!memcmp(id, "ifil", 4) && size >= 4) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%u.%02u", le16(data.data()), le16(data.data() + 2));
            out.version = buf;
        } else if (!memcmp(id, "isng", 4)) {
            out.engine = chunkString(data);
        } else if (!memcmp(id, "INAM", 4)) {
            out.name = chunkString(data);
        } else if (!memcmp(id, "irom", 4)) {
            out.romName = chunkString(data);
        } else if (!memcmp(id, "ICRD", 4)) {
            out.date = chunkString(data);
        } else if (!memcmp(id, "IENG", 4)) {
            out.author = chunkString(data);
        } else if (!memcmp(id, "IPRD", 4)) {
            out.product = chunkString(data);
        } else if (!memcmp(id, "ICOP", 4)) {
            out.copyright = chunkString(data);
        } else if (!memcmp(id, "ICMT", 4)) {
            out.comment = chunkString(data);
        } else if (!memcmp(id, "ISFT", 4)) {
            out.software = chunkString(data);
        }
    }
}

} // namespace

bool readSoundFontInfo(const std::string& path, SoundFontInfo& out) {
    out = SoundFontInfo{};
    std::error_code ec;
    out.fileSize = static_cast<uint64_t>(std::filesystem::file_size(platform::u8path(path), ec));
    if (ec) out.fileSize = 0;

    std::ifstream f(platform::u8path(path), std::ios::binary);
    if (!f) return false;
    unsigned char riff[12];
    if (!f.read(reinterpret_cast<char*>(riff), 12) || memcmp(riff, "RIFF", 4) != 0)
        return false;
    const uint64_t riffEnd = 8ull + le32(riff + 4);

    // Walk the top-level chunks until the INFO list shows up (it's the
    // first one in every SF2 file, but DLS files may put it elsewhere).
    uint64_t pos = 12;
    while (pos + 8 <= riffEnd) {
        f.seekg(static_cast<std::streamoff>(pos));
        unsigned char hdr[12];
        if (!f.read(reinterpret_cast<char*>(hdr), 8)) break;
        const uint32_t size = le32(hdr + 4);
        if (!memcmp(hdr, "LIST", 4) && size >= 4) {
            if (!f.read(reinterpret_cast<char*>(hdr + 8), 4)) break;
            if (!memcmp(hdr + 8, "INFO", 4)) {
                parseInfoList(f, size, out);
                break;
            }
        }
        pos += 8ull + size + (size & 1);
    }
    return true;
}

uint32_t scanDisabledControllers(const std::string& path) {
    std::ifstream f(platform::u8path(path), std::ios::binary);
    if (!f) return 0;
    unsigned char riff[12];
    if (!f.read(reinterpret_cast<char*>(riff), 12) || memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "sfbk", 4) != 0)
        return 0; // SF2/SF3 only (DLS has no SF2 modulators)
    const uint64_t riffEnd = 8ull + le32(riff + 4);

    // SF2 encodings (sfModSrcOper: index | CC<<7 | D<<8 | P<<9 | type<<10)
    // of the default modulators, with their destination generator.
    struct Default {
        uint16_t src, dest, amtSrc;
        uint32_t bit;
    };
    static const Default kDefaults[] = {
        {0x0081, 6, 0, kCtlModWheel},     // CC1 +uni lin -> vibLfoToPitch
        {0x0587, 48, 0, kCtlVolume},      // CC7 -uni concave -> attenuation
        {0x028A, 17, 0, kCtlPan},         // CC10 +bi lin -> pan
        {0x058B, 48, 0, kCtlExpression},  // CC11 -uni concave -> attenuation
        {0x00DB, 16, 0, kCtlReverb},      // CC91 +uni lin -> reverbSend
        {0x00DD, 15, 0, kCtlChorus},      // CC93 +uni lin -> chorusSend
        {0x020E, 52, 0x0010, kCtlPitchBend}, // wheel +bi lin x bend range -> fineTune
        {0x000D, 6, 0, kCtlAftertouch},   // chan. pressure +uni lin -> vibLfoToPitch
    };
    uint32_t zero = 0, nonZero = 0;

    uint64_t pos = 12;
    while (pos + 12 <= riffEnd) {
        f.seekg(static_cast<std::streamoff>(pos));
        unsigned char hdr[12];
        if (!f.read(reinterpret_cast<char*>(hdr), 12)) break;
        const uint32_t size = le32(hdr + 4);
        if (!memcmp(hdr, "LIST", 4) && !memcmp(hdr + 8, "pdta", 4)) {
            uint64_t sub = pos + 12, end = pos + 8 + size;
            while (sub + 8 <= end) {
                f.seekg(static_cast<std::streamoff>(sub));
                unsigned char sh[8];
                if (!f.read(reinterpret_cast<char*>(sh), 8)) break;
                const uint32_t ssize = le32(sh + 4);
                if (!memcmp(sh, "imod", 4) && ssize >= 10 && ssize < (64u << 20)) {
                    std::vector<unsigned char> mods(ssize);
                    if (!f.read(reinterpret_cast<char*>(mods.data()), ssize)) break;
                    // last record is the terminal one
                    for (size_t i = 0; i + 20 <= ssize; i += 10) {
                        const unsigned char* m = mods.data() + i;
                        const uint16_t src = le16(m), dest = le16(m + 2), amtSrc = le16(m + 6);
                        const int16_t amount = static_cast<int16_t>(le16(m + 4));
                        for (const auto& d : kDefaults)
                            if (src == d.src && dest == d.dest && amtSrc == d.amtSrc)
                                (amount == 0 ? zero : nonZero) |= d.bit;
                    }
                    break;
                }
                sub += 8ull + ssize + (ssize & 1);
            }
            break;
        }
        pos += 8ull + size + (size & 1);
    }
    return zero & ~nonZero;
}

bool isSoundFontFile(const std::string& path) {
    const auto ext = platform::u8string(platform::u8path(path).extension());
    return !platform::strCaseCmp(ext.c_str(), ".sf2") || !platform::strCaseCmp(ext.c_str(), ".sf3") ||
           !platform::strCaseCmp(ext.c_str(), ".dls");
}
