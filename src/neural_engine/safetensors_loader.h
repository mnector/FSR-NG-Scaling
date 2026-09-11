#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace fsrng {

// Minimal SHA-256 (self-contained, no external libs).
class Sha256 {
public:
    static std::string HashBytes(const uint8_t* data, size_t len);
    static std::string HashFile(const std::string& path);
    static std::string ToHex(const uint8_t digest[32]);
};

// A single tensor descriptor parsed from the SafeTensors JSON header.
struct TensorMeta {
    std::string dtype;      // e.g. F32, F16, U8, I64
    std::vector<uint64_t> shape;
    size_t offset = 0;      // byte offset within file (after JSON header)
    size_t byteLen = 0;
};

// Secure SafeTensors loader: parses the JSON metadata header, validates the
// on-disk SHA-256 against an expected value when provided, and exposes tensor
// data as contiguous CPU buffers. Does NOT touch the GPU or graphics stack.
class SafetensorsLoader {
public:
    // Load + validate a model file. Returns false on any failure (missing file,
    // bad header, SHA mismatch).
    bool Load(const std::string& path, const std::string& expectedSha256 = "");

    bool loaded() const { return loaded_; }
    const std::string& lastError() const { return lastError_; }

    // Accessors.
    const std::map<std::string, TensorMeta>& tensors() const { return tensors_; }
    const std::vector<uint8_t>& raw() const { return raw_; }

    // Returns a pointer into the loaded buffer for the named tensor, or nullptr.
    const uint8_t* GetData(const std::string& name);

    // Extracts tensor data converted to FP32. Handles F16 and F32 source dtypes.
    bool GetTensorF32(const std::string& name, std::vector<float>& out, size_t maxElements = 0);

    // Fast half-to-single precision conversion
    static void ConvertF16ToF32(const uint16_t* in, float* out, size_t count);

    uint64_t headerLength() const { return headerLen_; }

private:
    bool loaded_ = false;
    uint64_t headerLen_ = 0;
    std::string lastError_;
    std::vector<uint8_t> raw_;
    std::map<std::string, TensorMeta> tensors_;

    // Very small JSON parser sufficient for SafeTensors headers.
    bool ParseHeader(const std::string& json);
};

} // namespace fsrng
