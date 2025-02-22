/****************************************************************************
 *
 *    The MIT License (MIT)
 *
 *    Copyright (c) 2014 - 2023 Vivante Corporation
 *
 *    Permission is hereby granted, free of charge, to any person obtaining a
 *    copy of this software and associated documentation files (the "Software"),
 *    to deal in the Software without restriction, including without limitation
 *    the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *    and/or sell copies of the Software, and to permit persons to whom the
 *    Software is furnished to do so, subject to the following conditions:
 *
 *    The above copyright notice and this permission notice shall be included in
 *    all copies or substantial portions of the Software.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *    DEALINGS IN THE SOFTWARE.
 *
 *****************************************************************************
 *
 *    The GPL License (GPL)
 *
 *    Copyright (C) 2014 - 2023 Vivante Corporation
 *
 *    This program is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU General Public License
 *    as published by the Free Software Foundation; either version 2
 *    of the License, or (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software Foundation,
 *    Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *****************************************************************************
 *
 *    Note: This software is released under dual MIT and GPL licenses. A
 *    recipient may use this file under the terms of either the MIT license or
 *    GPL License. If you wish to use only one license not the other, you can
 *    indicate your decision by deleting one of the above license notices in your
 *    version of this file.
 *
 *****************************************************************************/

#include "gc_hal.h"
#include "gc_hal_kernel.h"
#include "gc_hal_kernel_context.h"

#define _GC_OBJ_ZONE gcvZONE_HARDWARE

typedef struct _gcsMCFE_DESCRIPTOR {
	gctUINT32 start;
	gctUINT32 end;
} gcsMCFE_DESCRIPTOR;

/* 2^DepthExp = Depth. */
#define MCFE_RINGBUF_DEPTH_EXP 9
/* Depth. */
#define MCFE_RINGBUF_DEPTH (1 << MCFE_RINGBUF_DEPTH_EXP)
/* MCFE descriptor size in bytes, fixed 8. */
#define MCFE_COMMAND_DESC_SIZE 8
/* FIFO size in bytes. */
#define MCFE_RINGBUF_SIZE (MCFE_RINGBUF_DEPTH * MCFE_COMMAND_DESC_SIZE)

typedef struct _gcsMCFE_RING_BUF {
	gckVIDMEM_NODE ringBufVideoMem;
	gctADDRESS ringBufAddress;
	gctUINT32 *ringBufLogical;
	gctSIZE_T ringBufBytes;

	gctADDRESS gpuAddress;
	gctPHYS_ADDR_T physical;

	/* Read ptr should be often out-of-date. */
	gctUINT32 readPtr;
	gctUINT32 writePtr;
} gcsMCFE_RING_BUF;

typedef struct _gcsMCFE_CHANNEL {
	gceMCFE_CHANNEL_TYPE binding;
	gcsMCFE_RING_BUF stdRingBuf;
	gcsMCFE_RING_BUF priRingBuf;
} gcsMCFE_CHANNEL;

struct _gckMCFE {
	gctUINT32 channelCount;
	gctBOOL mmuEnabled;

	/*
	 * Channels must be the last field.
	 * Will allocate struct size according to channel count.
	 */
	gcsMCFE_CHANNEL channels[1];
};

static gcmINLINE gctUINT32 _NextPtr(gctUINT32 Ptr)
{
	return (Ptr + 1) & (MCFE_RINGBUF_DEPTH - 1);
}

