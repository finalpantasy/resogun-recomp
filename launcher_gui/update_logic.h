// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher's update check, without Windows: release versions and their order, the GitHub
// "latest release" JSON, the .sha256 file beside each zip, SHA-256 itself, and which paths of an
// install an update may replace. launcher_gui/main.cpp does the HTTP and the file moves;
// tests/test_launcher_update.cpp checks this part.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace GolfUpdate {

// --- versions --------------------------------------------------------------------------------------

// A release version: "v0.1.0" or "0.1.0", optionally with a pre-release part ("v0.2.0-rc1"),
// which sorts before the release itself. Build metadata after '+' is ignored.
struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre; // empty for a release
};

inline bool ParseVersion(std::string_view text, Version& out) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
    if (const size_t plus = text.find('+'); plus != std::string_view::npos) text = text.substr(0, plus);
    std::string_view core = text, pre;
    if (const size_t dash = text.find('-'); dash != std::string_view::npos) {
        core = text.substr(0, dash);
        pre = text.substr(dash + 1);
        if (pre.empty()) return false;
    }
    int parts[3] = {0, 0, 0}, count = 0;
    size_t i = 0;
    while (count < 3) {
        if (i >= core.size() || core[i] < '0' || core[i] > '9') return false;
        long value = 0;
        while (i < core.size() && core[i] >= '0' && core[i] <= '9') {
            value = value * 10 + (core[i++] - '0');
            if (value > 1000000) return false;
        }
        parts[count++] = (int)value;
        if (i == core.size()) break;
        if (core[i++] != '.') return false;
    }
    if (count != 3 || i != core.size()) return false;
    for (char c : pre)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return false;
    out = {parts[0], parts[1], parts[2], std::string(pre)};
    return true;
}

// -1, 0 or 1 as a is older than, the same as, or newer than b. Pre-release parts compare by their
// dot-separated fields: numbers as numbers, below words.
inline int CompareVersions(const Version& a, const Version& b) {
    const int x[3] = {a.major, a.minor, a.patch}, y[3] = {b.major, b.minor, b.patch};
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;
    if (b.pre.empty()) return -1;
    std::string_view p = a.pre, q = b.pre;
    for (;;) {
        const size_t pe = p.find('.'), qe = q.find('.');
        const std::string_view f = p.substr(0, pe), g = q.substr(0, qe);
        const bool fn = !f.empty() && f.find_first_not_of("0123456789") == std::string_view::npos;
        const bool gn = !g.empty() && g.find_first_not_of("0123456789") == std::string_view::npos;
        if (fn && gn) {
            if (f.size() != g.size()) return f.size() < g.size() ? -1 : 1;
            if (f != g) return f < g ? -1 : 1;
        } else if (fn != gn) {
            return fn ? -1 : 1;
        } else if (f != g) {
            return f < g ? -1 : 1;
        }
        if (pe == std::string_view::npos || qe == std::string_view::npos)
            return pe == qe ? 0 : pe == std::string_view::npos ? -1 : 1;
        p.remove_prefix(pe + 1);
        q.remove_prefix(qe + 1);
    }
}

// Whether `tag` names a newer release than `current`. False when either is not a release version
// (a development build, "abc123-dirty", never updates itself).
inline bool IsNewer(std::string_view tag, std::string_view current) {
    Version t, c;
    return ParseVersion(tag, t) && ParseVersion(current, c) && CompareVersions(t, c) > 0;
}

// At most one automatic check a day: due when on, and a day has passed since the last (or the
// clock went back).
inline bool CheckDue(bool enabled, long long last_check, long long now) {
    return enabled && (last_check <= 0 || now < last_check || now - last_check >= 24 * 60 * 60);
}

// --- SHA-256 ---------------------------------------------------------------------------------------

