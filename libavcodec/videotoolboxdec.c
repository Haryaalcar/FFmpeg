
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"

#include "avcodec.h"
#include "internal.h"


typedef struct H264VideotoolboxContext {
    CMVideoFormatDescriptionRef formatDescription;
    VTDecompressionSessionRef decompressionSession;

    CVPixelBufferRef pixbuf;

    bool avc_type_parsed;
    bool is_avc; //avc means not annex b
    int nalu_length_size;

    int sps_size;
    int pps_size;

    uint8_t* sps;
    uint8_t* pps;

    int64_t pts;
    int64_t dts;
    int64_t reordered_opaque;
    int64_t duration;

} H264VideotoolboxContext;


typedef struct NALU {
    int nalu_type;
    int nalu_size;
    int nalu_delimeter_size;
    bool incomplete; //for avc only can detect
    uint8_t* nalu_ptr;
    uint8_t* nalu_data_ptr;
    struct NALU *next;
} NALU;


static void build_nalu_list(H264VideotoolboxContext *context, const uint8_t* frame_start, NALU *first_nalu) {

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


static void create_format_description(H264VideotoolboxContext *context) {
    //H264VideotoolboxContext *context = avctx->priv_data;
    OSStatus status;

     // now we set our H264 parameters
        uint8_t*  parameterSetPointers[2] = {context->sps, context->pps};
        size_t parameterSetSizes[2] = {context->sps_size, context->pps_size};

        // suggestion from @Kris Dude's answer below
        if (context->formatDescription) {
            CFRelease(context->formatDescription);
            context->formatDescription = NULL;
        }

        status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, 
                                                (const uint8_t *const*)parameterSetPointers, 
                                                parameterSetSizes, 4, 
                                                &context->formatDescription);

        av_log(NULL, AV_LOG_INFO, "\t\t Creation of CMVideoFormatDescription: %s\n", (status == noErr) ? "successful!" : "failed...");
        if(status != noErr) {
            av_log(NULL, AV_LOG_INFO, "\t\t Format Description ERROR type: %d\n", (int)status);
        }
}


inline static bool is_annexb_nalu(const uint8_t* nalu, int *annexb_delimeter_size) {
    if (AV_RB24(nalu) == 1) {
        if (annexb_delimeter_size) {
            *annexb_delimeter_size = 3;
        }
        return true;
    }
    else if (AV_RB32(nalu) == 1) {
        if (annexb_delimeter_size) {
            *annexb_delimeter_size = 4;
        }
        return true;
    }
    return false;
}


static int get_nalu_type(H264VideotoolboxContext *context, const uint8_t* naluptr) {
    if  (context->is_avc) {
        return naluptr[context->nalu_length_size] & 0x1F;
    }
    else if (AV_RB24(naluptr) == 1) {
         return naluptr[3] & 0x1F;
    }
    else if (AV_RB32(naluptr) == 1) {
         return naluptr[4] & 0x1F;
    }
    return -1;
}


static bool nalu_is_decodable(H264VideotoolboxContext *context, const uint8_t * nalu) {
    int nalu_type = get_nalu_type(context, nalu);

    return nalu_type == 5 || nalu_type == 1;
}


static bool nalu_is_last(H264VideotoolboxContext *context, const uint8_t * nalu, const int max_length) {
    return false;//TODO
}

//size without lenggth header/delimeter
static int get_nalu_size(H264VideotoolboxContext *context, const uint8_t * nalu, const int max_length) {
    if (context->is_avc) {
        int naluSize = 0;
        for (int i = 0; i < context->nalu_length_size; i++) {
            naluSize = (naluSize << 8) | nalu[i];
        }
        return naluSize;
    }
    else {
        int annexb_delimeter_size = 0;
        is_annexb_nalu(nalu, &annexb_delimeter_size);

        for (int i = annexb_delimeter_size; i < max_length - annexb_delimeter_size; i++) {
            if (is_annexb_nalu(nalu + i, NULL)) { //TODO: mb nalu[i] == 0 && nalu[i + 1] == 0 && (nalu[i + 2] == 1 || (nalu[i + 2] == 0 && nalu[i + 3] == 1))
                return i - annexb_delimeter_size;
            }
        }    
        return  max_length - annexb_delimeter_size;  
    }
}


