#pragma once
#include <functional>
#include <cstdint>

extern "C" {
#include <x264.h>
}

using NaluCallback = std::function<void(uint8_t*, int, uint32_t)>;

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    bool init(int width, int height, int fps);
    void encode(uint32_t pts, NaluCallback onNalu);

    uint8_t** getInImgPlanes() { return pic_in.img.plane; }
    int* getInImgStrides() { return pic_in.img.i_stride; }

private:
    x264_t* encoder = nullptr;
    x264_picture_t pic_in, pic_out;
    int width = 0;
    int height = 0;
};
