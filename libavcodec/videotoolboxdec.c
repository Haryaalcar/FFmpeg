
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"

#include "avcodec.h"
#include "internal.h"
#include <sys/queue.h>

//TODO: key frames?

typedef struct DecodedFrame {
    CVPixelBufferRef pixbuf;
    int64_t pts;
    int64_t duration;
    TAILQ_ENTRY(DecodedFrame) entries;
} DecodedFrame;


typedef struct H264VideotoolboxContext {
    CMVideoFormatDescriptionRef format_description;
    VTDecompressionSessionRef decompression_session;

    bool avc_type_parsed;
    bool is_avc;            //avc means not annex b
    int nalu_length_size;   //from extradata, does not apply to annex b delimeter size

    int sps_size;
    int pps_size;

    uint8_t* sps;
    uint8_t* pps;

    int64_t last_returned_pts;

    TAILQ_HEAD(DF, DecodedFrame) decoded_frames;
    int decoded_frames_count;
    int reorder_queue_size;

} H264VideotoolboxContext;


static void add_decoded_frame_to_queue(AVCodecContext *avctx, CVPixelBufferRef pixbuf, int64_t pts, int64_t duration) {
    H264VideotoolboxContext *context = avctx->priv_data;

    DecodedFrame *decoded_frame = (DecodedFrame *)malloc(sizeof(DecodedFrame));
    
    decoded_frame->pixbuf = CVPixelBufferRetain(pixbuf);
    decoded_frame->pts = pts;
    decoded_frame->duration = duration;

    DecodedFrame *first_frame = TAILQ_FIRST(&context->decoded_frames);

    if (!first_frame || pts < first_frame->pts) {
        TAILQ_INSERT_HEAD(&context->decoded_frames, decoded_frame, entries);
    }
    else {
        DecodedFrame* closest_earlier_frame = NULL;
        TAILQ_FOREACH_REVERSE(closest_earlier_frame, &context->decoded_frames, DF, entries) {
            if (closest_earlier_frame->pts < pts) {
                TAILQ_INSERT_AFTER(&context->decoded_frames, closest_earlier_frame, decoded_frame, entries);
                break;
            }
        }
    }

    {
        av_log(avctx, AV_LOG_INFO, "decoded frames queue:\n");
        DecodedFrame* frame = NULL;
        TAILQ_FOREACH(frame, &context->decoded_frames, entries) {
            av_log(avctx, AV_LOG_INFO, "pts: %lld\n", frame->pts);
        }
    }
    context->decoded_frames_count++;
}


static void drop_decoded_frame_queue_head(H264VideotoolboxContext *context) {
    DecodedFrame *frame = TAILQ_FIRST(&context->decoded_frames);
    if (frame) {
        TAILQ_REMOVE(&context->decoded_frames, frame, entries);
        CVPixelBufferRelease(frame->pixbuf);
        free(frame);
        context->decoded_frames_count--;
    }
}


static void set_sps(H264VideotoolboxContext *context, const uint8_t* sps, int sps_size) {
    if (context->sps) {
        free(context->sps);
        context->sps = 0;
    }

    if (sps && sps_size) {
        context->sps_size = sps_size;
        context->sps = malloc(sps_size);
        memcpy(context->sps, sps, sps_size);
    }
}

static void set_pps(H264VideotoolboxContext *context, const uint8_t* pps, int pps_size) {
    if (context->pps) {
        free(context->pps);
        context->pps = 0;
    }

    if (pps && pps_size) {
        context->pps_size = pps_size;
        context->pps = malloc(pps_size);
        memcpy(context->pps, pps, pps_size);
    }
}


typedef struct NALU {
    const uint8_t* ptr;
    const uint8_t* data_ptr;
    int type;
    int size;
    int nri;
    int data_size;
    int delimeter_size;
    bool is_incomplete; //for avc only can detect
    bool is_decodable;  //type is 1 or 5
    struct NALU *next;
} NALU;


