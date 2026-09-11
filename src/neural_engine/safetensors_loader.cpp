#include "safetensors_loader.h"
#include <fstream>
#include <sstream>
#include <cstring>

namespace fsrng {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), self-contained.
// ---------------------------------------------------------------------------
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(const uint8_t* block, uint32_t state[8]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | (block[i*4+2] << 8) | block[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

std::string Sha256::ToHex(const uint8_t digest[32]) {
    static const char* hex = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[i*2]   = hex[digest[i] >> 4];
        out[i*2+1] = hex[digest[i] & 0xf];
    }
    return out;
}

std::string Sha256::HashBytes(const uint8_t* data, size_t len) {
    uint32_t state[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    size_t processed = 0;
    while (len - processed >= 64) {
        sha256_block(data + processed, state);
        processed += 64;
    }
    // Final block with padding.
    uint8_t buf[128];
    size_t rem = len - processed;
    std::memcpy(buf, data + processed, rem);
    buf[rem] = 0x80;
    size_t pad = (rem < 56) ? 56 - rem : 120 - rem;
    std::memset(buf + rem + 1, 0, pad - 1);
    uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i)
        buf[pad + i] = static_cast<uint8_t>(bits >> (56 - i*8));
    sha256_block(buf, state);

    uint8_t digest[32];
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            digest[i*4+j] = static_cast<uint8_t>(state[i] >> (24 - j*8));
    return ToHex(digest);
}

std::string Sha256::HashFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(data.data()), size)) return "";
    return HashBytes(data.data(), data.size());
}

// ---------------------------------------------------------------------------
// SafeTensors parsing.
// ---------------------------------------------------------------------------
static std::string TrimStr(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Extract the value of a top-level key from the JSON header. Handles string or
// array-of-int values. Returns false if not found.
static bool JsonGet(const std::string& json, const std::string& key, std::string& out) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == ':')) ++p;
    if (p >= json.size()) return false;
    if (json[p] == '"') { // string value
        ++p;
        std::string val;
        while (p < json.size() && json[p] != '"') {
            if (json[p] == '\\' && p + 1 < json.size()) { val += json[p+1]; ++p; }
            else val += json[p];
            ++p;
        }
        out = val;
        return true;
    }
    // array or number: read until matching close bracket/brace.
    size_t start = p;
    if (json[p] == '[') {
        int depth = 0;
        for (; p < json.size(); ++p) {
            if (json[p] == '[') ++depth;
            else if (json[p] == ']') { --depth; if (depth == 0) { ++p; break; } }
        }
    } else { // scalar number
        while (p < json.size() && json[p] != ',' && json[p] != '}') ++p;
    }
    out = TrimStr(json.substr(start, p - start));
    return true;
}

bool SafetensorsLoader::ParseHeader(const std::string& json) {
    tensors_.clear();

    size_t pos = 0;
    size_t outer = json.find('{');
    if (outer == std::string::npos) return false;
    pos = outer + 1;

    while (pos < json.size()) {
        size_t nameStart = json.find('"', pos);
        if (nameStart == std::string::npos) break;
        size_t nameEnd = json.find('"', nameStart + 1);
        if (nameEnd == std::string::npos) break;
        std::string name = json.substr(nameStart + 1, nameEnd - nameStart - 1);
        pos = nameEnd + 1;

        size_t colon = json.find(':', pos);
        if (colon == std::string::npos) break;
        size_t brace = json.find('{', colon);
        if (brace == std::string::npos) break;

        // Find matching closing brace
        int depth = 0;
        size_t endBrace = brace;
        for (; endBrace < json.size(); ++endBrace) {
            if (json[endBrace] == '{') ++depth;
            else if (json[endBrace] == '}') {
                --depth;
                if (depth == 0) {
                    ++endBrace;
                    break;
                }
            }
        }

        if (name != "__metadata__") {
            std::string block = json.substr(brace, endBrace - brace);
            TensorMeta meta;
            std::string dtype, offsets;
            if (JsonGet(block, "dtype", dtype)) meta.dtype = dtype;
            if (JsonGet(block, "data_offsets", offsets)) {
                size_t c1 = offsets.find(',');
                if (c1 != std::string::npos) {
                    try {
                        meta.offset  = std::stoull(TrimStr(offsets.substr(0, c1)));
                        meta.byteLen = std::stoull(TrimStr(offsets.substr(c1 + 1))) - meta.offset;
                    } catch (...) {}
                }
            }

            std::string shape;
            if (JsonGet(block, "shape", shape)) {
                size_t s = shape.find('[');
                size_t e = shape.find(']');
                if (s != std::string::npos && e != std::string::npos) {
                    std::stringstream ss(shape.substr(s + 1, e - s - 1));
                    std::string tok;
                    while (std::getline(ss, tok, ',')) {
                        tok = TrimStr(tok);
                        if (!tok.empty()) meta.shape.push_back(std::stoull(tok));
                    }
                }
            }

            tensors_[name] = meta;
        }

        pos = endBrace;
    }
    return !tensors_.empty();
}

