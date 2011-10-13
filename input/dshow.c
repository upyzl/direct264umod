/*****************************************************************************
 * dshow.c: x264 DirectShow input module
 *****************************************************************************
 * Copyright (C) 2008-2011 Zhou Zongyi <zhouzy_wuxi@hotmail.com>
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

#include "input.h"
#include "common/cpu.h"
#include "filters/video/subtitle.h"

typedef struct
{
	uint32_t width : 12;
	uint32_t height : 12;
	uint32_t aspectX : 8;
	uint32_t aspectY : 8;
	uint32_t avgtimeperframe : 20;
	uint32_t reserved : 4;
} dshow_video_info_t;

typedef int (__stdcall *GrabSampleCallbackRoutine)
(PBYTE pData, int32_t iLen, int64_t i64TimeStamp);

typedef void* (__stdcall *TInitDShowGraphFromFile)
(const char *szFileName,
 GUID MediaType,
 uint32_t dwVideoID,
 uint32_t dwAudioID,
 GrabSampleCallbackRoutine pVideoCallback,
 GrabSampleCallbackRoutine pAudioCallback,
 dshow_video_info_t *pVideoInfo,
 uint32_t *pdwAudioInfo);

typedef int (__stdcall *TGraphOperate)(void *pdgi);
typedef int (__stdcall *TSeekGraph)(void *pdgi, int64_t timestamp);

const GUID MEDIASUBTYPE_YV12 =
{0x32315659, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

const GUID MEDIASUBTYPE_NV12 =
{0x3231564E, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

TGraphOperate StartGraph, StopGraph, DestroyGraph, GetGraphDuration;
TSeekGraph SeekGraph;

HANDLE hWaitEnc = 0, hWaitGraph = 0;
int b_stopped, b_eof, dshow_buf_mode = 0;
static cli_pic_t *g_ppic;
static int g_width, g_height, ds_width, ds_height, b_hardcrop = 0;
int lthresh, cthresh, maxinterval, ivtc;
int64_t startdelay = 0;
int deldup = 0, mbthresh = 400, smoothpts = 0, mbmax=-1;
double lth = 0.8, cth = 1.6, minfps = 12.0;
int64_t starttime = 0, endtime = -1;
uint32_t avgtimeperframe = 0;
int cropleft = 0, croptop = 0, cropright = 0, cropbottom = 0;
void x264_memcpy_aligned_mmx( void * dst, const void * src, size_t n ); // require mod32
void x264_plane_copy_mmxext( uint8_t *dst, int i_dst, uint8_t *src, int i_src, int w, int h); // require mod4
void* subrenderinst = 0;
void *g_pdgi = 0;
#define IMGTYPE_YUV420P 0 // YV12/I420
#define IMGTYPE_NV12 7 // NV12/NV21
void * ( __stdcall *open_phase_filter)(DWORD w, DWORD h, DWORD stride, int type, int mode, int skip);
int (__stdcall *close_phase_filter)(void *ppfi);
int (__stdcall *get_image)(void *ppfi, char *pimg, LONGLONG pts);
int (__stdcall *get_dscaler_retval_addr)();
static int *dscaler_indicies;
static int curr_ds_index;
static void *g_ppfi = 0;
cli_pic_t g_threadpicbuf, g_holdpicbuf;

extern x264_param_t *g_pparam;
double ts_scalefactor;
static int min_internal;
int b_nv12 = 1, b_sub = 1;

int inline do_deldup(cli_image_t *p1, cli_image_t *p2, int boost)
{
	uint32_t offset = g_width * g_height / 4;
	if (lthresh >= 0 && !comp_luma(p1->plane[0],p2->plane[0],g_width,g_height,lthresh<<boost,mbthresh,mbmax<<boost))
		return 1;
	if (cthresh >= 0)
	{
		if (p1->csp == X264_CSP_NV12)
			return !comp_chroma(p1->plane[1],p2->plane[1],offset<<1,cthresh<<(boost+1));
		else
			return !comp_chroma(p1->plane[1],p2->plane[1],offset,cthresh<<boost) || !comp_chroma(p1->plane[2],p2->plane[2],offset,cthresh<<boost);
	}
	return 0;
}

void inline hard_crop(PBYTE pData, int w, int h, int left, int right, int top, int bottom, int b_nv12)
{
	int dh = h - top - bottom;
	int dw = w - left - right;
	PBYTE pSrc = pData + left + top * w;
	PBYTE pDst = pData;

	if (dw != w || top)
		for (;dh;dh--)
		{
			memmove(pDst, pSrc, dw);
			pDst += dw;
			pSrc += w;
		}
	else
		pDst += w * dh;

	dh = (h - top - bottom) >> 1;
	pSrc = pData + w * h;

	if (!b_nv12)
	{
		w >>= 1;
		dw >>= 1;
		left >>= 1;
	}
	
	pSrc += left + (top >> 1) * w;
	for (;dh;dh--)
	{
		memmove(pDst, pSrc, dw);
		pDst += dw;
		pSrc += w;
	}
	if (!b_nv12)
	{
		pSrc += ((top + bottom) >> 1) * w;
		for (dh = (h - top - bottom) >> 1;dh;dh--)
		{
			memmove(pDst, pSrc, dw);
			pDst += dw;
			pSrc += w;
		}
	}
}

static int __stdcall CallBackProc(PBYTE pData, int32_t iLen, int64_t i64TimeStamp)
{
	static int64_t i64LastPTS = -1;
	static int64_t i64LastUnmodPTS = -1;
	static int b_firstframe = 1;
	static int drops = 0;
	static int ivtc_adjust = 0;
	static int lastinterval = 0;
	static int ended = 0;
	static int phaseret = 0;
	int pret = 0;
	uint32_t offset = g_width * g_height / 4;
	int tmpinterval;

	if (ivtc < 0)
	{
		if (!get_dscaler_retval_addr)
		{
			HANDLE hPhase = GetModuleHandleA("FLT_Phase.dll");
			if (NULL == hPhase)
			{
				x264_cli_log("dshow", X264_LOG_WARNING, "FLT_Phase.dll is not loaded, IVTC disabled\n");
				ivtc = 0;
			}
			else
			{
				get_dscaler_retval_addr = GetProcAddress(hPhase,"get_dscaler_retval_addr");
				if (0 == (dscaler_indicies = get_dscaler_retval_addr()))
				{
					x264_cli_log("dshow", X264_LOG_WARNING, "Phase filter is not loaded by DScaler, IVTC disabled\n");
					ivtc = 0;
				}
			}
			curr_ds_index = -1;
		}
		curr_ds_index = (curr_ds_index+1)&15;
	}

	if (avgtimeperframe && i64LastUnmodPTS != -1)
		i64TimeStamp = i64LastPTS + avgtimeperframe; // Hack for GSV
	if (endtime > 0 && i64TimeStamp > endtime)
	{
		if (ended) return 0;
		pData = NULL;
		ended = 1;
	}
	else if (pData)
	{
		if (ivtc > 0)
		{
			pret = get_image(g_ppfi,pData,i64TimeStamp+starttime);
		}
		else if (ivtc < 0)
		{
			pret = dscaler_indicies[curr_ds_index];
		}
		if (phaseret > 0 || drops > 0)
			pret &= 7;

		if (b_hardcrop)
			hard_crop(pData, ds_width, ds_height, cropleft, cropright, croptop, cropbottom, b_nv12);

		if (subrenderinst)
		{
			csri_frame frame;
			frame.planes[0] = pData;
			frame.planes[1] = pData + offset * 4;
			if (b_nv12)
			{
				frame.pixfmt = CSRI_F_NV12;
				frame.planes[2] = NULL;
				frame.strides[1] = g_width;
			}
			else
			{
				frame.pixfmt = CSRI_F_YV12;
				frame.planes[2] = pData + offset * 5;
				frame.strides[1] = frame.strides[2] = (g_width >> 1);
			}
			frame.planes[3] = NULL;
			frame.strides[0] = g_width;
			subtitle_render_frame(subrenderinst, &frame, (i64TimeStamp+starttime)*1E-7);
		}
	}

	i64TimeStamp -= startdelay;

	if (b_firstframe)
	{
		if (starttime > 0)
		{
			int64_t t = i64TimeStamp + starttime;
			x264_cli_log("dshow", X264_LOG_INFO, "start encoding from %u.%05u sec\n",(int)(t/10000000),(int)(t%10000000)/100);
		}
		startdelay = i64TimeStamp;
		i64TimeStamp = 0;
		if (dshow_buf_mode)
		{
			x264_cli_pic_alloc(&g_threadpicbuf, b_nv12? X264_CSP_NV12 : X264_CSP_YV12, g_width, g_height);
			g_ppic = &g_threadpicbuf;
		}
		else
			WaitForSingleObject(hWaitEnc,INFINITE);
		if (pData)
		{
			i64LastUnmodPTS = i64LastPTS = i64TimeStamp;
			g_ppic->pts = llrint(i64TimeStamp * ts_scalefactor);
			b_firstframe = 0;
			if (!dshow_buf_mode)
			{
				x264_free(g_ppic->img.plane[0]);
				x264_free(g_ppic->img.plane[1]);
				g_ppic->img.plane[0] = pData;
				g_ppic->img.plane[1] = pData + offset * 4;
				if (!b_nv12)
				{
					x264_free(g_ppic->img.plane[2]);
					g_ppic->img.plane[2] = pData + offset * 5;
				}
				SetEvent(hWaitGraph);
				WaitForSingleObject(hWaitEnc,INFINITE);
			}
			else
			{
				if (offset & 7)
					memcpy(g_ppic->img.plane[0], pData, offset * 4);
				else
				{
					x264_memcpy_aligned_mmx(g_ppic->img.plane[0], pData, offset * 4);
					x264_emms();
				}
				memcpy(g_ppic->img.plane[1], pData + offset * 4, b_nv12? offset << 1 : offset);
				if (!b_nv12)
					memcpy(g_ppic->img.plane[2], pData + offset * 5, offset);
				//if (2 == dshow_buf_mode)
				//	XCHG( cli_image_t, g_ppic->img, g_picbuf.img );
			}
			phaseret = pret;
		}
		else
		{
			g_ppic = NULL;
			SetEvent(hWaitGraph);
		}
		return 1;
	}
	else
	{
		if (pData)
		{
			int threshboost = 0;
			cli_image_t pimg;
			if (smoothpts)
			{
				if (lastinterval && lastinterval <= maxinterval)
				{
					tmpinterval = (int)(i64TimeStamp - i64LastUnmodPTS);
					i64LastUnmodPTS = i64TimeStamp;
					i64TimeStamp = i64LastPTS + (lastinterval + tmpinterval) / 2;
					lastinterval = tmpinterval;
				}
				else
				{
					lastinterval = (int)(i64TimeStamp - i64LastUnmodPTS);
					i64LastUnmodPTS = i64TimeStamp;
				}
			}
			if (b_stopped)
			{
				SetEvent(hWaitGraph);
				return 0;
			}
			if (ivtc > 1 || ivtc < 0)
			{
				threshboost = (pret&7) > phaseret? 4:0;
			}
			//if (!(dshow_buf_mode&2))
			{
				pimg.plane[0] = pData;
				pimg.plane[1] = pData + offset * 4;
				if (!b_nv12)
					pimg.plane[2] = pData + offset * 5;
			}
			if ( !deldup || (int)(i64TimeStamp - i64LastPTS) > maxinterval || do_deldup(&g_ppic->img, &pimg, threshboost) )
			{
				int drop_prev_frame = 0;
				// no drop
				if ((ivtc > 1 || ivtc < 0) && (int)(i64TimeStamp - i64LastPTS) < maxinterval*2)
				{
					if (pret & 8)
					{
						drops = 1; // drop bad frame
						drop_prev_frame = 1;
					}
					if (1 == drops)
					{
						ivtc_adjust = 1;
						i64TimeStamp -= (i64TimeStamp - i64LastPTS) * 3 / 8;
					}
					else if(1 == ivtc_adjust)
					{
						ivtc_adjust++;
						i64TimeStamp -= (i64TimeStamp - i64LastPTS) * 2 / 7;
					}
					else if (2 == ivtc_adjust)
					{
						ivtc_adjust = 0;
						i64TimeStamp -= (i64TimeStamp - i64LastPTS) / 6;
					}
				}
				drops = 0;
				if (dshow_buf_mode)
				{
					if (!drop_prev_frame)
					{
						SetEvent(hWaitGraph);
						WaitForSingleObject(hWaitEnc,INFINITE);
						if (b_stopped)
						{
							g_ppic = NULL;
							return 0;
						}
					}
					phaseret = pret&7;
					//if (dshow_buf_mode&2)
					//	XCHG( cli_image_t, g_ppic->img, g_picbuf.img );
					//else
					{
						if (offset & 7)
							memcpy(g_ppic->img.plane[0], pData, offset * 4);
						else
						{
							x264_memcpy_aligned_mmx(g_ppic->img.plane[0], pData, offset * 4);
							x264_emms();
						}
						memcpy(g_ppic->img.plane[1], pData + offset * 4, b_nv12? offset << 1 : offset);
						if (!b_nv12)
							memcpy(g_ppic->img.plane[2], pData + offset * 5, offset);
					}
					if (i64TimeStamp <= i64LastPTS)
						i64TimeStamp = i64LastPTS + min_internal;
					g_ppic->pts = llrint(i64TimeStamp * ts_scalefactor);
				}
				else
				{
					g_ppic->img.plane[0] = pData;
					g_ppic->img.plane[1] = pData + offset * 4;
					if (!b_nv12)
						g_ppic->img.plane[2] = pData + offset * 5;
					x264_emms();
					if (i64TimeStamp <= i64LastPTS)
						i64TimeStamp = i64LastPTS + min_internal;
					g_ppic->pts = llrint(i64TimeStamp * ts_scalefactor);
					//if (!drop_prev_frame)
					{
						SetEvent(hWaitGraph);
						WaitForSingleObject(hWaitEnc,INFINITE);
					}
				}
				i64LastPTS = i64TimeStamp;
			}
			else
			{
				drops++;
				ivtc_adjust = 0;
				lastinterval = 0;
			}
		}
		else // end null frame
		{
			if (dshow_buf_mode)
			{
				SetEvent(hWaitGraph);
				WaitForSingleObject(hWaitEnc,INFINITE);
			}
			g_ppic = NULL;
			SetEvent(hWaitGraph);
		}
		return 1;
	}
}

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
	dshow_video_info_t VideoInfo;
	TInitDShowGraphFromFile InitDShowGraphFromFile=NULL;
	HMODULE hDump = 0, hPhase = 0;
	csri_fmt fmt;
	int32_t duration;
	int timescale;

	if (NULL == (hDump = LoadLibraryA("dump.ax")))
	{
		x264_cli_log("dshow", X264_LOG_ERROR, "failed to load dump.ax\n");
		return -1;
	}
	InitDShowGraphFromFile = (TInitDShowGraphFromFile)GetProcAddress(hDump,"InitDShowGraphFromFile");
	StartGraph = (TGraphOperate)GetProcAddress(hDump,"StartGraph");
	StopGraph = (TGraphOperate)GetProcAddress(hDump,"StopGraph");
	DestroyGraph = (TGraphOperate)GetProcAddress(hDump,"DestroyGraph");
	SeekGraph = (TSeekGraph)GetProcAddress(hDump,"SeekGraph");
	GetGraphDuration = (TGraphOperate)GetProcAddress(hDump,"GetGraphDuration");

	if (!(g_pdgi = InitDShowGraphFromFile(psz_filename,b_nv12?MEDIASUBTYPE_NV12:MEDIASUBTYPE_YV12,0,-1,CallBackProc,NULL,&VideoInfo,0)))
	{
		if (VideoInfo.reserved != 7)
		{
			x264_cli_log("dshow", X264_LOG_ERROR, "failed to render %s\nerror code = %u\n",psz_filename,VideoInfo.reserved);
			return -1;
		}
		else
		{
			b_nv12 = !b_nv12;
			if (!(g_pdgi = InitDShowGraphFromFile(psz_filename,b_nv12?MEDIASUBTYPE_NV12:MEDIASUBTYPE_YV12,0,-1,CallBackProc,NULL,&VideoInfo,0)))
			{
				x264_cli_log("dshow", X264_LOG_ERROR, "failed to render %s\nerror code = %u\n",psz_filename,VideoInfo.reserved);
				return -1;
			}
		}
	}

	ds_width = VideoInfo.width;
	ds_height = VideoInfo.height;

	b_hardcrop = cropleft || cropright || croptop || cropbottom;
	g_width = info->width = ds_width - cropleft - cropright;
	g_height = info->height = ds_height - croptop - cropbottom;
	if (g_width < 2 || g_height < 2)
	{
		x264_cli_log("dshow", X264_LOG_ERROR, "invalid parameter for hard-crop\n");
		return -1;
	}
	if (b_hardcrop)
		x264_cli_log("dshow", X264_LOG_INFO, "hard-crop enabled, %ux%u => %ux%u\n", ds_width, ds_height, g_width, g_height);

	if (!VideoInfo.avgtimeperframe) // Hack for GSV
		avgtimeperframe = VideoInfo.avgtimeperframe = (uint64_t)info->fps_den * 10000000 / info->fps_num;
	if (VideoInfo.avgtimeperframe < 10000)
		VideoInfo.avgtimeperframe = 400000;

	switch (10000000 /  VideoInfo.avgtimeperframe)
	{
	case 11:case 23:case 47:
		timescale = 48000;
		break;
	case 12:case 24:case 48:case 15:case 30:case 60:case 120:
		timescale = 12000;
		break;
	case 14:case 29:case 59:case 119:
		if (ivtc > 1)
		{
			timescale = 48000;
			VideoInfo.avgtimeperframe = (VideoInfo.avgtimeperframe * 5 + 2) / 4;
		}
		else
			timescale = 60000; // PSP requires timescale <= 60000
		break;
	default:
		timescale = 60000;
	}

	if (1 == info->fps_den && 25 == info->fps_num)
	{
		info->fps_num = timescale;
		info->fps_den = ((int64_t)timescale * VideoInfo.avgtimeperframe + 5000000) / 10000000;
	}
	info->timebase_den = timescale;
	info->timebase_num = 1;
	info->vfr = 1;
	ts_scalefactor = timescale * 1E-7;
	min_internal = lrint(1. / ts_scalefactor);

	if (0 == info->sar_width && 0 == info->sar_height
		&& (VideoInfo.aspectX > 1 || VideoInfo.aspectY > 1 ))
	{
		info->sar_width = VideoInfo.aspectX;
		info->sar_height = VideoInfo.aspectY;
	}

	if (b_sub)
	{
		fmt.pixfmt = b_nv12? CSRI_F_NV12 : CSRI_F_YV12;
		fmt.width = g_width;
		fmt.height = g_height;
		subrenderinst = subtitle_new_renderer(&fmt, info->sar_width, info->sar_height);
	}

	dshow_buf_mode = 0;
	x264_emms();
	if ((ivtc>1 || ivtc<0) && !deldup)
	{
		deldup = 1;
		minfps = 24;
		lth = 1.2;
		cth = 2.4;
	}

	if (deldup)
	{
		if (lth < 0.0)
			lthresh = -1;
		else
			lthresh = lrint(g_width * g_height * lth);
		if (cth < 0.0)
			cthresh = -1;
		else
			cthresh = lrint(g_width * g_height * cth);
		if (mbmax < 0)
			mbmax = g_width / 10;
		dshow_buf_mode = 1;
	}
	// always set maxinterval
	maxinterval = lrint(10000000.0 / minfps);

	if (ivtc > 0)
	{
		if (NULL == (hPhase = LoadLibraryA("FLT_Phase.dll")))
		{
			x264_cli_log("dshow", X264_LOG_ERROR, "failed to load FLT_Phase.dll\n");
			return -1;
		}
		open_phase_filter = GetProcAddress(hPhase,"open_phase_filter");
		get_image = GetProcAddress(hPhase,"get_image");
		close_phase_filter = GetProcAddress(hPhase,"close_phase_filter");

		g_ppfi = open_phase_filter(ds_width,ds_height,ds_width,b_nv12? IMGTYPE_NV12 : IMGTYPE_YUV420P,ivtc-1,ivtc>1?maxinterval:0x7FFFFFFF);
	}

	info->csp = b_nv12? X264_CSP_NV12 : X264_CSP_YV12;
	info->num_frames = 0;
	info->thread_safe = 1;

	if(-1 != (duration = GetGraphDuration(g_pdgi)))
		x264_cli_log("dshow", X264_LOG_INFO, "duration %02u:%02u:%02u\n",duration / 3600, duration / 60 % 60, duration % 60);

	*p_handle = g_pdgi;

	hWaitEnc = CreateEventA(NULL, FALSE, FALSE, NULL);
	hWaitGraph = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (0 != starttime)
	{
		SeekGraph(g_pdgi,starttime);
		endtime -= starttime;
	}
	b_stopped = b_eof = 0;
	StartGraph(g_pdgi);
	return 0;
}

static int read_frame( cli_pic_t *p_pic, hnd_t handle, int i_frame )
{
	if (b_eof)
		return -1;
	if (dshow_buf_mode)
	{
		WaitForSingleObject(hWaitGraph,INFINITE);
		if (g_ppic)
		{
			if (!g_holdpicbuf.img.planes)
			{
				x264_cli_pic_alloc(&g_holdpicbuf, b_nv12? X264_CSP_NV12 : X264_CSP_YV12, g_width, g_height);
				XCHG( cli_image_t, g_holdpicbuf.img, g_ppic->img );
				g_holdpicbuf.pts = g_ppic->pts;
				SetEvent(hWaitEnc);
				WaitForSingleObject(hWaitGraph,INFINITE);
			}
			if (g_ppic)
			{
				XCHG( cli_image_t, p_pic->img, g_holdpicbuf.img );
				p_pic->pts = g_holdpicbuf.pts;
				p_pic->duration = g_holdpicbuf.duration = g_ppic->pts - g_holdpicbuf.pts;
				XCHG( cli_image_t, g_holdpicbuf.img, g_ppic->img );
				g_holdpicbuf.pts = g_ppic->pts;
				SetEvent(hWaitEnc);
				return 0;
			}
		}
	}
	else
	{
		g_ppic = p_pic;
		SetEvent(hWaitEnc);
		WaitForSingleObject(hWaitGraph,INFINITE);
		p_pic->duration = 0;
		if (g_ppic)
			return 0;
	}
	b_eof = 1;
	if (g_holdpicbuf.img.planes)
	{
		XCHG( cli_image_t, p_pic->img, g_holdpicbuf.img );
		p_pic->pts = g_holdpicbuf.pts;
		p_pic->duration = g_holdpicbuf.duration;
		x264_cli_pic_clean(&g_holdpicbuf);
		return 0;
	}
	return -1;
}

static void pic_clean( cli_pic_t *pic )
{
	if (dshow_buf_mode)
		x264_cli_pic_clean(pic);
}

static int close_file( hnd_t handle )
{
	b_stopped = 1;
	SetEvent(hWaitEnc);
	StopGraph(g_pdgi);
	DestroyGraph(g_pdgi);
	SetEvent(hWaitGraph);
	if (subrenderinst)
		subtitle_close(subrenderinst);
	if (ivtc > 0)
		close_phase_filter(g_ppfi);
	if (dshow_buf_mode)

		x264_cli_pic_clean(&g_threadpicbuf);
	CloseHandle(hWaitEnc);
	CloseHandle(hWaitGraph);
	return 0;
}

const cli_input_t dshow_input = { open_file, x264_cli_pic_alloc, read_frame, NULL, pic_clean, close_file };