static NALU * create_NALU(H264VideotoolboxContext *context, const uint8_t* frame, const int max_length) {
    NALU *nalu = (NALU *)malloc(sizeof(NALU));
    memset(nalu, 0, sizeof(NALU));
    if (context->is_avc) {
        if (max_length <= context->nalu_length_size) {
            free(nalu);
            av_log(NULL, AV_LOG_ERROR, "Invalid nalu data of size %d\n", max_length);
            return NULL;
        }

        int nalu_data_size = 0;
        for (int i = 0; i < context->nalu_length_size; i++) {
            nalu_data_size = (nalu_data_size << 8) | frame[i];
        }
        nalu->size = context->nalu_length_size + nalu_data_size;
        nalu->data_size = nalu_data_size;
        nalu->type = frame[context->nalu_length_size] & 0x1F;
        nalu->nri = (frame[context->nalu_length_size] & 0x60) >> 5;
        nalu->delimeter_size = context->nalu_length_size;
        nalu->is_incomplete = max_length < nalu->size;
        nalu->ptr = frame;
        nalu->data_ptr = frame + context->nalu_length_size;
    }
    else {
        if (max_length < 5) {
            free(nalu);
            av_log(NULL, AV_LOG_ERROR, "Invalid nalu data of size %d\n", max_length);
            return NULL;
        }

        int annexb_delimeter_size = 0;
        if (AV_RB24(frame) == 1) {
            annexb_delimeter_size = 3;
        }
        else if (AV_RB32(frame) == 1) {
            annexb_delimeter_size = 4;
        }
        int nalu_data_size = 0;
        for (int i = annexb_delimeter_size; i < max_length - annexb_delimeter_size; i++) {
             if (frame[i] == 0 && frame[i + 1] == 0 && (frame[i + 2] == 1 || (frame[i + 2] == 0 && frame[i + 3] == 1))) {
                nalu_data_size = i - annexb_delimeter_size;
                break;
            }
        }

        nalu_data_size = nalu_data_size ?: max_length - annexb_delimeter_size;
        
        nalu->size = annexb_delimeter_size + nalu_data_size;
        nalu->data_size = nalu_data_size;
        nalu->type = frame[annexb_delimeter_size] & 0x1F;
        nalu->nri = (frame[annexb_delimeter_size] & 0x60) >> 5;
        nalu->delimeter_size = annexb_delimeter_size;
        nalu->is_incomplete = (max_length < annexb_delimeter_size + 2); //cannot determine as for avc
        nalu->ptr = frame;
        nalu->data_ptr = frame + annexb_delimeter_size;
    }

    nalu->is_decodable = nalu->type == 1 || nalu->type == 5;

    if (nalu->is_incomplete) {
        av_log(NULL, AV_LOG_ERROR, "nalu is incomplete\n");
    }
    return nalu;
}


static NALU *build_nalu_list(H264VideotoolboxContext *context, const uint8_t* frame, const int max_length) {
    NALU *nalu = create_NALU(context, frame, max_length);
    if (nalu && (max_length > nalu->size)) {
        nalu->next = build_nalu_list(context, frame + nalu->size, max_length - nalu->size);
    }
    return nalu;
}


static void free_nalu_list(NALU *nalu) {
    nalu->next ? free_nalu_list(nalu->next) : free(nalu);
}


static void parse_avc_type(H264VideotoolboxContext *context, const uint8_t* frameStart) {
    if (context->avc_type_parsed) {
        return;
    }
    context->avc_type_parsed = true;

    if (AV_RB24(frameStart) == 1) {
        context->is_avc = false;
    } 
    else if (AV_RB32(frameStart) == 1) {
        context->is_avc = false;
    }
    else {
        context->is_avc = true;
    }
}


static void create_format_description(AVCodecContext *avctx) {
    H264VideotoolboxContext *context = avctx->priv_data;

    uint8_t*  parameterSetPointers[2] = {context->sps, context->pps};
    size_t parameterSetSizes[2] = {context->sps_size, context->pps_size};

    // suggestion from @Kris Dude's answer below
    if (context->format_description) {
        CFRelease(context->format_description);
        context->format_description = NULL;
    }

    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
                                                                          2,
                                                                          (const uint8_t* const*)parameterSetPointers,
                                                                          parameterSetSizes,
                                                                          4,
                                                                          &context->format_description);

    int level = (status == noErr) ? AV_LOG_INFO : AV_LOG_ERROR;
    av_log(avctx, level, "\t\t Create format Description: OSStatus: %d\n", (int)status);
}


