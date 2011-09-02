/*****************************************************************************
* deldup.c: x264's near duplicate frame decimation filter
*****************************************************************************
* Copyright (C) 2010 - 2011 Zhou Zongyi <zhouzy_wuxi@hotmail.com>
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

#include "x264cli.h"
#include "common/cpu.h"

#if HAVE_MMX
int __stdcall comp_luma(uint8_t *dstplane,uint8_t *srcplane, int w, int h, int thresh, int mb_thresh, int mb_max)
{
	int lmaxdiff = thresh/30;
	srcplane+=4;
	dstplane+=4;
#ifdef _MSC_VER
	__asm pxor mm6, mm6
#else
	__asm__ volatile("pxor %mm6, %mm6\n\t");
#endif
	for (h=((h-4)&(-4));h;h-=4,dstplane += (w*4),srcplane += (w*4))
	{
#ifdef _MSC_VER
		__asm
		{
			mov    ebx, w
			mov    esi, srcplane
			mov    edi, dstplane
			lea    edx, [ebx-12]
			neg    ebx
			shr    edx, 2
			mov    ecx, 4
			pxor   mm0, mm0
			pxor   mm1, mm1
			pxor   mm7, mm7
			align 16
		mb_loop:
			movq   mm2, [esi]
			movq   mm3, [edi]
			sub    esi, ebx
			sub    edi, ebx
			movq   mm4, mm2
			movq   mm5, mm3
			punpcklbw mm4,mm7
			punpcklbw mm5,mm7
			psadbw mm2, mm3
			psubw  mm4, mm5
			paddd  mm1, mm2
			paddw  mm0, mm4
			movq   mm2, [esi]
			movq   mm3, [edi]
			sub    esi, ebx
			sub    edi, ebx
			movq   mm4, mm2
			movq   mm5, mm3
			punpcklbw mm4,mm7
			punpcklbw mm5,mm7
			psadbw mm2, mm3
			psubw  mm4, mm5
			paddd  mm1, mm2
			paddw  mm0, mm4
			dec    ecx
			jnz    mb_loop
			movd   ecx, mm1
			movq   mm2, mm0
			punpckhdq mm0,mm7
			paddw  mm0, mm2
			movq   mm2, mm0
			pslld  mm0, 16
			paddw  mm0, mm2
			psrad  mm0, 16
			paddd  mm6, mm0
			cmp    ecx, mb_thresh
			adc    mb_max, -1
			lea    esi, [esi+ebx*8+4]
			lea    edi, [edi+ebx*8+4]
			jl     exit_luma
			sub    thresh, ecx
			pxor   mm1, mm1
			jl     exit_luma
			pxor   mm0, mm0
			dec    edx
			mov    ecx, 4
			jnz    mb_loop
			jmp    end_stride
		exit_luma:
			mov    thresh, -1
		end_stride:
		}
#else
        int i,j;
		__asm__ volatile
		(
			// start pre_loop
			"movl %2, %%esi \n\t"
			"movl %3, %%edi \n\t"
			"subl %%ebx, %%edx \n\t"
			"shrl $2, %%edx \n\t"
			"pxor %%mm0, %%mm0 \n\t"
			"pxor %%mm1, %%mm1 \n\t"
			"pxor %%mm7, %%mm7 \n\t"
			// start stride
			// start mb_loop
			".align (1<<4) \n\t"
			"3: \n\t"
			"movq (%%esi), %%mm2 \n\t"
			"movq (%%edi), %%mm3 \n\t"
			"subl %%ebx, %%esi \n\t" // stride
			"subl %%ebx, %%edi \n\t" // stride
			"movq %%mm2, %%mm4 \n\t"
			"movq %%mm3, %%mm5 \n\t"
			"punpcklbw %%mm7,%%mm4 \n\t"
			"punpcklbw %%mm7,%%mm5 \n\t"
			"psadbw %%mm3, %%mm2 \n\t"
			"psubw  %%mm5, %%mm4 \n\t"
			"paddd  %%mm2, %%mm1 \n\t"
			"paddw  %%mm4, %%mm0 \n\t"
			"movq (%%esi), %%mm2 \n\t"
			"movq (%%edi), %%mm3 \n\t"
			"subl %%ebx, %%esi \n\t" // stride
			"subl %%ebx, %%edi \n\t" // stride
			"movq %%mm2, %%mm4 \n\t"
			"movq %%mm3, %%mm5 \n\t"
			"punpcklbw %%mm7,%%mm4 \n\t"
			"punpcklbw %%mm7,%%mm5 \n\t"
			"psadbw %%mm3, %%mm2 \n\t"
			"psubw  %%mm5, %%mm4 \n\t"
			"paddd  %%mm2, %%mm1 \n\t"
			"paddw  %%mm4, %%mm0 \n\t"
			"decl %%ecx \n\t"
			"jnz 3b \n\t"
			// end mb_loop
			"movd %%mm1, %%ecx \n\t" // ecx = mm1 = mb_sad
			"movq %%mm0, %%mm2 \n\t"
			"punpckhdq %%mm7,%%mm0 \n\t"
			"paddw %%mm2, %%mm0 \n\t"
			"movq %%mm0, %%mm2 \n\t"
			"pslld $16, %%mm0 \n\t"
			"paddw %%mm2, %%mm0 \n\t"
			"psrad $16, %%mm0 \n\t"
			"paddd %%mm0, %%mm6 \n\t"
			"cmpl %4, %%ecx \n\t" //
			"adc $-1, %5 \n\t" // if (mb_tresh < mb_sad)mb_count --
			"leal 4(%%esi,%%ebx,8), %%esi \n\t" // next mb
			"leal 4(%%edi,%%ebx,8), %%edi \n\t" // next mb
			"jl 5f \n\t"
			"subl %%ecx, %6 \n\t" // thresh -= mb_sad
			"pxor %%mm1, %%mm1 \n\t"
			"jl 5f \n\t" // thesh < 0 -> break
			"pxor %%mm0, %%mm0 \n\t"
			"decl %%edx \n\t" // edx--
			"movl $4, %%ecx \n\t" // ecx = 4
			"jnz 3b \n\t" // edx != 0 -> loop
			"jmp 6f \n\t"
			// end stride
			"5: \n\t"
			"movl $-1, %6 \n\t"  // if (mb_c > mb_max || thresh < 0) thresh=-1;
			"6: \n\t"
			: "=d"(i),"=c"(j)
			: "m"(srcplane), "m" (dstplane), "m"(mb_thresh), "m" (mb_max),"m"(thresh), "b"(-w), "d"(-12), "c"(4)
			: "memory", "%edi", "%esi"
		);
#endif
		if (thresh < 0)
		{
			x264_emms();
			return 0;
		}
	}
#ifdef _MSC_VER
	__asm
	{
		movd ecx, mm6
		mov eax, thresh
		mov edx, lmaxdiff
		xor edi, edi
		cmp ecx, edx
		cmovg eax, edi
		neg ecx
		cmp ecx, edx
		cmovg eax, edi
		emms
	}
#else
	__asm__ volatile
	(
		"movd  %%mm6, %%ecx \n\t"
		"cmpl  %%edx, %%ecx \n\t"
		"cmovg %%edi, %%eax \n\t"
		"negl  %%ecx \n\t"
		"cmpl  %%edx, %%ecx \n\t"
		"cmovg %%edi, %%eax \n\t"
		"emms \n\t"
		:"+a"(thresh)
		:"d"(lmaxdiff), "D"(0)
		:"memory", "%ecx"
	);
	return thresh;
#endif
}

int __stdcall comp_chroma(uint8_t *dstplane, uint8_t *srcplane, int csize, int thresh)
{
#ifdef _MSC_VER
	__asm
	{
		mov     ecx, srcplane
		mov     edi, dstplane
		lea     esi, [ecx+7]
		and     esi, -8
		sub     ecx, esi
		sub     edi, ecx
		add     ecx, csize
		movd    mm0, thresh
		shr     ecx, 5
		align 16
	chroma_loop:
		movq    mm1, [esi]
		movq    mm3, [esi+8]
		psadbw  mm1, [edi]
		psadbw  mm3, [edi+8]
		psubd   mm0, mm1
		psubd   mm0, mm3
		movq    mm1, [esi+16]
		movq    mm3, [esi+24]
		psadbw  mm1, [edi+16]
		psadbw  mm3, [edi+24]
		psubd   mm0, mm1
		psubd   mm0, mm3
		add     edi, 32
		movd    edx, mm0
		add     esi, 32
		cmp     edx, 0
		jge     next_chroma
		mov     ecx, 1
		mov     thresh, 0
	next_chroma:
		dec     ecx
		jnz     chroma_loop
		emms
	}
#else
	__asm__ volatile
	(
		"leal 7(%%ecx), %%esi \n\t"
		"andl $-8, %%esi \n\t"
		"subl %%esi, %%ecx \n\t"
		"subl %%ecx, %%edi \n\t"
		"addl %2, %%ecx \n\t"
		"movd %3, %%mm0 \n\t"
		"shrl $5, %%ecx \n\t"
		".align (1<<4) \n\t"
		"1: \n\t"
		"movq (%%esi), %%mm1 \n\t"
		"movq 8(%%esi), %%mm3 \n\t"
		"psadbw (%%edi), %%mm1 \n\t"
		"psadbw 8(%%edi), %%mm3 \n\t"
		"psubd %%mm1, %%mm0 \n\t"
		"psubd %%mm3, %%mm0 \n\t"
		"movq 16(%%esi), %%mm1 \n\t"
		"movq 24(%%esi), %%mm3 \n\t"
		"psadbw 16(%%edi), %%mm1 \n\t"
		"psadbw 24(%%edi), %%mm3 \n\t"
		"psubd %%mm1, %%mm0 \n\t"
		"psubd %%mm3, %%mm0 \n\t"
		"addl $32, %%edi \n\t"
		"movd %%mm0, %%edx \n\t"
		"addl $32, %%esi \n\t"
		"cmpl $0, %%edx \n\t"
		"jge 2f \n\t"
		"movl $1, %%ecx \n\t"
		"movl $0, %3 \n\t"
		"2: \n\t"
		"decl %%ecx \n\t"
		"jnz 1b \n\t"
		"emms \n\t"
		:
		: "c"(srcplane), "D" (dstplane), "m" (csize), "m"(thresh)
		: "memory", "%edx", "%esi"
	);
#endif
	return thresh;
}

#else
int __stdcall comp_luma(uint8_t *dstplane, uint8_t *srcplane, int w, int h, int thresh, int mb_thresh, int mb_max)
{
	uint8_t *psrc = srcplane, *pdst = dstplane;
	int i,j,k,l,mb_sad,diff=0,lmaxdiff=thresh/30;
	for (i=0,h=((h-4)&(-4));h;h-=4)
	{
		for (j=4;j<w-8;j+=4)
		{
			mb_sad = 0;
			for (k=0;k<8;k++)
			{
				for (l=0;l<4;l++)
				{
					diff += (int)psrc[(i*4+k)*w+j+l] - (int)pdst[(i*4+k)*w+j+l];
					mb_sad += abs((int)psrc[(i*4+k)*w+j+l] - (int)pdst[(i*4+k)*w+j+l]);
				}
				for (;l<8;l++)
					mb_sad += abs((int)psrc[(i*4+k)*w+j+l] - (int)pdst[(i*4+k)*w+j+l]);
			}
			if (mb_sad > mb_thresh)
			{
				if (--mb_max<0)
					return 0;
			}
			if ((thresh -= mb_sad) < 0)
				return 0;
		}
		++i;
	}
	return abs(diff) > lmaxdiff ? 0:thresh;
}

int __stdcall comp_chroma(uint8_t *dstplane, uint8_t *srcplane, int size, int thresh)
{
	uint8_t *psrc = srcplane, *pdst = dstplane;
	int i=size;
	for (;i;i --)
		if ((thresh -= abs((int)*psrc++ - (int)*pdst++)) < 0)
			return 0;
	return thresh;
}
#endif