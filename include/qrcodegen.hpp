/* 
 * QR Code generator library (C++)
 * 
 * Copyright (c) Project Nayuki. (MIT License)
 * https://www.nayuki.io/page/qr-code-generator-library
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qrcodegen {

class QrSegment final {
public:
    enum class Mode {
        NUMERIC = 0x1,
        ALPHANUMERIC = 0x2,
        BYTE = 0x4,
        KANJI = 0x8,
        ECI = 0x7
    };

    static QrSegment makeBytes(const std::vector<uint8_t> &data);
    static QrSegment makeNumeric(const char *digits);
    static QrSegment makeAlphanumeric(const char *text);
    static std::vector<QrSegment> makeSegments(const char *text);

    QrSegment(Mode md, int numCh, const std::vector<bool> &dt);
    QrSegment(Mode md, int numCh, std::vector<bool> &&dt);

    Mode getMode() const;
    int getNumChars() const;
    const std::vector<bool> &getData() const;
    static int getTotalBits(const std::vector<QrSegment> &segs, int version);

private:
    Mode mode;
    int numChars;
    std::vector<bool> data;
};

class QrCode final {
public:
    enum class Ecc {
        LOW = 0,
        MEDIUM,
        QUARTILE,
        HIGH
    };

    static constexpr int MIN_VERSION = 1;
    static constexpr int MAX_VERSION = 40;

    static QrCode encodeText(const char *text, Ecc ecc);
    static QrCode encodeBinary(const std::vector<uint8_t> &data, Ecc ecc);
    static QrCode encodeSegments(const std::vector<QrSegment> &segs, Ecc ecc,
        int minVersion = 1, int maxVersion = 40, int mask = -1, bool boostEcl = true);

    QrCode(int ver, Ecc ecl, const std::vector<uint8_t> &dataCodewords, int msk);

    int getVersion() const;
    int getSize() const;
    Ecc getErrorCorrectionLevel() const;
    int getMask() const;
    bool getModule(int x, int y) const;

private:
    int version;
    int size;
    Ecc errorCorrectionLevel;
    int mask;
    std::vector<std::vector<bool>> modules;
    std::vector<std::vector<bool>> isFunction;

    void drawFunctionPatterns();
    void drawFormatBits(int msk);
    void drawVersion();
    void drawFinderPattern(int x, int y);
    void drawAlignmentPattern(int x, int y);
    void setFunctionModule(int x, int y, bool isBlack);
    bool module(int x, int y, bool defaultValue = false) const;
    void drawCodewords(const std::vector<uint8_t> &data);
    void applyMask(int msk);
    long getPenaltyScore() const;

    static bool getBit(int x, int i);
    static const int PENALTY_N1;
    static const int PENALTY_N2;
    static const int PENALTY_N3;
    static const int PENALTY_N4;
};

}
