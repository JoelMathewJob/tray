/* 
 * QR Code generator library (C++)
 * 
 * Copyright (c) Project Nayuki. (MIT License)
 * https://www.nayuki.io/page/qr-code-generator-library
 */

#include "qrcodegen.hpp"
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

using std::int8_t;
using std::uint8_t;
using std::size_t;
using std::vector;

namespace qrcodegen {

const int QrCode::PENALTY_N1 =  3;
const int QrCode::PENALTY_N2 =  3;
const int QrCode::PENALTY_N3 = 40;
const int QrCode::PENALTY_N4 = 10;

static const int8_t ECC_CODEWORDS_PER_BLOCK[4][41] = {
    // Version: (note that index 0 is for padding)
    //0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40    Error correction level
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Low
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},  // Medium
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Quartile
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // High
};

static const int8_t NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
    // Version: (note that index 0 is for padding)
    //0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40    Error correction level
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,  8,  9,  9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},  // Low
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5,  5,  8,  9,  9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},  // Medium
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8,  8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},  // Quartile
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 72, 74, 77},  // High
};

QrSegment::QrSegment(Mode md, int numCh, const vector<bool> &dt) :
    mode(md),
    numChars(numCh),
    data(dt) {
    if (numCh < 0)
        throw std::domain_error("Invalid char count");
}

QrSegment::QrSegment(Mode md, int numCh, vector<bool> &&dt) :
    mode(md),
    numChars(numCh),
    data(std::move(dt)) {
    if (numCh < 0)
        throw std::domain_error("Invalid char count");
}

QrSegment::Mode QrSegment::getMode() const {
    return mode;
}

int QrSegment::getNumChars() const {
    return numChars;
}

const vector<bool> &QrSegment::getData() const {
    return data;
}

static void appendBits(std::uint32_t val, int len, vector<bool> &result) {
    for (int i = len - 1; i >= 0; i--)
        result.push_back(((val >> i) & 1) != 0);
}

QrSegment QrSegment::makeBytes(const vector<uint8_t> &data) {
    if (data.size() > static_cast<unsigned int>(INT_MAX))
        throw std::length_error("Data too long");
    vector<bool> bb;
    for (uint8_t b : data)
        appendBits(b, 8, bb);
    return QrSegment(Mode::BYTE, static_cast<int>(data.size()), std::move(bb));
}

QrSegment QrSegment::makeNumeric(const char *digits) {
    vector<bool> bb;
    int len = 0;
    while (digits[len] != '\0') {
        char c = digits[len];
        if (c < '0' || c > '9')
            throw std::domain_error("String contains non-numeric characters");
        len++;
    }
    for (int i = 0; i < len; ) {
        int n = std::min(len - i, 3);
        int val = 0;
        for (int j = 0; j < n; j++)
            val = val * 10 + (digits[i + j] - '0');
        appendBits(static_cast<uint32_t>(val), n * 3 + 1, bb);
        i += n;
    }
    return QrSegment(Mode::NUMERIC, len, std::move(bb));
}

static const char *ALPHANUMERIC_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

QrSegment QrSegment::makeAlphanumeric(const char *text) {
    vector<bool> bb;
    int len = 0;
    while (text[len] != '\0') {
        const char *p = std::strchr(ALPHANUMERIC_CHARSET, text[len]);
        if (!p)
            throw std::domain_error("String contains unencodable characters");
        len++;
    }
    for (int i = 0; i < len; ) {
        int n = std::min(len - i, 2);
        int val = 0;
        for (int j = 0; j < n; j++) {
            const char *p = std::strchr(ALPHANUMERIC_CHARSET, text[i + j]);
            val = val * 45 + static_cast<int>(p - ALPHANUMERIC_CHARSET);
        }
        appendBits(static_cast<uint32_t>(val), n == 2 ? 11 : 6, bb);
        i += n;
    }
    return QrSegment(Mode::ALPHANUMERIC, len, std::move(bb));
}

vector<QrSegment> QrSegment::makeSegments(const char *text) {
    vector<QrSegment> result;
    if (*text == '\0')
        return result;
    vector<uint8_t> bytes;
    while (*text != '\0') {
        bytes.push_back(static_cast<uint8_t>(*text));
        text++;
    }
    result.push_back(makeBytes(bytes));
    return result;
}

