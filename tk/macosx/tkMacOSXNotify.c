/*
 * tkMacOSXNotify.c --
 *
 *	This file contains the implementation of a tcl event source
 *	for the AppKit event loop.
 *
 * Copyright (c) 1995-1997 Sun Microsystems, Inc.
 * Copyright 2001, Apple Computer, Inc.
 * Copyright (c) 2005-2009 Daniel A. Steffen <das@users.sourceforge.net>
 * Copyright 2008-2009, Apple Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tkMacOSXPrivate.h"
#include "tkMacOSXEvent.h"
#include <tclInt.h>
#include <pthread.h>
#import <objc/objc-auto.h>

typedef struct ThreadSpecificData {
    int initialized, sendEventNestingLevel;
    NSEvent *currentEvent;
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

#define TSD_INIT() ThreadSpecificData *tsdPtr = Tcl_GetThreadData(&dataKey, \
	    sizeof(ThreadSpecificData))

static void TkMacOSXNotifyExitHandler(ClientData clientData);
static void TkMacOSXEventsSetupProc(ClientData clientData, int flags);
static void TkMacOSXEventsCheckProc(ClientData clientData, int flags);

#pragma mark TKApplication(TKNotify)

@implementation TKApplication(TKNotify)
- (NSEvent *)nextEventMatchingMask:(NSUInteger)mask
	untilDate:(NSDate *)expiration inMode:(NSString *)mode
	dequeue:(BOOL)deqFlag {
    NSAutoreleasePool *pool = [NSAutoreleasePool new];
    int oldMode = Tcl_SetServiceMode(TCL_SERVICE_ALL);
    NSEvent *event = [[super nextEventMatchingMask:mask untilDate:expiration
	    inMode:mode dequeue:deqFlag] retain];
    Tcl_SetServiceMode(oldMode);
    if (event) {
	TSD_INIT();
	if (tsdPtr->sendEventNestingLevel) {
	    event = [NSApp tkProcessEvent:event];
	}
    }
    [pool drain];
    return [event autorelease];
}
- (void)sendEvent:(NSEvent *)theEvent {
    TSD_INIT();
    int oldMode = Tcl_SetServiceMode(TCL_SERVICE_ALL);
    tsdPtr->sendEventNestingLevel++;
    [super sendEvent:theEvent];
    tsdPtr->sendEventNestingLevel--;
    Tcl_SetServiceMode(oldMode);
}
@end

#pragma mark -

/*
 *----------------------------------------------------------------------
 *
 * Tk_MacOSXSetupTkNotifier --
 *
 *	This procedure is called during Tk initialization to create
 *	the event source for TkAqua events.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A new event source is created.
 *
 *----------------------------------------------------------------------
 */

void
Tk_MacOSXSetupTkNotifier(void)
{
    TSD_INIT();
    if (!tsdPtr->initialized) {
	tsdPtr->initialized = 1;

	/*
	 * Install TkAqua event source in main event loop thread.
	 */

	if (CFRunLoopGetMain() == CFRunLoopGetCurrent()) {
	    if (!pthread_main_np()) {
		/*
		 * Panic if main runloop is not on the main application thread.
		 */

		Tcl_Panic("Tk_MacOSXSetupTkNotifier: %s",
		    "first [load] of TkAqua has to occur in the main thread!");
	    }
	    Tcl_CreateEventSource(TkMacOSXEventsSetupProc,
		    TkMacOSXEventsCheckProc, GetMainEventQueue());
	    TkCreateExitHandler(TkMacOSXNotifyExitHandler, NULL);
	    Tcl_SetServiceMode(TCL_SERVICE_ALL);
	    TclMacOSXNotifierAddRunLoopMode(NSEventTrackingRunLoopMode);
	    TclMacOSXNotifierAddRunLoopMode(NSModalPanelRunLoopMode);
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkMacOSXNotifyExitHandler --
 *
 *	This function is called during finalization to clean up the
 *	TkMacOSXNotify module.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
TkMacOSXNotifyExitHandler(
    ClientData clientData)	/* Not used. */
{
    TSD_INIT();
    Tcl_DeleteEventSource(TkMacOSXEventsSetupProc,
	    TkMacOSXEventsCheckProc, GetMainEventQueue());
    tsdPtr->initialized = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TkMacOSXEventsSetupProc --
 *
 *	This procedure implements the setup part of the TkAqua Events event
 *	source. It is invoked by Tcl_DoOneEvent before entering the notifier
 *	to check for events.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If TkAqua events are queued, then the maximum block time will be set
 *	to 0 to ensure that the notifier returns control to Tcl.
 *
 *----------------------------------------------------------------------
 */

static void
TkMacOSXEventsSetupProc(
    ClientData clientData,
    int flags)
{
    if (flags & TCL_WINDOW_EVENTS) {
	static Tcl_Time zeroBlockTime = { 0, 0 };
	TSD_INIT();
	if (!tsdPtr->currentEvent) {
	    NSEvent *currentEvent = [NSApp nextEventMatchingMask:NSAnyEventMask
		    untilDate:[NSDate distantPast]
		    inMode:NSDefaultRunLoopMode dequeue:YES];
	    if (currentEvent) {
		tsdPtr->currentEvent =
			TkMacOSXMakeUncollectableAndRetain(currentEvent);
	    }
	}
	if (tsdPtr->currentEvent) {
	    Tcl_SetMaxBlockTime(&zeroBlockTime);
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkMacOSXEventsCheckProc --
 *
 *	This procedure processes events sitting in the TkAqua event queue.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Moves applicable queued TkAqua events onto the Tcl event queue.
 *
 *----------------------------------------------------------------------
 */

static void
TkMacOSXEventsCheckProc(
    ClientData clientData,
    int flags)
{
    if (flags & TCL_WINDOW_EVENTS) {
	NSEvent *currentEvent = nil;
	NSAutoreleasePool *pool = nil;
	TSD_INIT();
	if (tsdPtr->currentEvent) {
	    currentEvent = TkMacOSXMakeCollectableAndAutorelease(
		    tsdPtr->currentEvent);
	}
	do {
	    if (!currentEvent) {
		currentEvent = [NSApp nextEventMatchingMask:NSAnyEventMask
			untilDate:[NSDate distantPast]
			inMode:NSDefaultRunLoopMode dequeue:YES];
	    }
	    if (!currentEvent) {
		break;
	    }
	    [currentEvent retain];
	    pool = [NSAutoreleasePool new];
	    if (tkMacOSXGCEnabled) {
		objc_clear_stack(0);
	    }
	    currentEvent = [NSApp tkProcessEvent:currentEvent];
	    if (currentEvent) {
#ifdef TK_MAC_DEBUG_EVENTS
		TKLog(@"   event: %@", currentEvent);
#endif
		[NSApp sendEvent:currentEvent];
		[currentEvent release];
		currentEvent = nil;
	    }
	    [pool drain];
	    pool = nil;
	} while (1);
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 79
 * coding: utf-8
 * End:
 */
