#include "usb_audio_device.hpp"
#include <cstring>

namespace qm::usbip::wire
{
unsigned number(const unsigned char *p)
{
    return unsigned(p[0]) << 24 | unsigned(p[1]) << 16 | unsigned(p[2]) << 8 | p[3];
}
void put(Bytes &b, size_t at, unsigned value)
{
    for (int i = 3; i >= 0; --i)
    {
        b[at + i] = value & 255;
        value >>= 8;
    }
}
Bytes operation(unsigned code, unsigned status)
{
    Bytes b{1, 0x11, static_cast<unsigned char>(code >> 8), static_cast<unsigned char>(code), 0, 0, 0, 0};
    put(b, 4, status);
    return b;
}
Bytes description()
{
    Bytes b(312);
    const char *path = "/quietmic/audio/1-1";
    std::memcpy(b.data(), path, std::strlen(path));
    std::memcpy(b.data() + 256, "1-1", 3);
    put(b, 288, 1);
    put(b, 292, 1);
    put(b, 296, 2);
    b[300] = 255;
    b[301] = 255;
    b[302] = 0x51;
    b[303] = 0xca;
    b[305] = 1;
    b[309] = 1;
    b[310] = 1;
    b[311] = 2;
    return b;
}
Bytes configuration()
{
    // 설정 → AudioControl(입력/출력 단자) → AudioStreaming(alt 0/1) →
    // 48kHz 모노 PCM16 형식 → 1ms당 96바이트 IN 엔드포인트 순이다.
    return {9,    2,    100,  0,  2, 1, 0,    0x80, 50,   9, 4, 0, 0, 0,  1,    1, 0, 0, 9, 0x24,
            1,    0,    1,    30, 0, 1, 1,    12,   0x24, 2, 1, 1, 2, 0,  1,    0, 0, 0, 0, 9,
            0x24, 3,    2,    1,  1, 0, 1,    0,    9,    4, 1, 0, 0, 1,  2,    0, 0, 9, 4, 1,
            1,    1,    1,    2,  0, 0, 7,    0x24, 1,    2, 1, 1, 0, 11, 0x24, 2, 1, 1, 2, 16,
            1,    0x80, 0xbb, 0,  9, 5, 0x81, 0x0d, 96,   0, 1, 0, 0, 7,  0x25, 1, 0, 0, 0, 0};
}
Bytes descriptor(unsigned type, unsigned index)
{
    if (type == 1)
        return {18, 1, 0x10, 1, 0, 0, 0, 64, 255, 255, 0xca, 0x51, 1, 0, 1, 2, 3, 1};
    if (type == 2)
        return configuration();
    if (type != 3)
        return {};
    if (!index)
        return {4, 3, 9, 4};
    const std::string value = index == 1   ? "QuietMic"
                              : index == 2 ? "QuietMic Output"
                              : index == 3 ? serial
                                           : "";
    if (value.empty())
        return {};
    Bytes b{static_cast<unsigned char>(2 + value.size() * 2), 3};
    for (unsigned char c : value)
    {
        b.push_back(c);
        b.push_back(0);
    }
    return b;
}
} // namespace qm::usbip::wire
