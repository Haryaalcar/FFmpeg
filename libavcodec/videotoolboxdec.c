
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"

#include "avcodec.h"
#include "internal.h"
#include <sys/queue.h>


typedef struct DecodedFrame {
    CVPixelBufferRef pixbuf;
    int64_t pts;
    int64_t duration;

    TAILQ_ENTRY(DecodedFrame) entries;
} DecodedFrame;


typedef struct NALU {
    int type;
    int size;
    int data_size;
    int delimeter_size;
    bool is_incomplete; //for avc only can detect
    const uint8_t* ptr;
    const uint8_t* data_ptr;
    struct NALU *next;
} NALU;


typedef struct H264VideotoolboxContext {
    CMVideoFormatDescriptionRef formatDescription;
    VTDecompressionSessionRef decompressionSession;

    bool avc_type_parsed;
    bool is_avc; //avc means not annex b
    int nalu_length_size; //from extradata, does not apply to annex b delimeter size

    int sps_size;
    int pps_size;

    uint8_t* sps;
    uint8_t* pps;

    CVPixelBufferRef pixbuf;
    int64_t pts;
    int64_t duration;
    int nalu_count;

    NALU *first_decodable_nalu;
    TAILQ_HEAD(, DecodedFrame) decoded_frames;

} H264VideotoolboxContext;


static void add_decoded_frame_to_queue(H264VideotoolboxContext *context, CVPixelBufferRef pixbuf, int64_t pts, int64_t duration) {
    //context->head
    DecodedFrame *decoded_frame = (DecodedFrame *)malloc(sizeof(DecodedFrame));
    if (decoded_frame) {
        decoded_frame->pixbuf = CVPixelBufferRetain(pixbuf);
        decoded_frame->pts = pts;
        decoded_frame->duration = duration;
    }

    //av_log(NULL, AV_LOG_INFO, "\n");

    TAILQ_INSERT_TAIL(&context->decoded_frames, decoded_frame, entries);
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


static void set_pixbuffer(H264VideotoolboxContext *context, CVPixelBufferRef pixbuf) {
    if (context->pixbuf) {
        CVPixelBufferRelease(context->pixbuf);
        context->pixbuf = 0;
    }
    if (pixbuf) {
        context->pixbuf = CVPixelBufferRetain(pixbuf);
    }
}



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
        nalu->delimeter_size = annexb_delimeter_size;
        nalu->is_incomplete = (max_length < annexb_delimeter_size + 2); //cannot determine as for avc
        nalu->ptr = frame;
        nalu->data_ptr = frame + annexb_delimeter_size;
    }

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
    if (context->formatDescription) {
        CFRelease(context->formatDescription);
        context->formatDescription = NULL;
    }

    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2,
                                            (const uint8_t *const*)parameterSetPointers,
                                            parameterSetSizes, 4,
                                            &context->formatDescription);

    av_log(avctx, AV_LOG_INFO, "\t\t Creation of CMVideoFormatDescription: %s\n", (status == noErr) ? "successful!" : "failed...");
    if(status != noErr) {
        av_log(avctx, AV_LOG_INFO, "\t\t Format Description ERROR type: %d\n", (int)status);
    }
}


static void decompressionSessionDecodeFrameCallback(void *decompressionOutputRefCon,
                                             void *sourceFrameRefCon,
                                             OSStatus status,
                                             VTDecodeInfoFlags infoFlags,
                                             CVImageBufferRef imageBuffer,
                                             CMTime presentationTimeStamp,
                                             CMTime presentationDuration) {
    AVCodecContext *avctx = decompressionOutputRefCon;
    H264VideotoolboxContext *context = avctx->priv_data;

    if (status != noErr) {
        CFErrorRef error = CFErrorCreate(NULL, kCFErrorDomainOSStatus, status, NULL);
        CFStringRef error_description = CFErrorCopyDescription(error);
        av_log(avctx, AV_LOG_ERROR, "Decompressed OSStatus: %d description:%s\n", status, CFStringGetCStringPtr(error_description, kCFStringEncodingUTF8));
        CFRelease(error);
        CFRelease(error_description);
    }
    else {
        av_log(avctx, AV_LOG_INFO, "Decompressed sucessfully, img %p\n", imageBuffer);

        //add_decoded_frame_to_queue(context, CVPixelBufferRetain(imageBuffer), presentationTimeStamp.value, presentationDuration.value);
        set_pixbuffer(context, imageBuffer);
        context->pts = presentationTimeStamp.value;
        context->duration = presentationDuration.value;

        av_log(avctx, AV_LOG_INFO, "Decompressed PTS:%lld\n", presentationTimeStamp.value);
    }
}


