/*
 * tkMacOSXInit.c --
 *
 *	This file contains Mac OS X -specific interpreter initialization
 *	functions.
 *
 * Copyright (c) 1995-1997 Sun Microsystems, Inc.
 * Copyright 2001, Apple Computer, Inc.
 * Copyright (c) 2005-2008 Daniel A. Steffen <das@users.sourceforge.net>
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tkMacOSXPrivate.h"

#include <sys/stat.h>
#include <sys/utsname.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <objc/objc-auto.h>

/*
 * The following structures are used to map the script/language codes of a
 * font to the name that should be passed to Tcl_GetEncoding() to obtain the
 * encoding for that font. The set of numeric constants is fixed and defined
 * by Apple.
 */

typedef struct Map {
    CFStringEncoding numKey;
    const char *strKey;
} Map;

static Map scriptMap[] = {
    {smRoman,		"macRoman"},
    {smJapanese,	"macJapan"},
    {smTradChinese,	"macChinese"},
    {smKorean,		"macKorean"},
    {smArabic,		"macArabic"},
    {smHebrew,		"macHebrew"},
    {smGreek,		"macGreek"},
    {smCyrillic,	"macCyrillic"},
    {smRSymbol,		"macRSymbol"},
    {smDevanagari,	"macDevanagari"},
    {smGurmukhi,	"macGurmukhi"},
    {smGujarati,	"macGujarati"},
    {smOriya,		"macOriya"},
    {smBengali,		"macBengali"},
    {smTamil,		"macTamil"},
    {smTelugu,		"macTelugu"},
    {smKannada,		"macKannada"},
    {smMalayalam,	"macMalayalam"},
    {smSinhalese,	"macSinhalese"},
    {smBurmese,		"macBurmese"},
    {smKhmer,		"macKhmer"},
    {smThai,		"macThailand"},
    {smLaotian,		"macLaos"},
    {smGeorgian,	"macGeorgia"},
    {smArmenian,	"macArmenia"},
    {smSimpChinese,	"macSimpChinese"},
    {smTibetan,		"macTIbet"},
    {smMongolian,	"macMongolia"},
    {smGeez,		"macEthiopia"},
    {smEastEurRoman,	"macCentEuro"},
    {smVietnamese,	"macVietnam"},
    {smExtArabic,	"macSindhi"},
    {0,			NULL}
};

static char tkLibPath[PATH_MAX + 1] = "";
Tcl_Encoding TkMacOSXCarbonEncoding = NULL;

/*
 * If the App is in an App package, then we want to add the Scripts directory
 * to the auto_path.
 */

static char scriptPath[PATH_MAX + 1] = "";

#pragma mark TKApplication(TKInit)

#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
#define NSTextInputContextKeyboardSelectionDidChangeNotification @"NSTextInputContextKeyboardSelectionDidChangeNotification"
static void keyboardChanged(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef userInfo) {
    [[NSNotificationCenter defaultCenter] postNotificationName:NSTextInputContextKeyboardSelectionDidChangeNotification object:nil userInfo:nil];
}
#endif

@interface TKApplication(TKWindowEvent)
- (void)_setupWindowNotifications;
@end

@interface TKApplication(TKKeyboard)
- (void)keyboardChanged:(NSNotification *)notification;
@end

@implementation TKApplication
@end

@implementation TKApplication(TKInit)
#ifdef TK_MAC_DEBUG_NOTIFICATIONS
- (void)_postedNotification:(NSNotification *)notification {
    TKLog(@"-[%@(%p) %s] %@", [self class], self, _cmd, notification);
}
#endif
#define observe(n, s) [nc addObserver:self selector:@selector(s) name:(n) object:nil]
- (void)_setupApplicationNotifications {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    observe(NSTextInputContextKeyboardSelectionDidChangeNotification, keyboardChanged:);
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
    CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(), NULL, &keyboardChanged, kTISNotifySelectedKeyboardInputSourceChanged, NULL, CFNotificationSuspensionBehaviorCoalesce);