static gceSTATUS _AllocateDescRingBuf(gckHARDWARE Hardware, gcsMCFE_RING_BUF *Channel)
{
	gceSTATUS status;
	gcePOOL pool	    = gcvPOOL_DEFAULT;
	gckKERNEL kernel    = Hardware->kernel;
	gctUINT32 allocFlag = 0;

#if gcdENABLE_CACHEABLE_COMMAND_BUFFER
	allocFlag |= gcvALLOC_FLAG_CACHEABLE;
#endif

	Channel->ringBufBytes = MCFE_RINGBUF_SIZE;

	/* Allocate video memory node for mcfe ring buffer. */
	gcmkONERROR(gckKERNEL_AllocateVideoMemory(kernel, 64, gcvVIDMEM_TYPE_COMMAND, allocFlag,
						  &Channel->ringBufBytes, &pool,
						  &Channel->ringBufVideoMem));

	/* Lock for GPU access. */
	gcmkONERROR(gckVIDMEM_NODE_Lock(kernel, Channel->ringBufVideoMem, &Channel->gpuAddress));

	/* Lock for kernel side CPU access. */
	gcmkONERROR(gckVIDMEM_NODE_LockCPU(kernel, Channel->ringBufVideoMem, gcvFALSE, gcvFALSE,
					   (gctPOINTER *)&Channel->ringBufLogical));

	/* Get GPU physical address. */
	gcmkONERROR(gckVIDMEM_NODE_GetGPUPhysical(kernel, Channel->ringBufVideoMem, 0,
						  &Channel->physical));

	if (Channel->physical > 0xffffffffull) {
		gcmkPRINT("%s(%d): MCFE ring buffer physical over 4G: 0x%llx", __FUNCTION__,
			  __LINE__, (unsigned long long)Channel->physical);
	}

	/* Default to use physical. */
	Channel->ringBufAddress = (gctADDRESS)Channel->physical;

	return gcvSTATUS_OK;

OnError:
	return status;
}

static void _DestroyDescRingBuf(gckHARDWARE Hardware, gcsMCFE_RING_BUF *Channel)
{
	gckKERNEL kernel = Hardware->kernel;

	if (Channel->ringBufVideoMem) {
		gcmkVERIFY_OK(gckVIDMEM_NODE_UnlockCPU(kernel, Channel->ringBufVideoMem, 0,
						       gcvFALSE, gcvFALSE));

		gcmkVERIFY_OK(gckVIDMEM_NODE_Dereference(kernel, Channel->ringBufVideoMem));

		Channel->ringBufVideoMem = gcvNULL;
		Channel->ringBufLogical	 = gcvNULL;
	}
}

static gcmINLINE void _DestroyMCFE(gckHARDWARE Hardware, gckMCFE FE)
{
	if (FE) {
		gctUINT i;

		for (i = 0; i < FE->channelCount; i++) {
			if (FE->channels[i].binding) {
				_DestroyDescRingBuf(Hardware, &FE->channels[i].stdRingBuf);
				_DestroyDescRingBuf(Hardware, &FE->channels[i].priRingBuf);
			}
		}

		gcmkOS_SAFE_FREE(Hardware->os, FE);
	}
}

static gceSTATUS _ConstructChannel(gckHARDWARE Hardware, gceMCFE_CHANNEL_TYPE ChannelType,
				   gcsMCFE_CHANNEL *Channel)
{
	Channel->binding = ChannelType;

	return gcvSTATUS_OK;
}

gceSTATUS gckMCFE_Construct(gckHARDWARE Hardware, gckMCFE *FE)
{
	gceSTATUS status;
	gckMCFE fe = gcvNULL;
	gctUINT32 i;
	gctSIZE_T size = sizeof(struct _gckMCFE);

	if (Hardware->mcfeChannelCount > 1)
		size += sizeof(gcsMCFE_CHANNEL) * (Hardware->mcfeChannelCount - 1);

	gcmkONERROR(gckOS_Allocate(Hardware->os, size, (gctPOINTER *)&fe));

	gckOS_ZeroMemory(fe, size);

	fe->channelCount = Hardware->mcfeChannelCount;

	for (i = 0; i < fe->channelCount; i++) {
		gcmkONERROR(
			_ConstructChannel(Hardware, Hardware->mcfeChannels[i], &fe->channels[i]));
	}

	*FE = fe;
	return gcvSTATUS_OK;

OnError:
	_DestroyMCFE(Hardware, fe);
	return status;
}

void gckMCFE_Destroy(gckHARDWARE Hardware, gckMCFE FE)
{
	_DestroyMCFE(Hardware, FE);
}