static void decompress_sample_buffer(AVCodecContext *avctx, CMSampleBufferRef sampleBuffer) {
    H264VideotoolboxContext *context = avctx->priv_data;
    
    OSStatus status;
    VTDecodeFrameFlags flags = 0;// kVTDecodeFrame_EnableTemporalProcessing | kVTDecodeFrame_EnableAsynchronousDecompression;
    VTDecodeInfoFlags decode_info_flags;
    status = VTDecompressionSessionDecodeFrame(context->decompressionSession, sampleBuffer, 
    flags,
    NULL,
    &decode_info_flags);

    av_log(avctx, AV_LOG_INFO, "VTDecompressionSessionDecodeFrame OSStatus: %d, info flags:%d\n", status, decode_info_flags);

    if (status == noErr) {
        status = VTDecompressionSessionWaitForAsynchronousFrames(context->decompressionSession);
    }
    av_log(avctx, AV_LOG_INFO, "VTDecompressionSessionWaitForAsynchronousFrames OSStatus: %d\n", status);
}


static void create_decompression_session(AVCodecContext *avctx) {
    H264VideotoolboxContext *context = avctx->priv_data;

    // make sure to destroy the old VTD session
    context->decompressionSession = NULL;
    VTDecompressionOutputCallbackRecord callBackRecord;
    callBackRecord.decompressionOutputCallback = decompressionSessionDecodeFrameCallback;

    // this is necessary if you need to make calls to Objective C "self" from within in the callback method.
    callBackRecord.decompressionOutputRefCon = avctx;

    // you can set some desired attributes for the destination pixel buffer.  I didn't use this but you may
    // if you need to set some attributes, be sure to uncomment the dictionary in VTDecompressionSessionCreate
    // NSDictionary *destinationImageBufferAttributes = [NSDictionary dictionaryWithObjectsAndKeys:
    //                                                   [NSNumber numberWithBool:YES],
    //                                                   (id)kCVPixelBufferOpenGLESCompatibilityKey,
    //                                                   nil];

    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                          4,
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
                                                          
    int pixel_format = kCVPixelFormatType_420YpCbCr8Planar;
    CFNumberRef pixelFormat = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);

    CFDictionarySetValue(dict, kCVPixelBufferPixelFormatTypeKey, pixelFormat);


    OSStatus status =  VTDecompressionSessionCreate(NULL, context->formatDescription, NULL,
                                                    dict, // (__bridge CFDictionaryRef)(destinationImageBufferAttributes)
                                                    &callBackRecord, 
                                                    &context->decompressionSession);
    CFRelease(dict);
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

//    int size = av_image_get_buffer_size(avctx->pix_fmt, width, height, 1);

//     status = av_image_copy_to_buffer(avframe->data, size,
//                                      src_data, src_linesize,
//                                      avctx->pix_fmt, 
//                                      width, height, 1);


    av_image_copy(avframe->data, 
    avframe->linesize, 
    (const uint8_t **) src_data, 
    src_linesize, 
    avctx->pix_fmt, avctx->width, avctx->height);

    CVPixelBufferUnlockBaseAddress(image_buffer, 0);

    return status;
}