int QrSegment::getTotalBits(const vector<QrSegment> &segs, int version) {
    int result = 0;
    for (const QrSegment &seg : segs) {
        int ccbits;
        switch (seg.getMode()) {
            case Mode::NUMERIC:      ccbits = (version < 10) ? 10 : (version < 27) ? 12 : 14; break;
            case Mode::ALPHANUMERIC: ccbits = (version < 10) ?  9 : (version < 27) ? 11 : 13; break;
            case Mode::BYTE:         ccbits = (version < 10) ?  8 : 16; break;
            case Mode::KANJI:        ccbits = (version < 10) ?  8 : (version < 27) ? 10 : 12; break;
            case Mode::ECI:          ccbits = 0; break;
            default:                 throw std::logic_error("Unreachable");
        }
        result += 4 + ccbits + static_cast<int>(seg.getData().size());
    }
    return result;
}

QrCode QrCode::encodeText(const char *text, Ecc ecc) {
    vector<QrSegment> segs = QrSegment::makeSegments(text);
    return encodeSegments(segs, ecc);
}

QrCode QrCode::encodeBinary(const vector<uint8_t> &data, Ecc ecc) {
    vector<QrSegment> segs{QrSegment::makeBytes(data)};
    return encodeSegments(segs, ecc);
}

static int getNumRawDataModules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7)
            result -= 36;
    }
    return result;
}

static int getNumDataCodewords(int ver, QrCode::Ecc ecl) {
    return getNumRawDataModules(ver) / 8
        - ECC_CODEWORDS_PER_BLOCK[static_cast<int>(ecl)][ver]
        * NUM_ERROR_CORRECTION_BLOCKS[static_cast<int>(ecl)][ver];
}

QrCode QrCode::encodeSegments(const vector<QrSegment> &segs, Ecc ecc, int minVersion, int maxVersion, int mask, bool boostEcl) {
    if (!(MIN_VERSION <= minVersion && minVersion <= maxVersion && maxVersion <= MAX_VERSION))
        throw std::invalid_argument("Invalid version range");

    int version, dataUsedBits;
    for (version = minVersion; ; version++) {
        int dataCapacityBits = getNumDataCodewords(version, ecc) * 8;
        dataUsedBits = QrSegment::getTotalBits(segs, version);
        if (dataUsedBits <= dataCapacityBits)
            break;
        if (version >= maxVersion)
            throw std::length_error("Data too long for QR Code");
    }

    if (boostEcl) {
        for (Ecc newEcc : {Ecc::MEDIUM, Ecc::QUARTILE, Ecc::HIGH}) {
            if (dataUsedBits <= getNumDataCodewords(version, newEcc) * 8)
                ecc = newEcc;
        }
    }

    vector<bool> bb;
    for (const QrSegment &seg : segs) {
        appendBits(static_cast<uint32_t>(seg.getMode()), 4, bb);
        int ccbits;
        switch (seg.getMode()) {
            case QrSegment::Mode::NUMERIC:      ccbits = (version < 10) ? 10 : (version < 27) ? 12 : 14; break;
            case QrSegment::Mode::ALPHANUMERIC: ccbits = (version < 10) ?  9 : (version < 27) ? 11 : 13; break;
            case QrSegment::Mode::BYTE:         ccbits = (version < 10) ?  8 : 16; break;
            case QrSegment::Mode::KANJI:        ccbits = (version < 10) ?  8 : (version < 27) ? 10 : 12; break;
            default: ccbits = 0; break;
        }
        appendBits(static_cast<uint32_t>(seg.getNumChars()), ccbits, bb);
        bb.insert(bb.end(), seg.getData().begin(), seg.getData().end());
    }

    int capacityBits = getNumDataCodewords(version, ecc) * 8;
    int terminatorBits = std::min(4, capacityBits - static_cast<int>(bb.size()));
    appendBits(0, terminatorBits, bb);
    while (bb.size() % 8 != 0)
        bb.push_back(false);

    static const uint8_t PAD_BYTES[2] = {0xEC, 0x11};
    int padIdx = 0;
    while (static_cast<int>(bb.size()) < capacityBits) {
        appendBits(PAD_BYTES[padIdx], 8, bb);
        padIdx = 1 - padIdx;
    }

    vector<uint8_t> dataCodewords(bb.size() / 8);
    for (size_t i = 0; i < bb.size(); i++)
        dataCodewords[i / 8] |= (bb[i] ? 1 : 0) << (7 - (i % 8));

    return QrCode(version, ecc, dataCodewords, mask);
}

static uint8_t reedSolomonMultiply(uint8_t x, uint8_t y) {
    // GF(2^8) multiplication modulo 0x11D
    uint8_t z = 0;
    for (int i = 7; i >= 0; i--) {
        z = static_cast<uint8_t>((z << 1) ^ ((z >> 7) * 0x11D));
        z ^= ((y >> i) & 1) * x;
    }
    return z;
}

