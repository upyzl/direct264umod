/*****************************************************************************
 * flv.c: flv muxer
 *****************************************************************************
 * Copyright (C) 2009-2011 x264 project
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#include "output.h"
#include "flv_bytestream.h"

#define CHECK(x)\
do {\
    if( (x) < 0 )\
        return -1;\
} while( 0 )

typedef struct
{
    flv_buffer *c;

    uint8_t *sei;
    int sei_len;

    int64_t i_framenum;
    int     i_num_delayed_frames;
    double  d_scalefactor;

    uint64_t i_framerate_pos;
    uint64_t i_duration_pos;
    uint64_t i_filesize_pos;
    uint64_t i_bitrate_pos;

    uint8_t b_write_length;
    int64_t i_delayed_pts;
    int64_t i_prev_dts[3];

    unsigned start;
} flv_hnd_t;

static int write_header( flv_buffer *c )
{
    flv_put_tag( c, "FLV" ); // Signature
    flv_put_byte( c, 1 );    // Version
    flv_put_byte( c, 1 );    // Video Only
    flv_put_be32( c, 9 );    // DataOffset
    flv_put_be32( c, 0 );    // PreviousTagSize0

    return flv_flush_data( c );
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt )
{
    flv_hnd_t *p_flv = malloc( sizeof(*p_flv) );
    *p_handle = NULL;
    if( !p_flv )
        return -1;
    memset( p_flv, 0, sizeof(*p_flv) );

    p_flv->c = flv_create_writer( psz_filename );
    if( !p_flv->c )
        return -1;

    CHECK( write_header( p_flv->c ) );
    *p_handle = p_flv;

    return 0;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    flv_hnd_t *p_flv = handle;
    flv_buffer *c = p_flv->c;
    int length;
    int start = c->d_cur;

    flv_put_byte( c, FLV_TAG_TYPE_META ); // Tag Type "script data"

    flv_put_be24( c, 0 ); // data length
    flv_put_be24( c, 0 ); // timestamp
    flv_put_be32( c, 0 ); // reserved

    flv_put_byte( c, AMF_DATA_TYPE_STRING );
    flv_put_amf_string( c, "onMetaData" );

    flv_put_byte( c, AMF_DATA_TYPE_MIXEDARRAY );
    flv_put_be32( c, 7 );

    flv_put_amf_string( c, "width" );
    flv_put_amf_double( c, p_param->i_width );

    flv_put_amf_string( c, "height" );
    flv_put_amf_double( c, p_param->i_height );

    flv_put_amf_string( c, "framerate" );

    if( p_param->b_vfr_input )
        p_flv->i_framerate_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, (double)p_param->i_fps_num / p_param->i_fps_den );

    flv_put_amf_string( c, "videocodecid" );
    flv_put_amf_double( c, FLV_CODECID_H264 );

    flv_put_amf_string( c, "duration" );
    p_flv->i_duration_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

    flv_put_amf_string( c, "filesize" );
    p_flv->i_filesize_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

    flv_put_amf_string( c, "videodatarate" );
    p_flv->i_bitrate_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

    flv_put_amf_string( c, "" );
    flv_put_byte( c, AMF_END_OF_OBJECT );

    length = c->d_cur - start;
    flv_rewrite_amf_be24( c, length - 10, start );

    flv_put_be32( c, length + 1 ); // tag length

    p_flv->d_scalefactor = (double)(p_param->i_timebase_num * 1000) / p_param->i_timebase_den;;
    p_flv->i_num_delayed_frames = 0;

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal, int i_nal )
{
    flv_hnd_t *p_flv = handle;
    flv_buffer *c = p_flv->c;

    int sps_size = p_nal[0].i_payload;
    int pps_size = p_nal[1].i_payload;
    int sei_size = i_nal > 2? p_nal[2].i_payload : 0;
    uint8_t *sps;
    int length;

    // SEI
    /* It is within the spec to write this as-is but for
     * mplayer/ffmpeg playback this is deferred until before the first frame */

    if (sei_size)
    {
        p_flv->sei = malloc( sei_size );
        if( !p_flv->sei )
            return -1;
        p_flv->sei_len = sei_size;

        memcpy( p_flv->sei, p_nal[2].p_payload, sei_size );
    }
    else
        p_flv->sei = 0;

    // SPS
    sps = p_nal[0].p_payload + 4;

    flv_put_byte( c, FLV_TAG_TYPE_VIDEO );
    flv_put_be24( c, 0 ); // rewrite later
    flv_put_be24( c, 0 ); // timestamp
    flv_put_byte( c, 0 ); // timestamp extended
    flv_put_be24( c, 0 ); // StreamID - Always 0
    p_flv->start = c->d_cur; // needed for overwriting length

    flv_put_byte( c, 7 | FLV_FRAME_KEY ); // Frametype and CodecID
    flv_put_byte( c, 0 ); // AVC sequence header
    flv_put_be24( c, 0 ); // composition time

    flv_put_byte( c, 1 );      // version
    flv_put_byte( c, sps[1] ); // profile
    flv_put_byte( c, sps[2] ); // profile
    flv_put_byte( c, sps[3] ); // level
    flv_put_byte( c, 0xff );   // 6 bits reserved (111111) + 2 bits nal size length - 1 (11)
    flv_put_byte( c, 0xe1 );   // 3 bits reserved (111) + 5 bits number of sps (00001)

    flv_put_be16( c, sps_size - 4 );
    flv_append_data( c, sps, sps_size - 4 );

    // PPS
    flv_put_byte( c, 1 ); // number of pps
    flv_put_be16( c, pps_size - 4 );
    flv_append_data( c, p_nal[1].p_payload + 4, pps_size - 4 );

    // rewrite data length info
    length = c->d_cur - p_flv->start;
    flv_rewrite_amf_be24( c, length, p_flv->start - 10 );
    flv_put_be32( c, length + 11 ); // Last tag size
    CHECK( flv_flush_data( c ) );

    return sei_size + sps_size + pps_size;
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    flv_hnd_t *p_flv = handle;
    flv_buffer *c = p_flv->c;

    int64_t dts, dts0;
    int64_t cts;
    int offset;
    int length;
    int delay = p_flv->i_num_delayed_frames;

    dts0 = llrint(p_picture->i_dts * p_flv->d_scalefactor);
    cts  = llrint(p_picture->i_pts * p_flv->d_scalefactor);
    dts  = delay?  p_flv->i_prev_dts[delay]: dts0;
    p_flv->i_prev_dts[2] = p_flv->i_prev_dts[1];
    p_flv->i_prev_dts[1] = dts0;

    if (!p_flv->i_delayed_pts && cts > dts0)
        p_flv->i_delayed_pts = dts0;
    else if (p_flv->i_delayed_pts >= cts)
        p_flv->i_delayed_pts = 0;
    if (p_flv->i_delayed_pts && p_flv->i_delayed_pts <= dts)
    {
        dts = (p_flv->i_delayed_pts + p_flv->i_prev_dts[0] + 1) >> 1;
        p_flv->i_num_delayed_frames++;
    }
    p_flv->i_prev_dts[0] = dts;
    offset = cts - dts;

    // A new frame - write packet header
    flv_put_byte( c, FLV_TAG_TYPE_VIDEO );
    flv_put_be24( c, 0 ); // calculated later
    flv_put_be24( c, dts );
    flv_put_byte( c, dts >> 24 );
    flv_put_be24( c, 0 );

    p_flv->start = c->d_cur;
    flv_put_byte( c, p_picture->b_keyframe ? FLV_FRAME_KEY : FLV_FRAME_INTER );
    flv_put_byte( c, 1 ); // AVC NALU
    flv_put_be24( c, offset );

    if( p_flv->sei )
    {
        flv_append_data( c, p_flv->sei, p_flv->sei_len );
        free( p_flv->sei );
        p_flv->sei = NULL;
    }
    flv_append_data( c, p_nalu, i_size );

    length = c->d_cur - p_flv->start;
    flv_rewrite_amf_be24( c, length, p_flv->start - 10 );
    flv_put_be32( c, 11 + length ); // Last tag size
    CHECK( flv_flush_data( c ) );

    p_flv->i_framenum++;

    return i_size;
}