static bool process_nalu(AVCodecContext *avctx, NALU *nalu, AVPacket *avpkt) {
    H264VideotoolboxContext *context = avctx->priv_data;

    switch (nalu->type) {
        case 6: { //SEI
            break;
        }
        case 7: { //SPS
            set_sps(context, nalu->data_ptr, nalu->size);
            break;
        }
        case 8: { //PPS
            set_pps(context, nalu->data_ptr, nalu->size);
            create_format_description(avctx);
            break;
        }
        case 1:
        case 5: {
            if (!context->first_decodable_nalu) {
                context->first_decodable_nalu = nalu;
            }

            NALU *next_nalu = nalu->next;
            if (next_nalu && (next_nalu->type == 1  || next_nalu->type == 5)) {
                return true;
            }

            if (next_nalu && !(next_nalu->type == 1 || next_nalu->type == 5)) {
                av_log(avctx, AV_LOG_WARNING, "Trailing NAL units after data NAL block; type %d", next_nalu->type);
            }

            if (context->decompressionSession == NULL) {
                create_decompression_session(avctx);
            }

            //all next nal units should be given to decompressor as one chunk
            int block_length = nalu->ptr + nalu->size - context->first_decodable_nalu->data_ptr + context->nalu_length_size;
            uint8_t *data = data = malloc(block_length);

            if (context->is_avc) {
                data = memcpy(data, context->first_decodable_nalu->ptr, block_length);
            }
            else {
                //
                memcpy(&data[context->nalu_length_size], context->first_decodable_nalu->data_ptr, block_length - context->first_decodable_nalu->delimeter_size);
                uint32_t dataLength32 = htonl(nalu->data_size);
                memcpy(data, &dataLength32, sizeof (uint32_t));
            }

            //  av_log(avctx, AV_LOG_INFO, "Decompress nalu of length %d beginning with bytes %02X%02X%02X%02X %02X%02X\n",
            //     (int) block_length,
            //     (int) data[0],
            //     (int) data[1],
            //     (int) data[2],
            //     (int) data[3],
            //     (int) data[4],
            //     (int) data[5]);

            CMSampleBufferRef sampleBuffer = NULL;
            CMBlockBufferRef blockBuffer = NULL;
            OSStatus status = noErr;

            status = CMBlockBufferCreateWithMemoryBlock(NULL,
                                                        data,
                                                        block_length,
                                                        kCFAllocatorNull,
                                                        NULL,
                                                        0,
                                                        block_length,
                                                        0, &blockBuffer);

            av_log(avctx, AV_LOG_INFO, "\t\t BlockBufferCreation: \t %s\n", (status == kCMBlockBufferNoErr) ? "successful!" : "failed...");

            if(blockBuffer && status == noErr) {

                CMSampleTimingInfo timeInfoArray[1] = { {
                    .duration = CMTimeMake(avpkt->duration, 1),
                    .presentationTimeStamp =  CMTimeMake(avpkt->pts, 1),
                    .decodeTimeStamp = CMTimeMake(avpkt->dts, 1),
                } };
                
                // const size_t sampleSize = blockLength;
                status = CMSampleBufferCreate(kCFAllocatorDefault,
                                                blockBuffer,
                                                true,
                                                NULL,
                                                NULL,
                                                context->formatDescription,
                                                1,
                                                1,
                                                timeInfoArray,
                                                0,
                                                NULL,
                                                &sampleBuffer);

                av_log(avctx, AV_LOG_INFO, "\t\t SampleBufferCreate: \t %s\n", (status == noErr) ? "successful!" : "failed...");
            }

            if(sampleBuffer && status == noErr) {
                // CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, true);
                // CFMutableDictionaryRef dict = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
                // CFDictionarySetValue(dict, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);

                decompress_sample_buffer(avctx, sampleBuffer);
                CFRelease(sampleBuffer);
            }

            if (NULL != data) {
                free(data);
                data = NULL;
            }

            if (blockBuffer) {
                CFRelease(blockBuffer);
            }

            return false;
            break;
        }
        default:
            av_log(avctx, AV_LOG_INFO, "Unhadnled nalu od type:%d\n", nalu->type);
        break;
    }
    return true;
}