static vector<uint8_t> reedSolomonComputeDivisor(int degree) {
    vector<uint8_t> result(static_cast<size_t>(degree));
    result.back() = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (size_t j = 0; j < result.size(); j++) {
            result[j] = reedSolomonMultiply(result[j], root);
            if (j + 1 < result.size())
                result[j] ^= result[j + 1];
        }
        root = reedSolomonMultiply(root, 0x02);
    }
    return result;
}

static vector<uint8_t> reedSolomonComputeRemainder(const vector<uint8_t> &data, const vector<uint8_t> &divisor) {
    vector<uint8_t> result(divisor.size());
    for (uint8_t b : data) {
        uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < divisor.size(); i++)
            result[i] ^= reedSolomonMultiply(divisor[i], factor);
    }
    return result;
}

QrCode::QrCode(int ver, Ecc ecl, const vector<uint8_t> &dataCodewords, int msk) :
    version(ver),
    size(ver * 4 + 17),
    errorCorrectionLevel(ecl),
    mask(msk),
    modules(size, vector<bool>(size, false)),
    isFunction(size, vector<bool>(size, false)) {

    drawFunctionPatterns();

    // Perform error correction code computation and interleaving
    int numBlocks = NUM_ERROR_CORRECTION_BLOCKS[static_cast<int>(ecl)][ver];
    int blockEccLen = ECC_CODEWORDS_PER_BLOCK[static_cast<int>(ecl)][ver];
    int rawCodewords = getNumRawDataModules(ver) / 8;
    int numShortBlocks = numBlocks - rawCodewords % numBlocks;
    int shortBlockDataLen = rawCodewords / numBlocks - blockEccLen;

    vector<vector<uint8_t>> blocks;
    const vector<uint8_t> rsDiv = reedSolomonComputeDivisor(blockEccLen);
    for (int i = 0, k = 0; i < numBlocks; i++) {
        int datLen = shortBlockDataLen + (i < numShortBlocks ? 0 : 1);
        vector<uint8_t> dat(dataCodewords.begin() + k, dataCodewords.begin() + (k + datLen));
        k += datLen;
        const vector<uint8_t> ecc = reedSolomonComputeRemainder(dat, rsDiv);
        if (i < numShortBlocks)
            dat.push_back(0);
        dat.insert(dat.end(), ecc.begin(), ecc.end());
        blocks.push_back(std::move(dat));
    }

    // Interleave (all data bytes, then all ECC bytes)
    vector<uint8_t> allCodewords;
    for (size_t i = 0; i < static_cast<size_t>(shortBlockDataLen + 1); i++) {
        for (size_t j = 0; j < blocks.size(); j++) {
            // Skip the padding byte in short blocks
            if (i != static_cast<size_t>(shortBlockDataLen) || j >= static_cast<size_t>(numShortBlocks))
                allCodewords.push_back(blocks[j][i]);
        }
    }
    for (size_t i = static_cast<size_t>(shortBlockDataLen + 1); i < blocks[0].size(); i++) {
        for (size_t j = 0; j < blocks.size(); j++)
            allCodewords.push_back(blocks[j][i]);
    }

    drawCodewords(allCodewords);

    if (mask < 0) {
        long minPenalty = LONG_MAX;
        for (int m = 0; m < 8; m++) {
            applyMask(m);
            drawFormatBits(m);
            long penalty = getPenaltyScore();
            if (penalty < minPenalty) {
                minPenalty = penalty;
                mask = m;
            }
            applyMask(m);
        }
    }
    applyMask(mask);
    drawFormatBits(mask);
}

int QrCode::getVersion() const { return version; }
int QrCode::getSize() const { return size; }
QrCode::Ecc QrCode::getErrorCorrectionLevel() const { return errorCorrectionLevel; }
int QrCode::getMask() const { return mask; }
bool QrCode::getModule(int x, int y) const {
    return (0 <= x && x < size && 0 <= y && y < size) && modules[y][x];
}

void QrCode::setFunctionModule(int x, int y, bool isBlack) {
    modules[y][x] = isBlack;
    isFunction[y][x] = true;
}

void QrCode::drawFinderPattern(int x, int y) {
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int dist = std::max(std::abs(dx), std::abs(dy));
            int xx = x + dx, yy = y + dy;
            if (0 <= xx && xx < size && 0 <= yy && yy < size)
                setFunctionModule(xx, yy, dist != 2 && dist != 4);
        }
    }
}

void QrCode::drawAlignmentPattern(int x, int y) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++)
            setFunctionModule(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }
}