static void rewrite_amf_double( FILE *fp, uint64_t position, double value )
{
    uint64_t x = endian_fix64( flv_dbl2int( value ) );
    fseek( fp, position, SEEK_SET );
    fwrite( &x, 8, 1, fp );
}

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    flv_hnd_t *p_flv = handle;
    flv_buffer *c = p_flv->c;
    double total_duration;

    CHECK( flv_flush_data( c ) );

    total_duration = (p_flv->i_prev_dts[1] * 2 - p_flv->i_prev_dts[2]) * 0.001;

    if( x264_is_regular_file( c->fp ) && total_duration > 0 )
    {
        double framerate;
        int64_t filesize = ftell( c->fp );

        if( p_flv->i_framerate_pos )
        {
            framerate = (double)p_flv->i_framenum / total_duration;
            rewrite_amf_double( c->fp, p_flv->i_framerate_pos, framerate );
        }

        rewrite_amf_double( c->fp, p_flv->i_duration_pos, total_duration );
        rewrite_amf_double( c->fp, p_flv->i_filesize_pos, filesize );
        rewrite_amf_double( c->fp, p_flv->i_bitrate_pos, (double)filesize / ( total_duration * (1000 / 8)) );
    }

    fclose( c->fp );
    free( p_flv );
    free( c );

    return 0;
}

const cli_output_t flv_output = { open_file, set_param, write_headers, write_frame, close_file };