static av_cold int h264_videotoolbox_decode_init(AVCodecContext *avctx) {
    H264VideotoolboxContext *ctx = avctx->priv_data;
    memset(ctx, 0, sizeof(H264VideotoolboxContext));

    ctx->nalu_length_size = 4;

    TAILQ_INIT(&ctx->decoded_frames);

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (avctx->extradata_size && avctx->extradata) {
        if (avctx->extradata[0] == 1) {

            __unused int avc_profile = avctx->extradata[1];
            __unused int avc_compat = avctx->extradata[2];
            __unused int avc_level = avctx->extradata[3];
            int nalu_length_size_minus_one = avctx->extradata[4] & 3; //2 lower bits ?? little endian

            ctx->nalu_length_size = nalu_length_size_minus_one + 1;

            int number_of_sps_nalus = avctx->extradata[5] & 31; //5 bits
            if (number_of_sps_nalus > 1) {
                av_log(avctx, AV_LOG_INFO, "Should handle multiple sps: %d\n", (int)number_of_sps_nalus);
                assert(false);
            }
            int sps_size = (avctx->extradata[6] << 8) + avctx->extradata[7];
            uint8_t *sps_ptr = &avctx->extradata[8];

            int number_of_pps_nalus = *(sps_ptr + sps_size);
            if (number_of_pps_nalus > 1) {
                av_log(avctx, AV_LOG_INFO, "Should handle multiple pps: %d\n", (int)number_of_pps_nalus);
                assert(false);
            }
            uint8_t* pps_size_ptr = sps_ptr + sps_size + 1;
            int pps_size = (pps_size_ptr[0] << 8) + pps_size_ptr[1];
            uint8_t *pps_ptr = pps_size_ptr + 2;

            set_sps(ctx, sps_ptr, sps_size);
            set_pps(ctx, pps_ptr, pps_size);

            av_log(avctx, AV_LOG_INFO, "AVC nalu parse complete\n");

            create_format_description(avctx);
        }
    }

    return 0;
}


static av_cold int h264_videotoolbox_decode_end(AVCodecContext *avctx) {
    H264VideotoolboxContext *ctx = avctx->priv_data;
    int ret;
    ret = 0;

    set_sps(ctx, 0, 0);
    set_pps(ctx, 0, 0);

    DecodedFrame *frame = NULL;
    while ((frame = TAILQ_FIRST(&ctx->decoded_frames))) {
        TAILQ_REMOVE(&ctx->decoded_frames, frame, entries);
        free(frame);
    }

    return ret;
}