static void decompressionSessionDecodeFrameCallback(void* decompressionOutputRefCon,
                                                    void* sourceFrameRefCon,
                                                    OSStatus status,
                                                    VTDecodeInfoFlags infoFlags,
                                                    CVImageBufferRef imageBuffer,
                                                    CMTime presentationTimeStamp,
                                                    CMTime presentationDuration) {
    AVCodecContext* avctx = decompressionOutputRefCon;

    if (status != noErr) {
        CFErrorRef error = CFErrorCreate(NULL, kCFErrorDomainOSStatus, status, NULL);
        CFStringRef error_description = CFErrorCopyDescription(error);
        av_log(avctx, AV_LOG_ERROR, "Decompressed OSStatus: %d description:%s\n", status, CFStringGetCStringPtr(error_description, kCFStringEncodingUTF8));
        CFRelease(error);
        CFRelease(error_description);
    }
    else {
        av_log(avctx, AV_LOG_INFO, "Decompressed sucessfully, PTS: %lld,  img %p\n", presentationTimeStamp.value, imageBuffer);

        add_decoded_frame_to_queue(avctx, imageBuffer, presentationTimeStamp.value, presentationDuration.value);
    }
}


static void decompress_sample_buffer(AVCodecContext *avctx, CMSampleBufferRef sampleBuffer) {
    H264VideotoolboxContext *context = avctx->priv_data;
    
    OSStatus status;
    VTDecodeFrameFlags flags = 0;// kVTDecodeFrame_EnableTemporalProcessing | kVTDecodeFrame_EnableAsynchronousDecompression;
    VTDecodeInfoFlags decode_info_flags;
    status = VTDecompressionSessionDecodeFrame(context->decompression_session,
                                               sampleBuffer,
                                               flags,
                                               NULL,
                                               &decode_info_flags);

    if (status != noErr) {
        av_log(avctx, AV_LOG_ERROR, "VTDecompressionSessionDecodeFrame failed with OSStatus: %d, info flags:%d\n", status, decode_info_flags);
    }

    if (status == noErr) {
        status = VTDecompressionSessionWaitForAsynchronousFrames(context->decompression_session);
    }

    if (status != noErr) {
        av_log(avctx, AV_LOG_ERROR, "VTDecompressionSessionWaitForAsynchronousFrames failed with OSStatus: %d\n", status);
    }
}


static void create_decompression_session(AVCodecContext *avctx) {
    H264VideotoolboxContext *context = avctx->priv_data;

    if (context->decompression_session) {
        VTDecompressionSessionInvalidate(context->decompression_session);
        CFRelease(context->decompression_session);
    }

    context->decompression_session = NULL;

    VTDecompressionOutputCallbackRecord callBackRecord;
    callBackRecord.decompressionOutputCallback = decompressionSessionDecodeFrameCallback;
    callBackRecord.decompressionOutputRefCon = avctx;

    CFMutableDictionaryRef image_buffer_attributes = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                                               4,
                                                                               &kCFTypeDictionaryKeyCallBacks,
                                                                               &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(image_buffer_attributes, kCVPixelBufferIOSurfaceOpenGLTextureCompatibilityKey, kCFBooleanTrue);

    int pixel_format = kCVPixelFormatType_420YpCbCr8Planar;
    CFNumberRef pixelFormat = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
    CFDictionarySetValue(image_buffer_attributes, kCVPixelBufferPixelFormatTypeKey, pixelFormat);

    OSStatus status = VTDecompressionSessionCreate(NULL,
                                                   context->format_description,
                                                   NULL,
                                                   image_buffer_attributes,
                                                   &callBackRecord,
                                                   &context->decompression_session);
    CFRelease(image_buffer_attributes);
    CFRelease(pixelFormat);
    av_log(avctx, AV_LOG_INFO, "Video Decompression Session Create: \t %s\n", (status == noErr) ? "successful!" : "failed...");
    if(status != noErr) {
        av_log(avctx, AV_LOG_INFO, "\t\t VTD ERROR type: %d\n", (int)status);
    }
}


static int copy_cvpixelbuffer(AVCodecContext *avctx, CVPixelBufferRef image_buffer, AVFrame *avframe) {
    int src_linesize[4];
    const uint8_t *src_data[4];
    __unused int width  = CVPixelBufferGetWidth(image_buffer);
    __unused int height = CVPixelBufferGetHeight(image_buffer);
    int status;

    memset(src_linesize, 0, sizeof(src_linesize));
    memset(src_data, 0, sizeof(src_data));

    status = CVPixelBufferLockBaseAddress(image_buffer, 0);
    if (status != kCVReturnSuccess) {
        av_log(avctx, AV_LOG_ERROR, "Could not lock base address: %d\n", status);
        return AVERROR_EXTERNAL;
    }

    if (CVPixelBufferIsPlanar(image_buffer)) {
        size_t plane_count = CVPixelBufferGetPlaneCount(image_buffer);
        int i;
        for(i = 0; i < plane_count; i++){
            src_linesize[i] = CVPixelBufferGetBytesPerRowOfPlane(image_buffer, i);
            src_data[i] = CVPixelBufferGetBaseAddressOfPlane(image_buffer, i);
        }
    }
    else {
        src_linesize[0] = CVPixelBufferGetBytesPerRow(image_buffer);
        src_data[0] = CVPixelBufferGetBaseAddress(image_buffer);
    }

    av_image_copy(avframe->data,
                  avframe->linesize,
                  (const uint8_t**)src_data,
                  src_linesize,
                  avctx->pix_fmt,
                  avctx->width,
                  avctx->height);

    CVPixelBufferUnlockBaseAddress(image_buffer, 0);

    return status;
}