void QrCode::drawFunctionPatterns() {
    for (int i = 0; i < size; i++) {
        setFunctionModule(6, i, i % 2 == 0);
        setFunctionModule(i, 6, i % 2 == 0);
    }
    drawFinderPattern(3, 3);
    drawFinderPattern(size - 4, 3);
    drawFinderPattern(3, size - 4);

    // Alignment patterns
    if (version > 1) {
        static const int ALIGN_POS[41][7] = {
            {}, {}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34},
            {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}
        };
        int numAlign = (version < 7) ? 2 : 3;
        for (int i = 0; i < numAlign; i++) {
            for (int j = 0; j < numAlign; j++) {
                int px = ALIGN_POS[version][i];
                int py = ALIGN_POS[version][j];
                if (!((i == 0 && j == 0) || (i == 0 && j == numAlign - 1) || (i == numAlign - 1 && j == 0)))
                    drawAlignmentPattern(px, py);
            }
        }
    }

    drawFormatBits(0);
}

void QrCode::drawFormatBits(int msk) {
    static const int FORMAT_BITS[4][8] = {
        {0x77C4, 0x72F3, 0x7DAA, 0x789D, 0x662F, 0x6318, 0x6C41, 0x6976}, // L
        {0x5412, 0x5125, 0x5E7C, 0x5B4B, 0x45F9, 0x40CE, 0x4F97, 0x4AA0}, // M
        {0x355F, 0x3068, 0x3F31, 0x3A06, 0x24B4, 0x2183, 0x2EDA, 0x2BED}, // Q
        {0x1689, 0x13BE, 0x1CE7, 0x19D0, 0x0762, 0x0255, 0x0D0C, 0x083B}  // H
    };
    int data = FORMAT_BITS[static_cast<int>(errorCorrectionLevel)][msk];
    for (int i = 0; i <= 5; i++)
        setFunctionModule(8, i, ((data >> i) & 1) != 0);
    setFunctionModule(8, 7, ((data >> 6) & 1) != 0);
    setFunctionModule(8, 8, ((data >> 7) & 1) != 0);
    setFunctionModule(7, 8, ((data >> 8) & 1) != 0);
    for (int i = 9; i < 15; i++)
        setFunctionModule(14 - i, 8, ((data >> i) & 1) != 0);

    for (int i = 0; i < 8; i++)
        setFunctionModule(size - 1 - i, 8, ((data >> i) & 1) != 0);
    for (int i = 8; i < 15; i++)
        setFunctionModule(8, size - 15 + i, ((data >> i) & 1) != 0);
    setFunctionModule(8, size - 8, true);
}

void QrCode::drawCodewords(const vector<uint8_t> &data) {
    size_t i = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right--;
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (!isFunction[y][x] && i < data.size() * 8) {
                    modules[y][x] = ((data[i / 8] >> (7 - (i % 8))) & 1) != 0;
                    i++;
                }
            }
        }
    }
}

void QrCode::applyMask(int msk) {
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (isFunction[y][x]) continue;
            bool invert;
            switch (msk) {
                case 0:  invert = (x + y) % 2 == 0; break;
                case 1:  invert = y % 2 == 0; break;
                case 2:  invert = x % 3 == 0; break;
                case 3:  invert = (x + y) % 3 == 0; break;
                case 4:  invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5:  invert = (x * y) % 2 + (x * y) % 3 == 0; break;
                case 6:  invert = ((x * y) % 2 + (x * y) % 3) % 2 == 0; break;
                case 7:  invert = ((x + y) % 2 + (x * y) % 3) % 2 == 0; break;
                default: throw std::logic_error("Invalid mask");
            }
            modules[y][x] = modules[y][x] ^ invert;
        }
    }
}

long QrCode::getPenaltyScore() const {
    long result = 0;
    for (int y = 0; y < size; y++) {
        bool runColor = false;
        int runVal = 0;
        for (int x = 0; x < size; x++) {
            if (modules[y][x] == runColor) {
                runVal++;
                if (runVal == 5) result += PENALTY_N1;
                else if (runVal > 5) result++;
            } else {
                runColor = modules[y][x];
                runVal = 1;
            }
        }
    }
    for (int x = 0; x < size; x++) {
        bool runColor = false;
        int runVal = 0;
        for (int y = 0; y < size; y++) {
            if (modules[y][x] == runColor) {
                runVal++;
                if (runVal == 5) result += PENALTY_N1;
                else if (runVal > 5) result++;
            } else {
                runColor = modules[y][x];
                runVal = 1;
            }
        }
    }
    return result;
}

}