static int h264_videotoolbox_decode_frame(AVCodecContext *avctx, void *outdata,
                             int *got_frame, AVPacket *avpkt) {
    H264VideotoolboxContext *context = avctx->priv_data;

    AVFrame *avframe = outdata;
    uint8_t * frame = avpkt->data;
    int frame_size = avpkt->size;

    if (frame_size == 0) {
        VTDecompressionSessionFinishDelayedFrames(context->decompressionSession);
        //TODO: flush decompressor? repeat frame?
    }
    else if (frame_size < 4) {
        av_log(avctx, AV_LOG_WARNING, "Got packet of length %d.\n", (int)avpkt->size);
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

    av_log(avctx, AV_LOG_INFO, "packet pts %lld.\n", avpkt->pts);

    if (frame_size > 0) {
        parse_avc_type(context, frame);  //Annex B or AVC

        NALU* first_nalu = build_nalu_list(context, frame, frame_size);

        NALU* current_nalu = first_nalu;

        bool should_continue = false;
        do {
            av_log(avctx, AV_LOG_INFO, "~~~~~~~ Processing NALU Type \"%d\" data_size %d~~~~~~~~\n", current_nalu->type, current_nalu->data_size);
            should_continue = process_nalu(avctx, current_nalu, avpkt);
        } while ((current_nalu = current_nalu->next) && should_continue);

        context->first_decodable_nalu = NULL;
        if (first_nalu) {
            free_nalu_list(first_nalu);
        }
        
        add_decoded_frame_to_queue(context, context->pixbuf, context->pts, context->duration);
        set_pixbuffer(context, NULL);
    }

    // VTDecompressionSessionWaitForAsynchronousFrames(context->decompressionSession);

     //if (!context->pixbuf) {
    if (TAILQ_EMPTY(&context->decoded_frames)) {
         av_log(avctx, AV_LOG_WARNING, "Empty pixbuf: return\n");
         return avpkt->size;
    }

    DecodedFrame *decoded_frame = TAILQ_FIRST(&context->decoded_frames);
    CVPixelBufferRef pixbuf = decoded_frame->pixbuf;

    if (ff_get_buffer(avctx, avframe, 0) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unable to allocate buffer\n");
        return 0;  //AVERROR(ENOMEM);
    }

    int width = CVPixelBufferGetWidth(pixbuf);
    int height = CVPixelBufferGetHeight(pixbuf);
    int numberOfPlanes = CVPixelBufferGetPlaneCount(pixbuf);

    av_log(avctx, AV_LOG_INFO, "return pixbuf %p:(%dx%d) planes:%d\n", pixbuf, width, height, numberOfPlanes);
    av_log(avctx, AV_LOG_INFO, "rerurn pts %lld\n", decoded_frame->pts);

    int ret = ff_set_dimensions(avctx, width, height);
    av_log(avctx, AV_LOG_INFO, "ff_set_dimensions:%d\n", ret);

    copy_cvpixelbuffer(avctx, pixbuf, avframe);
    CVPixelBufferRelease(pixbuf);

    // av_log(avctx, AV_LOG_INFO, "avctx->reordered_opaque %lld\n", avctx->reordered_opaque);
    // av_log(avctx, AV_LOG_INFO, "avpkt->pts %lld\n", avpkt->pts);
    // av_log(avctx, AV_LOG_INFO, "avpkt->dts %lld\n", avpkt->dts);
    // av_log(avctx, AV_LOG_INFO, "avpkt->duration %lld\n", avpkt->duration);

    //avframe->pkt_dts = AV_NOPTS_VALUE;
    avframe->pts = decoded_frame->pts;
 //   avframe->pts = context->pts;
    avframe->reordered_opaque = decoded_frame->pts;
    //avframe->pkt_duration = context->duration;
    //avframe->pkt_dts = context->dts;

    //   avframe->pts     = avpkt->pts; // can be different if we are returning some earlier frame
    //   avframe->pkt_dts = AV_NOPTS_VALUE;
   //    avframe->reordered_opaque = avctx->reordered_opaque;

     // I don't know why this thing is needed:
 #if FF_API_PKT_PTS
 FF_DISABLE_DEPRECATION_WARNINGS
     avframe->pkt_pts = decoded_frame->pts;
 FF_ENABLE_DEPRECATION_WARNINGS
 #endif

    TAILQ_REMOVE(&context->decoded_frames, decoded_frame, entries);
    free(decoded_frame);

    av_log(avctx, AV_LOG_INFO, "~~~~~~~Frame decoded~~~~~~~~\n\n");

    *got_frame = 1;
    return avpkt->size;
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
    .capabilities          = /*AV_CODEC_CAP_DRAW_HORIZ_BAND |*/ AV_CODEC_CAP_DR1 |
                             AV_CODEC_CAP_DELAY 
                             ,
    //.hw_configs            = (const AVCodecHWConfigInternal*[]) {

    .caps_internal         = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_EXPORTS_CROPPING,
    // .flush                 = flush_dpb,
    // .init_thread_copy      = ONLY_IF_THREADS_ENABLED(decode_init_thread_copy),
    // .update_thread_context = ONLY_IF_THREADS_ENABLED(ff_h264_update_thread_context),
    // .profiles              = NULL_IF_CONFIG_SMALL(ff_h264_profiles),
    // .priv_class            = &h264_class,
    //.bsfs           = "h264_mp4toannexb",
    .wrapper_name   = "h264_videotoolbox",
};
