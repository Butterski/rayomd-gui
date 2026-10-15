// SPDX-License-Identifier: Apache-2.0
// The JPEG decoder (see jpeg.h). Written from ITU-T T.81 (the marker syntax of annex B, the
// Huffman decoding of annex F and the progressive procedures of annex G), with libjpeg's
// conventions where T.81 leaves a choice: the Adobe APP14 colour transform, refusing a Huffman
// table that uses an all-ones code, quantization tables latched at a component's first scan.
#include "pdf/jpeg.h"

#include <cstring>

#include "gfx/kernels.h"

namespace rgui::pdf {
namespace {

// The natural (row-major) index of the k-th coefficient in zigzag order.
constexpr uint8_t kNaturalOrder[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr int kMaxDimension = 65535;
constexpr uint64_t kMaxPixels = uint64_t(1) << 28;
// More scans than any encoder writes (a fully successive-approximated progressive image has a
// few dozen); bounds the work of a hostile file.
constexpr int kMaxScans = 1000;
constexpr int kLookupBits = 9;

int16_t Saturate16(int32_t v) {
    return static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

// T.81 F.2.2.1 EXTEND: the s-bit magnitude v as a signed value.
int Extend(int v, int s) {
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

struct HuffmanTable {
    bool defined = false;
    int total = 0;
    // (length << 8) | symbol for every 9-bit prefix that starts with a code of length <= 9;
    // 0 where the code is longer.
    uint16_t lookup[1 << kLookupBits] = {};
    int32_t maxCode[17] = {};      // the largest code of each length, -1 when there is none
    int32_t valueOffset[17] = {};  // values[code + valueOffset[length]]
    uint8_t values[256] = {};
};

// Builds the canonical code of T.81 C.2 from the 16 counts. False for a table whose codes do
// not fit their lengths or use an all-ones code (libjpeg refuses those too).
bool BuildHuffman(const uint8_t* counts, const uint8_t* symbols, int total, HuffmanTable& table) {
    table = HuffmanTable{};
    int32_t code = 0;
    int k = 0;
    for (int length = 1; length <= 16; length++) {
        const int n = counts[length - 1];
        table.valueOffset[length] = k - code;
        table.maxCode[length] = n ? code + n - 1 : -1;
        for (int i = 0; i < n; i++, k++, code++) {
            if (length <= kLookupBits) {
                const int shift = kLookupBits - length;
                const int base = code << shift;
                if (base + (1 << shift) > (1 << kLookupBits)) return false;
                for (int j = 0; j < (1 << shift); j++) {
                    table.lookup[base + j] = static_cast<uint16_t>((length << 8) | symbols[k]);
                }
            }
        }
        if (code >= (int32_t(1) << length)) return false;
        code <<= 1;
    }
    std::memcpy(table.values, symbols, static_cast<size_t>(total));
    table.total = total;
    table.defined = true;
    return true;
}

// Reads the entropy-coded data of a scan: removes the stuffed zero after each 0xFF and stops at
// a marker. Past the marker (or the end of the data) it supplies zero bits and counts them; a
// decoder that consumed any of them ran past its data.
struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;     // the next byte to read; at the 0xFF of the marker once `marker` is set
    uint64_t buffer = 0;  // left-aligned
    int count = 0;        // valid bits in `buffer`
    int fake = 0;         // how many of them, at the end, are padding past the data
    bool marker = false;

    void Start(size_t at) {
        pos = at;
        buffer = 0;
        count = 0;
        fake = 0;
        marker = false;
    }

    void Fill() {
        while (count <= 56) {
            uint32_t byte = 0;
            if (!marker && pos < size) {
                byte = data[pos];
                if (byte == 0xFF) {
                    size_t next = pos + 1;
                    while (next < size && data[next] == 0xFF) next++;
                    if (next < size && data[next] == 0x00) {
                        pos = next + 1;
                    } else {
                        marker = true;
                        pos = next - 1;
                        byte = 0;
                        fake += 8;
                    }
                } else {
                    pos++;
                }
            } else {
                fake += 8;
            }
            buffer |= static_cast<uint64_t>(byte) << (56 - count);
            count += 8;
        }
    }

    bool Overrun() const { return count < fake; }

    uint32_t Peek16() {
        if (count < 16) Fill();
        return static_cast<uint32_t>(buffer >> 48);
    }

    void Consume(int n) {
        buffer <<= n;
        count -= n;
    }

    // n in [0, 16].
    int Bits(int n) {
        if (n == 0) return 0;
        if (count < n) Fill();
        const int v = static_cast<int>(buffer >> (64 - n));
        Consume(n);
        return v;
    }

    int Bit() { return Bits(1); }

    // The symbol of the next code, -1 for a code the table does not have.
    int Decode(const HuffmanTable& table) {
        const uint32_t bits16 = Peek16();
        const uint16_t entry = table.lookup[bits16 >> (16 - kLookupBits)];
        if (entry) {
            Consume(entry >> 8);
            return entry & 0xFF;
        }
        for (int length = kLookupBits + 1; length <= 16; length++) {
            const int32_t code = static_cast<int32_t>(bits16 >> (16 - length));
            if (code <= table.maxCode[length]) {
                const int32_t index = code + table.valueOffset[length];
                if (index < 0 || index >= table.total) return -1;
                Consume(length);
                return table.values[index];
            }
        }
        return -1;
    }
};

struct Component {
    int id = 0;
    int h = 1;
    int v = 1;
    int quantSlot = 0;
    int widthBlocks = 0;   // blocks a non-interleaved scan codes: ceil(component width / 8)
    int heightBlocks = 0;
    int blocksW = 0;       // blocks of the MCU-padded component (planes and coefficients)
    int blocksH = 0;
    bool latched = false;  // quantization table taken (at the component's first scan)
    uint16_t quant[64] = {};  // natural order
    int dcTable = 0;
    int acTable = 0;
    int dcPredictor = 0;
    std::vector<int16_t> coefficients;  // progressive: blocksW * blocksH blocks of 64
    std::vector<uint8_t> plane;         // blocksW * 8 by blocksH * 8 samples

    size_t PlaneStride() const { return static_cast<size_t>(blocksW) * 8; }
};

enum class ScanMode { Sequential, DcFirst, DcRefine, AcFirst, AcRefine };

class Decoder {
public:
    enum class Goal { Header, Color, Gray };

    Decoder(const uint8_t* data, size_t size, Goal goal) : data_(data), size_(size), goal_(goal) {
        bits_.data = data;
        bits_.size = size;
    }

    bool Run();

    const std::string& Reason() const { return reason_; }
    JpegInfo Info() const {
        JpegInfo info;
        info.width = width_;
        info.height = height_;
        info.components = componentCount_;
        info.progressive = progressive_;
        return info;
    }
    bool ToImage(gfx::Image& out);
    void ToGray(std::vector<uint8_t>& gray) const;

private:
    bool Fail(const char* reason) {
        reason_ = reason;
        return false;
    }
    bool NextMarker(int& marker);
    bool Segment(const uint8_t*& segment, size_t& length);
    bool ReadFrame(int marker, const uint8_t* s, size_t n);
    bool ReadQuant(const uint8_t* s, size_t n);
    bool ReadHuffman(const uint8_t* s, size_t n);
    bool ReadScan(const uint8_t* s, size_t n);
    bool Allocate();
    bool DecodeScan();
    bool Restart();
    bool FinishScan();
    bool DecodeBlock(Component& c, int bx, int by);
    bool DecodeSequential(Component& c, int bx, int by);
    bool DecodeDcFirst(Component& c, int16_t* block);
    bool DecodeDcRefine(int16_t* block);
    bool DecodeAcFirst(const Component& c, int16_t* block);
    bool DecodeAcRefine(const Component& c, int16_t* block);
    void InverseTransformAll();

    const uint8_t* data_;
    size_t size_;
    Goal goal_;
    size_t pos_ = 0;
    std::string reason_;

    bool frame_ = false;
    bool progressive_ = false;
    int width_ = 0;
    int height_ = 0;
    int componentCount_ = 0;
    int hMax_ = 1;
    int vMax_ = 1;
    int mcusX_ = 0;
    int mcusY_ = 0;
    Component components_[4];
    bool allocated_ = false;
    int scans_ = 0;

    bool adobe_ = false;
    int adobeTransform_ = -1;

    bool quantDefined_[4] = {};
    uint16_t quant_[4][64] = {};  // natural order
    HuffmanTable dc_[4];
    HuffmanTable ac_[4];
    int restartInterval_ = 0;

    // The current scan.
    int scanCount_ = 0;
    int scanComponents_[4] = {};
    int ss_ = 0;
    int se_ = 63;
    int ah_ = 0;
    int al_ = 0;
    ScanMode mode_ = ScanMode::Sequential;
    int eobRun_ = 0;
    int nextRestart_ = 0;
    BitReader bits_;
};

bool Decoder::NextMarker(int& marker) {
    // Skip anything before the 0xFF (libjpeg does the same, with a warning), then fill bytes.
    while (pos_ < size_ && data_[pos_] != 0xFF) pos_++;
    while (pos_ < size_ && data_[pos_] == 0xFF) pos_++;
    if (pos_ >= size_) return false;
    marker = data_[pos_++];
    return true;
}

bool Decoder::Segment(const uint8_t*& segment, size_t& length) {
    if (size_ - pos_ < 2) return Fail("jpeg-truncated");
    const size_t total = (static_cast<size_t>(data_[pos_]) << 8) | data_[pos_ + 1];
    if (total < 2) return Fail("jpeg-corrupt");
    if (size_ - pos_ < total) return Fail("jpeg-truncated");
    segment = data_ + pos_ + 2;
    length = total - 2;
    pos_ += total;
    return true;
}

bool Decoder::Run() {
    if (size_ < 4 || data_[0] != 0xFF || data_[1] != 0xD8) return Fail("jpeg-not-jpeg");
    pos_ = 2;
    for (;;) {
        int marker = 0;
        if (!NextMarker(marker)) return Fail("jpeg-truncated");
        if (marker == 0xD9) {  // EOI
            if (!frame_) return Fail("jpeg-no-frame");
            if (scans_ == 0) return Fail("jpeg-no-scan");
            if (progressive_) InverseTransformAll();
            return true;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;  // TEM, stray RSTn
        if (marker == 0xD8) return Fail("jpeg-corrupt");
        if (marker == 0xDC) return Fail("jpeg-dnl");
        if (marker == 0xCC) return Fail("jpeg-arithmetic");  // DAC
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4) {
            if (marker >= 0xC9) return Fail("jpeg-arithmetic");
            if (marker == 0xC3 || marker == 0xC7) return Fail("jpeg-lossless");
            if (marker >= 0xC5) return Fail("jpeg-hierarchical");
            if (marker == 0xC8) return Fail("jpeg-unsupported-frame");
        }
        const uint8_t* s = nullptr;
        size_t n = 0;
        if (!Segment(s, n)) return false;
        switch (marker) {
        case 0xC0:
        case 0xC1:
        case 0xC2:
            if (!ReadFrame(marker, s, n)) return false;
            if (goal_ == Goal::Header) return true;
            break;
        case 0xC4:
            if (!ReadHuffman(s, n)) return false;
            break;
        case 0xDB:
            if (!ReadQuant(s, n)) return false;
            break;
        case 0xDD:
            if (n != 2) return Fail("jpeg-corrupt");
            restartInterval_ = (s[0] << 8) | s[1];
            break;
        case 0xDA:
            if (!ReadScan(s, n)) return false;
            if (!Allocate() || !DecodeScan()) return false;
            break;
        case 0xEE:  // APP14
            if (n >= 12 && std::memcmp(s, "Adobe", 5) == 0) {
                adobe_ = true;
                adobeTransform_ = s[11];
            }
            break;
        default:  // APPn, COM and reserved markers: skipped
            break;
        }
    }
}

bool Decoder::ReadFrame(int marker, const uint8_t* s, size_t n) {
    if (frame_) return Fail("jpeg-corrupt");
    if (n < 6) return Fail("jpeg-corrupt");
    if (s[0] == 12) return Fail("jpeg-12-bit");
    if (s[0] != 8) return Fail("jpeg-precision");
    height_ = (s[1] << 8) | s[2];
    width_ = (s[3] << 8) | s[4];
    componentCount_ = s[5];
    if (height_ == 0) return Fail("jpeg-dnl");  // the height comes in a DNL marker
    if (width_ == 0 || width_ > kMaxDimension || height_ > kMaxDimension) return Fail("jpeg-dimensions");
    if (static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_) > kMaxPixels) return Fail("jpeg-too-large");
    if (componentCount_ != 1 && componentCount_ != 3 && componentCount_ != 4) return Fail("jpeg-components");
    if (n != 6 + 3 * static_cast<size_t>(componentCount_)) return Fail("jpeg-corrupt");
    if (goal_ == Goal::Gray && componentCount_ != 1) return Fail("jpeg-not-gray");
    progressive_ = marker == 0xC2;
    hMax_ = 1;
    vMax_ = 1;
    for (int i = 0; i < componentCount_; i++) {
        Component& c = components_[i];
        c.id = s[6 + 3 * i];
        c.h = s[7 + 3 * i] >> 4;
        c.v = s[7 + 3 * i] & 15;
        c.quantSlot = s[8 + 3 * i];
        if (c.h < 1 || c.h > 2 || c.v < 1 || c.v > 2) return Fail("jpeg-sampling");
        if (c.quantSlot > 3) return Fail("jpeg-corrupt");
        for (int j = 0; j < i; j++) {
            if (components_[j].id == c.id) return Fail("jpeg-corrupt");
        }
        if (c.h > hMax_) hMax_ = c.h;
        if (c.v > vMax_) vMax_ = c.v;
    }
    mcusX_ = (width_ + 8 * hMax_ - 1) / (8 * hMax_);
    mcusY_ = (height_ + 8 * vMax_ - 1) / (8 * vMax_);
    for (int i = 0; i < componentCount_; i++) {
        Component& c = components_[i];
        const int w = (width_ * c.h + hMax_ - 1) / hMax_;
        const int h = (height_ * c.v + vMax_ - 1) / vMax_;
        c.widthBlocks = (w + 7) / 8;
        c.heightBlocks = (h + 7) / 8;
        c.blocksW = mcusX_ * c.h;
        c.blocksH = mcusY_ * c.v;
    }
    frame_ = true;
    return true;
}

bool Decoder::ReadQuant(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const int precision = s[i] >> 4;
        const int slot = s[i] & 15;
        if (precision > 1 || slot > 3) return Fail("jpeg-corrupt");
        const size_t bytes = precision ? 128 : 64;
        if (n - i - 1 < bytes) return Fail("jpeg-corrupt");
        const uint8_t* q = s + i + 1;
        for (int k = 0; k < 64; k++) {
            quant_[slot][kNaturalOrder[k]] =
                precision ? static_cast<uint16_t>((q[2 * k] << 8) | q[2 * k + 1]) : q[k];
        }
        quantDefined_[slot] = true;
        i += 1 + bytes;
    }
    return true;
}

bool Decoder::ReadHuffman(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const int tableClass = s[i] >> 4;
        const int slot = s[i] & 15;
        if (tableClass > 1 || slot > 3) return Fail("jpeg-corrupt");
        if (n - i - 1 < 16) return Fail("jpeg-corrupt");
        const uint8_t* counts = s + i + 1;
        int total = 0;
        for (int k = 0; k < 16; k++) total += counts[k];
        if (total > 256 || n - i - 17 < static_cast<size_t>(total)) return Fail("jpeg-corrupt");
        HuffmanTable& table = tableClass ? ac_[slot] : dc_[slot];
        if (!BuildHuffman(counts, counts + 16, total, table)) return Fail("jpeg-corrupt");
        i += 17 + static_cast<size_t>(total);
    }
    return true;
}

bool Decoder::ReadScan(const uint8_t* s, size_t n) {
    if (!frame_) return Fail("jpeg-corrupt");
    if (++scans_ > kMaxScans) return Fail("jpeg-too-many-scans");
    if (n < 1) return Fail("jpeg-corrupt");
    scanCount_ = s[0];
    if (scanCount_ < 1 || scanCount_ > componentCount_ || n != 4 + 2 * static_cast<size_t>(scanCount_)) {
        return Fail("jpeg-corrupt");
    }
    int blocksPerMcu = 0;
    for (int i = 0; i < scanCount_; i++) {
        const int id = s[1 + 2 * i];
        int index = -1;
        for (int j = 0; j < componentCount_; j++) {
            if (components_[j].id == id) index = j;
        }
        if (index < 0) return Fail("jpeg-corrupt");
        for (int j = 0; j < i; j++) {
            if (scanComponents_[j] == index) return Fail("jpeg-corrupt");
        }
        scanComponents_[i] = index;
        Component& c = components_[index];
        c.dcTable = s[2 + 2 * i] >> 4;
        c.acTable = s[2 + 2 * i] & 15;
        if (c.dcTable > 3 || c.acTable > 3) return Fail("jpeg-corrupt");
        blocksPerMcu += c.h * c.v;
    }
    if (scanCount_ > 1 && blocksPerMcu > 10) return Fail("jpeg-corrupt");
    const uint8_t* p = s + 1 + 2 * scanCount_;
    ss_ = p[0];
    se_ = p[1];
    ah_ = p[2] >> 4;
    al_ = p[2] & 15;
    if (!progressive_) {
        // libjpeg ignores Ss, Se, Ah and Al of a sequential scan; so do we.
        ss_ = 0;
        se_ = 63;
        ah_ = 0;
        al_ = 0;
        mode_ = ScanMode::Sequential;
    } else {
        if (ss_ > se_ || se_ > 63 || al_ > 13) return Fail("jpeg-corrupt");
        if (ss_ == 0 && se_ != 0) return Fail("jpeg-corrupt");
        if (ss_ > 0 && scanCount_ != 1) return Fail("jpeg-corrupt");
        if (ah_ != 0 && al_ != ah_ - 1) return Fail("jpeg-corrupt");
        if (ss_ == 0) {
            mode_ = ah_ == 0 ? ScanMode::DcFirst : ScanMode::DcRefine;
        } else {
            mode_ = ah_ == 0 ? ScanMode::AcFirst : ScanMode::AcRefine;
        }
    }
    for (int i = 0; i < scanCount_; i++) {
        Component& c = components_[scanComponents_[i]];
        const bool needsDc = mode_ == ScanMode::Sequential || mode_ == ScanMode::DcFirst;
        const bool needsAc = mode_ == ScanMode::Sequential || mode_ == ScanMode::AcFirst ||
                             mode_ == ScanMode::AcRefine;
        if (needsDc && !dc_[c.dcTable].defined) return Fail("jpeg-corrupt");
        if (needsAc && !ac_[c.acTable].defined) return Fail("jpeg-corrupt");
        if (!c.latched) {
            if (!quantDefined_[c.quantSlot]) return Fail("jpeg-corrupt");
            std::memcpy(c.quant, quant_[c.quantSlot], sizeof(c.quant));
            c.latched = true;
        }
    }
    return true;
}

bool Decoder::Allocate() {
    if (allocated_) return true;
    // Every block of every component is coded by at least one Huffman code of at least one bit
    // (a sequential scan or the first DC scan), so a frame cannot have more blocks than eight
    // times the bytes after its first scan header. This refuses a forged size before the buffers
    // it would need are allocated.
    uint64_t blocks = 0;
    for (int i = 0; i < componentCount_; i++) {
        blocks += static_cast<uint64_t>(components_[i].widthBlocks) * static_cast<uint64_t>(components_[i].heightBlocks);
    }
    if (blocks > 8 * static_cast<uint64_t>(size_ - pos_)) return Fail("jpeg-truncated");
    for (int i = 0; i < componentCount_; i++) {
        Component& c = components_[i];
        const size_t count = static_cast<size_t>(c.blocksW) * static_cast<size_t>(c.blocksH);
        if (progressive_) {
            c.coefficients.assign(count * 64, 0);
        } else {
            c.plane.assign(count * 64, 128);
        }
    }
    allocated_ = true;
    return true;
}

bool Decoder::DecodeScan() {
    int mcusX = mcusX_;
    int mcusY = mcusY_;
    if (scanCount_ == 1) {
        const Component& c = components_[scanComponents_[0]];
        mcusX = c.widthBlocks;
        mcusY = c.heightBlocks;
    }
    for (int i = 0; i < scanCount_; i++) components_[scanComponents_[i]].dcPredictor = 0;
    eobRun_ = 0;
    nextRestart_ = 0;
    bits_.Start(pos_);
    int restartsLeft = restartInterval_;
    for (int my = 0; my < mcusY; my++) {
        for (int mx = 0; mx < mcusX; mx++) {
            if (restartInterval_) {
                if (restartsLeft == 0) {
                    if (!Restart()) return false;
                    restartsLeft = restartInterval_;
                }
                restartsLeft--;
            }
            if (scanCount_ == 1) {
                if (!DecodeBlock(components_[scanComponents_[0]], mx, my)) return false;
            } else {
                for (int i = 0; i < scanCount_; i++) {
                    Component& c = components_[scanComponents_[i]];
                    for (int y = 0; y < c.v; y++) {
                        for (int x = 0; x < c.h; x++) {
                            if (!DecodeBlock(c, mx * c.h + x, my * c.v + y)) return false;
                        }
                    }
                }
            }
            if (bits_.Overrun()) return Fail("jpeg-truncated");
        }
    }
    return FinishScan();
}

// Leaves the entropy-coded data of an interval at its marker: what is left in the bit buffer is
// padding. Bytes before the marker that no MCU used are skipped (libjpeg does the same).
static size_t SkipToMarker(const uint8_t* data, size_t size, BitReader& bits) {
    size_t pos = bits.pos;
    if (!bits.marker) {
        while (pos + 1 < size && !(data[pos] == 0xFF && data[pos + 1] != 0x00 && data[pos + 1] != 0xFF)) pos++;
    }
    return pos;
}

bool Decoder::Restart() {
    if (bits_.Overrun()) return Fail("jpeg-truncated");
    size_t pos = SkipToMarker(data_, size_, bits_);
    while (pos < size_ && data_[pos] == 0xFF) pos++;
    if (pos >= size_) return Fail("jpeg-truncated");
    if (data_[pos] != 0xD0 + (nextRestart_ & 7)) return Fail("jpeg-corrupt");
    nextRestart_++;
    for (int i = 0; i < scanCount_; i++) components_[scanComponents_[i]].dcPredictor = 0;
    eobRun_ = 0;
    bits_.Start(pos + 1);
    return true;
}

bool Decoder::FinishScan() {
    if (bits_.Overrun()) return Fail("jpeg-truncated");
    pos_ = SkipToMarker(data_, size_, bits_);
    return true;
}

bool Decoder::DecodeBlock(Component& c, int bx, int by) {
    if (mode_ == ScanMode::Sequential) return DecodeSequential(c, bx, by);
    int16_t* block = c.coefficients.data() +
                     (static_cast<size_t>(by) * static_cast<size_t>(c.blocksW) + static_cast<size_t>(bx)) * 64;
    switch (mode_) {
    case ScanMode::DcFirst: return DecodeDcFirst(c, block);
    case ScanMode::DcRefine: return DecodeDcRefine(block);
    case ScanMode::AcFirst: return DecodeAcFirst(c, block);
    default: return DecodeAcRefine(c, block);
    }
}

// A sequential block: decoded, dequantized and transformed into the component plane.
bool Decoder::DecodeSequential(Component& c, int bx, int by) {
    int16_t block[64] = {};
    const int s = bits_.Decode(dc_[c.dcTable]);
    if (s < 0 || s > 15) return Fail("jpeg-corrupt");
    const int diff = s ? Extend(bits_.Bits(s), s) : 0;
    const int dc = c.dcPredictor + diff;
    if (dc < -32768 || dc > 32767) return Fail("jpeg-corrupt");
    c.dcPredictor = dc;
    block[0] = Saturate16(dc * static_cast<int32_t>(c.quant[0]));
    const HuffmanTable& ac = ac_[c.acTable];
    for (int k = 1; k < 64; k++) {
        const int rs = bits_.Decode(ac);
        if (rs < 0) return Fail("jpeg-corrupt");
        const int r = rs >> 4;
        const int size = rs & 15;
        if (size == 0) {
            if (r != 15) break;
            k += 15;
            continue;
        }
        k += r;
        if (k > 63) return Fail("jpeg-corrupt");
        const int natural = kNaturalOrder[k];
        block[natural] = Saturate16(Extend(bits_.Bits(size), size) * static_cast<int32_t>(c.quant[natural]));
    }
    uint8_t* out = c.plane.data() + static_cast<size_t>(by) * 8 * c.PlaneStride() + static_cast<size_t>(bx) * 8;
    gfx::IdctBlock(block, out, c.PlaneStride());
    return true;
}

bool Decoder::DecodeDcFirst(Component& c, int16_t* block) {
    const int s = bits_.Decode(dc_[c.dcTable]);
    if (s < 0 || s > 15) return Fail("jpeg-corrupt");
    const int diff = s ? Extend(bits_.Bits(s), s) : 0;
    const int dc = c.dcPredictor + diff;
    if (dc < -32768 || dc > 32767) return Fail("jpeg-corrupt");
    c.dcPredictor = dc;
    block[0] = Saturate16(dc * (int32_t(1) << al_));
    return true;
}

bool Decoder::DecodeDcRefine(int16_t* block) {
    if (bits_.Bit()) block[0] = static_cast<int16_t>(block[0] | (1 << al_));
    return true;
}

bool Decoder::DecodeAcFirst(const Component& c, int16_t* block) {
    if (eobRun_ > 0) {
        eobRun_--;
        return true;
    }
    const HuffmanTable& ac = ac_[c.acTable];
    for (int k = ss_; k <= se_; k++) {
        const int rs = bits_.Decode(ac);
        if (rs < 0) return Fail("jpeg-corrupt");
        const int r = rs >> 4;
        const int s = rs & 15;
        if (s == 0) {
            if (r < 15) {
                eobRun_ = (1 << r) - 1;
                if (r) eobRun_ += bits_.Bits(r);
                break;
            }
            k += 15;
            continue;
        }
        k += r;
        if (k > se_) return Fail("jpeg-corrupt");
        block[kNaturalOrder[k]] = Saturate16(Extend(bits_.Bits(s), s) * (int32_t(1) << al_));
    }
    return true;
}

// T.81 G.1.2.3, in libjpeg's formulation (decode_mcu_AC_refine).
bool Decoder::DecodeAcRefine(const Component& c, int16_t* block) {
    const int p1 = 1 << al_;
    const int m1 = -p1;
    int k = ss_;
    auto refine = [&](int16_t& coefficient) {
        if (bits_.Bit() && (coefficient & p1) == 0) {
            coefficient = Saturate16(coefficient + (coefficient >= 0 ? p1 : m1));
        }
    };
    if (eobRun_ == 0) {
        const HuffmanTable& ac = ac_[c.acTable];
        for (; k <= se_; k++) {
            const int rs = bits_.Decode(ac);
            if (rs < 0) return Fail("jpeg-corrupt");
            int r = rs >> 4;
            int s = rs & 15;
            if (s) {
                if (s != 1) return Fail("jpeg-corrupt");
                s = bits_.Bit() ? p1 : m1;
            } else if (r != 15) {
                eobRun_ = 1 << r;
                if (r) eobRun_ += bits_.Bits(r);
                break;  // the rest of this block is handled with the end-of-band run below
            }
            // Skip r zero coefficients (refining the nonzero ones passed on the way), then
            // place the new coefficient, if any.
            for (; k <= se_; k++) {
                int16_t& coefficient = block[kNaturalOrder[k]];
                if (coefficient != 0) {
                    refine(coefficient);
                } else {
                    if (r == 0) break;
                    r--;
                }
            }
            if (s) {
                if (k > se_) return Fail("jpeg-corrupt");
                block[kNaturalOrder[k]] = static_cast<int16_t>(s);
            }
        }
    }
    if (eobRun_ > 0) {
        for (; k <= se_; k++) {
            int16_t& coefficient = block[kNaturalOrder[k]];
            if (coefficient != 0) refine(coefficient);
        }
        eobRun_--;
    }
    return true;
}

void Decoder::InverseTransformAll() {
    for (int i = 0; i < componentCount_; i++) {
        Component& c = components_[i];
        const size_t stride = c.PlaneStride();
        c.plane.assign(stride * static_cast<size_t>(c.blocksH) * 8, 0);
        if (c.coefficients.empty()) continue;  // allocated with the first scan, always there
        int16_t block[64];
        for (int by = 0; by < c.blocksH; by++) {
            for (int bx = 0; bx < c.blocksW; bx++) {
                const int16_t* in = c.coefficients.data() +
                                    (static_cast<size_t>(by) * static_cast<size_t>(c.blocksW) + static_cast<size_t>(bx)) * 64;
                for (int k = 0; k < 64; k++) block[k] = Saturate16(in[k] * static_cast<int32_t>(c.quant[k]));
                gfx::IdctBlock(block, c.plane.data() + static_cast<size_t>(by) * 8 * stride + static_cast<size_t>(bx) * 8, stride);
            }
        }
        std::vector<int16_t>().swap(c.coefficients);
    }
}

bool Decoder::ToImage(gfx::Image& out) {
    const size_t width = static_cast<size_t>(width_);
    std::vector<uint32_t> pixels(width * static_cast<size_t>(height_));
    // Rows of the components at full width: the plane row itself, or a replicated copy.
    std::vector<uint8_t> expanded[4];
    for (int i = 0; i < componentCount_; i++) {
        if (components_[i].h != hMax_) expanded[i].resize(width);
    }
    enum class Model { Gray, YCbCr, Rgb, Cmyk, Ycck } model = Model::Gray;
    if (componentCount_ == 3) model = adobe_ && adobeTransform_ == 0 ? Model::Rgb : Model::YCbCr;
    if (componentCount_ == 4) model = adobe_ && adobeTransform_ == 2 ? Model::Ycck : Model::Cmyk;
    // Adobe writes CMYK inverted (255 - ink); plain CMYK stores the ink.
    const uint32_t ink = model == Model::Cmyk && !adobe_ ? 255 : 0;
    const gfx::Kernels& kernels = gfx::ActiveKernels();
    for (int y = 0; y < height_; y++) {
        const uint8_t* rows[4] = {};
        for (int i = 0; i < componentCount_; i++) {
            const Component& c = components_[i];
            const int row = c.v == vMax_ ? y : y / 2;
            const uint8_t* src = c.plane.data() + static_cast<size_t>(row) * c.PlaneStride();
            if (c.h == hMax_) {
                rows[i] = src;
            } else {
                uint8_t* e = expanded[i].data();
                for (size_t x = 0; x < width; x++) e[x] = src[x >> 1];
                rows[i] = e;
            }
        }
        uint32_t* dst = pixels.data() + static_cast<size_t>(y) * width;
        switch (model) {
        case Model::Gray:
            for (size_t x = 0; x < width; x++) dst[x] = 0xFF000000u | (uint32_t(rows[0][x]) * 0x010101u);
            break;
        case Model::YCbCr:
            kernels.yccToBgra(rows, dst, width);
            break;
        case Model::Rgb:
            for (size_t x = 0; x < width; x++) dst[x] = gfx::PackArgb(255, rows[0][x], rows[1][x], rows[2][x]);
            break;
        case Model::Cmyk:
            for (size_t x = 0; x < width; x++) {
                const uint32_t k = rows[3][x] ^ ink;
                dst[x] = gfx::PackArgb(255, static_cast<uint8_t>(gfx::Mul255(rows[0][x] ^ ink, k)),
                                       static_cast<uint8_t>(gfx::Mul255(rows[1][x] ^ ink, k)),
                                       static_cast<uint8_t>(gfx::Mul255(rows[2][x] ^ ink, k)));
            }
            break;
        case Model::Ycck:
            kernels.yccToBgra(rows, dst, width);
            for (size_t x = 0; x < width; x++) {
                const uint32_t k = rows[3][x];
                const uint32_t p = dst[x];
                dst[x] = gfx::PackArgb(255, static_cast<uint8_t>(gfx::Mul255(255 - ((p >> 16) & 255), k)),
                                       static_cast<uint8_t>(gfx::Mul255(255 - ((p >> 8) & 255), k)),
                                       static_cast<uint8_t>(gfx::Mul255(255 - (p & 255), k)));
            }
            break;
        }
    }
    out.width = width_;
    out.height = height_;
    out.pixels = std::move(pixels);
    out.opaque = true;
    return true;
}

void Decoder::ToGray(std::vector<uint8_t>& gray) const {
    const Component& c = components_[0];
    const size_t width = static_cast<size_t>(width_);
    std::vector<uint8_t> samples(width * static_cast<size_t>(height_));
    for (int y = 0; y < height_; y++) {
        std::memcpy(samples.data() + static_cast<size_t>(y) * width,
                    c.plane.data() + static_cast<size_t>(y) * c.PlaneStride(), width);
    }
    gray = std::move(samples);
}

} // namespace

bool ReadJpegInfo(const uint8_t* data, size_t size, JpegInfo& info, std::string* reason) {
    Decoder decoder(data, size, Decoder::Goal::Header);
    if (!decoder.Run()) {
        if (reason) *reason = decoder.Reason();
        return false;
    }
    info = decoder.Info();
    return true;
}

bool DecodeJpeg(const uint8_t* data, size_t size, gfx::Image& out, std::string* reason) {
    Decoder decoder(data, size, Decoder::Goal::Color);
    if (!decoder.Run()) {
        if (reason) *reason = decoder.Reason();
        return false;
    }
    return decoder.ToImage(out);
}

bool DecodeJpegGray(const uint8_t* data, size_t size, int& width, int& height,
                    std::vector<uint8_t>& gray, std::string* reason) {
    Decoder decoder(data, size, Decoder::Goal::Gray);
    if (!decoder.Run()) {
        if (reason) *reason = decoder.Reason();
        return false;
    }
    decoder.ToGray(gray);
    width = decoder.Info().width;
    height = decoder.Info().height;
    return true;
}

} // namespace rgui::pdf