static gceSTATUS _ProgramDescRingBuf(gckHARDWARE Hardware, gctBOOL MMUEnabled,
				     gcsMCFE_RING_BUF *Channel, gctUINT32 Index, gctBOOL Priority)
{
	gctUINT32 ringBufStartReg;
	gctUINT32 depthExpReg;
	gctUINT32 readPtrReg;
	//gctUINT32 writePtrReg;
	gctUINT32 data = 0;
	gctUINT32 address;

	if (Priority) {
		ringBufStartReg = 0x02800;
		depthExpReg	= 0x02900;
		readPtrReg	= 0x02B00;
		//writePtrReg	= 0x02A00;
	} else {
		ringBufStartReg = 0x02400;
		depthExpReg	= 0x02500;
		readPtrReg	= 0x02700;
		//writePtrReg	= 0x02600;
	}

	ringBufStartReg += Index << 2;
	depthExpReg += Index << 2;
	readPtrReg += Index << 2;
	//writePtrReg += Index << 2;

	Channel->ringBufAddress = MMUEnabled ? Channel->gpuAddress : (gctADDRESS)Channel->physical;

	gcmkSAFECASTVA(address, Channel->ringBufAddress);

	/* Channel ringBuf start address. */
	gcmkVERIFY_OK(
		gckOS_WriteRegisterEx(Hardware->os, Hardware->kernel, ringBufStartReg, address));

	/* Channel ringBuf depth (exponent of 2). */
	gcmkVERIFY_OK(gckOS_WriteRegisterEx(Hardware->os, Hardware->kernel, depthExpReg,
					    MCFE_RINGBUF_DEPTH_EXP));

	/* The RD ptr could keep unchanged, read and compute WR ptr. */
	gcmkVERIFY_OK(gckOS_ReadRegisterEx(Hardware->os, Hardware->kernel, readPtrReg, &data));

	/* Priority ring buffer write ptr. */
	/* gcmkVERIFY_OK(gckOS_WriteRegisterEx(Hardware->os, Hardware->kernel,
	 *                                     writePtrReg, data));
	 */

	/* No valid descriptor initially. */
	Channel->readPtr  = data;
	Channel->writePtr = data;

	return gcvSTATUS_OK;
}

static gceSTATUS _InitializeChannel(gckHARDWARE Hardware, gctBOOL MMUEnabled,
				    gcsMCFE_CHANNEL *Channel, gctUINT32 Index)
{
	gceSTATUS status;

	/* Allocate ring buffer descriptor memory. */
	if (!Channel->stdRingBuf.ringBufVideoMem)
		gcmkONERROR(_AllocateDescRingBuf(Hardware, &Channel->stdRingBuf));

	/* Index 0 is system channel, no priority ring buffer in system channel. */
	if (!Channel->priRingBuf.ringBufVideoMem && Index != 0)
		gcmkONERROR(_AllocateDescRingBuf(Hardware, &Channel->priRingBuf));

	gcmkONERROR(
		_ProgramDescRingBuf(Hardware, MMUEnabled, &Channel->stdRingBuf, Index, gcvFALSE));

	/* No priority channel in system engine. */
	if (Channel->binding != gcvMCFE_CHANNEL_SYSTEM) {
		gcmkONERROR(_ProgramDescRingBuf(Hardware, MMUEnabled, &Channel->priRingBuf, Index,
						gcvTRUE));
	}

	return gcvSTATUS_OK;

OnError:
	/* It's OK to leave ringBuf memory not free'd here. */
	return status;
}

gceSTATUS gckMCFE_Initialize(gckHARDWARE Hardware, gctBOOL MMUEnabled, gckMCFE FE)
{
	gctUINT32 i;
	gceSTATUS status;
	gctUINT32 eventEnable = 0xFFFFFFFF;

	gcmkHEADER_ARG("Hardware=%p MMUEnabled=%d FE=%p", Hardware, MMUEnabled, FE);

	for (i = 0; i < FE->channelCount; i++) {
		/* If channel is constructed. */
		if (FE->channels[i].binding)
			gcmkONERROR(_InitializeChannel(Hardware, MMUEnabled, &FE->channels[i], i));
	}

	/* Enable all events. */
	gcmkONERROR(gckOS_WriteRegisterEx(Hardware->os, Hardware->kernel, 0x00014, eventEnable));

	FE->mmuEnabled = MMUEnabled;

	gcmkFOOTER_NO();
	return gcvSTATUS_OK;

OnError:
	gcmkFOOTER();
	return status;
}