#endif
}
#undef observe(n, s)
- (void)_setupEventLoop {
    _running = 1;
    if (!_appFlags._hasBeenRun) {
        _appFlags._hasBeenRun = YES;
	[self finishLaunching];
	objc_collect(OBJC_COLLECT_IF_NEEDED);
    }
    [self setWindowsNeedUpdate:YES];
}
- (void)_setup {
    [self setDelegate:self];
#ifdef TK_MAC_DEBUG_NOTIFICATIONS
    [[NSNotificationCenter defaultCenter] addObserver:self
	    selector:@selector(_postedNotification:) name:nil object:nil];
#endif
    [self _setupWindowNotifications];
    [self _setupApplicationNotifications];
    [[NSUserDefaults standardUserDefaults] registerDefaults:
	    [NSDictionary dictionaryWithObjectsAndKeys:
	    [NSNumber numberWithBool:YES],
	    @"_NSCanWrapButtonTitles",
	    [NSNumber numberWithInt:-1],
	    @"NSStringDrawingTypesetterBehavior",
	    nil]];
}
- (NSBundle *)tkFrameworkBundle {
    if (tkLibPath[0] != '\0') {
	return [NSBundle bundleWithPath:[NSString stringWithFormat:@"%s/../..",
		tkLibPath]];
    } else {
	return nil;
    }
}
@end
#pragma mark -



/*
 *----------------------------------------------------------------------
 *
 * TkpInit --
 *
 *	Performs Mac-specific interpreter initialization related to the
 *	tk_library variable.
 *
 * Results:
 *	Returns a standard Tcl result. Leaves an error message or result in
 *	the interp's result.
 *
 * Side effects:
 *	Sets "tk_library" Tcl variable, runs "tk.tcl" script.
 *
 *----------------------------------------------------------------------
 */