//sends decodable NAL units data to decoder
static void decode_nalu(AVCodecContext *avctx, NALU* nalu, AVPacket *avpkt) {
    H264VideotoolboxContext *context = avctx->priv_data;

    int decode_data_size = 0;
    NALU* current_nalu = nalu;
    do {
        av_log(avctx, AV_LOG_INFO, "~~~~~~~ Decode NALU Type \"%d\" data_size %d~~~~~~~~\n", current_nalu->type, current_nalu->data_size);
        decode_data_size += current_nalu->data_size + context->nalu_length_size;
    } while ((current_nalu = current_nalu->next) && current_nalu->is_decodable);

   if (current_nalu) {
        av_log(avctx, AV_LOG_WARNING, "Trailing NAL units after data NAL block; type %d", current_nalu->type);
    }

    uint8_t *data = data = malloc(decode_data_size);

    if (context->is_avc) {
        memcpy(data, nalu->ptr, decode_data_size);
    }
    else {
        current_nalu = nalu;
        int offset = 0;
        do {
            //Replace Aanex B delimeter with nalu size, as VideoToolbox needs
            uint32_t dataLength32 = htonl(current_nalu->data_size);
            memcpy(&data[offset], &dataLength32 + sizeof(uint32_t) - context->nalu_length_size, context->nalu_length_size);

            offset += context->nalu_length_size;
            memcpy(&data[offset], current_nalu->data_ptr, current_nalu->data_size);
            offset += current_nalu->data_size;
        } while ((current_nalu = current_nalu->next) && current_nalu->is_decodable);
    }

    CMSampleBufferRef sampleBuffer = NULL;
    CMBlockBufferRef blockBuffer = NULL;
    OSStatus status = noErr;

    status = CMBlockBufferCreateWithMemoryBlock(NULL,
                                                data,
                                                decode_data_size,
                                                kCFAllocatorNull,
                                                NULL,
                                                0,
                                                decode_data_size,
                                                0, &blockBuffer);

    av_log(avctx, AV_LOG_INFO, "\t\t BlockBufferCreation: \t %s\n", (status == kCMBlockBufferNoErr) ? "successful!" : "failed...");

    if (blockBuffer && status == noErr) {
        CMSampleTimingInfo timeInfoArray[1] = {{
            .duration = CMTimeMake(avpkt->duration, 1),
            .presentationTimeStamp = CMTimeMake(avpkt->pts, 1),
            .decodeTimeStamp = CMTimeMake(avpkt->dts, 1),
        }};

        status = CMSampleBufferCreate(kCFAllocatorDefault,
                                      blockBuffer,
                                      true,
                                      NULL,
                                      NULL,
                                      context->format_description,
                                      1,
                                      1,
                                      timeInfoArray,
                                      0,
                                      NULL,
                                      &sampleBuffer);

        if (status != noErr) {
            av_log(avctx, AV_LOG_ERROR, "\t\t SampleBufferCreate: failed with OSStatus %d ", status);
        }
    }

    if (context->decompression_session == NULL) {
        create_decompression_session(avctx);
    }

    if (sampleBuffer && status == noErr) {
        // CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, true);
        // CFMutableDictionaryRef dict = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        // CFDictionarySetValue(dict, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);

        decompress_sample_buffer(avctx, sampleBuffer);
        CFRelease(sampleBuffer);
    }

    free(data);

    if (blockBuffer) {
        CFRelease(blockBuffer);
    }
}