gceSTATUS gckMCFE_Nop(gckHARDWARE Hardware, gctPOINTER Logical, gctSIZE_T *Bytes)
{
	gctUINT32_PTR logical = (gctUINT32_PTR)Logical;
	gceSTATUS status;

	gcmkHEADER_ARG("Hardware=%p Logical=%p *Bytes=%lu", Hardware, Logical, gcmOPT_VALUE(Bytes));

	/* Verify the arguments. */
	gcmkVERIFY_OBJECT(Hardware, gcvOBJ_HARDWARE);
	gcmkVERIFY_ARGUMENT((Logical == gcvNULL) || (Bytes != gcvNULL));

	if (Logical != gcvNULL) {
		if (*Bytes < 8) {
			/* Command queue too small. */
			gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
		}

		/* Append NOP. */
		logical[0] =
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 31 : 27) -
									 (0 ? 31 : 27) + 1)))))))
			    << (0 ? 31 : 27))) |
			 (((gctUINT32)(0x03 &
				       ((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 31 : 27) -
								       (0 ? 31 : 27) + 1)))))))
			  << (0 ? 31 : 27)));
		logical[1] =
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 31 : 27) -
									 (0 ? 31 : 27) + 1)))))))
			    << (0 ? 31 : 27))) |
			 (((gctUINT32)(0x03 &
				       ((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 31 : 27) -
								       (0 ? 31 : 27) + 1)))))))
			  << (0 ? 31 : 27)));

		gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_HARDWARE, "%p: NOP", Logical);
	}

	if (Bytes != gcvNULL) {
		/* Return number of bytes required by the NOP command. */
		*Bytes = 8;
	}

	/* Success. */
	gcmkFOOTER_ARG("*Bytes=%lu", gcmOPT_VALUE(Bytes));
	return gcvSTATUS_OK;

OnError:
	/* Return the status. */
	gcmkFOOTER();
	return status;
}

gceSTATUS gckMCFE_Event(gckHARDWARE Hardware, gctPOINTER Logical, gctUINT8 Event,
			gceKERNEL_WHERE FromWhere, gctUINT32 *Bytes)
{
	gctUINT size;
	gctUINT32_PTR logical = (gctUINT32_PTR)Logical;
	gceSTATUS status;

	gcmkHEADER_ARG("Hardware=%p Logical=%p Event=%u FromWhere=%d *Bytes=%lu", Hardware, Logical,
		       Event, FromWhere, gcmOPT_VALUE(Bytes));

	/* Verify the arguments. */
	gcmkVERIFY_OBJECT(Hardware, gcvOBJ_HARDWARE);
	gcmkVERIFY_ARGUMENT((Logical == gcvNULL) || (Bytes != gcvNULL));
	gcmkVERIFY_ARGUMENT(Event < 32);

	/* Ignored. */
	(void)FromWhere;

	size = 8;

	if (Logical != gcvNULL) {
		if (*Bytes < size) {
			/* Command queue too small. */
			gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
		}

		/* Append EVENT(Event). */
		logical[0] =
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 31 : 27) -
									 (0 ? 31 : 27) + 1)))))))
			    << (0 ? 31 : 27))) |
			 (((gctUINT32)(0x16 &
				       ((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 31 : 27) -
								       (0 ? 31 : 27) + 1)))))))
			  << (0 ? 31 : 27))) |
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 25 : 16) - (0 ? 25 : 16) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 25 : 16) -
									 (0 ? 25 : 16) + 1)))))))
			    << (0 ? 25 : 16))) |
			 (((gctUINT32)(0x006 &
				       ((gctUINT32)((((1 ? 25 : 16) - (0 ? 25 : 16) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 25 : 16) -
								       (0 ? 25 : 16) + 1)))))))
			  << (0 ? 25 : 16))) |
			Event;

		logical[1] =
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 31 : 27) -
									 (0 ? 31 : 27) + 1)))))))
			    << (0 ? 31 : 27))) |
			 (((gctUINT32)(0x03 &
				       ((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 31 : 27) -
								       (0 ? 31 : 27) + 1)))))))
			  << (0 ? 31 : 27)));

