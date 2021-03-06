/*
 * copyright (c) 2008 Paul Kendall <paul@kcbbs.gen.nz>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file latmaac.c
 * LATM wrapped AAC decoder
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>

#include "parser.h"
#include "bitstream.h"
#include "mpeg4audio.h"
#include "neaacdec.h"

#define min(a,b) ((a)<(b) ? (a) : (b))


/*
    Note: This decoder filter is intended to decode LATM streams transferred
    in MPEG transport streams which are only supposed to contain one program.
    To do a more complex LATM demuxing a separate LATM demuxer should be used.
*/

#define AAC_NONE 0            // mode not detected (or indicated in mediatype)
#define AAC_LATM 1            // LATM packets (ISO/IEC 14496-3  1.7.3 Multiplex layer)

#define SYNC_LATM 0x2b7            // 11 bits

#define MAX_SIZE 8*1024

typedef struct AACConfig
{
    uint8_t    extra[64];            // should be way enough
    int        extrasize;

    int        audioObjectType;
    int        samplingFrequencyIndex;
    int        samplingFrequency;
    int        channelConfiguration;
    int        channels;
} AACConfig;

typedef struct AACParser
{
    AACConfig          config;
    uint8_t            frameLengthType;
    uint16_t           muxSlotLengthBytes;

    uint8_t            audio_mux_version;
    uint8_t            audio_mux_version_A;
    int                taraFullness;
    uint8_t            config_crc;
    int64_t            other_data_bits;

    int                mode;
    int                offset;        // byte offset in "buf" buffer
    uint8_t            buf[MAX_SIZE]; // allocated buffer
    int                count;         // number of bytes written in buffer
} AACParser;

typedef struct AACDecoder 
{
    AACParser          *parser;
    faacDecHandle      aac_decoder;
    int                open;
    uint32_t           in_samplerate;
    uint8_t            in_channels;
} AACDecoder;

typedef struct {
    AACDecoder*        decoder;
} FAACContext;

static inline int64_t latm_get_value(GetBitContext *b)
{
    uint8_t bytesForValue = get_bits(b, 2);
    int64_t value = 0;
    int i;
    for (i=0; i<=bytesForValue; i++) {
        value <<= 8;
        value |= get_bits(b, 8);
    }
    return value;
}

static void readGASpecificConfig(struct AACConfig *cfg, GetBitContext *b, PutBitContext *o)
{
    int framelen_flag = get_bits(b, 1);
    put_bits(o, 1, framelen_flag);
    int dependsOnCoder = get_bits(b, 1);
    put_bits(o, 1, dependsOnCoder);
    int ext_flag;
    int delay;
    int layerNr;

    if (dependsOnCoder) {
        delay = get_bits(b, 14);
        put_bits(o, 14, delay);
    }
    ext_flag = get_bits(b, 1);
    put_bits(o, 1, ext_flag);
    if (!cfg->channelConfiguration) {
        // program config element
        // TODO:
    }

    if (cfg->audioObjectType == 6 || cfg->audioObjectType == 20) {
        layerNr = get_bits(b, 3);
        put_bits(o, 3, layerNr);
    }
    if (ext_flag) {
        if (cfg->audioObjectType == 22) {
            skip_bits(b, 5);                    // numOfSubFrame
            skip_bits(b, 11);                    // layer_length

            put_bits(o, 16, 0);
        }
        if (cfg->audioObjectType == 17 ||
            cfg->audioObjectType == 19 ||
            cfg->audioObjectType == 20 ||
            cfg->audioObjectType == 23) {

            skip_bits(b, 3);                    // stuff
            put_bits(o, 3, 0);
        }

        skip_bits(b, 1);                        // extflag3
        put_bits(o, 1, 0);
    }
}