static inline const uint8_t * get_nalu_data_ptr(H264VideotoolboxContext *context, const uint8_t * nalu) {
    if (context->is_avc) {
        return nalu + context->nalu_length_size;
    }
    else {
        int annexb_delimeter_size = 0;
        is_annexb_nalu(nalu, &annexb_delimeter_size);
        return nalu + annexb_delimeter_size;
    }
}


static const uint8_t * next_nalu(H264VideotoolboxContext *context, const uint8_t * nalu, const int max_length) {
    int nalu_size = get_nalu_size(context, nalu, max_length);

    if (nalu_size == max_length) {
        return 0;
    }

    return get_nalu_data_ptr(context, nalu) + nalu_size;
}


static void process_nalu(H264VideotoolboxContext *context, const uint8_t * nalu, const int max_length) {
    int nalu_type = get_nalu_type(context, nalu);
    int nalu_size = get_nalu_size(context, nalu, max_length);

    switch (nalu_type) {
        case 6: { //SEI
            break;
        }
        case 7: { //SPS
            set_sps(context, get_nalu_data_ptr(context, nalu), nalu_size);
            break;
        }
        case 8: { //PPS
            set_pps(context, get_nalu_data_ptr(context, nalu), nalu_size);
            create_format_description(context);
            break;
        }
        default:
            av_log(context, AV_LOG_INFO, "Unhadnled nalu od type:%d\n", nalu_type);
        break;
    }
}


static void decompressionSessionDecodeFrameCallback(void *decompressionOutputRefCon,
                                             void *sourceFrameRefCon,
                                             OSStatus status,
                                             VTDecodeInfoFlags infoFlags,
                                             CVImageBufferRef imageBuffer,
                                             CMTime presentationTimeStamp,
                                             CMTime presentationDuration)
{
    AVCodecContext *avctx = decompressionOutputRefCon;
    H264VideotoolboxContext *context = avctx->priv_data;

    if (status != noErr) {
        CFErrorRef error = CFErrorCreate(NULL, kCFErrorDomainOSStatus, status, NULL);
        CFStringRef error_description = CFErrorCopyDescription(error);
        av_log(avctx, AV_LOG_INFO, "Decompressed OSStatus: %d description:%s\n", status, CFStringGetCStringPtr(error_description, kCFStringEncodingUTF8));
        CFRelease(error);
        CFRelease(error_description);
    }
    else {
        av_log(avctx, AV_LOG_INFO, "Decompressed sucessfully\n");

        //here is output: deompressed frame picture
        context->pixbuf = CVPixelBufferRetain(imageBuffer);
        context->pts = presentationTimeStamp.value;
        context->duration = presentationDuration.value;

        av_log(avctx, AV_LOG_INFO, "Decompressed PTS:%lld\n", presentationTimeStamp.value);
    }
}