#if gcmIS_DEBUG(gcdDEBUG_TRACE)
		{
			gctPHYS_ADDR_T phys;

			gckOS_GetPhysicalAddress(Hardware->os, Logical, &phys);
			gckOS_CPUPhysicalToGPUPhysical(Hardware->os, phys, &phys);
			gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_HARDWARE, "0x%08llx: EVENT %d", phys,
				       Event);
		}
#endif

#if gcdINTERRUPT_STATISTIC
		if (Event < (gctUINT8)Hardware->kernel->eventObj->totalQueueCount)
			gckOS_AtomSetMask(Hardware->pendingEvent, 1 << Event);
#endif
	}

	if (Bytes != gcvNULL) {
		/* Return number of bytes required by the EVENT command. */
		*Bytes = size;
	}

	/* Success. */
	gcmkFOOTER_ARG("*Bytes=%lu", gcmOPT_VALUE(Bytes));
	return gcvSTATUS_OK;

OnError:
	/* Return the status. */
	gcmkFOOTER();
	return status;
}

gceSTATUS gckMCFE_SendSemaphore(gckHARDWARE Hardware, gctPOINTER Logical, gctUINT32 SemaId,
				gctUINT32 *Bytes)
{
	gctUINT32_PTR logical = (gctUINT32_PTR)Logical;
	gceSTATUS status;

	gcmkHEADER_ARG("Hardware=%p Logical=%p SemaId=%u *Bytes=%lu", Hardware, Logical, SemaId,
		       gcmOPT_VALUE(Bytes));

	/* Verify the arguments. */
	gcmkVERIFY_OBJECT(Hardware, gcvOBJ_HARDWARE);
	gcmkVERIFY_ARGUMENT((Logical == gcvNULL) || (Bytes != gcvNULL));
	gcmkVERIFY_ARGUMENT(SemaId < 0xFFFF);

	if (Logical != gcvNULL) {
		if (*Bytes < 8) {
			/* Command queue too small. */
			gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
		}

		/* Append SEND_SEMAPHORE(SemaId). */
		logical[0] =
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 31 : 27) -
									 (0 ? 31 : 27) + 1)))))))
			    << (0 ? 31 : 27))) |
			 (((gctUINT32)(0x16 &
				       ((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 31 : 27) -
								       (0 ? 31 : 27) + 1)))))))
			  << (0 ? 31 : 27))) |
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 25 : 16) - (0 ? 25 : 16) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 25 : 16) -
									 (0 ? 25 : 16) + 1)))))))
			    << (0 ? 25 : 16))) |
			 (((gctUINT32)(0x002 &
				       ((gctUINT32)((((1 ? 25 : 16) - (0 ? 25 : 16) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 25 : 16) -
								       (0 ? 25 : 16) + 1)))))))
			  << (0 ? 25 : 16))) |
			SemaId;

		logical[1] =
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 31 : 27) -
									 (0 ? 31 : 27) + 1)))))))
			    << (0 ? 31 : 27))) |
			 (((gctUINT32)(0x03 &
				       ((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 31 : 27) -
								       (0 ? 31 : 27) + 1)))))))
			  << (0 ? 31 : 27)));
	}

	if (Bytes != gcvNULL)
		*Bytes = 8;

	/* Success. */
	gcmkFOOTER_ARG("*Bytes=%lu", gcmOPT_VALUE(Bytes));
	return gcvSTATUS_OK;

OnError:
	/* Return the status. */
	gcmkFOOTER();
	return status;
}

