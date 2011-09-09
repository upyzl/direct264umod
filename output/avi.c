/*****************************************************************************
 * avi.c: x264 avi output module
 *****************************************************************************
 * Copyright (C) 2009 - 2011 Zhou Zongyi <zhouzy@os.pku.edu.cn>
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
 *****************************************************************************/

#include "output.h"

#define MAX_CHUNK_SIZE 0x400000

#ifdef MAKEFOURCC
#undef MAKEFOURCC
#endif

#ifndef WORDS_BIGENDIAN
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
                ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | \
                ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))
#define LE64(x) (x)
#define LE32(x) (x)
#define LE16(x) (x)
#else
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
                ((uint32_t)(uint8_t)(ch3) | ((uint32_t)(uint8_t)(ch2) << 8) | \
                ((uint32_t)(uint8_t)(ch1) << 16) | ((uint32_t)(uint8_t)(ch0) << 24 ))

uint32_t inline LE32(uint32_t x)
{
    return (x>>24) + ((x>>8)&0xFF00) + ((x<<8)&0xFF0000) + (x<<24);
}

uint16_t inline LE16(uint16_t x)
{
    return (x>>8) + (x<<8);
}

uint64_t inline LE64(uint64_t x)
{
    return ((uint64_t)LE32(x)<<32) + LE32(x>>32);
}
#endif

#ifdef _MSC_VER
#pragma pack(4)
#define PACK
#else
#define PACK __attribute__((__packed__))
#endif

typedef struct
{
    uint32_t dwList;
    uint32_t dwSize;
    uint32_t fcc;
} list_header_t;

typedef struct
{
    uint32_t dwFourCC;
    uint32_t dwSize;
} chunk_header_t;

typedef struct
{
    int64_t qwOffset;
    uint32_t dwSize;
    uint32_t dwDuration;
} PACK avi_superindex_entry_t;

typedef struct
{
    uint32_t dwFourCC;
    uint32_t dwSize;
    uint16_t wLongsPerEntry;
    uint8_t  bIndexSubType;
    uint8_t  bIndexType;
    uint32_t nEntriesInUse;
    uint32_t dwChunkId;
    uint32_t dwReserved[3];
    avi_superindex_entry_t aIndex[512];
} PACK avi_superindex_t;

typedef struct
{
    uint32_t dwOffset;
    uint32_t dwSize;
} avi_stdindex_entry_t;

typedef struct
{
    uint32_t fcc;
    uint32_t cb;
    uint16_t wLongsPerEntry;
    uint8_t  bIndexSubType;
    uint8_t  bIndexType;
    uint32_t nEntriesInUse;
    uint32_t dwChunkId;
    int64_t  qwBaseOffset;
    uint32_t dwReserved3;
} PACK avi_stdindex_t;

typedef struct
{
    list_header_t riff_hdr;
    list_header_t hdrl_hdr;

        chunk_header_t avih;
        uint32_t dwMicroSecPerFrame; // frame display rate (or 0)
        uint32_t dwMaxBytesPerSec; // max. transfer rate
        uint32_t dwPaddingGranularity; // pad to multiples of this
        uint32_t dwavihFlags; // the ever-present flags
        uint32_t dwTotalFrames; // # frames in file
        uint32_t dwavihInitialFrames;
        uint32_t dwStreams;
        uint32_t dwavihSuggestedBufferSize;
        uint32_t dwWidth;
        uint32_t dwHeight;
        uint32_t dwReserved[4];

    list_header_t strl_hdr;

        chunk_header_t strh;
        uint32_t fccType;
        uint32_t fccHandler;
        uint32_t dwstrhFlags;
        uint16_t wPriority;
        uint16_t wLanguage;
        uint32_t dwstrhInitialFrames;
        uint32_t dwScale;
        uint32_t dwRate; /* dwRate / dwScale == samples/second */
        uint32_t dwStart;
        uint32_t dwLength; /* In units above... */
        uint32_t dwstrhSuggestedBufferSize;
        uint32_t dwQuality;
        uint32_t dwSampleSize;
        uint16_t rcFrameLeft, rcFrameTop, rcFrameRight, rcFrameBottom;

        chunk_header_t strf;
        uint32_t biSize;
        uint32_t biWidth;
        int32_t  biHeight;
        uint16_t biPlanes;
        uint16_t biBitCount;
        uint32_t biCompression;
        uint32_t biSizeImage;
        int32_t  biXPelsPerMeter;
        int32_t  biYPelsPerMeter;
        uint32_t biClrUsed;
        uint32_t biClrImportant;

        avi_superindex_t indx;

    list_header_t movi_hdr;
} PACK avi_header_t;