static void decompress(AVCodecContext *avctx, CMSampleBufferRef sampleBuffer) {
    H264VideotoolboxContext *context = avctx->priv_data;
    
    OSStatus status;
  //  VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
    //VTDecodeInfoFlags flagOut;
   //NSDate* currentTime = [NSDate date];
    status = VTDecompressionSessionDecodeFrame(context->decompressionSession, sampleBuffer, 
    0, 
    NULL,
    0);
//    flags,
                                  //(void*)CFBridgingRetain(currentTime), &flagOut);

    av_log(avctx, AV_LOG_INFO, "VTDecompressionSessionDecodeFrame OSStatus: %d\n", status);

    if (status == noErr) {
        status = VTDecompressionSessionWaitForAsynchronousFrames(context->decompressionSession);
    }

    av_log(avctx, AV_LOG_INFO, "VTDecompressionSessionWaitForAsynchronousFrames OSStatus: %d\n", status);

    CFRelease(sampleBuffer);
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


static av_cold int h264_videotoolbox_decode_init(AVCodecContext *avctx) {
    H264VideotoolboxContext *ctx = avctx->priv_data;
    ctx->pixbuf = 0;
    ctx->avc_type_parsed = false;
    ctx->formatDescription = NULL;

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

            create_format_description(ctx);
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

    return ret;
}


//int (*decode)(AVCodecContext *, void *outdata, int *outdata_size, AVPacket *avpkt);
static int h264_videotoolbox_decode_frame(AVCodecContext *avctx, void *outdata,
                             int *got_frame, AVPacket *avpkt) {
    H264VideotoolboxContext *context = avctx->priv_data;

    AVFrame *avframe = outdata;
    
    uint8_t * frame = avpkt->data;
    int frameSize = avpkt->size;

    OSStatus status = noErr;
    uint8_t *data = NULL;

    CMSampleBufferRef sampleBuffer = NULL;
    CMBlockBufferRef blockBuffer = NULL;

    if (frameSize == 0) {
        //TODO: flush decompressor? repeat frame?
    }

    if (frameSize < 4) {
        av_log(avctx, AV_LOG_WARNING, "Got packet of length %d.\n", (int) avpkt->size);
        return 0;
    }

    if (avpkt->size < 6) {
         av_log(avctx, AV_LOG_INFO, "Got packet of length %d.\n", (int) avpkt->size);
     } 
     else {
         av_log(avctx, AV_LOG_INFO, "Got packet of length %d beginning with bytes %02X%02X%02X%02X %02X%02X\n",
                (int) avpkt->size,
                (int) avpkt->data[0],
                (int) avpkt->data[1],
                (int) avpkt->data[2],
                (int) avpkt->data[3],
                (int) avpkt->data[4],
                (int) avpkt->data[5]);
     } 
     av_log(avctx, AV_LOG_INFO, "packet pts %lld.\n", avpkt->pts);
     

    parse_avc_type(context, frame); //Annex B or AVC

    const uint8_t * cur_nalu = frame;
    int max_length = frameSize;

    while (!nalu_is_decodable(context, cur_nalu)) {
        av_log(avctx, AV_LOG_INFO, "~~~~~~~ Processing NALU Type \"%d\" ~~~~~~~~\n", get_nalu_type(context, cur_nalu));
        process_nalu(context, cur_nalu, max_length);
        cur_nalu = next_nalu(context, cur_nalu, max_length);
        max_length = frameSize - (int)(cur_nalu - frame);
    }

    int nalu_type = get_nalu_type(context, cur_nalu);
    av_log(avctx, AV_LOG_INFO, "~~~~~~~ Decompressing NALU Type \"%d\" ~~~~~~~~\n", nalu_type);

    if (context->decompressionSession == NULL) {
        create_decompression_session(avctx);
    }

    if (nalu_type == 1 || nalu_type == 5) {
        int blockLength = max_length;
        if (context->is_avc) {
            data = malloc(blockLength);
            data = memcpy(data, cur_nalu, blockLength);
        }

        int dataLengthWithLengthHeader = blockLength;

        //replace the start header with the size of the NALU, as VideoToolbox requires
        if (!context->is_avc) {
            int annexb_delimeter_size = 0;
            is_annexb_nalu(cur_nalu, &annexb_delimeter_size);

            int dataLengthWithoutHeader = get_nalu_size(context, cur_nalu, max_length);
            dataLengthWithLengthHeader = dataLengthWithoutHeader + annexb_delimeter_size;

            // dataLengthWithLengthHeader = blockLength - annexb_delimeter_size + context->nalu_length_size;
            // int dataLengthWithoutHeader = blockLength - annexb_delimeter_size;

            data = malloc(blockLength);
            
            memcpy(&data[context->nalu_length_size], &cur_nalu[annexb_delimeter_size], blockLength - annexb_delimeter_size);

            uint32_t dataLength32 = htonl(dataLengthWithoutHeader);
            memcpy(data, &dataLength32, sizeof (uint32_t));
        }

        status = CMBlockBufferCreateWithMemoryBlock(NULL, 
                                                    data,
                                                    dataLengthWithLengthHeader,
                                                    kCFAllocatorNull, 
                                                    NULL,
                                                    0,
                                                    dataLengthWithLengthHeader,
                                                    0, &blockBuffer);

        av_log(avctx, AV_LOG_INFO, "\t\t BlockBufferCreation: \t %s\n", (status == kCMBlockBufferNoErr) ? "successful!" : "failed...");
    }

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

        decompress(avctx, sampleBuffer);
    }

    if (NULL != data) {
        free (data);
        data = NULL;
    }

    if (blockBuffer) {
        CFRelease(blockBuffer);
    }

     if (!context->pixbuf) {
         //decoder has accepted our input data, but no output frame is available so far
         av_log(avctx, AV_LOG_ERROR, "Empty pixbuf: return\n");
         return avpkt->size;
     }

     if (ff_get_buffer(avctx, avframe, 0) < 0) {
         av_log(avctx, AV_LOG_ERROR, "Unable to allocate buffer\n");
         return 0;//AVERROR(ENOMEM);
     }

    int width = CVPixelBufferGetWidth(context->pixbuf);
    int height = CVPixelBufferGetHeight(context->pixbuf);
    int numberOfPlanes = CVPixelBufferGetPlaneCount(context->pixbuf);

    av_log(avctx, AV_LOG_INFO, "ctx->pixbuf %ld:(%dx%d) planes:%d\n", (long int)context->pixbuf, width, height, numberOfPlanes);

    int ret = ff_set_dimensions(avctx, width, height);
    av_log(avctx, AV_LOG_INFO, "ff_set_dimensions:%d\n", ret);

    copy_cvpixelbuffer(avctx, context->pixbuf, avframe);
    CVPixelBufferRelease(context->pixbuf);

    av_log(avctx, AV_LOG_INFO, "avctx->reordered_opaque %lld\n", avctx->reordered_opaque);
    av_log(avctx, AV_LOG_INFO, "avpkt->pts %lld\n", avpkt->pts);
    av_log(avctx, AV_LOG_INFO, "avpkt->dts %lld\n", avpkt->dts);
    av_log(avctx, AV_LOG_INFO, "avpkt->duration %lld\n", avpkt->duration);

    //avframe->pkt_dts = AV_NOPTS_VALUE;
    avframe->pts = avpkt->pts;
 //   avframe->pts = context->pts;
    avframe->reordered_opaque = avpkt->pts;
    //avframe->pkt_duration = context->duration;
    avframe->pkt_dts = context->dts;

    //   avframe->pts     = avpkt->pts; // can be different if we are returning some earlier frame
    //   avframe->pkt_dts = AV_NOPTS_VALUE;
   //    avframe->reordered_opaque = avctx->reordered_opaque;

     // I don't know why this thing is needed:
 #if FF_API_PKT_PTS
 FF_DISABLE_DEPRECATION_WARNINGS
     avframe->pkt_pts = avpkt->pts;
 FF_ENABLE_DEPRECATION_WARNINGS
 #endif

    av_log(avctx, AV_LOG_INFO, "~~~~~~~Frame decoded~~~~~~~~\n\n");

    *got_frame = 1;
    return avpkt->size;
}


AVCodec ff_h264_videotoolbox_decoder = {
    .name                  = "h264",
    .long_name             = NULL_IF_CONFIG_SMALL("H.264 Decodeer with videotoolbox"),
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
 //   .bsfs           = "h264_mp4toannexb",
    .wrapper_name   = "h264_videotoolbox",
};
