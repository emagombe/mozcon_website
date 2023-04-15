#define LOG_TAG "A_TAG"

#include <iostream>
#include <fstream>
#include "jni.h"
#include <android/log.h>
#include <vector>

extern "C" {

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "libavutil/audio_fifo.h"

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096
#define TAG "AudioWaveform"

int countLines(const std::string& filename) {
    std::ifstream file(filename);
    int count = 0;
    std::string line;
    while (std::getline(file, line)) {
        count++;
    }
    file.close();
    return count;
};

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_znbox_beatlevels_modules_Waveform_decode_1to_1float(JNIEnv* env, jobject thiz, jstring filePath, jstring outputPath ) {
    av_log_set_level(AV_LOG_ERROR);

    const char *path = env->GetStringUTFChars(filePath, nullptr);
    const char *outpath = env->GetStringUTFChars(outputPath, nullptr);

    AVFormatContext *formatContext = avformat_alloc_context();
    if (!formatContext) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not allocate format context");
        env->ReleaseStringUTFChars(filePath, path);
        return nullptr;
    }
    if (avformat_open_input(&formatContext, path, nullptr, nullptr) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not open file '%s'", path);
        avformat_free_context(formatContext);
        env->ReleaseStringUTFChars(filePath, path);
        return nullptr;
    }
    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not find stream information");
        avformat_close_input(&formatContext);
        avformat_free_context(formatContext);
        env->ReleaseStringUTFChars(filePath, path);
        return nullptr;
    }
    int streamIndex = -1;
    for (int i = 0; i < formatContext->nb_streams; i++) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            streamIndex = i;
            break;
        }
    }
    if (streamIndex == -1) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not find audio stream");
        avformat_close_input(&formatContext);
        avformat_free_context(formatContext);
        env->ReleaseStringUTFChars(filePath, path);
        return nullptr;
    }
    AVCodecParameters *codecParams = formatContext->streams[streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not find codec");
        avformat_close_input(&formatContext);
        avformat_free_context(formatContext);
        env->ReleaseStringUTFChars(filePath, path);
        return nullptr;
    }
    AVCodecContext *codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not allocate codec context");
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        avformat_free_context(formatContext);
        env->ReleaseStringUTFChars(filePath, path);
        return nullptr;
    }
    if (avcodec_parameters_to_context(codecContext, codecParams) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not copy codec parameters to context");
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        avformat_free_context(formatContext);
        env->ReleaseStringUTFChars(filePath, path);
        return nullptr;
    }
    AVFrame *frame = av_frame_alloc();
    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Could not open codec");
        avformat_close_input(&formatContext);
        avcodec_free_context(&codecContext);
        av_frame_free(&frame);
        return nullptr;
    }

    AVPacket *packet = av_packet_alloc();

    SwrContext *resampler{swr_alloc_set_opts(
            nullptr,
            formatContext->streams[streamIndex]->codecpar->channel_layout,
            AV_SAMPLE_FMT_S16,
            1,
            formatContext->streams[streamIndex]->codecpar->channel_layout,
            (AVSampleFormat) formatContext->streams[streamIndex]->codecpar->format,
            formatContext->streams[streamIndex]->codecpar->format,
            1,
            0
    )};

    std::vector<float> waveform_floats;
    std::ofstream out(outpath, std::ios::binary);
    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == streamIndex) {
            int result = avcodec_send_packet(codecContext, packet);
            if (result < 0) {
                av_packet_unref(packet);
                avformat_close_input(&formatContext);
                avcodec_free_context(&codecContext);
                av_frame_free(&frame);
                swr_free(&resampler);
                out.close();
                return nullptr;
            }

            while (result >= 0) {
                result = avcodec_receive_frame(codecContext, frame);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                    break;
                }
                else if (result < 0) {
                    av_packet_unref(packet);
                    avformat_close_input(&formatContext);
                    avcodec_free_context(&codecContext);
                    av_frame_free(&frame);
                    swr_free(&resampler);
                    out.close();
                    return nullptr;
                }

                /*for (int i = 0; i < frame->nb_samples; i++) {
                    float sampleValue = frame->data[0][i] / (float)INT16_MAX;
                    int index = (int)(100 * ((double)frame->pts / (double)formatContext->duration));
                    if (index < 100) {
                        waveform[index] = sampleValue;
                    }
                }*/

                AVFrame *resampler_frame = av_frame_alloc();
                resampler_frame->sample_rate = 10;
                resampler_frame->ch_layout = frame->ch_layout;
                resampler_frame->format = AV_SAMPLE_FMT_S16;

                if(swr_convert_frame(resampler, resampler_frame, frame) >= 0) {
                    auto *samples = (int16_t *) frame->data[0];
                    for(int c = 0; c < resampler_frame->ch_layout.nb_channels; c ++) {
                        float sum = 0;
                        for(int i = 0; i < resampler_frame->nb_samples; i ++) {
                            if(samples[i * resampler_frame->ch_layout.nb_channels + c] < 0) {
                                sum += (float) samples[i * resampler_frame->ch_layout.nb_channels + c] * (-1);
                            } else {
                                sum += (float) samples[i * resampler_frame->ch_layout.nb_channels + c];
                            }
                            int average_point = (int) ((sum * 2) / (float) resampler_frame->nb_samples);
                            if(average_point > 0) {
                                //waveform_floats.push_back(average_point);
                                out << average_point << "\n";
                            }
                        }
                    }
                }

                av_frame_unref(frame);
                av_frame_unref(resampler_frame);
            }
        }

        av_packet_unref(packet);
    }
    swr_free(&resampler);
    avformat_close_input(&formatContext);
    avcodec_free_context(&codecContext);
    av_frame_free(&frame);
    out.close();
    return nullptr;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_znbox_beatlevels_modules_Waveform_decode_1to_1pcm(JNIEnv *env, jobject thiz, jstring input, jstring output) {
    const char * filename = (*env).GetStringUTFChars(input, 0);
    const char * outfilename = (*env).GetStringUTFChars(output, 0);
    JavaVM *vm;
    env->GetJavaVM(&vm);
    try {
        /* Open File */
        AVFormatContext * format_ctx{nullptr};
        int result_open = avformat_open_input(&format_ctx, filename, nullptr, nullptr);
        if(result_open < 0) {
            __android_log_print(ANDROID_LOG_ERROR, "exception: ", "%s", av_err2str(result_open));
            return;
        }

        result_open = avformat_find_stream_info(format_ctx, nullptr);
        if(result_open < 0) {
            __android_log_print(ANDROID_LOG_ERROR, "exception: ", "%s", "Failed to read stream info");
            return;
        }

        int index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if(index < 0) {
            __android_log_print(ANDROID_LOG_ERROR, "exception: ", "%s", "No audio stream inside the file");
            return;
        }

        /* Finding decoder */
        AVStream *streams = format_ctx->streams[index];

        const AVCodec *decoder = avcodec_find_decoder(streams->codecpar->codec_id);
        if(!decoder) {
            __android_log_print(ANDROID_LOG_ERROR, "exception: ", "%s", "No decoders available for the audio format");
            return;
        }

        AVCodecContext *codec_ctx{avcodec_alloc_context3(decoder)};

        avcodec_parameters_to_context(codec_ctx, streams->codecpar);

        /* Opening decoder */
        result_open = avcodec_open2(codec_ctx, decoder, nullptr);
        if(result_open < 0) {
            __android_log_print(ANDROID_LOG_ERROR, "exception: ", "%s", "Failed to open decoder");
            return;
        }

        /* Decoding the audio */
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        SwrContext *resampler{swr_alloc_set_opts(
                nullptr,
                streams->codecpar->channel_layout,
                AV_SAMPLE_FMT_S16,
                1,
                streams->codecpar->channel_layout,
                (AVSampleFormat) streams->codecpar->format,
                streams->codecpar->format,
                1,
                0
        )};

        std::ofstream out(outfilename, std::ios::binary);
        while (av_read_frame(format_ctx, packet) >= 0) {
            if(packet->stream_index != streams->index) {
                continue;
            }

            result_open = avcodec_send_packet(codec_ctx, packet);
            if(result_open < 0) {
                // AVERROR(EAGAIN) --> Send the packet again getting frames out!
                if(result_open != AVERROR(EAGAIN)) {
                    __android_log_print(ANDROID_LOG_ERROR, "exception: ", "%s", "Error decoding...");
                }
                return;
            }
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                /* Resample the frame */
                AVFrame *resampler_frame = av_frame_alloc();
                resampler_frame->sample_rate = 10;
                resampler_frame->ch_layout = frame->ch_layout;
                resampler_frame->format = AV_SAMPLE_FMT_S16;

                result_open = swr_convert_frame(resampler, resampler_frame, frame);
                if(result_open >= 0) {
                    auto *samples = (int16_t *) frame->data[0];
                    for(int c = 0; c < resampler_frame->ch_layout.nb_channels; c ++) {
                        float sum = 0;
                        for(int i = 0; i < resampler_frame->nb_samples; i ++) {
                            if(samples[i * resampler_frame->ch_layout.nb_channels + c] < 0) {
                                sum += (float) samples[i * resampler_frame->ch_layout.nb_channels + c] * (-1);
                            } else {
                                sum += (float) samples[i * resampler_frame->ch_layout.nb_channels + c];
                            }
                            int average_point = (int) ((sum * 2) / (float) resampler_frame->nb_samples);
                            if(average_point > 0) {
                                out << average_point << "\n";
                            }
                        }
                    }
                    samples = nullptr;
                    frame->data[0] = nullptr;
                }
                av_frame_unref(resampler_frame);
                av_frame_free(&resampler_frame);
                resampler_frame = nullptr;
            }
        }

        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        swr_free(&resampler);
        avformat_close_input(&format_ctx);


        format_ctx = nullptr;
        codec_ctx = nullptr;
        packet = nullptr;
        frame = nullptr;
        decoder = nullptr;
        streams = nullptr;

        out.close();
        (*env).ReleaseStringUTFChars(input, filename);
        (*env).ReleaseStringUTFChars(output, outfilename);
        return;
    } catch (std::exception &exception) {
        __android_log_print(ANDROID_LOG_ERROR, "exception: ", "%s", exception.what());
    }
}
};