static int readAudioSpecificConfig(struct AACConfig *cfg, GetBitContext *b)
{
    PutBitContext o;
    init_put_bits(&o, cfg->extra, sizeof(cfg->extra));

    // returns the number of bits read
    int ret = 0;
    int sbr_present = -1;

    // object
    cfg->audioObjectType = get_bits(b, 5);
        put_bits(&o, 5, cfg->audioObjectType);
    if (cfg->audioObjectType == 31) {
        uint8_t n = get_bits(b, 6);
        put_bits(&o, 6, n);
        cfg->audioObjectType = 32 + n;
    }

    cfg->samplingFrequencyIndex = get_bits(b, 4);
    cfg->samplingFrequency = ff_mpeg4audio_sample_rates[cfg->samplingFrequencyIndex];
    put_bits(&o, 4, cfg->samplingFrequencyIndex);
    if (cfg->samplingFrequencyIndex == 0x0f) {
        uint32_t f = get_bits_long(b, 24);
        put_bits(&o, 24, f);
        cfg->samplingFrequency = f;
    }
    cfg->channelConfiguration = get_bits(b, 4);
    put_bits(&o, 4, cfg->channelConfiguration);
    cfg->channels = ff_mpeg4audio_channels[cfg->channelConfiguration];

    if (cfg->audioObjectType == 5) {
        sbr_present = 1;

        // TODO: parsing !!!!!!!!!!!!!!!!
    }

    switch (cfg->audioObjectType) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 6:
    case 7:
    case 17:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
        readGASpecificConfig(cfg, b, &o);
        break;
    }

    if (sbr_present == -1) {
        if (cfg->samplingFrequency <= 24000) {
            cfg->samplingFrequency *= 2;
        }            
    }

    // count the extradata
    ret = put_bits_count(&o);
    align_put_bits(&o);
    flush_put_bits(&o);
    cfg->extrasize = (ret + 7) >> 3;
    return ret;
}

static void readStreamMuxConfig(struct AACParser *parser, GetBitContext *b)
{
    parser->audio_mux_version_A = 0;
    parser->audio_mux_version = get_bits(b, 1);
    if (parser->audio_mux_version == 1) {                // audioMuxVersion
        parser->audio_mux_version_A = get_bits(b, 1);
    }

    if (parser->audio_mux_version_A == 0) {
        if (parser->audio_mux_version == 1) {
            parser->taraFullness = latm_get_value(b);
        }
        get_bits(b, 1);                    // allStreamSameTimeFraming = 1
        get_bits(b, 6);                    // numSubFrames = 0
        get_bits(b, 4);                    // numPrograms = 0

        // for each program
        get_bits(b, 3);                    // numLayer = 0

        // for each layer
        if (parser->audio_mux_version == 0) {
            // audio specific config.
            readAudioSpecificConfig(&parser->config, b);
        } else {
            int ascLen = latm_get_value(b);
            ascLen -= readAudioSpecificConfig(&parser->config, b);

            // fill bits
            while (ascLen > 16) {
                skip_bits(b, 16);
                ascLen -= 16;
            }
            skip_bits(b, ascLen);                    
        }

        // these are not needed... perhaps
        int frame_length_type = get_bits(b, 3);
        parser->frameLengthType = frame_length_type;
        if (frame_length_type == 0) {
            get_bits(b, 8);
        } else if (frame_length_type == 1) {
            get_bits(b, 9);
        } else if (frame_length_type == 3 ||
            frame_length_type == 4 ||
            frame_length_type == 5) {
            int celp_table_index = get_bits(b, 6);
        } else if (frame_length_type == 6 ||
            frame_length_type == 7) {
            int hvxc_table_index = get_bits(b, 1);
        }

        // other data
        parser->other_data_bits = 0;
        if (get_bits(b, 1)) {
            // other data present
            if (parser->audio_mux_version == 1) {
                parser->other_data_bits = latm_get_value(b);
            } else {
                // other data not present
                parser->other_data_bits = 0;
                int esc, tmp;
                do {
                    parser->other_data_bits <<= 8;
                    esc = get_bits(b, 1);
                    tmp = get_bits(b, 8);
                    parser->other_data_bits |= tmp;
                } while (esc);
            }
        }

        // CRC
        if (get_bits(b, 1)) {
            parser->config_crc = get_bits(b, 8);
        }
    } else {
        // tbd
    }
}

static void readPayloadLengthInfo(struct AACParser *parser, GetBitContext *b)
{
    uint8_t tmp;
    if (parser->frameLengthType == 0) {
        parser->muxSlotLengthBytes = 0;
        do {
            tmp = get_bits(b, 8);
            parser->muxSlotLengthBytes += tmp;
        } while (tmp == 255);
    } else {
        if (parser->frameLengthType == 5 ||
            parser->frameLengthType == 7 ||
            parser->frameLengthType == 3) {
            get_bits(b, 2);
        }
    }
}

