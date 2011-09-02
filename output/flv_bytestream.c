/*****************************************************************************
 * flv_bytestream.c: flv muxer utilities
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

/* flv writing functions */

flv_buffer *flv_create_writer( const char *filename )
{
    flv_buffer *c = malloc( sizeof(*c) );

    if( !c )
        return NULL;
    memset( c, 0, sizeof(*c) );

    if( !strcmp( filename, "-" ) )
        c->fp = stdout;
    else
        c->fp = fopen( filename, "wb" );
    if( !c->fp )
    {
        free( c );
        return NULL;
    }

    return c;
}

int flv_append_data( flv_buffer *c, uint8_t *data, unsigned size )
{
    unsigned ns = c->d_cur + size;

    if( ns > c->d_max )
    {
        void *dp;
        unsigned dn = 16;
        while( ns > dn )
            dn <<= 1;

        dp = realloc( c->data, dn );
        if( !dp )
            return -1;

        c->data = dp;
        c->d_max = dn;
    }

    memcpy( c->data + c->d_cur, data, size );

    c->d_cur = ns;

    return 0;
}

void flv_rewrite_amf_be24( flv_buffer *c, unsigned length, unsigned start )
{
     *(c->data + start + 0) = length >> 16;
     *(c->data + start + 1) = length >> 8;
     *(c->data + start + 2) = length >> 0;
}

int flv_flush_data( flv_buffer *c )
{
    if( !c->d_cur )
        return 0;

    if( fwrite( c->data, c->d_cur, 1, c->fp ) != 1 )
        return -1;

    c->d_total += c->d_cur;

    c->d_cur = 0;

    return 0;
}