typedef struct
{
    uint32_t ckid;
    uint32_t dwFlags;
    uint32_t dwChunkOffset;
    uint32_t dwChunkLength;
} avi_index_entry_t;

typedef struct index_list_s
{
    avi_stdindex_entry_t entries[2048];
    struct index_list_s *next;
} index_list_t;

typedef struct
{
    double d_scalefactor;
    FILE *f;
    int32_t i_curr_index;
    uint32_t i_chunk_start;
    int32_t i_frame;
    int32_t i_spsppslen;
    uint32_t i_remainriffsize;
    int32_t i_remainchunksize;
    uint32_t i_dataoffset;
    int32_t i_maxframe;
    avi_header_t avi_hdr;
    char spsppsbuf[1024];
    avi_stdindex_t ix00;
    index_list_t aIndex;
    index_list_t *p_curr_index;
} avi_t;

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt )
{
    avi_t *p_avi = malloc(sizeof(avi_t));
    avi_header_t *p_hdr;

    if (!p_avi)
        return -1;

    memset(p_avi, 0, sizeof(*p_avi));
    if (!(p_avi->f = fopen(psz_filename,"wb"))){
        free(p_avi);
        return -1;
    }

    p_hdr = &p_avi->avi_hdr;
    p_hdr->riff_hdr.dwList = MAKEFOURCC('R','I','F','F');
    p_hdr->riff_hdr.fcc  = MAKEFOURCC('A','V','I',' ');
    p_hdr->hdrl_hdr.dwList = MAKEFOURCC('L','I','S','T');
    p_hdr->hdrl_hdr.fcc  = MAKEFOURCC('h','d','r','l');
    p_hdr->strl_hdr.dwList = MAKEFOURCC('L','I','S','T');
    p_hdr->strl_hdr.fcc  = MAKEFOURCC('s','t','r','l');
    p_hdr->movi_hdr.dwList = MAKEFOURCC('L','I','S','T');
    p_hdr->movi_hdr.fcc  = MAKEFOURCC('m','o','v','i');
    p_hdr->avih.dwFourCC = MAKEFOURCC('a','v','i','h');
    p_hdr->strh.dwFourCC = MAKEFOURCC('s','t','r','h');
    p_hdr->strf.dwFourCC = MAKEFOURCC('s','t','r','f');
    p_hdr->fccType = MAKEFOURCC('v','i','d','s');
    p_hdr->fccHandler = p_hdr->biCompression = MAKEFOURCC('H','2','6','4');

    p_hdr->dwavihFlags = LE32(0x0130); //AVIF_HASINDEX | AVIF_MUSTUSEINDEX | AVIF_ISINTERLEAVED
    p_hdr->dwStreams = LE32(1);
    p_hdr->avih.dwSize = LE32(4*16 - 8);
    p_hdr->strh.dwSize = LE32(4*16 - 8);
    p_hdr->strf.dwSize = LE32(4*12 - 8);
    p_hdr->biSize = LE32(4*12 - 8);
    p_hdr->biPlanes = LE16(1);
    p_hdr->biBitCount = LE16(24);

    p_hdr->indx.dwFourCC = MAKEFOURCC('i','n','d','x');
    p_hdr->indx.wLongsPerEntry = LE16(4);
    p_hdr->indx.bIndexSubType = 0;
    p_hdr->indx.bIndexType = 0; //AVI_INDEX_OF_INDEXES
    p_hdr->indx.dwChunkId = MAKEFOURCC('0','0','d','c');

    p_avi->i_curr_index = 0;
    p_avi->i_chunk_start = 0;
    p_avi->i_dataoffset = 0;
    p_avi->aIndex.next = 0;
    p_avi->p_curr_index = &p_avi->aIndex;
    p_avi->i_remainriffsize = 0x40000 - sizeof(avi_header_t) - sizeof(chunk_header_t);

    p_avi->ix00.fcc = MAKEFOURCC('i','x','0','0');
    p_avi->ix00.wLongsPerEntry = LE16(2);
    p_avi->ix00.bIndexSubType = 0;
    p_avi->ix00.bIndexType = 1;
    p_avi->ix00.dwChunkId = MAKEFOURCC('0','0','d','c');
    p_avi->ix00.qwBaseOffset = LE64(sizeof(avi_header_t));

    *p_handle = p_avi;
    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal, int i_nal )
{
    avi_t *p_avi = handle;
    int size = 0, i = 0;
    for (;i < i_nal;i++)
        size += p_nal[i].i_payload;
    memcpy(p_avi->spsppsbuf, p_nal[0].p_payload, size);
    p_avi->i_spsppslen = size;
    return size;
}