static void process_metainfo_nalu(AVCodecContext *avctx, NALU *nalu, AVPacket *avpkt) {
    H264VideotoolboxContext *context = avctx->priv_data;

    switch (nalu->type) {
        case 7: { //SPS
            set_sps(context, nalu->data_ptr, nalu->size);
            break;
        }
        case 8: { //PPS
            set_pps(context, nalu->data_ptr, nalu->size);
            create_format_description(avctx);
            create_decompression_session(avctx);
            break;
        }
        case 1:
        case 5: {
            av_log(avctx, AV_LOG_ERROR, "Data NAL units should not be processed here");
            break;
        }
        case 6: //SEI
        default:
            av_log(avctx, AV_LOG_INFO, "Unhadnled nalu od type:%d\n", nalu->type);
        break;
    }
}


static av_cold int h264_videotoolbox_decode_init(AVCodecContext *avctx) {
    H264VideotoolboxContext *context = avctx->priv_data;
    memset(context, 0, sizeof(H264VideotoolboxContext));

    context->nalu_length_size = 4;

    TAILQ_INIT(&context->decoded_frames);

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (avctx->extradata_size && avctx->extradata) {
        if (avctx->extradata[0] == 1) {

            __unused int avc_profile = avctx->extradata[1];
            __unused int avc_compat = avctx->extradata[2];
            __unused int avc_level = avctx->extradata[3];
            int nalu_length_size_minus_one = avctx->extradata[4] & 3;

            context->nalu_length_size = nalu_length_size_minus_one + 1;

            int number_of_sps_nalus = avctx->extradata[5] & 31; //5 bits
            if (number_of_sps_nalus > 1) {
                av_log(avctx, AV_LOG_WARNING, "Multiple sps in extradata: %d\n", (int)number_of_sps_nalus);
            }

            int sps_size = 0;
            uint8_t *sps_ptr = NULL;
            int sps_offset = 0;

            //last sps will remain
            for (int i = 0; i < number_of_sps_nalus; i++) {
                sps_size = (avctx->extradata[6 + sps_offset] << 8) + avctx->extradata[7 + sps_offset];
                sps_ptr = &avctx->extradata[8 + sps_offset];
                sps_offset += sps_size;
            }

            int number_of_pps_nalus = *(sps_ptr + sps_size);
            if (number_of_pps_nalus > 1) {
                av_log(avctx, AV_LOG_WARNING, "Should handle multiple pps: %d\n", (int)number_of_pps_nalus);
            }

            //take first pps
            uint8_t* pps_size_ptr = sps_ptr + sps_size + 1;
            int pps_size = (pps_size_ptr[0] << 8) + pps_size_ptr[1];
            uint8_t *pps_ptr = pps_size_ptr + 2;

            set_sps(context, sps_ptr, sps_size);
            set_pps(context, pps_ptr, pps_size);

            av_log(avctx, AV_LOG_INFO, "AVC nalu parse complete\n");

            create_format_description(avctx);
        }
    }

    return 0;
}


static av_cold int h264_videotoolbox_decode_end(AVCodecContext *avctx) {
    H264VideotoolboxContext *context = avctx->priv_data;
    int ret;
    ret = 0;

    set_sps(context, 0, 0);
    set_pps(context, 0, 0);

    while (TAILQ_FIRST(&context->decoded_frames)) {
        drop_decoded_frame_queue_head(context);
    }
    context->decoded_frames_count = 0;

    return ret;
}


