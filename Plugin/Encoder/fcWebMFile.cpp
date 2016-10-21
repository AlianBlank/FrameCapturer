#include "pch.h"
#include "fcFoundation.h"
#include "fcWebMFile.h"

#ifdef fcSupportWebM
#include "fcI420.h"
#include "fcVorbisEncoder.h"
#include "fcVPXEncoder.h"
#include "fcWebMWriter.h"
#include "GraphicsDevice/fcGraphicsDevice.h"


template<class T>
class ResourceQueue
{
public:
    void push(T v)
    {
        std::unique_lock<std::mutex> l(m_mutex);
        m_resources.push_back(v);
    }

    T pop()
    {
        T ret;
        for (;;) {
            {
                std::unique_lock<std::mutex> l(m_mutex);
                if (!m_resources.empty()) {
                    ret = m_resources.back();
                    m_resources.pop_back();
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return ret;
    }

private:
    std::mutex m_mutex;
    std::deque<T> m_resources;
};



class fcWebMContext : public fcIWebMContext
{
public:
    using VideoEncoderPtr = std::unique_ptr<fcIWebMVideoEncoder>;
    using AudioEncoderPtr = std::unique_ptr<fcIWebMAudioEncoder>;
    using WriterPtr       = std::unique_ptr<fcIWebMWriter>;
    using WriterPtrs      = std::vector<WriterPtr>;

    using VideoBuffer       = Buffer;
    using VideoBufferPtr    = std::shared_ptr<VideoBuffer>;
    using VideoBufferQueue  = ResourceQueue<VideoBufferPtr>;

    using AudioBuffer       = RawVector<float>;
    using AudioBufferPtr    = std::shared_ptr<AudioBuffer>;
    using AudioBufferQueue  = ResourceQueue<AudioBufferPtr>;

    using TaskQueue = std::deque<std::function<void()>>;
    using Lock = std::unique_lock<std::mutex>;


    fcWebMContext(fcWebMConfig &conf, fcIGraphicsDevice *gd);
    ~fcWebMContext() override;
    void release() override;

    void addOutputStream(fcStream *s) override;

    bool addVideoFrameTexture(void *tex, fcPixelFormat fmt, fcTime timestamp) override;
    bool addVideoFramePixels(const void *pixels, fcPixelFormat fmt, fcTime timestamp) override;
    bool addVideoFramePixelsImpl(const void *pixels, fcPixelFormat fmt, fcTime timestamp);
    void flushVideo();

    bool addAudioFrame(const float *samples, int num_samples, fcTime timestamp) override;
    void flushAudio();


    // Body: [](fcIWebMWriter& writer) {}
    template<class Body>
    void eachStreams(const Body &b)
    {
        for (auto& s : m_writers) { b(*s); }
    }

private:
    void processVideoTasks(); // thread entry point
    void kickVideoTask(const std::function<void()>& v);

    void processAudioTasks(); // thread entry point
    void kickAudioTask(const std::function<void()>& v);


    fcWebMConfig        m_conf;
    fcIGraphicsDevice   *m_gdev = nullptr;
    bool                m_stop = false;

    WriterPtrs          m_writers;

    TaskQueue               m_video_tasks;
    std::thread             m_video_thread;
    std::mutex              m_video_mutex;
    std::condition_variable m_video_condition;
    VideoEncoderPtr         m_video_encoder;
    VideoBufferQueue        m_video_buffers;
    Buffer                  m_rgba_image;
    fcI420Image             m_i420_image;
    fcWebMVideoFrame        m_video_frame;

    TaskQueue               m_audio_tasks;
    std::thread             m_audio_thread;
    std::mutex              m_audio_mutex;
    std::condition_variable m_audio_condition;
    AudioEncoderPtr         m_audio_encoder;
    AudioBufferQueue        m_audio_buffers;
    fcWebMAudioFrame        m_audio_frame;
};


fcWebMContext::fcWebMContext(fcWebMConfig &conf, fcIGraphicsDevice *gd)
    : m_conf(conf)
    , m_gdev(gd)
{
    if (conf.video) {
        fcVPXEncoderConfig econf;
        econf.width = conf.video_width;
        econf.height = conf.video_height;
        econf.target_bitrate = conf.video_bitrate;

        switch (conf.video_encoder) {
        case fcWebMVideoEncoder::VP8:
            m_video_encoder.reset(fcCreateVP8Encoder(econf));
            break;
        case fcWebMVideoEncoder::VP9:
            m_video_encoder.reset(fcCreateVP9Encoder(econf));
            break;
        }

        for (int i = 0; i < 4; ++i) {
            m_video_buffers.push(VideoBufferPtr(new VideoBuffer()));
        }
        m_video_thread = std::thread([this]() { processVideoTasks(); });
    }

    if (conf.audio) {
        fcVorbisEncoderConfig econf;
        econf.sample_rate = conf.audio_sample_rate;
        econf.num_channels = conf.audio_num_channels;
        econf.target_bitrate = conf.audio_bitrate;

        switch (conf.audio_encoder) {
        case fcWebMAudioEncoder::Vorbis:
            m_audio_encoder.reset(fcCreateVorbisEncoder(econf));
            break;
        case fcWebMAudioEncoder::Opus:
            m_audio_encoder.reset(fcCreateOpusEncoder(econf));
            break;
        }

        for (int i = 0; i < 4; ++i) {
            m_audio_buffers.push(AudioBufferPtr(new AudioBuffer()));
        }
        m_audio_thread = std::thread([this]() { processAudioTasks(); });
    }
}

fcWebMContext::~fcWebMContext()
{
    flushVideo();
    flushAudio();

    m_stop = true;
    if (m_conf.video) {
        m_video_condition.notify_all();
        m_video_thread.join();
    }
    if (m_conf.audio) {
        m_audio_condition.notify_all();
        m_audio_thread.join();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    m_video_encoder.reset();
    m_audio_encoder.reset();
    m_writers.clear();
}

void fcWebMContext::release()
{
    delete this;
}

void fcWebMContext::addOutputStream(fcStream *s)
{
    auto *writer = fcCreateWebMWriter(*s, m_conf);
    if (m_video_encoder) { writer->setVideoEncoderInfo(*m_video_encoder); }
    if (m_audio_encoder) { writer->setAudioEncoderInfo(*m_audio_encoder); }
    m_writers.emplace_back(writer);
}


void fcWebMContext::processVideoTasks()
{
    while (!m_stop)
    {
        std::function<void()> task;
        {
            Lock lock(m_video_mutex);
            while (!m_stop && m_video_tasks.empty()) {
                m_video_condition.wait(lock);
            }
            if (m_stop && m_video_tasks.empty()) { return; }

            task = m_video_tasks.front();
            m_video_tasks.pop_front();
        }
        task();
    }
}

void fcWebMContext::kickVideoTask(const std::function<void()>& v)
{
    {
        Lock l(m_video_mutex);
        m_video_tasks.push_back(v);
    }
    m_video_condition.notify_one();
}


void fcWebMContext::processAudioTasks()
{
    while (!m_stop)
    {
        std::function<void()> task;
        {
            Lock lock(m_audio_mutex);
            while (!m_stop && m_audio_tasks.empty()) {
                m_audio_condition.wait(lock);
            }
            if (m_stop && m_audio_tasks.empty()) { return; }

            task = m_audio_tasks.front();
            m_audio_tasks.pop_front();
        }
        task();
    }
}

void fcWebMContext::kickAudioTask(const std::function<void()>& v)
{
    {
        Lock l(m_audio_mutex);
        m_audio_tasks.push_back(v);
    }
    m_audio_condition.notify_one();
}


bool fcWebMContext::addVideoFrameTexture(void *tex, fcPixelFormat fmt, fcTime timestamp)
{
    if (!tex || !m_video_encoder || !m_gdev) { return false; }

    auto buf = m_video_buffers.pop();
    size_t psize = fcGetPixelSize(fmt);
    size_t size = m_conf.video_width * m_conf.video_height * psize;
    buf->resize(size);
    if (m_gdev->readTexture(buf->data(), buf->size(), tex, m_conf.video_width, m_conf.video_height, fmt)) {
        kickVideoTask([this, buf, fmt, timestamp]() {
            addVideoFramePixelsImpl(buf->data(), fmt, timestamp);
            m_video_buffers.push(buf);
        });
    }
    else {
        m_video_buffers.push(buf);
        return false;
    }
    return true;
}

bool fcWebMContext::addVideoFramePixels(const void *pixels, fcPixelFormat fmt, fcTime timestamp)
{
    if (!pixels || !m_video_encoder) { return false; }

    auto buf = m_video_buffers.pop();
    size_t psize = fcGetPixelSize(fmt);
    size_t size = m_conf.video_width * m_conf.video_height * psize;
    buf->resize(size);
    memcpy(buf->data(), pixels, size);

    kickVideoTask([this, buf, fmt, timestamp]() {
        addVideoFramePixelsImpl(buf->data(), fmt, timestamp);
        m_video_buffers.push(buf);
    });
    return true;
}

bool fcWebMContext::addVideoFramePixelsImpl(const void *pixels, fcPixelFormat fmt, fcTime timestamp)
{
    fcI420Data i420;

    if (fmt == fcPixelFormat_I420) {
        int frame_size = m_conf.video_width * m_conf.video_height;
        i420.y = pixels;
        i420.u = (char*)i420.y + frame_size;
        i420.v = (char*)i420.u + (frame_size >> 2);
    }
    else if (fmt == fcPixelFormat_RGBAu8) {
        m_i420_image.resize(m_conf.video_width, m_conf.video_height);
        fcRGBA2I420(m_i420_image, pixels, m_conf.video_width, m_conf.video_height);
        i420 = m_i420_image.data();
    }
    else {
        m_rgba_image.resize(m_conf.video_width * m_conf.video_height * 4);
        fcConvertPixelFormat(m_rgba_image.data(), fcPixelFormat_RGBAu8, pixels, fmt, m_conf.video_width * m_conf.video_height);

        m_i420_image.resize(m_conf.video_width, m_conf.video_height);
        fcRGBA2I420(m_i420_image, m_rgba_image.data(), m_conf.video_width, m_conf.video_height);
        i420 = m_i420_image.data();
    }

    if (m_video_encoder->encode(m_video_frame, i420, timestamp)) {
        eachStreams([&](auto& writer) {
            writer.addVideoFrame(m_video_frame);
        });
        m_video_frame.clear();
    }

    return true;
}


void fcWebMContext::flushVideo()
{
    if (!m_video_encoder) { return; }

    kickVideoTask([this]() {
        if (m_video_encoder->flush(m_video_frame)) {
            eachStreams([&](auto& writer) {
                writer.addVideoFrame(m_video_frame);
            });
            m_video_frame.clear();
        }
    });
}


bool fcWebMContext::addAudioFrame(const float *samples, int num_samples, fcTime timestamp)
{
    if (!samples || !m_audio_encoder) { return false; }

    auto buf = m_audio_buffers.pop();
    buf->assign(samples, num_samples);

    kickAudioTask([this, buf]() {
        if (m_audio_encoder->encode(m_audio_frame, buf->data(), buf->size())) {
            eachStreams([&](auto& writer) {
                writer.addAudioFrame(m_audio_frame);
            });
            m_audio_frame.clear();
        }
        m_audio_buffers.push(buf);
    });
    return true;
}

void fcWebMContext::flushAudio()
{
    if (!m_audio_encoder) { return; }

    kickAudioTask([this]() {
        if (m_audio_encoder->flush(m_audio_frame)) {
            eachStreams([&](auto& writer) {
                writer.addAudioFrame(m_audio_frame);
            });
            m_audio_frame.clear();
        }
    });
}


fcWebMAPI fcIWebMContext* fcWebMCreateContextImpl(fcWebMConfig &conf, fcIGraphicsDevice *gd) { return new fcWebMContext(conf, gd); }

#else  // fcSupportWebM

fcWebMAPI fcIWebMContext* fcWebMCreateContextImpl(fcWebMConfig &conf, fcIGraphicsDevice *gd) { return nullptr; }

#endif // fcSupportWebM