static void flush_chunk( avi_t *p_avi )
{
    chunk_header_t chunk;
    chunk.dwFourCC = MAKEFOURCC('0','0','d','c');
    if (p_avi->i_dataoffset & 1) {
        p_avi->i_dataoffset++;
        fputc(0, p_avi->f);
    }
    chunk.dwSize = LE32(p_avi->i_dataoffset - p_avi->i_chunk_start - sizeof(chunk_header_t));
    fseek(p_avi->f, p_avi->ix00.qwBaseOffset + p_avi->i_chunk_start, SEEK_SET);
    fwrite(&chunk, sizeof(chunk_header_t), 1, p_avi->f);
    fseek(p_avi->f, LE32(chunk.dwSize), SEEK_CUR);
    if (LE32(p_avi->avi_hdr.dwstrhSuggestedBufferSize) < LE32(chunk.dwSize) + sizeof(chunk_header_t))
        p_avi->avi_hdr.dwstrhSuggestedBufferSize = LE32(LE32(chunk.dwSize) + sizeof(chunk_header_t));
    p_avi->i_chunk_start = -1; //hack
}

static void flush_riff( avi_t *p_avi )
{
    list_header_t hdr[2];
    index_list_t *p_index = &p_avi->aIndex;
    int i;
    avi_header_t *p_hdr = &p_avi->avi_hdr;
    avi_superindex_entry_t *p_indexentry = &p_hdr->indx.aIndex[p_hdr->indx.nEntriesInUse];
    if (p_avi->i_chunk_start != -1)
        flush_chunk(p_avi);

    p_avi->ix00.nEntriesInUse = LE32(p_avi->i_curr_index);
    p_avi->ix00.cb = LE32(p_avi->i_curr_index * sizeof(avi_stdindex_entry_t) + sizeof(avi_stdindex_t) - sizeof(chunk_header_t));
    p_indexentry->qwOffset = LE64(ftell(p_avi->f));
    p_indexentry->dwSize = LE32(sizeof(avi_stdindex_t) + p_avi->i_curr_index * sizeof(avi_stdindex_entry_t));
    p_indexentry->dwDuration = p_avi->ix00.nEntriesInUse;
    p_hdr->indx.nEntriesInUse++;
    fwrite(&p_avi->ix00, sizeof(avi_stdindex_t), 1, p_avi->f);
    for (i = p_avi->i_curr_index;i >= 2048;i -= 2048) {
        fwrite(p_index->entries, 2048, sizeof(avi_stdindex_entry_t), p_avi->f);
        p_index = p_index->next;
    }
    fwrite(p_index->entries, sizeof(avi_stdindex_entry_t), i, p_avi->f);

    if (!p_hdr->riff_hdr.dwSize) {
        p_hdr->riff_hdr.dwSize = LE32(ftell(p_avi->f) - sizeof(list_header_t) + 4);
        p_hdr->movi_hdr.dwSize = LE32(LE32(p_hdr->riff_hdr.dwSize) - LE64(p_avi->ix00.qwBaseOffset) + sizeof(list_header_t));
        p_hdr->dwTotalFrames = p_avi->ix00.nEntriesInUse;
        return;
    }

    hdr[1].dwList = MAKEFOURCC('L','I','S','T');
    hdr[1].dwSize = LE32(ftell(p_avi->f) - LE64(p_avi->ix00.qwBaseOffset) + 4);
    hdr[1].fcc = MAKEFOURCC('m','o','v','i');
    hdr[0].dwList = MAKEFOURCC('R','I','F','F');
    hdr[0].dwSize = LE32(LE32(hdr[1].dwSize) + sizeof(list_header_t));
    hdr[0].fcc = MAKEFOURCC('A','V','I','X');
    
    fseek(p_avi->f, 4 - (int64_t)LE32(hdr[0].dwSize) - sizeof(list_header_t), SEEK_CUR);
    fwrite(hdr, sizeof(list_header_t), 2, p_avi->f);
    fseek(p_avi->f, LE32(hdr[0].dwSize) - 4, SEEK_CUR);
}