static int h264_videotoolbox_decode_frame(AVCodecContext* avctx, void* outdata, int* got_frame, AVPacket* avpkt) {
    H264VideotoolboxContext* context = avctx->priv_data;

    AVFrame* avframe = outdata;
    uint8_t* frame = avpkt->data;
    int frame_size = avpkt->size;

    if (frame_size == 0) {
        VTDecompressionSessionFinishDelayedFrames(context->decompression_session);
    }
    else if (frame_size < 4) {
        av_log(avctx, AV_LOG_ERROR, "Got packet of length %d.\n", (int)avpkt->size);
        return 0;
    }

    if (avpkt->size < 6) {
        av_log(avctx, AV_LOG_INFO, "Got packet of length %d.\n", (int)avpkt->size);
    }
    else {
        av_log(avctx, AV_LOG_INFO, "Got packet of length %d beginning with bytes %02X%02X%02X%02X %02X%02X\n",
               (int)avpkt->size,
               (int)avpkt->data[0],
               (int)avpkt->data[1],
               (int)avpkt->data[2],
               (int)avpkt->data[3],
               (int)avpkt->data[4],
               (int)avpkt->data[5]);
    }

    if (avpkt->side_data_elems) {
        av_log(avctx, AV_LOG_INFO, "avpkt->side_data_elems %d\n", avpkt->side_data_elems);
    }

    av_log(avctx, AV_LOG_INFO, "packet pts %lld. avpkt->stream_index %lld\n", avpkt->pts, (long long)avpkt->stream_index);
    av_log(avctx, AV_LOG_INFO, "avctx->reordered_opaque %lld\n", avctx->reordered_opaque);

    //Input processing

    if (frame_size > 0) {   
        parse_avc_type(context, frame);  //Annex B or AVC

        NALU* first_nalu = build_nalu_list(context, frame, frame_size);
        if (!first_nalu) {
            av_log(avctx, AV_LOG_ERROR, "No NAL units parsed\n");
            return 0;
        }

        NALU* current_nalu = first_nalu;

        while (current_nalu && !current_nalu->is_decodable) {
            av_log(avctx, AV_LOG_INFO, "~~~~~~~ Processing NALU Type \"%d\" data_size %d :%02X %02X~~~~~~~~\n", current_nalu->type,
                   current_nalu->data_size,
                   current_nalu->data_ptr[0],
                   current_nalu->data_ptr[1]);
            process_metainfo_nalu(avctx, current_nalu, avpkt);
            current_nalu = current_nalu->next;
        }

        if (current_nalu) {
            decode_nalu(avctx, current_nalu, avpkt);
        }

        free_nalu_list(first_nalu);
    }

    //Output

    if (TAILQ_EMPTY(&context->decoded_frames)) {
         av_log(avctx, AV_LOG_WARNING, "Empty decoded frames queue\n");
         return avpkt->size;
    }

    DecodedFrame *decoded_frame = TAILQ_FIRST(&context->decoded_frames);
    CVPixelBufferRef pixbuf = decoded_frame->pixbuf;

    int width = CVPixelBufferGetWidth(pixbuf);
    int height = CVPixelBufferGetHeight(pixbuf);
    int numberOfPlanes = CVPixelBufferGetPlaneCount(pixbuf);

    av_log(avctx, AV_LOG_INFO, "return pixbuf %p:(%dx%d) planes:%d\n", pixbuf, width, height, numberOfPlanes);
    av_log(avctx, AV_LOG_INFO, "return pts %lld\n", decoded_frame->pts);

    int ret = ff_set_dimensions(avctx, width, height);
    av_log(avctx, AV_LOG_INFO, "ff_set_dimensions:%d\n", ret);

    if (ff_get_buffer(avctx, avframe, 0) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unable to allocate buffer\n");
        return 0;
    }

    copy_cvpixelbuffer(avctx, pixbuf, avframe);

    avframe->pts = decoded_frame->pts;
    avframe->reordered_opaque = decoded_frame->pts;

    if (context->last_returned_pts > decoded_frame->pts) {
        context->reorder_queue_size ++;
    }
    context->last_returned_pts = avframe->pts;

    if (context->decoded_frames_count > context->reorder_queue_size || frame_size == 0) {
        drop_decoded_frame_queue_head(context);
    }

    av_log(avctx, AV_LOG_INFO, "~~~~~~~Frame decoded~~~~~~~~\n\n");
    *got_frame = 1;
    return avpkt->size;
}


static void h264_videotoolbox_flush(AVCodecContext *avctx) {
    H264VideotoolboxContext* context = avctx->priv_data;
    av_log(avctx, AV_LOG_INFO, "h264_videotoolbox_flush\n");

    //drop reorder queue after seek
    while (TAILQ_FIRST(&context->decoded_frames)) {
        drop_decoded_frame_queue_head(context);
    }
    context->decoded_frames_count = 0;
    context->last_returned_pts = 0;
}


AVCodec ff_h264_videotoolbox_decoder = {
    .name                  = "h264vt",
    .long_name             = NULL_IF_CONFIG_SMALL("H.264 Decoder with videotoolbox"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_H264,
    .priv_data_size        = sizeof(H264VideotoolboxContext),
    .init                  = h264_videotoolbox_decode_init,
    .close                 = h264_videotoolbox_decode_end,
    .decode                = h264_videotoolbox_decode_frame,
    .flush                 = h264_videotoolbox_flush,
    .capabilities          = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY ,
    .caps_internal         = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_EXPORTS_CROPPING,
    //.bsfs           = "h264_mp4toannexb",
    .wrapper_name   = "h264_videotoolbox",
};