gceSTATUS gckMCFE_WaitSemaphore(gckHARDWARE Hardware, gctPOINTER Logical, gctUINT32 SemaId,
				gctUINT32 *Bytes)
{
	gctUINT32_PTR logical = (gctUINT32_PTR)Logical;
	gceSTATUS status;

	gcmkHEADER_ARG("Hardware=%p Logical=%p SemaId=%u *Bytes=%lu", Hardware, Logical, SemaId,
		       gcmOPT_VALUE(Bytes));

	/* Verify the arguments. */
	gcmkVERIFY_OBJECT(Hardware, gcvOBJ_HARDWARE);
	gcmkVERIFY_ARGUMENT((Logical == gcvNULL) || (Bytes != gcvNULL));
	gcmkVERIFY_ARGUMENT(SemaId < 0xFFFF);

	if (Logical != gcvNULL) {
		if (*Bytes < 8) {
			/* Command queue too small. */
			gcmkONERROR(gcvSTATUS_BUFFER_TOO_SMALL);
		}

		/* Append WAIT_SEMAPHORE(SemaId). */
		logical[0] =
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 31 : 27) -
									 (0 ? 31 : 27) + 1)))))))
			    << (0 ? 31 : 27))) |
			 (((gctUINT32)(0x16 &
				       ((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 31 : 27) -
								       (0 ? 31 : 27) + 1)))))))
			  << (0 ? 31 : 27))) |
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 25 : 16) - (0 ? 25 : 16) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 25 : 16) -
									 (0 ? 25 : 16) + 1)))))))
			    << (0 ? 25 : 16))) |
			 (((gctUINT32)(0x003 &
				       ((gctUINT32)((((1 ? 25 : 16) - (0 ? 25 : 16) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 25 : 16) -
								       (0 ? 25 : 16) + 1)))))))
			  << (0 ? 25 : 16))) |
			SemaId;

		logical[1] =
			((((gctUINT32)(0)) &
			  ~(((gctUINT32)(((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								    ~0U :
								    (~(~0U << ((1 ? 31 : 27) -
									 (0 ? 31 : 27) + 1)))))))
			    << (0 ? 31 : 27))) |
			 (((gctUINT32)(0x03 &
				       ((gctUINT32)((((1 ? 31 : 27) - (0 ? 31 : 27) + 1) == 32) ?
								  ~0U :
								  (~(~0U << ((1 ? 31 : 27) -
								       (0 ? 31 : 27) + 1)))))))
			  << (0 ? 31 : 27)));
	}

	if (Bytes != gcvNULL)
		*Bytes = 8;

	/* Success. */
	gcmkFOOTER_ARG("*Bytes=%lu", gcmOPT_VALUE(Bytes));
	return gcvSTATUS_OK;

OnError:
	/* Return the status. */
	gcmkFOOTER();
	return status;
}