static void readAudioMuxElement(struct AACParser *parser, GetBitContext *b, uint8_t *payload, int *payloadsize)
{
    uint8_t    use_same_mux = get_bits(b, 1);
    if (!use_same_mux) {
        readStreamMuxConfig(parser, b);
    }

    if (parser->audio_mux_version_A == 0) {
        int j;

        readPayloadLengthInfo(parser, b);

        // copy data
        for (j=0; j<parser->muxSlotLengthBytes; j++) {
            *payload++ = get_bits(b, 8);
        }
        *payloadsize = parser->muxSlotLengthBytes;

        // ignore otherdata
    } else {
        // TBD
    }
}

static int readAudioSyncStream(struct AACParser *parser, GetBitContext *b, int size, uint8_t *payload, int *payloadsize)
{
    // ISO/IEC 14496-3 Table 1.28 - Syntax of AudioMuxElement()
    if (get_bits(b, 11) != 0x2b7) return -1;        // not LATM
    int muxlength = get_bits(b, 13);

    if (3+muxlength > size) return 0;            // not enough data

    readAudioMuxElement(parser, b, payload, payloadsize);

    // we don't parse anything else here...
    return (3+muxlength);
}


static void flush_buf(struct AACParser *parser, int offset) {
    int bytes_to_flush = min(parser->count, offset);
    int left = (parser->count - bytes_to_flush);

    if (bytes_to_flush > 0) {
        if (left > 0) {
            memcpy(parser->buf, parser->buf+bytes_to_flush, left);
            parser->count = left;
        } else {
            parser->count = 0;
        }
    }
}

static struct AACParser *latm_create_parser()
{
    struct AACParser *parser = (struct AACParser *)av_malloc(sizeof(struct AACParser));
    memset(parser, 0, sizeof(struct AACParser));
    return parser;
}

static void latm_destroy_parser(struct AACParser *parser)
{
    av_free(parser);
}

static void latm_flush(struct AACParser *parser)
{
    parser->offset = 0;
    parser->count = 0;
}

static void latm_write_data(struct AACParser *parser, uint8_t *data, int len)
{
    // buffer overflow check... just ignore the data before
    if (parser->count + len > MAX_SIZE) {
        flush_buf(parser, parser->offset);
        parser->offset = 0;
        if (parser->count + len > MAX_SIZE) {
            int to_flush = (parser->count+len) - MAX_SIZE;
            flush_buf(parser, to_flush);
        }
    }

    // append data
    memcpy(parser->buf+parser->count, data, len);
    parser->count += len;
}

static int latm_parse_packet(struct AACParser *parser, uint8_t *data, int maxsize)
{
    /*
        Return value is either number of bytes parsed or
        -1 when failed.
        0 = need more data.
    */

    uint8_t    *start = parser->buf + parser->offset;
    int        bytes  = parser->count - parser->offset;
    GetBitContext    b;
    init_get_bits(&b, start, bytes);

    if (parser->mode == AAC_LATM) {
        int outsize = 0;
        int    ret = readAudioSyncStream(parser, &b, bytes, data, &outsize);

        if (ret < 0) return -1;
        if (ret == 0) return 0;

        // update the offset
        parser->offset += ret;
        return outsize;
    }

    // check for syncwords
    while (bytes > 2) {
        if (show_bits(&b, 11) == SYNC_LATM) {
            // we must parse config first...
            int outsize = 0;

            // check if there is a complete packet available...
            int ret = readAudioSyncStream(parser, &b, bytes, data, &outsize);
            if (ret < 0) return -1;
            if (ret == 0) return 0;
            parser->offset += ret;

            parser->mode = AAC_LATM;
            return outsize;
        }
        skip_bits(&b, 8);
        parser->offset++;
        bytes--;
    }
    return 0;
}

static void aac_filter_close(AACDecoder *decoder)
{
    if (decoder->aac_decoder) {
        NeAACDecClose(decoder->aac_decoder);
        decoder->aac_decoder = NULL;
    }
    decoder->open = 0;
}

static int aac_decoder_open(AACDecoder *decoder)
{
    if (decoder->aac_decoder) return 0;

    decoder->aac_decoder = NeAACDecOpen();
    if (!decoder->aac_decoder) return -1;

    // are we going to initialize from decoder specific info ?
    if (decoder->parser->config.extrasize > 0) {
        char ret = NeAACDecInit2(decoder->aac_decoder, (unsigned char*)decoder->parser->config.extra, decoder->parser->config.extrasize, &decoder->in_samplerate, &decoder->in_channels);
        if (ret < 0) {
            aac_filter_close(decoder);        // gone wrong ?
            return -1;
        }
        decoder->open = 1;
    } else {
        // we'll open the decoder later...
        decoder->open = 0;
    }
    return 0;
}