class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, init, sizeof h_);
        length_ = 0;
        used_ = 0;
    }
    void update(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        length_ += size;
        while (size) {
            const size_t take = std::min(size, sizeof block_ - used_);
            std::memcpy(block_ + used_, p, take);
            used_ += take;
            p += take;
            size -= take;
            if (used_ == sizeof block_) { compress(block_); used_ = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = length_ * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used_ != 56) update(&zero, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = uint8_t(bits >> (56 - 8 * i));
        update(tail, 8);
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (uint32_t word : h_)
            for (int shift = 28; shift >= 0; shift -= 4) out += digits[(word >> shift) & 15];
        reset();
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress(const uint8_t* b) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    uint32_t h_[8];
    uint64_t length_;
    uint8_t block_[64];
    size_t used_;
};

inline std::string Sha256Hex(std::string_view data) {
    Sha256 s;
    s.update(data.data(), data.size());
    return s.hex();
}

// The digest in a .sha256 file: "<64 hex digits>  <file name>" (sha256sum's format; the name may be
// missing or start with '*'). Lowercase, or empty when the text does not start with a digest or
// names another file than `file_name` (when given).
inline std::string ParseSha256File(std::string_view text, std::string_view file_name = {}) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf)
        text.remove_prefix(3);
    while (!text.empty() && (text[0] == ' ' || text[0] == '\t' || text[0] == '\r' || text[0] == '\n')) text.remove_prefix(1);
    if (text.size() < 64) return {};
    std::string digest;
    for (size_t i = 0; i < 64; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
        digest += c;
    }
    std::string_view rest = text.substr(64);
    if (!rest.empty() && rest[0] != ' ' && rest[0] != '\t' && rest[0] != '\r' && rest[0] != '\n') return {};
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) rest.remove_prefix(1);
    if (!rest.empty() && rest[0] == '*') rest.remove_prefix(1);
    const size_t end = rest.find_first_of("\r\n");
    const std::string_view name = rest.substr(0, end);
    if (!file_name.empty() && !name.empty() && name != file_name) return {};
    return digest;
}

// Whether `actual` (hex) is the digest the .sha256 file gives; false when the file gave none.
inline bool HashMatches(std::string_view expected, std::string_view actual) {
    if (expected.size() != 64 || actual.size() != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        char a = expected[i], b = actual[i];
        if (a >= 'A' && a <= 'F') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'F') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// --- the GitHub release JSON -----------------------------------------------------------------------

struct Asset { std::string name, url; };
struct Release { std::string tag, name, notes, page; std::vector<Asset> assets; };

// A small JSON reader for the release object: strings (with \u escapes as UTF-8), numbers, literals,
// arrays and objects; it keeps only the fields Release names.
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : s_(text) {}
    bool release(Release& out) {
        if (!object([&](const std::string& key) {
                if (key == "tag_name") return string_or_null(out.tag);
                if (key == "name") return string_or_null(out.name);
                if (key == "body") return string_or_null(out.notes);
                if (key == "html_url") return string_or_null(out.page);
                if (key == "assets") return array([&] {
                    Asset a;
                    if (!object([&](const std::string& k) {
                            if (k == "name") return string_or_null(a.name);
                            if (k == "browser_download_url") return string_or_null(a.url);
                            return skip();
                        }))
                        return false;
                    out.assets.push_back(a);
                    return true;
                });
                return skip();
            }))
            return false;
        space();
        return i_ == s_.size() && !out.tag.empty();
    }

private:
    void space() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_; }
    bool eat(char c) { space(); if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; } return false; }
    template <class F> bool object(F&& field) {
        if (++depth_ > 64 || !eat('{')) return false;
        if (!eat('}')) {
            do {
                std::string key;
                space();
                if (!string(key) || !eat(':') || !field(key)) return false;
            } while (eat(','));
            if (!eat('}')) return false;
        }
        --depth_;
        return true;
    }
    template <class F> bool array(F&& item) {
        if (++depth_ > 64 || !eat('[')) return false;
        if (!eat(']')) {
            do { if (!item()) return false; } while (eat(','));
            if (!eat(']')) return false;
        }
        --depth_;
        return true;
    }
    static void put_utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    bool hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t v;
                if (!hex4(v)) return false;
                if (v >= 0xd800 && v < 0xdc00 && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                    i_ += 2;
                    uint32_t low;
                    if (!hex4(low) || low < 0xdc00 || low >= 0xe000) return false;
                    v = 0x10000 + ((v - 0xd800) << 10) + (low - 0xdc00);
                } else if (v >= 0xd800 && v < 0xe000) {
                    v = 0xfffd;
                }
                put_utf8(out, v);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool string_or_null(std::string& out) {
        space();
        if (s_.substr(i_, 4) == "null") { i_ += 4; out.clear(); return true; }
        return string(out);
    }
    bool skip() {
        space();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '"') { std::string ignored; return string(ignored); }
        if (c == '{') return object([&](const std::string&) { return skip(); });
        if (c == '[') return array([&] { return skip(); });
        for (std::string_view word : {"true", "false", "null"})
            if (s_.substr(i_, word.size()) == word) { i_ += word.size(); return true; }
        const size_t start = i_;
        while (i_ < s_.size() && std::strchr("+-0123456789.eE", s_[i_])) ++i_;
        return i_ > start;
    }
    std::string_view s_;
    size_t i_ = 0;
    int depth_ = 0;
};

