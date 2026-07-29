#pragma once
#include "sdkconfig.h"

#ifndef CONFIG_IDF_TARGET_ESP32
#include <lvgl.h>
#include <thread>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "jpg/image_to_jpeg.h"
#include "esp_video_init.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class StackChanCamera : public Camera {
private:
    struct FrameBuffer {
        uint8_t* data         = nullptr;
        size_t len            = 0;
        uint16_t width        = 0;
        uint16_t height       = 0;
        v4l2_pix_fmt_t format = 0;
    } frame_;
    v4l2_pix_fmt_t sensor_format_ = 0;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    uint16_t sensor_width_  = 0;
    uint16_t sensor_height_ = 0;
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    int video_fd_      = -1;
    bool streaming_on_ = false;
    struct MmapBuffer {
        void* start   = nullptr;
        size_t length = 0;
    };
    std::vector<MmapBuffer> mmap_buffers_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;

public:
    StackChanCamera(const esp_video_init_config_t& config);
    ~StackChanCamera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture() override;
    bool StreamCaptures();

    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);

    // JPEG-encode the last Capture() and hand the bytes back to the caller instead of
    // POSTing them to a remote VLM the way Explain() does. Same encoder, same frame --
    // only the destination differs, so a connected host can see the actual pixels rather
    // than someone else's prose about them. Empty string on failure.
    //
    // quality is the JPEG quality passed to the encoder. Streaming wants it lower than a
    // one-off snapshot: the frames are transient, and both encode time and the number of
    // bytes crossing USB scale with it.
    std::string CaptureToJpeg(int quality = 80);

    /**
     * @brief Grab a frame that is actually current, for streaming.
     *
     * StreamCaptures() returns whatever is sitting in the driver's queue, and V4L2 hands
     * back the OLDEST buffer. When frames are pulled on demand at 1 fps that buffer was
     * filled moments after the previous requeue -- so it is nearly a second old, and a
     * model answering from it describes what the user was showing a question ago.
     *
     * The fix is to discard everything already queued and take the next frame the sensor
     * produces. The discard count is the queue depth, so it stays correct if the driver
     * is ever configured with more buffers -- unlike hardcoding "twice", which happens to
     * be right only for the single-buffer case.
     *
     * Costs one sensor frame interval (~50 ms at 20 fps) on top of a normal grab.
     */
    bool CaptureFresh();

    const uint8_t* GetFrameData()
    {
        return frame_.data;
    }
    size_t GetFrameSize()
    {
        return frame_.len;
    }
    int GetFrameWidth()
    {
        return frame_.width;
    }
    int GetFrameHeight()
    {
        return frame_.height;
    }
    int GetFrameFormat()
    {
        return frame_.format;
    }
};

#endif  // ndef CONFIG_IDF_TARGET_ESP32