int
TkpInit(
    Tcl_Interp *interp)
{
    static int initialized = 0;

    Tk_MacOSXSetupTkNotifier();

    /*
     * Since it is possible for TkInit to be called multiple times and we
     * don't want to do the following initialization multiple times we protect
     * against doing it more than once.
     */

    if (!initialized) {
	int bundledExecutable = 0;
	CFBundleRef bundleRef;
	CFURLRef bundleUrl = NULL;
	CFStringEncoding encoding;
	const char *encodingStr = NULL;
	int i;
	struct utsname name;
	long osVersion = 0;
	struct stat st;

	initialized = 1;
	
	/*
	 * Initialize/check OS version variable for runtime checks.
	 */

	#if MAC_OS_X_VERSION_MIN_REQUIRED < 1050
	#error Mac OS X 10.5 required
	#endif

	if (!uname(&name)) {
	    osVersion = strtol(name.release, NULL, 10) - 4;
	}
	if (osVersion && osVersion < (MAC_OS_X_VERSION_MIN_REQUIRED-1000)/10) {
	    Tcl_Panic("Mac OS X 10.%d or later required !",
		    (MAC_OS_X_VERSION_MIN_REQUIRED-1000)/10);
	}

#ifdef TK_FRAMEWORK
	/*
	 * When Tk is in a framework, force tcl_findLibrary to look in the
	 * framework scripts directory.
	 * FIXME: Should we come up with a more generic way of doing this?
	 */

	if (Tcl_MacOSXOpenVersionedBundleResources(interp,
		"com.tcltk.tklibrary", TK_FRAMEWORK_VERSION, 0, PATH_MAX,
		tkLibPath) != TCL_OK) {
	    TkMacOSXDbgMsg("Tcl_MacOSXOpenVersionedBundleResources failed");
	}
#endif

	static NSAutoreleasePool *pool = nil;
	if (!pool) {
	    pool = [NSAutoreleasePool new];
	}
	[TKApplication sharedApplication];
	[NSApp _setup];

	/* Check whether we are a bundled executable: */
	bundleRef = CFBundleGetMainBundle();
	if (bundleRef) {
	    bundleUrl = CFBundleCopyBundleURL(bundleRef);
	}
	if (bundleUrl) {
	    /*
	     * A bundled executable is two levels down from its main bundle
	     * directory (e.g. Wish.app/Contents/MacOS/Wish), whereas an
	     * unbundled executable's main bundle directory is just the
	     * directory containing the executable. So to check whether we are
	     * bundled, we delete the last three path components of the
	     * executable's url and compare the resulting url with the main
	     * bundle url.
	     */

	    int j = 3;
	    CFURLRef url = CFBundleCopyExecutableURL(bundleRef);

	    while (url && j--) {
		CFURLRef parent =
			CFURLCreateCopyDeletingLastPathComponent(NULL, url);

		CFRelease(url);
		url = parent;
	    }
	    if (url) {
		bundledExecutable = CFEqual(bundleUrl, url);
		CFRelease(url);
	    }
	    CFRelease(bundleUrl);
	}

	if (!bundledExecutable) {
	    /*
	     * If we are loaded into an executable that is not a bundled
	     * application, the window server does not let us come to the
	     * foreground. For such an executable, notify the window server
	     * that we are now a full GUI application.
	     */

	    OSStatus err = procNotFound;
	    ProcessSerialNumber psn = { 0, kCurrentProcess };

	    err = ChkErr(TransformProcessType, &psn,
		    kProcessTransformToForegroundApplication);

	    /*
	     * Set application to generic Tk icon.
	     */

	    NSString *path = [[NSApp tkFrameworkBundle]
		    pathForImageResource:@"Tk.icns"];
#ifdef TK_MAC_DEBUG
	    if (!path) {
		// FIXME: fallback to absolute path specific to my box
		path = @"/Volumes/Users/steffen/Development/TclTk/git/HEAD/tk/macosx/Tk.icns";
		TkMacOSXDbgMsg("Fallback to hardcoded path!");
	    }
#endif
	    if (path) {
		NSImage *image = [[NSImage alloc] initWithContentsOfFile:path];
		if (image) {
		    [NSApp setApplicationIconImage:image];
		}
	    }
	}

	TkMacOSXInitAppleEvents(interp);
	TkMacOSXInitCarbonEvents(interp);
	TkMacOSXInitMenus(interp);
	TkMacOSXUseAntialiasedText(interp, -1);
	TkMacOSXInitCGDrawing(interp, TRUE, 0);
	TkMacOSXInitKeyboard(interp);

	encoding = CFStringGetSystemEncoding();

	for (i = 0; scriptMap[i].strKey != NULL; i++) {
	    if (scriptMap[i].numKey == encoding) {
		encodingStr = scriptMap[i].strKey;
		break;
	    }
	}
	if (encodingStr == NULL) {
	    encodingStr = "macRoman";
	}
	[NSApp _setupEventLoop];
	[pool drain];

	TkMacOSXCarbonEncoding = Tcl_GetEncoding(NULL, encodingStr);
	if (TkMacOSXCarbonEncoding == NULL) {
	    TkMacOSXCarbonEncoding = Tcl_GetEncoding(NULL, NULL);
	}

	/*
	 * FIXME: Close stdin & stdout for remote debugging otherwise we will
	 * fight with gdb for stdin & stdout
	 */

	if (getenv("XCNOSTDIN") != NULL) {
	    close(0);
	    close(1);
	}

	/*
	 * If we don't have a TTY and stdin is a special character file of
	 * length 0, (e.g. /dev/null, which is what Finder sets when double
	 * clicking Wish) then use the Tk based console interpreter.
	 */

	if (getenv("TK_CONSOLE") ||
		(!isatty(0) && (fstat(0, &st) ||
		(S_ISCHR(st.st_mode) && st.st_blocks == 0)))) {
	    Tk_InitConsoleChannels(interp);
	    Tcl_RegisterChannel(interp, Tcl_GetStdChannel(TCL_STDIN));
	    Tcl_RegisterChannel(interp, Tcl_GetStdChannel(TCL_STDOUT));
	    Tcl_RegisterChannel(interp, Tcl_GetStdChannel(TCL_STDERR));

	    /*
	     * Only show the console if we don't have a startup script
	     * and tcl_interactive hasn't been set already.
	     */

	    if (Tcl_GetStartupScript(NULL) == NULL) {
		const char *intvar = Tcl_GetVar(interp,
			"tcl_interactive", TCL_GLOBAL_ONLY);

		if (intvar == NULL) {
		    Tcl_SetVar(interp, "tcl_interactive", "1",
			    TCL_GLOBAL_ONLY);
		}
	    }
	    if (Tk_CreateConsoleWindow(interp) == TCL_ERROR) {
		return TCL_ERROR;
	    }
	}
    }

    if (tkLibPath[0] != '\0') {
	Tcl_SetVar(interp, "tk_library", tkLibPath, TCL_GLOBAL_ONLY);
    }

    if (scriptPath[0] != '\0') {
	Tcl_SetVar(interp, "auto_path", scriptPath,
		TCL_GLOBAL_ONLY|TCL_LIST_ELEMENT|TCL_APPEND_VALUE);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpGetAppName --
 *
 *	Retrieves the name of the current application from a platform specific
 *	location. For Unix, the application name is the tail of the path
 *	contained in the tcl variable argv0.
 *
 * Results:
 *	Returns the application name in the given Tcl_DString.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TkpGetAppName(
    Tcl_Interp *interp,
    Tcl_DString *namePtr)	/* A previously initialized Tcl_DString. */
{
    const char *p, *name;

    name = Tcl_GetVar(interp, "argv0", TCL_GLOBAL_ONLY);
    if ((name == NULL) || (*name == 0)) {
	name = "tk";
    } else {
	p = strrchr(name, '/');
	if (p != NULL) {
	    name = p+1;
	}
    }
    Tcl_DStringAppend(namePtr, name, -1);
}

/*
 *----------------------------------------------------------------------
 *
 * TkpDisplayWarning --
 *
 *	This routines is called from Tk_Main to display warning messages that
 *	occur during startup.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Generates messages on stdout.
 *
 *----------------------------------------------------------------------
 */

void
TkpDisplayWarning(
    const char *msg,		/* Message to be displayed. */
    const char *title)		/* Title of warning. */
{
    Tcl_Channel errChannel = Tcl_GetStdChannel(TCL_STDERR);

    if (errChannel) {
	Tcl_WriteChars(errChannel, title, -1);
	Tcl_WriteChars(errChannel, ": ", 2);
	Tcl_WriteChars(errChannel, msg, -1);
	Tcl_WriteChars(errChannel, "\n", 1);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkMacOSXDefaultStartupScript --
 *
 *	On MacOS X, we look for a file in the Resources/Scripts directory
 *	called AppMain.tcl and if found, we set argv[1] to that, so that the
 *	rest of the code will find it, and add the Scripts folder to the
 *	auto_path. If we don't find the startup script, we just bag it,
 *	assuming the user is starting up some other way.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Tcl_SetStartupScript() called when AppMain.tcl found.
 *
 *----------------------------------------------------------------------
 */

MODULE_SCOPE void
TkMacOSXDefaultStartupScript(void)
{
    CFBundleRef bundleRef;

    bundleRef = CFBundleGetMainBundle();

    if (bundleRef != NULL) {
	CFURLRef appMainURL = CFBundleCopyResourceURL(bundleRef,
		CFSTR("AppMain"), CFSTR("tcl"), CFSTR("Scripts"));

	if (appMainURL != NULL) {
	    CFURLRef scriptFldrURL;
	    char startupScript[PATH_MAX + 1];

	    if (CFURLGetFileSystemRepresentation (appMainURL, true,
		    (unsigned char *) startupScript, PATH_MAX)) {
		Tcl_SetStartupScript(Tcl_NewStringObj(startupScript,-1), NULL);
		scriptFldrURL = CFURLCreateCopyDeletingLastPathComponent(NULL,
			appMainURL);
		if (scriptFldrURL != NULL) {
		    CFURLGetFileSystemRepresentation(scriptFldrURL, true,
			    (unsigned char *) scriptPath, PATH_MAX);
		    CFRelease(scriptFldrURL);
		}
	    }
	    CFRelease(appMainURL);
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkMacOSXGetNamedSymbol --
 *
 *	Dynamically acquire address of a named symbol from a loaded dynamic
 *	library, so that we can use API that may not be available on all OS
 *	versions. If module is non-NULL and not the empty string, use twolevel
 *	namespace lookup.
 *
 * Results:
 *	Address of given symbol or NULL if unavailable.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

MODULE_SCOPE void*
TkMacOSXGetNamedSymbol(
    const char* module,
    const char* symbol)
{
    NSSymbol nsSymbol = NULL;

    if (module && *module) {
	if (NSIsSymbolNameDefinedWithHint(symbol, module)) {
	    nsSymbol = NSLookupAndBindSymbolWithHint(symbol, module);
	}
    } else {
	if (NSIsSymbolNameDefined(symbol)) {
	    nsSymbol = NSLookupAndBindSymbol(symbol);
	}
    }

    if (!nsSymbol) {
	return NULL;
    }
    return NSAddressOfSymbol(nsSymbol);
}

/*
 *----------------------------------------------------------------------
 *
 * TkMacOSXGetStringObjFromCFString --
 *
 *	Get a string object from a CFString as efficiently as possible.
 *
 * Results:
 *	New string object or NULL if conversion failed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

MODULE_SCOPE Tcl_Obj*
TkMacOSXGetStringObjFromCFString(
    CFStringRef str)
{
    Tcl_Obj *obj = NULL;
    const char *c = CFStringGetCStringPtr(str, kCFStringEncodingUTF8);

    if (c) {
	obj = Tcl_NewStringObj(c, -1);
    } else {
	CFRange all = CFRangeMake(0, CFStringGetLength(str));
	CFIndex len;

	if (CFStringGetBytes(str, all, kCFStringEncodingUTF8, 0, false, NULL,
		0, &len) > 0) {
	    obj = Tcl_NewObj();
	    Tcl_SetObjLength(obj, len);
	    CFStringGetBytes(str, all, kCFStringEncodingUTF8, 0, false,
		    (UInt8*) obj->bytes, len, NULL);
	}
    }
    return obj;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 79
 * coding: utf-8
 * End:
 */