inline bool ParseRelease(std::string_view json, Release& out) {
    out = {};
    return JsonReader(json).release(out);
}

// The Windows zip of a release and its .sha256: the asset named "...-windows-x64.zip" and the
// one named the same plus ".sha256". False when either is missing.
inline bool PickWindowsAssets(const Release& r, Asset& zip, Asset& sha) {
    static constexpr std::string_view suffix = "-windows-x64.zip";
    for (const Asset& a : r.assets) {
        if (a.name.size() <= suffix.size() || a.name.compare(a.name.size() - suffix.size(), suffix.size(), suffix)) continue;
        for (const Asset& b : r.assets) {
            if (b.name == a.name + ".sha256") { zip = a; sha = b; return true; }
        }
    }
    return false;
}

// --- what an update may replace ----------------------------------------------------------------------

// A zip entry's path inside the release's top folder ("everybodys-golf-pc-v0.1.0/libs/x.dll" gives
// "libs/x.dll"), with '\' as '/'; empty for the top folder itself, for a directory, and for any
// path that is absolute, has a drive, "..", "." or an empty part (so nothing lands outside the
// install folder).
inline std::string InstallPath(std::string_view entry) {
    std::string path(entry);
    for (char& c : path) if (c == '\\') c = '/';
    if (path.empty() || path[0] == '/' || path.find(':') != std::string::npos || path.back() == '/') return {};
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return {};
    std::string rest = path.substr(slash + 1);
    size_t start = 0;
    while (start <= rest.size()) {
        const size_t end = rest.find('/', start);
        const std::string_view part = std::string_view(rest).substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") return {};
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return rest;
}

inline bool SameText(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// The player's own files, which an update never writes: data\ (the launcher's saves, settings, logs,
// prepared game and texture packs), and what the command line keeps beside the program (save\,
// game\, log\, shaders\, gpu\, host\, paths.ini, settings.ini).
inline bool PlayerOwned(std::string_view path) {
    const size_t slash = path.find('/');
    const std::string_view first = path.substr(0, slash);
    for (std::string_view folder : {"data", "save", "saves", "game", "log", "logs", "shaders", "gpu", "host", "dump",
                                    "textures", "backups", "update"})
        if (SameText(first, folder) && slash != std::string_view::npos) return true;
    for (std::string_view file : {"paths.ini", "settings.ini", "launcher.ini"})
        if (SameText(path, file)) return true;
    return false;
}

} // namespace GolfUpdate
