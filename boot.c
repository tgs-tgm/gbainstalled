/**************************************************************************
 *                                                                        *
 *               Copyright (C) 1995, Silicon Graphics, Inc.               *
 *                                                                        *
 *  These coded instructions, statements, and computer programs  contain  *
 *  unpublished  proprietary  information of Silicon Graphics, Inc., and  *
 *  are protected by Federal copyright  law.  They  may not be disclosed  *
 *  to  third  parties  or copied or duplicated in any form, in whole or  *
 *  in part, without the prior written consent of Silicon Graphics, Inc.  *
 *                                                                        *
 *************************************************************************/

/*---------------------------------------------------------------------*
        Copyright (C) 1998 Nintendo. (Originated by SGI)
        
        $RCSfile: boot.c,v $
        $Revision: 1.9 $
        $Date: 1998/12/24 15:54:00 $
 *---------------------------------------------------------------------*/

/*
 * File:  boot.c
 * Created: Thu Dec 14 16:48:01 PST 1995
 *
 */

#include <ultra64.h>
#include <PR/ramrom.h>		/* needed for argument passing into the app */

#include "game.h"
#include "controller.h"
#include "memory.h"
#include "src/rom.h"
#include "src/debug_out.h"
#include "render.h"
#include "src/faulthandler.h"
#include "src/save.h"
#include "src/rspppu.h"

#ifdef USE_DEBUGGER
#include "debugger/debugger.h"
#endif

/*
 * Symbol genererated by "makerom" (RAM)
 */
extern char     _codeSegmentEnd[];
extern char     _codeSegmentTextEnd[];
extern char     _codeSegmentTextStart[];

/*
 * Symbols generated by "makerom" (ROM)
 */
extern char     _staticSegmentRomStart[],
                _staticSegmentRomEnd[];
extern char     _gbromSegmentRomStart[];

/*
 * Stacks for the threads as well as message queues for synchronization
 * This stack is ridiculously large, and could also be reclaimed once
 * the main thread is started.
 */
/*
 * This stack is used only during boot and could be reclaimed later
 */
u64             bootStack[STACKSIZE / sizeof(u64)];

/*
 * Threads
 */
static void     idle(void *);
static void     mainproc(void *);
extern void	game(void);

static OSThread idleThread;
static u64      idleThreadStack[STACKSIZE / sizeof(u64)];

OSThread mainThread;
static u64      mainThreadStack[STACKSIZE / sizeof(u64)];

/*
 * Messages and message queues
 */
static OSMesg   PiMessages[NUM_PI_MSGS];
static OSMesgQueue PiMessageQ;

OSMesgQueue     dmaMessageQ,
                rdpMessageQ,
                retraceMessageQ;
OSMesg          dmaMessageBuf,
                rdpMessageBuf,
                retraceMessageBuf[20];
OSIoMesg        dmaIOMessageBuf;	/* * see man page to understand this */

/*
 * global variables
 */
int		rdp_flag = 0;
char		*staticSegment;
char        *_gEndSegments;



OSPiHandle	*handler;

void
boot(void)
{
	/*
	 * notice that you can't call osSyncPrintf() until you set
	 * up an idle thread.
	 */

	osInitialize();

	handler = osCartRomInit();

	
	osCreateThread(&idleThread, 1, idle, (void *) 0,
			   idleThreadStack + STACKSIZE / sizeof(u64), 10);

	osStartThread(&idleThread);

	/*
	 * never reached 
	 */
}

static void
idle(void *arg)
{
	/*
	 * Initialize video 
	 */
	osCreateViManager(OS_PRIORITY_VIMGR);

	switch (osTvType) {
		case 0: // PAL
			osViSetMode(&osViModeTable[OS_VI_PAL_HPF1]);
			break;
		case 1: // NTSC
			osViSetMode(&osViModeTable[OS_VI_NTSC_HPF1]);
			break;
		case 2: // MPAL
			osViSetMode(&osViModeTable[OS_VI_MPAL_HPF1]);
			break;
	}
	osViBlack(1);
	osViSetSpecialFeatures(OS_VI_GAMMA_OFF |
			OS_VI_GAMMA_DITHER_OFF |
			OS_VI_DIVOT_OFF |
			OS_VI_DITHER_FILTER_OFF);

	/*
	 * Start PI Mgr for access to cartridge
	 */
	osCreatePiManager((OSPri) OS_PRIORITY_PIMGR, &PiMessageQ, PiMessages,
		  NUM_PI_MSGS);

	/*
	 * Create main thread
	 */
	osCreateThread(&mainThread, 3, mainproc, arg,
		   mainThreadStack + STACKSIZE / sizeof(u64), 10);

	osStartThread(&mainThread);

#ifndef USE_DEBUGGER
	installFaultHandler(&mainThread);
#endif

	/*
	 * Become the idle thread
	 */
	osSetThreadPri(0, 0);

	for (;;) ;
}

/*
 * This is the main routine of the app.
 */
static void mainproc(void *arg)
{
	/*
	 * Setup the message queues
	 */
	osCreateMesgQueue(&dmaMessageQ, &dmaMessageBuf, 1);

	osCreateMesgQueue(&rdpMessageQ, &rdpMessageBuf, 1);
	osSetEventMesg(OS_EVENT_DP, &rdpMessageQ, NULL);

	osCreateMesgQueue(&retraceMessageQ, retraceMessageBuf, 20);
	osViSetEvent(&retraceMessageQ, NULL, 1);

	/*
	 * Stick the static segment right after the code/data segment
	 */
	staticSegment = _codeSegmentEnd;

	osInvalDCache((void *)staticSegment, 
		(u32) _staticSegmentRomEnd - (u32) _staticSegmentRomStart);


	dmaIOMessageBuf.hdr.pri      = OS_MESG_PRI_NORMAL;
	dmaIOMessageBuf.hdr.retQueue = &dmaMessageQ;
	dmaIOMessageBuf.dramAddr     = staticSegment;
	dmaIOMessageBuf.devAddr      = (u32)_staticSegmentRomStart;
	dmaIOMessageBuf.size         = (u32)_staticSegmentRomEnd-(u32)_staticSegmentRomStart;

	osEPiStartDma(handler, &dmaIOMessageBuf, OS_READ);

	/*
	 * Wait for DMA to finish
	 */
	(void) osRecvMesg(&dmaMessageQ, NULL, OS_MESG_BLOCK);


#ifdef USE_DEBUGGER
	OSThread* threads = &mainThread;
	enum GDBError err = gdbInitDebugger(handler, &dmaMessageQ, &threads, 1);
	if (err != GDBErrorNone)
	{
		DEBUG_PRINT_F("Failed to initialize debugger %x", err);
	}
#endif
	

	/*
	 * Stick the texture segment right after the static segment
	 */
	_gEndSegments = staticSegment + 
		(u32) _staticSegmentRomEnd - (u32) _staticSegmentRomStart;
		
	clearDebugOutput();

	void* heapEnd = initColorBuffers((void*)OS_PHYSICAL_TO_K0(osMemSize));

	initHeap(heapEnd);
	
	initSaveCallbacks();

	initRomLayout(&gGBRom, _gbromSegmentRomStart);

	initControllers(MAXCONTROLLERS);

	setupPPU();

	game();
}