gceSTATUS gckMCFE_Execute(gckHARDWARE Hardware, gctBOOL Priority, gctUINT32 ChannelId,
			  gctADDRESS Address, gctUINT32 Bytes)
{
	gceSTATUS status;
	gctUINT32 regBase;
	gcsMCFE_DESCRIPTOR *desc;
	gckMCFE mcFE		  = Hardware->mcFE;
	gcsMCFE_CHANNEL *channel  = gcvNULL;
	gcsMCFE_RING_BUF *ringBuf = gcvNULL;

	gcmkHEADER_ARG("Hardware=0x%x Priority=0x%x ChannelId=%u Address=%llx Bytes=%u", Hardware,
		       Priority, ChannelId, Address, Bytes);

	/* ChannelId should be valid. */
	gcmkASSERT(mcFE && ChannelId < mcFE->channelCount);

	channel = &mcFE->channels[ChannelId];

	/* No priority channel in system channel by design. */
	gcmkASSERT(!(channel->binding == gcvMCFE_CHANNEL_SYSTEM && Priority == 1));

	ringBuf = Priority ? &channel->priRingBuf : &channel->stdRingBuf;

	/*
	 * If no more descriptor space to write in ring buffer.
	 * To be improved to wait signal instead of blindly delay.
	 */
	while (_NextPtr(ringBuf->writePtr) == ringBuf->readPtr) {
		gctUINT32 data;

		regBase = Priority ? 0x02B00 : 0x02700;

		/* DescFifoRdPtr is 4 bytes aligned. */
		gcmkVERIFY_OK(gckOS_ReadRegisterEx(Hardware->os, Hardware->kernel,
						   regBase + ChannelId * 4, &data));

		ringBuf->readPtr = data;

		if (_NextPtr(ringBuf->writePtr) == ringBuf->readPtr) {
			gcmkPRINT("%s: MCFE channel %s-%d ringBuf is full!", __FUNCTION__,
				  Priority ? "Pri" : "Std", ChannelId);

			gckOS_Delay(Hardware->os, 100);
		}
	}

	/* Write the execute address to free descriptor. */
	regBase = Priority ? 0x02A00 : 0x02600;

	/* ringBufLogical is in uint32, 2 uint32 (command start, command end)
	 * contributes 1 descriptr.
	 */
	desc	    = (gcsMCFE_DESCRIPTOR *)&ringBuf->ringBufLogical[ringBuf->writePtr * 2];
	desc->start = (gctUINT32)Address;
	desc->end   = (gctUINT32)Address + Bytes;

	gcmkDUMP(Hardware->os, "#[descriptor %d: channel %s-%d]", ringBuf->writePtr,
		 Priority ? "Pri" : "Std", ChannelId);

	gcmkDUMP_BUFFER(Hardware->os, gcvDUMP_BUFFER_KERNEL_COMMAND, desc,
			ringBuf->ringBufAddress + ringBuf->writePtr * 8, 8);

	gcmkONERROR(
		gckVIDMEM_NODE_CleanCache(Hardware->kernel, ringBuf->ringBufVideoMem, 0, desc, 8));

	ringBuf->writePtr = _NextPtr(ringBuf->writePtr);

	gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_HARDWARE, "0x%x - 0x%x: %06d bytes, Channel=%s-%d",
		       desc->start, desc->end, Bytes, Priority ? "Pri" : "Std", ChannelId);

	/* DescFifoWrPtr is 4 bytes aligned, move it to next for
	 * coming descriptor.
	 */
	gcmkONERROR(gckOS_WriteRegisterEx(Hardware->os, Hardware->kernel, regBase + ChannelId * 4,
					  ringBuf->writePtr));

	/* Success. */
	gcmkFOOTER_NO();
	return gcvSTATUS_OK;

OnError:
	/* Return the status. */
	gcmkFOOTER();
	return status;
}

gceSTATUS gckMCFE_HardwareIdle(gckHARDWARE Hardware, gctBOOL_PTR isIdle)
{
	gceSTATUS status;
	gctUINT32 idle;
	gctUINT32 regRBase;
	gctUINT32 readPtr;
	gctUINT32 ChannelId	  = 0;
	gctBOOL Priority	  = gcvFALSE;
	gckMCFE mcFE		  = Hardware->mcFE;
	gcsMCFE_CHANNEL *channel  = gcvNULL;
	gcsMCFE_RING_BUF *ringBuf = gcvNULL;

	gcmkHEADER();

	/* ChannelId should be valid. */
	gcmkASSERT(mcFE && ChannelId < mcFE->channelCount);

	channel = &mcFE->channels[ChannelId];
	ringBuf = Priority ? &channel->priRingBuf : &channel->stdRingBuf;

	*isIdle = gcvTRUE;

	/* Read idle register. */
	gcmkONERROR(gckOS_ReadRegisterEx(Hardware->os, Hardware->kernel, 0x00004, &idle));

	/* Pipe must be idle. */
	if ((idle | (1 << 14)) != 0x7fffffff) {
		/* Something is busy. */
		*isIdle = gcvFALSE;
		return status;
	}

	regRBase = Priority ? 0x02B00 : 0x02700;

	gcmkONERROR(gckOS_ReadRegisterEx(Hardware->os, Hardware->kernel, regRBase + ChannelId * 4,
					 &readPtr));

	/* No more descriptor to execute. */
	if (readPtr != ringBuf->writePtr)
		*isIdle = gcvFALSE;

	gcmkFOOTER();

OnError:
	return status;
}