static void new_chunk( avi_t *p_avi )
{
    p_avi->i_remainchunksize = MAX_CHUNK_SIZE - sizeof(chunk_header_t);
    fseek(p_avi->f, sizeof(chunk_header_t), SEEK_CUR); // make room for chunk header;
    p_avi->i_chunk_start = p_avi->i_dataoffset;
    p_avi->i_dataoffset += sizeof(chunk_header_t);
    p_avi->i_remainriffsize -= sizeof(chunk_header_t);
}

static void new_riff( avi_t *p_avi )
{
    fseek(p_avi->f, sizeof(list_header_t) * 2, SEEK_CUR); // make room for riff and movi headers
    p_avi->p_curr_index = &p_avi->aIndex;
    p_avi->i_curr_index = 0;
    p_avi->i_dataoffset = 0;
    p_avi->i_remainriffsize = -(int64_t)sizeof(list_header_t);
    p_avi->ix00.qwBaseOffset = ftell(p_avi->f);
    new_chunk(p_avi);
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    avi_t *p_avi = handle;
    uint32_t imgsize;

    p_avi->avi_hdr.dwWidth = p_avi->avi_hdr.biWidth = LE32(p_param->i_width);
    p_avi->avi_hdr.rcFrameRight = LE16((uint16_t)p_param->i_width);
    p_avi->avi_hdr.dwHeight = p_avi->avi_hdr.biHeight = LE32(p_param->i_height);
    p_avi->avi_hdr.rcFrameBottom = LE16((uint16_t)p_param->i_height);
    imgsize = p_param->i_width * p_param->i_height;
    p_avi->avi_hdr.biSizeImage = LE32(imgsize * 3);

    p_avi->avi_hdr.dwRate  = LE32(p_param->i_fps_num);
    p_avi->avi_hdr.dwScale = LE32(p_param->i_fps_den);
    p_avi->avi_hdr.dwMicroSecPerFrame = LE32(lrintf(p_param->i_fps_den * 1E6f / p_param->i_fps_num));
    p_avi->d_scalefactor = (double)p_param->i_fps_num * p_param->i_timebase_num / p_param->i_timebase_den;

    fseek(p_avi->f, sizeof(avi_header_t), SEEK_SET);
    new_chunk(p_avi);
    return 0;
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    avi_t *p_avi = handle;
    avi_stdindex_entry_t *p_entry;
    uint64_t dts;
    int needlateflush = 0;
    uint32_t entrysize = i_size;

    if (p_avi->i_curr_index && !(p_avi->i_curr_index & 2047)) {
        if (!(p_avi->p_curr_index->next))
            if (!(p_avi->p_curr_index->next = malloc(sizeof(index_list_t))))
                return -1;
        p_avi->p_curr_index = p_avi->p_curr_index->next;
        p_avi->p_curr_index->next = 0;
    }

    if (dts = llrint(p_picture->i_dts * p_avi->d_scalefactor)) {
        uint32_t interval = LE32(p_avi->avi_hdr.dwScale);
        uint64_t currdts = (uint64_t)(p_avi->i_frame) * interval;
        for(currdts -= interval / 2;dts > currdts;currdts += interval){
            // insert zero-byte chunks
            if (sizeof(avi_stdindex_entry_t) > p_avi->i_remainriffsize) {
                flush_riff(p_avi);
                new_riff(p_avi);
            }
            p_entry = &p_avi->p_curr_index->entries[p_avi->i_curr_index & 2047];
            p_entry->dwOffset = LE32(p_avi->i_dataoffset);
            p_entry->dwSize = 0;
            p_avi->i_frame++;
            p_avi->i_curr_index++;
            p_avi->i_remainriffsize -= 8;

            if (!(p_avi->i_curr_index & 2047)) {
                if (!(p_avi->p_curr_index->next))
                    if (!(p_avi->p_curr_index->next = malloc(sizeof(index_list_t))))
                        return -1;
                p_avi->p_curr_index = p_avi->p_curr_index->next;
                p_avi->p_curr_index->next = 0;
            }
        }
    }

    if (p_avi->i_spsppslen > 0) {
        entrysize += p_avi->i_spsppslen - 1;
        fwrite(p_avi->spsppsbuf + 1, p_avi->i_spsppslen - 1, 1, p_avi->f);
        p_avi->i_spsppslen = -1;
    }

    if (entrysize + sizeof(avi_stdindex_entry_t) > p_avi->i_remainriffsize) {
        if (!p_avi->i_curr_index)
            needlateflush = 1;
        else {
            flush_riff(p_avi);
            new_riff(p_avi);
        }
    } else if (p_avi->i_remainchunksize <= 0) {
        flush_chunk(p_avi);
        new_chunk(p_avi);
    }

    p_entry = &p_avi->p_curr_index->entries[p_avi->i_curr_index & 2047];
    p_entry->dwOffset = LE32(p_avi->i_dataoffset);
    p_avi->i_frame++;
    if (entrysize > p_avi->i_maxframe)
        p_avi->i_maxframe = entrysize;

    fwrite(p_nalu, i_size, 1, p_avi->f);
    p_avi->i_dataoffset += entrysize;
    p_avi->i_remainchunksize -= entrysize;
    if (needlateflush)
        p_avi->i_remainriffsize = 0;
    else
        p_avi->i_remainriffsize -= entrysize + sizeof(avi_stdindex_entry_t);
    p_avi->i_curr_index++;
    p_entry->dwSize = LE32(entrysize | (~(int32_t)p_picture->b_keyframe << 31));

    return i_size;
}

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    avi_t *p_avi = handle;
    avi_header_t *p_hdr;
    index_list_t *p;
    int i;

    if (!p_avi)
        return 0;

    flush_riff(p_avi);
    p_hdr = &p_avi->avi_hdr;
    p_hdr->dwavihSuggestedBufferSize = 0;
    p_hdr->dwLength = LE32(p_avi->i_frame);

    for (p = p_avi->aIndex.next;p;) {
        index_list_t *q = p;
        p = p->next;
        free(q);
    }

    i = p_hdr->indx.nEntriesInUse;
    p_hdr->indx.dwSize = LE32(sizeof(avi_superindex_t) - sizeof(chunk_header_t) - sizeof(avi_superindex_entry_t) * (512 - i));
    if (i < 512) {
        chunk_header_t *junk = (chunk_header_t*)&p_hdr->indx.aIndex[i];
        junk->dwFourCC =  MAKEFOURCC('J','U','N','K');
        junk->dwSize = LE32(sizeof(avi_superindex_entry_t) * (512 - i) - sizeof(chunk_header_t));
    }
    p_hdr->strl_hdr.dwSize = LE32(4 + 4*16 + 4*12 + sizeof(chunk_header_t) + LE32(p_hdr->indx.dwSize));
    p_hdr->hdrl_hdr.dwSize = LE32(4 + 4*16 + 12 + 4*16 + 4*12 + sizeof(chunk_header_t) + LE32(p_hdr->indx.dwSize));
    p_hdr->dwMaxBytesPerSec = LE32(lrintf((p_avi->i_maxframe + sizeof(avi_stdindex_entry_t)) * 1E6f / LE32(p_hdr->dwMicroSecPerFrame)));

    fseek(p_avi->f, 0, SEEK_SET);
    fwrite(p_hdr, sizeof(avi_header_t), 1, p_avi->f);

    fclose(p_avi->f);
    free(p_avi);
    return 0;
}

const cli_output_t avi_output = { open_file, set_param, write_headers, write_frame, close_file };