AACDecoder *aac_filter_create()
{
    AACDecoder *decoder = (AACDecoder *)av_malloc(sizeof(AACDecoder));
    decoder->parser = latm_create_parser();
    decoder->aac_decoder = NULL;
    decoder->open = 0;
    return (void *)decoder;
}

void aac_filter_destroy(AACDecoder *decoder)
{
    aac_filter_close(decoder);
    latm_destroy_parser(decoder->parser);
    av_free(decoder);
}

int aac_filter_receive(AACDecoder *decoder, void *out, int *out_size, uint8_t *data, int size)
{
    uint8_t    tempbuf[32*1024];
    int        ret;
    int        consumed = size;
    int        decoded;
    int        max_size = *out_size;
    
    *out_size = 0;

    //-------------------------------------------------------------------------
    // Multiplex Parsing
    //-------------------------------------------------------------------------

    latm_write_data(decoder->parser, data, size);

    do {
        ret = latm_parse_packet(decoder->parser, tempbuf, sizeof(tempbuf));
                if (ret < 0) {
                        latm_flush(decoder->parser);
                        return consumed;
                }
        if (ret == 0) return consumed;

        data = tempbuf;
        size = ret;

        //-------------------------------------------------------------------------
        // Initialize decoder (if necessary)
        //-------------------------------------------------------------------------
        if (!decoder->open) {
            aac_filter_close(decoder);
            if (decoder->parser->mode == AAC_LATM) {
                ret = aac_decoder_open(decoder);
                if (ret < 0) return consumed;
            }

            if(!decoder->open) return consumed;
        }

        //-------------------------------------------------------------------------
        // Decode samples
        //-------------------------------------------------------------------------
        NeAACDecFrameInfo    info;
        void *buf = NeAACDecDecode(decoder->aac_decoder, &info, data, size);

        if (buf) {
            decoder->in_samplerate = info.samplerate;
            decoder->in_channels = info.channels;

            //---------------------------------------------------------------------
            // Deliver decoded samples
            //---------------------------------------------------------------------

            // kram dekoduje 16-bit. my vypustame 16-bit. takze by to malo byt okej
            decoded = info.samples * sizeof(short);

            // napraskame tam sample
            *out_size += decoded;
            if(*out_size > max_size) {
                av_log(NULL, AV_LOG_ERROR, "overflow!\n");
            } else {
                memcpy(out, buf, decoded);
                out = (unsigned char *)out + decoded;
            }
        } else {
            // need more data
            break;
        }

    } while (1);    // decode all packets
    return consumed;
}

void aac_filter_getinfo(AACDecoder *decoder, int *sample_rate, int *channels)
{
    if(!decoder->open) return;
    *sample_rate = decoder->in_samplerate;
    *channels = decoder->in_channels;
}

static int faac_decode_init(AVCodecContext *avctx)
{
    FAACContext *s = avctx->priv_data;
    avctx->frame_size = 360;
    avctx->sample_rate = 48000;
    avctx->channels = 2;
    avctx->bit_rate = 8192 * 8 * avctx->sample_rate / avctx->frame_size;
    s->decoder = aac_filter_create();
    return 0;
}

static int faac_decode_frame(AVCodecContext *avctx,
                             void *data, int *data_size,
                             uint8_t *buf, int buf_size)
{
    FAACContext *s = avctx->priv_data;
    int ret;

    if (s->decoder == NULL) faac_decode_init(avctx);
    ret = aac_filter_receive(s->decoder, data, data_size, buf, buf_size);
    aac_filter_getinfo(s->decoder, &(avctx->sample_rate), &(avctx->channels));
    return ret;
}

static int faac_decode_end(AVCodecContext *avctx)
{
    FAACContext *s = avctx->priv_data;
    if(s->decoder != NULL) {
        aac_filter_destroy(s->decoder);
    }
    return 0;
}

AVCodec libfaad2_decoder = {
    .name = "AAC_LATM",
    .type = CODEC_TYPE_AUDIO,
    .id = CODEC_ID_AAC_LATM,
    .priv_data_size = sizeof (FAACContext),
    .init = faac_decode_init,
    .close = faac_decode_end,
    .decode = faac_decode_frame,
    .long_name = "AAC over LATM",
};

