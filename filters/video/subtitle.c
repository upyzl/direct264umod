/*****************************************************************************
* subtitle.c: subtitle render filter using vsfilter
*****************************************************************************
* Copyright (C) 2011 Zhou Zongyi <zhouzy_wuxi@hotmail.com>
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

#include <Windows.h>
#include "x264cli.h"
#include "subtitle.h"
#include "video.h"

csri_open_file_t csri_open_file;
csri_add_file_t csri_add_file;
csri_request_fmt_t csri_request_fmt;
csri_render_t csri_render;
csri_close_t csri_close;

//csri_openflag flags={};
char* subfilename[16];
int subtotal = 0;
HMODULE hVSFilter = 0;

int add_sub(char *filename)
{
	if (subtotal<16)
	{
		subfilename[subtotal++] = filename;
		return 1;
	}
	return 0;
}

const char* get_csri_fmt_name(unsigned int fmt)
{
	switch(fmt)
	{
	case CSRI_F_RGBA:
		return "RGBA";
	case CSRI_F_ARGB:
		return "ARGB";
	case CSRI_F_BGRA:
		return "BGRA";
	case CSRI_F_ABGR:
		return "ABGR";
	case CSRI_F_RGB_:
	case CSRI_F__RGB:
	case CSRI_F_RGB:
		return "RGB24";
	case CSRI_F_BGR_:
	case CSRI_F__BGR:
	case CSRI_F_BGR:
		return "BGR24";
//	case CSRI_F_AYUV:
//		return "AYUV";
//	case CSRI_F_YUVA:
//		return "YUVA";
//	case CSRI_F_YVUA:
//		return "YVUA";
	case CSRI_F_YUY2:
		return "YUY2";
//	case CSRI_F_YV12A:
//		return "YV12A";
	case CSRI_F_YV12:
		return "YV12";
	case CSRI_F_NV12:
		return "NV12";
	case CSRI_F_NV21:
		return "NV21";
	}
	return "Unknown";
}

void* subtitle_new_renderer(const csri_fmt *fmt, uint32_t sarw, uint32_t sarh)
{
	int i;
	csri_openflag flag;
	void *subrenderinst;
	if (!subtotal)
		return 0;

	if (!hVSFilter)
	{
		if (NULL == (hVSFilter = LoadLibraryA("vsfilter.dll")))
		{
			x264_cli_log("subtitle", X264_LOG_ERROR, "failed to load vsfilter.dll\n");
			return 0;
		}
		csri_open_file = (csri_open_file_t)GetProcAddress(hVSFilter,"csri_open_file");
		csri_close = (csri_close_t)GetProcAddress(hVSFilter,"csri_close");
		csri_request_fmt = (csri_request_fmt_t)GetProcAddress(hVSFilter,"csri_request_fmt");
		csri_render = (csri_render_t)GetProcAddress(hVSFilter,"csri_render");
		csri_add_file = (csri_add_file_t)GetProcAddress(hVSFilter,"csri_add_file");
	}
	if (sarw != sarh) // non-square par
	{
		flag.name = "PAR";
		flag.data.dval = (double)sarw / sarh;
		flag.next = NULL;
	}
	else
		flag.name = NULL;
	
	if (NULL == (subrenderinst = csri_open_file((void*)"vsfilter", subfilename[0], flag.name?&flag:NULL)))
	{
		x264_cli_log("subtitle", X264_LOG_ERROR, "failed to create subtitle renderer\n");
		return 0;
	}
	if (csri_request_fmt(subrenderinst, fmt))
	{
		x264_cli_log("subtitle", X264_LOG_ERROR, "csri does not support %s input\n", get_csri_fmt_name(fmt->pixfmt));
		return 0;
	}
	x264_cli_log("subtitle", X264_LOG_INFO, "loaded subtitle \"%s\"\n", subfilename[0]);
	if (csri_add_file)
		for (i=1; i<subtotal; i++)
		{
			if (csri_add_file(subrenderinst, subfilename[i], flag.name?&flag:NULL))
				x264_cli_log("subtitle", X264_LOG_INFO, "loaded subtitle \"%s\"\n", subfilename[0]);
			else
				x264_cli_log("subtitle", X264_LOG_WARNING, "failed to load subtitle \"%s\"\n", subfilename[0]);
		}
	else
		x264_cli_log("subtitle", X264_LOG_WARNING, "no csri_add_file interface, only first subtitle loaded\n");
	return subrenderinst;
}

#define NAME "subtitle"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )

cli_vid_filter_t subtitle_filter;

typedef struct
{
	hnd_t prev_hnd;
	cli_vid_filter_t prev_filter;
	void *subrenderinst;
	int csp;
	unsigned int fmt;
	double scale_factor;
} subtitle_hnd_t;

static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string )
{
	subtitle_hnd_t *h;
	csri_fmt fmt;
	if (!(h = calloc(1, sizeof(subtitle_hnd_t))))
		return -1;
	fmt.width = info->width;
	fmt.height = info->height;
	h->csp = info->csp & X264_CSP_MASK;
	switch(h->csp)
	{
	case X264_CSP_I420:
	case X264_CSP_YV12:
		fmt.pixfmt = CSRI_F_YV12;
		break;
	case X264_CSP_NV12:
		fmt.pixfmt = CSRI_F_NV12;
		break;
	case X264_CSP_BGR:
		fmt.pixfmt = CSRI_F_BGR;
		break;
	case X264_CSP_BGRA:
		fmt.pixfmt = CSRI_F_BGRA;
		break;
	case X264_CSP_RGB:
		fmt.pixfmt = CSRI_F_RGB;
		break;
	default:
		x264_cli_log( NAME, X264_LOG_ERROR, "unsupported colorspace\n");
		fmt.pixfmt = -1;
	}
	if (fmt.pixfmt == -1 || !(h->subrenderinst = subtitle_new_renderer(&fmt, info->sar_height, info->sar_height)))
	{
		free(h);
		return -1;
	}
	h->fmt = fmt.pixfmt;
	h->scale_factor = (double)(info->timebase_num) / info->timebase_den;

	h->prev_filter = *filter;
	h->prev_hnd = *handle;
	*handle = h;
	*filter = subtitle_filter;
	return 0;
}

static int get_frame( hnd_t handle, cli_pic_t *output, int frame )
{
	subtitle_hnd_t *h = handle;
	csri_frame fr;
	if( h->prev_filter.get_frame( h->prev_hnd, output, frame ) )
		return -1;
	fr.planes[0] = output->img.plane[0];
	fr.strides[0] = output->img.stride[0];
	switch(h->csp)
	{
	case X264_CSP_I420:
		fr.planes[1] = output->img.plane[2];
		fr.planes[2] = output->img.plane[1];
		goto L_YV12;
	case X264_CSP_YV12:
		fr.planes[1] = output->img.plane[1];
		fr.planes[2] = output->img.plane[2];
		L_YV12:
		fr.strides[1] = fr.strides[2] = output->img.stride[1];
		break;
	case X264_CSP_NV12:
		fr.planes[1] = output->img.plane[1];
		fr.planes[2] = 0;
		fr.strides[1] = output->img.stride[1];
		break;
	}
	fr.pixfmt = h->fmt;
	subtitle_render_frame(h->subrenderinst, &fr, output->pts * h->scale_factor);
	return 0;
}

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
	subtitle_hnd_t *h = handle;
	/* NO filter should ever have a dependent release based on the plane pointers,
	 * so avoid unnecessary unshifting */
	return h->prev_filter.release_frame( h->prev_hnd, pic, frame );
}

static void free_filter( hnd_t handle )
{
	subtitle_hnd_t *h = handle;
	h->prev_filter.free( h->prev_hnd );
	free( h );
}

static void help( int longhelp )
{
	printf( "      "NAME":\n" );
	if( longhelp )
		printf( "           overlay subtitle using VSFilter\n" );
}

cli_vid_filter_t subtitle_filter = { NAME, help, init, get_frame, release_frame, free_filter, NULL };
