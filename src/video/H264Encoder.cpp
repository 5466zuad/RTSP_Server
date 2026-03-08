#include "video/H264Encoder.h"
#include <iostream>

H264Encoder::H264Encoder() {}

H264Encoder::~H264Encoder() {
    if (encoder) {
        x264_picture_clean(&pic_in);
        x264_encoder_close(encoder);
    }
}

bool H264Encoder::init(int width, int height, int fps) {
    this->width = width;
    this->height = height;

    x264_param_t param;
    x264_param_default_preset(&param, "ultrafast", "zerolatency");
    param.i_threads    = 0;
    param.i_width      = width;
    param.i_height     = height;
    param.i_fps_num    = fps;
    param.i_fps_den    = 1;
    param.i_keyint_max = fps * 2;
    param.b_intra_refresh = 0;
    param.rc.i_rc_method  = X264_RC_CRF;
    param.rc.f_rf_constant = 23.0f;
    x264_param_apply_profile(&param, "main");

    encoder = x264_encoder_open(&param);
    if (!encoder) return false;

    x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);
    return true;
}

void H264Encoder::encode(uint32_t pts, NaluCallback onNalu) {
    pic_in.i_pts = pts;
    x264_nal_t* nals = nullptr;
    int num_nals = 0;
    int frame_size = x264_encoder_encode(encoder, &nals, &num_nals, &pic_in, &pic_out);

    if (frame_size > 0 && onNalu) {
        for (int i = 0; i < num_nals; ++i) {
            onNalu(nals[i].p_payload, nals[i].i_payload, pts);
        }
    }
}