bool SafetensorsLoader::Load(const std::string& path, const std::string& expectedSha256) {
    raw_.clear();
    tensors_.clear();
    lastError_.clear();

    std::ifstream f(path, std::ios::binary);
    if (!f) { lastError_ = "cannot open file: " + path; return false; }

    // Read entire file.
    f.seekg(0, std::ios::end);
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    raw_.resize(static_cast<size_t>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(raw_.data()), size)) {
        lastError_ = "failed to read file";
        return false;
    }

    // Verify SHA-256 if requested.
    if (!expectedSha256.empty()) {
        std::string actual = Sha256::HashBytes(raw_.data(), raw_.size());
        if (actual != expectedSha256) {
            lastError_ = "SHA-256 mismatch: expected " + expectedSha256 + " got " + actual;
            return false;
        }
    }

    // SafeTensors layout: JSON header length as u64 LE, then the JSON bytes.
    if (raw_.size() < 8) { lastError_ = "file too small"; return false; }
    uint64_t headerLen = 0;
    std::memcpy(&headerLen, raw_.data(), 8);
    if (headerLen == 0 || 8 + headerLen > raw_.size()) {
        lastError_ = "invalid header length";
        return false;
    }
    headerLen_ = headerLen;

    std::string json(reinterpret_cast<const char*>(raw_.data() + 8), headerLen);
    if (!ParseHeader(json)) { lastError_ = "failed to parse JSON header"; return false; }

    loaded_ = true;
    return true;
}

const uint8_t* SafetensorsLoader::GetData(const std::string& name) {
    auto it = tensors_.find(name);
    if (it == tensors_.end()) return nullptr;
    size_t absOff = 8 + headerLen_ + it->second.offset;
    if (absOff >= raw_.size() || absOff + it->second.byteLen > raw_.size()) return nullptr;
    return raw_.data() + absOff;
}

void SafetensorsLoader::ConvertF16ToF32(const uint16_t* in, float* out, size_t count) {
    if (!in || !out || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        uint16_t h = in[i];
        uint32_t sign = (h >> 15) & 0x0001;
        uint32_t exp  = (h >> 10) & 0x001f;
        uint32_t mant = h & 0x03ff;
        if (exp == 0) {
            out[i] = (sign ? -0.0f : 0.0f);
        } else if (exp == 31) {
            out[i] = (sign ? -1.0f : 1.0f);
        } else {
            uint32_t fExp = exp + (127 - 15);
            uint32_t fMant = mant << 13;
            uint32_t fBits = (sign << 31) | (fExp << 23) | fMant;
            float val = 0.0f;
            std::memcpy(&val, &fBits, sizeof(float));
            out[i] = val;
        }
    }
}

bool SafetensorsLoader::GetTensorF32(const std::string& name, std::vector<float>& out, size_t maxElements) {
    auto it = tensors_.find(name);
    if (it == tensors_.end()) return false;
    const uint8_t* raw = GetData(name);
    if (!raw) return false;

    if (it->second.dtype == "F16") {
        size_t numElements = it->second.byteLen / 2;
        if (maxElements > 0 && numElements > maxElements) numElements = maxElements;
        out.resize(numElements);
        ConvertF16ToF32(reinterpret_cast<const uint16_t*>(raw), out.data(), numElements);
        return true;
    } else if (it->second.dtype == "F32") {
        size_t numElements = it->second.byteLen / 4;
        if (maxElements > 0 && numElements > maxElements) numElements = maxElements;
        out.resize(numElements);
        std::memcpy(out.data(), raw, numElements * sizeof(float));
        return true;
    }
    return false;
}

} // namespace fsrng
