///
///	@file softhddevice.cpp	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 zille.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

#define __STDC_CONSTANT_MACROS		///< needed for ffmpeg UINT64_C

#include <vdr/plugin.h>
#include <vdr/dvbspu.h>

#include "softhddev.h"
#include "softhddevice-drm.h"
#include "softhddevice_service.h"

extern "C"
{
#include <libavcodec/avcodec.h>

#include "audio.h"
#include "video.h"
#include "codec.h"
}

//////////////////////////////////////////////////////////////////////////////

    /// vdr-plugin version number.
    /// Makefile extracts the version number for generating the file name
    /// for the distribution archive.
static const char *const VERSION = "0.0.1rc1"
#ifdef GIT_REV
    "-GIT" GIT_REV
#endif
    ;

    /// vdr-plugin description.
static const char *const DESCRIPTION =
trNOOP("A software and GPU emulated HD device");

    /// vdr-plugin text of main menu entry
static const char *MAINMENUENTRY = trNOOP("SoftHdDevice-drm");

    /// single instance of softhddevice plugin device.
static class cSoftHdDevice *MyDevice;

//////////////////////////////////////////////////////////////////////////////

static char ConfigMakePrimary;		///< config primary wanted
static char ConfigHideMainMenuEntry;	///< config hide main menu entry

char ConfigSWDeinterlacer;			///< config use sw deinterlacer

static int ConfigVideoAudioDelay;	///< config audio delay
static char ConfigAudioPassthrough;	///< config audio pass-through mask
static char AudioPassthroughState;	///< flag audio pass-through on/off
static char ConfigAudioDownmix;		///< config ffmpeg audio downmix
static char ConfigAudioSoftvol;		///< config use software volume
static char ConfigAudioNormalize;	///< config use normalize volume
static int ConfigAudioMaxNormalize;	///< config max normalize factor
static char ConfigAudioCompression;	///< config use volume compression
static int ConfigAudioMaxCompression;	///< config max volume compression
static int ConfigAudioStereoDescent;	///< config reduce stereo loudness
int ConfigAudioBufferTime;			///< config size ms of audio buffer
static int ConfigAudioAutoAES;		///< config automatic AES handling
static int ConfigAudioEq;			///< config equalizer filter 
static int SetupAudioEqBand[18];	///< config equalizer filter bands

static volatile int DoMakePrimary;	///< switch primary device to this

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin OSD class.
*/
class cSoftOsd:public cOsd
{
  public:
    static volatile char Dirty;		///< flag force redraw everything
    int OsdLevel;			///< current osd level FIXME: remove

     cSoftOsd(int, int, uint);		///< osd constructor
     virtual ~ cSoftOsd(void);		///< osd destructor
    /// set the sub-areas to the given areas
    virtual eOsdError SetAreas(const tArea *, int);
    virtual void Flush(void);		///< commits all data to the hardware
    virtual void SetActive(bool);	///< sets OSD to be the active one
};

volatile char cSoftOsd::Dirty;		///< flag force redraw everything

/**
**	Sets this OSD to be the active one.
**
**	@param on	true on, false off
**
**	@note only needed as workaround for text2skin plugin with
**	undrawn areas.
*/
void cSoftOsd::SetActive(bool on)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev] OSD %s: %d level %d\n", __FUNCTION__, on, OsdLevel);
#endif

    if (Active() == on) {
	return;				// already active, no action
    }
    cOsd::SetActive(on);

    if (on) {
	Dirty = 1;
	// only flush here if there are already bitmaps
	if (GetBitmap(0)) {
	    Flush();
	}
    } else {
	OsdClose();
    }
}

/**
**	Constructor OSD.
**
**	Initializes the OSD with the given coordinates.
**
**	@param left	x-coordinate of osd on display
**	@param top	y-coordinate of osd on display
**	@param level	level of the osd (smallest is shown)
*/
cSoftOsd::cSoftOsd(int left, int top, uint level)
:cOsd(left, top, level)
{
#ifdef OSD_DEBUG
    /* FIXME: OsdWidth/OsdHeight not correct!
     */
    dsyslog("[softhddev] OSD %s: %dx%d%+d%+d, %d\n", __FUNCTION__, OsdWidth(),
	OsdHeight(), left, top, level);
#endif

    OsdLevel = level;
}

/**
**	OSD Destructor.
**
**	Shuts down the OSD.
*/
cSoftOsd::~cSoftOsd(void)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev] OSD %s: level %d\n", __FUNCTION__, OsdLevel);
#endif

    SetActive(false);
    // done by SetActive: OsdClose();
}

/**
**	Set the sub-areas to the given areas
*/
eOsdError cSoftOsd::SetAreas(const tArea * areas, int n)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev] OSD %s: %d areas \n", __FUNCTION__, n);
#endif

    // clear old OSD, when new areas are set
    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

	for (i = 0; (bitmap = GetBitmap(i)); i++) {
	    bitmap->Clean();
	}
    }
    if (Active()) {
	VideoOsdClear();
	Dirty = 1;
    }
    return cOsd::SetAreas(areas, n);
}

/**
**	Actually commits all data to the OSD hardware.
*/
void cSoftOsd::Flush(void)
{
    cPixmapMemory *pm;

#ifdef OSD_DEBUG
    dsyslog("[softhddev] OSD %s: level %d active %d\n", __FUNCTION__, OsdLevel,
	Active());
#endif

    if (!Active()) {			// this osd is not active
	return;
    }

    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

#ifdef OSD_DEBUG
	static char warned;

	if (!warned) {
	    dsyslog("[softhddev] OSD %s: FIXME: should be truecolor\n",
		__FUNCTION__);
	    warned = 1;
	}
#endif
	// draw all bitmaps
	for (i = 0; (bitmap = GetBitmap(i)); ++i) {
	    uint8_t *argb;
	    int xs;
	    int ys;
	    int x;
	    int y;
	    int w;
	    int h;
	    int x1;
	    int y1;
	    int x2;
	    int y2;

	    // get dirty bounding box
	    if (Dirty) {		// forced complete update
		x1 = 0;
		y1 = 0;
		x2 = bitmap->Width() - 1;
		y2 = bitmap->Height() - 1;
	    } else if (!bitmap->Dirty(x1, y1, x2, y2)) {
		continue;		// nothing dirty continue
	    }
	    // convert and upload only visible dirty areas
	    xs = bitmap->X0() + Left();
	    ys = bitmap->Y0() + Top();
	    // FIXME: negtative position bitmaps
	    w = x2 - x1 + 1;
	    h = y2 - y1 + 1;
	    // clip to screen
	    if (1) {			// just for the case it makes trouble
		int width;
		int height;
		double video_aspect;

		if (xs < 0) {
		    if (xs + x1 < 0) {
			x1 -= xs + x1;
			w += xs + x1;
			if (w <= 0) {
			    continue;
			}
		    }
		    xs = 0;
		}
		if (ys < 0) {
		    if (ys + y1 < 0) {
			y1 -= ys + y1;
			h += ys + y1;
			if (h <= 0) {
			    continue;
			}
		    }
		    ys = 0;
		}
		::GetScreenSize(&width, &height, &video_aspect);
		if (w > width - xs - x1) {
		    w = width - xs - x1;
		    if (w <= 0) {
			continue;
		    }
		    x2 = x1 + w - 1;
		}
		if (h > height - ys - y1) {
		    h = height - ys - y1;
		    if (h <= 0) {
			continue;
		    }
		    y2 = y1 + h - 1;
		}
	    }
#ifdef DEBUG
	    if (w > bitmap->Width() || h > bitmap->Height()) {
		esyslog(tr("[softhddev]: dirty area too big\n"));
		abort();
	    }
#endif
	    argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));
	    for (y = y1; y <= y2; ++y) {
		for (x = x1; x <= x2; ++x) {
		    ((uint32_t *) argb)[x - x1 + (y - y1) * w] =
			bitmap->GetColor(x, y);
		}
	    }
#ifdef OSD_DEBUG
	    dsyslog("[softhddev] OSD %s: draw %dx%d%+d%+d bm\n", __FUNCTION__, w, h,
		xs + x1, ys + y1);
#endif
	    OsdDrawARGB(0, 0, w, h, w * sizeof(uint32_t), argb, xs + x1,
		ys + y1);

	    bitmap->Clean();
	    // FIXME: reuse argb
	    free(argb);
	}
	Dirty = 0;
	return;
    }

    LOCK_PIXMAPS;
    while ((pm = (dynamic_cast < cPixmapMemory * >(RenderPixmaps())))) {
	int xp;
	int yp;
	int stride;
	int x;
	int y;
	int w;
	int h;

	x = pm->ViewPort().X();
	y = pm->ViewPort().Y();
	w = pm->ViewPort().Width();
	h = pm->ViewPort().Height();
	stride = w * sizeof(tColor);

	// clip to osd
	xp = 0;
	if (x < 0) {
	    xp = -x;
	    w -= xp;
	    x = 0;
	}

	yp = 0;
	if (y < 0) {
	    yp = -y;
	    h -= yp;
	    y = 0;
	}

	if (w > Width() - x) {
	    w = Width() - x;
	}
	if (h > Height() - y) {
	    h = Height() - y;
	}

	x += Left();
	y += Top();

	// clip to screen
	if (1) {			// just for the case it makes trouble
	    // and it can happen!
	    int width;
	    int height;
	    double video_aspect;

	    if (x < 0) {
		w += x;
		xp += -x;
		x = 0;
	    }
	    if (y < 0) {
		h += y;
		yp += -y;
		y = 0;
	    }

	    ::GetScreenSize(&width, &height, &video_aspect);
	    if (w > width - x) {
		w = width - x;
	    }
	    if (h > height - y) {
		h = height - y;
	    }
	}
#ifdef OSD_DEBUG
	dsyslog("[softhddev] OSD %s: draw %dx%d%+d%+d*%d -> %+d%+d %p\n",
	    __FUNCTION__, w, h, xp, yp, stride, x, y, pm->Data());
#endif
	OsdDrawARGB(xp, yp, w, h, stride, pm->Data(), x, y);

	DestroyPixmap(pm);
    }
    Dirty = 0;
}

//////////////////////////////////////////////////////////////////////////////
//	OSD provider
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin OSD provider class.
*/
class cSoftOsdProvider:public cOsdProvider
{
  private:
    static cOsd *Osd;			///< single OSD
  public:
    virtual cOsd * CreateOsd(int, int, uint);
    virtual bool ProvidesTrueColor(void);
    cSoftOsdProvider(void);		///< OSD provider constructor
    //virtual ~cSoftOsdProvider();	///< OSD provider destructor
};

cOsd *cSoftOsdProvider::Osd;		///< single osd

/**
**	Create a new OSD.
**
**	@param left	x-coordinate of OSD
**	@param top	y-coordinate of OSD
**	@param level	layer level of OSD
*/
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev] OSD %s: %d, %d, %d\n", __FUNCTION__, left, top, level);
#endif

    return Osd = new cSoftOsd(left, top, level);
}

/**
**	Check if this OSD provider is able to handle a true color OSD.
**
**	@returns true we are able to handle a true color OSD.
*/
bool cSoftOsdProvider::ProvidesTrueColor(void)
{
    return true;
}

/**
**	Create cOsdProvider class.
*/
cSoftOsdProvider::cSoftOsdProvider(void)
:  cOsdProvider()
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev] OSD %s:\n", __FUNCTION__);
#endif
}

/**
**	Destroy cOsdProvider class.
cSoftOsdProvider::~cSoftOsdProvider()
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
}
*/

//////////////////////////////////////////////////////////////////////////////
//	cMenuSetupPage
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin menu setup page class.
*/
class cMenuSetupSoft:public cMenuSetupPage
{
  protected:
    ///
    /// local copies of global setup variables:
    /// @{
    int General;
    int MakePrimary;
    int HideMainMenuEntry;

    int Video;
    int SWDeinterlacer;

    int Audio;
    int AudioDelay;
    int AudioPassthroughDefault;
    int AudioPassthroughPCM;
    int AudioPassthroughAC3;
    int AudioPassthroughEAC3;
    int AudioDownmix;
    int AudioSoftvol;
    int AudioNormalize;
    int AudioMaxNormalize;
    int AudioCompression;
    int AudioMaxCompression;
    int AudioStereoDescent;
    int AudioBufferTime;
    int AudioAutoAES;
    int AudioFilter;
    int AudioEq;
    int AudioEqBand[18];

    /// @}
  private:
     inline cOsdItem * CollapsedItem(const char *, int &, const char * = NULL);
    void Create(void);			// create sub-menu
  protected:
     virtual void Store(void);
  public:
     cMenuSetupSoft(void);
    virtual eOSState ProcessKey(eKeys);	// handle input
};

/**
**	Create a seperator item.
**
**	@param label	text inside separator
*/
static inline cOsdItem *SeparatorItem(const char *label)
{
    cOsdItem *item;

    item = new cOsdItem(cString::sprintf("* %s: ", label));
    item->SetSelectable(false);

    return item;
}

/**
**	Create a collapsed item.
**
**	@param label	text inside collapsed
**	@param flag	flag handling collapsed or opened
**	@param msg	open message
*/
inline cOsdItem *cMenuSetupSoft::CollapsedItem(const char *label, int &flag,
    const char *msg)
{
    cOsdItem *item;

    item =
	new cMenuEditBoolItem(cString::sprintf("* %s", label), &flag,
	msg ? msg : tr("show"), tr("hide"));

    return item;
}

/**
**	Create setup menu.
*/
void cMenuSetupSoft::Create(void)
{
    int current;

    current = Current();		// get current menu item index
    Clear();				// clear the menu

    //
    //	general
    //
    Add(CollapsedItem(tr("General"), General));

    if (General) {
	Add(new cMenuEditBoolItem(tr("Make primary device"), &MakePrimary,
		trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Hide main menu entry"),
		&HideMainMenuEntry, trVDR("no"), trVDR("yes")));
	//
	//	osd
	//
    }
    //
    //	video
    //
    Add(CollapsedItem(tr("Video"), Video));
    if (Video) {
	Add(new cMenuEditBoolItem(tr("Use SW Deinterlacer"),
		&SWDeinterlacer, trVDR("no"), trVDR("yes")));
	}
    //
    //	audio
    //
    Add(CollapsedItem(tr("Audio"), Audio));

    if (Audio) {
	Add(new cMenuEditIntItem(tr("Audio/Video delay (ms)"), &AudioDelay,
		-1000, 1000));
	Add(new cMenuEditBoolItem(tr("Pass-through default"),
		&AudioPassthroughDefault, trVDR("off"), trVDR("on")));
	Add(new cMenuEditBoolItem(tr("\040\040PCM pass-through"),
		&AudioPassthroughPCM, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("\040\040AC-3 pass-through"),
		&AudioPassthroughAC3, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("\040\040E-AC-3 pass-through"),
		&AudioPassthroughEAC3, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Enable (E-)AC-3 (decoder) downmix"),
		&AudioDownmix, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Volume control"), &AudioSoftvol,
		tr("Hardware"), tr("Software")));
	Add(new cMenuEditBoolItem(tr("Enable normalize volume"),
		&AudioNormalize, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditIntItem(tr("  Max normalize factor (/1000)"),
		&AudioMaxNormalize, 0, 10000));
	Add(new cMenuEditBoolItem(tr("Enable volume compression"),
		&AudioCompression, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditIntItem(tr("  Max compression factor (/1000)"),
		&AudioMaxCompression, 0, 10000));
	Add(new cMenuEditIntItem(tr("Reduce stereo volume (/1000)"),
		&AudioStereoDescent, 0, 1000));
	Add(new cMenuEditIntItem(tr("Audio buffer size (ms)"),
		&AudioBufferTime, 0, 1000));
	Add(new cMenuEditBoolItem(tr("Enable automatic AES"), &AudioAutoAES,
		trVDR("no"), trVDR("yes")));
    }
    Add(CollapsedItem(tr("Audio Filter"), AudioFilter));
    if (AudioFilter) {
		Add(new cMenuEditBoolItem(tr(" Enable Audio Equalizer"), &AudioEq,
			trVDR("no"), trVDR("yes")));
		if (AudioEq) {
			Add(new cMenuEditIntItem(tr("  65 Hz band gain"),
				&AudioEqBand[0], -10, 10));
			Add(new cMenuEditIntItem(tr("  92 Hz band gain"),
				&AudioEqBand[1], -10, 10));
			Add(new cMenuEditIntItem(tr("  131 Hz band gain"),
				&AudioEqBand[2], -10, 10));
			Add(new cMenuEditIntItem(tr("  185 Hz band gain"),
				&AudioEqBand[3], -10, 10));
			Add(new cMenuEditIntItem(tr("  262 Hz band gain"),
				&AudioEqBand[4], -10, 10));
			Add(new cMenuEditIntItem(tr("  370 Hz band gain"),
				&AudioEqBand[5], -10, 10));
			Add(new cMenuEditIntItem(tr("  523 Hz band gain"),
				&AudioEqBand[6], -10, 10));
			Add(new cMenuEditIntItem(tr("  740 Hz band gain"),
				&AudioEqBand[7], -10, 10));
			Add(new cMenuEditIntItem(tr("  1047 Hz band gain"),
				&AudioEqBand[8], -10, 10));
			Add(new cMenuEditIntItem(tr("  1480 Hz band gain"),
				&AudioEqBand[9], -10, 10));
			Add(new cMenuEditIntItem(tr("  2093 Hz band gain"),
				&AudioEqBand[10], -10, 10));
			Add(new cMenuEditIntItem(tr("  2960 Hz band gain"),
				&AudioEqBand[11], -10, 10));
			Add(new cMenuEditIntItem(tr("  4186 Hz band gain"),
				&AudioEqBand[12], -10, 10));
			Add(new cMenuEditIntItem(tr("  5920 Hz band gain"),
				&AudioEqBand[13], -10, 10));
			Add(new cMenuEditIntItem(tr("  8372 Hz band gain"),
				&AudioEqBand[14], -10, 10));
			Add(new cMenuEditIntItem(tr("  11840 Hz band gain"),
				&AudioEqBand[15], -10, 10));
			Add(new cMenuEditIntItem(tr("  16744 Hz band gain"),
				&AudioEqBand[16], -10, 10));
			Add(new cMenuEditIntItem(tr("  20000 Hz band gain"),
				&AudioEqBand[17], -10, 10));
		}
	}

    SetCurrent(Get(current));		// restore selected menu entry
    Display();				// display build menu
}

/**
**	Process key for setup menu.
*/
eOSState cMenuSetupSoft::ProcessKey(eKeys key)
{
    int old_general = General;
    int old_video = Video;
    int old_audio = Audio;
    int old_audiofilter = AudioFilter;
    int old_audioeq = AudioEq;
    eOSState state = cMenuSetupPage::ProcessKey(key);

    if (key != kNone) {
		// update menu only, if something on the structure has changed
		// this is needed because VDR menus are evil slow
		if (old_general != General || old_video != Video ||
			old_audio != Audio || old_audiofilter != AudioFilter ||
			old_audioeq != AudioEq) {
			Create();			// update menu
		}
    }

    return state;
}

/**
**	Constructor setup menu.
**
**	Import global config variables into setup.
*/
cMenuSetupSoft::cMenuSetupSoft(void)
{
    //
    //	general
    //
    General = 0;
    MakePrimary = ConfigMakePrimary;
    HideMainMenuEntry = ConfigHideMainMenuEntry;

    //
    //	video
    //
    Video = 0;
    SWDeinterlacer = ConfigSWDeinterlacer;

    //
    //	audio
    //
    Audio = 0;
    AudioDelay = ConfigVideoAudioDelay;
    AudioPassthroughDefault = AudioPassthroughState;
    AudioPassthroughPCM = ConfigAudioPassthrough & CodecPCM;
    AudioPassthroughAC3 = ConfigAudioPassthrough & CodecAC3;
    AudioPassthroughEAC3 = ConfigAudioPassthrough & CodecEAC3;
    AudioDownmix = ConfigAudioDownmix;
    AudioSoftvol = ConfigAudioSoftvol;
    AudioNormalize = ConfigAudioNormalize;
    AudioMaxNormalize = ConfigAudioMaxNormalize;
    AudioCompression = ConfigAudioCompression;
    AudioMaxCompression = ConfigAudioMaxCompression;
    AudioStereoDescent = ConfigAudioStereoDescent;
    AudioBufferTime = ConfigAudioBufferTime;
    AudioAutoAES = ConfigAudioAutoAES;
	//
	// audio filter
	//
    AudioEq = ConfigAudioEq;
    AudioFilter = 0;
    for (int i = 0; i < 18; i++) {
		AudioEqBand[i] = SetupAudioEqBand[i];
	}

    Create();
}

/**
**	Store setup.
*/
void cMenuSetupSoft::Store(void)
{
    SetupStore("MakePrimary", ConfigMakePrimary = MakePrimary);
    SetupStore("HideMainMenuEntry", ConfigHideMainMenuEntry = HideMainMenuEntry);

    SetupStore("SWDeinterlacer", ConfigSWDeinterlacer = SWDeinterlacer);
	VideoSetSWDeinterlacer(ConfigSWDeinterlacer);

    SetupStore("AudioDelay", ConfigVideoAudioDelay = AudioDelay);
    VideoSetAudioDelay(ConfigVideoAudioDelay);

    // FIXME: can handle more audio state changes here
    // downmix changed reset audio, to get change direct
    if (ConfigAudioDownmix != AudioDownmix) {
	ResetChannelId();
    }
    ConfigAudioPassthrough = (AudioPassthroughPCM ? CodecPCM : 0)
	| (AudioPassthroughAC3 ? CodecAC3 : 0)
	| (AudioPassthroughEAC3 ? CodecEAC3 : 0);
    AudioPassthroughState = AudioPassthroughDefault;
    if (AudioPassthroughState) {
	SetupStore("AudioPassthrough", ConfigAudioPassthrough);
	CodecSetAudioPassthrough(ConfigAudioPassthrough);
    } else {
	SetupStore("AudioPassthrough", -ConfigAudioPassthrough);
	CodecSetAudioPassthrough(0);
    }
    SetupStore("AudioDownmix", ConfigAudioDownmix = AudioDownmix);
    CodecSetAudioDownmix(ConfigAudioDownmix);
    SetupStore("AudioSoftvol", ConfigAudioSoftvol = AudioSoftvol);
    AudioSetSoftvol(ConfigAudioSoftvol);
    SetupStore("AudioNormalize", ConfigAudioNormalize = AudioNormalize);
    SetupStore("AudioMaxNormalize", ConfigAudioMaxNormalize =
	AudioMaxNormalize);
    AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
    SetupStore("AudioCompression", ConfigAudioCompression = AudioCompression);
    SetupStore("AudioMaxCompression", ConfigAudioMaxCompression =
	AudioMaxCompression);
    AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
    SetupStore("AudioStereoDescent", ConfigAudioStereoDescent =
	AudioStereoDescent);
    AudioSetStereoDescent(ConfigAudioStereoDescent);
    SetupStore("AudioBufferTime", ConfigAudioBufferTime = AudioBufferTime);
    SetupStore("AudioAutoAES", ConfigAudioAutoAES = AudioAutoAES);
    AudioSetAutoAES(ConfigAudioAutoAES);
	SetupStore("AudioEq", ConfigAudioEq = AudioEq);
	SetupStore("AudioEqBand01b", SetupAudioEqBand[0] = AudioEqBand[0]);
	SetupStore("AudioEqBand02b", SetupAudioEqBand[1] = AudioEqBand[1]);
	SetupStore("AudioEqBand03b", SetupAudioEqBand[2] = AudioEqBand[2]);
	SetupStore("AudioEqBand04b", SetupAudioEqBand[3] = AudioEqBand[3]);
	SetupStore("AudioEqBand05b", SetupAudioEqBand[4] = AudioEqBand[4]);
	SetupStore("AudioEqBand06b", SetupAudioEqBand[5] = AudioEqBand[5]);
	SetupStore("AudioEqBand07b", SetupAudioEqBand[6] = AudioEqBand[6]);
	SetupStore("AudioEqBand08b", SetupAudioEqBand[7] = AudioEqBand[7]);
	SetupStore("AudioEqBand09b", SetupAudioEqBand[8] = AudioEqBand[8]);
	SetupStore("AudioEqBand10b", SetupAudioEqBand[9] = AudioEqBand[9]);
	SetupStore("AudioEqBand11b", SetupAudioEqBand[10] = AudioEqBand[10]);
	SetupStore("AudioEqBand12b", SetupAudioEqBand[11] = AudioEqBand[11]);
	SetupStore("AudioEqBand13b", SetupAudioEqBand[12] = AudioEqBand[12]);
	SetupStore("AudioEqBand14b", SetupAudioEqBand[13] = AudioEqBand[13]);
	SetupStore("AudioEqBand15b", SetupAudioEqBand[14] = AudioEqBand[14]);
	SetupStore("AudioEqBand16b", SetupAudioEqBand[15] = AudioEqBand[15]);
	SetupStore("AudioEqBand17b", SetupAudioEqBand[16] = AudioEqBand[16]);
	SetupStore("AudioEqBand18b", SetupAudioEqBand[17] = AudioEqBand[17]);
    AudioSetEq(SetupAudioEqBand, ConfigAudioEq);
}


//////////////////////////////////////////////////////////////////////////////
//	cOsdMenu
//////////////////////////////////////////////////////////////////////////////

/**
**	Hotkey parsing state machine.
*/
typedef enum
{
    HksInitial,				///< initial state
    HksBlue,				///< blue button pressed
    HksBlue1,				///< blue and 1 number pressed
    HksRed,				///< red button pressed
} HkState;

/**
**	Soft device plugin menu class.
*/
class cSoftHdMenu:public cOsdMenu
{
  private:
    HkState HotkeyState;		///< current hot-key state
    int HotkeyCode;			///< current hot-key code
    void Create(void);			///< create plugin main menu
  public:
    cSoftHdMenu(const char *, int = 0, int = 0, int = 0, int = 0, int = 0);
    virtual ~ cSoftHdMenu();
    virtual eOSState ProcessKey(eKeys);
};

/**
**	Create main menu.
*/
void cSoftHdMenu::Create(void)
{
    int current;
    int missed;
    int duped;
    int dropped;
    int counter;

    current = Current();		// get current menu item index
    Clear();				// clear the menu

    SetHasHotkeys();

    Add(new cOsdItem(NULL, osUnknown, false));
    Add(new cOsdItem(NULL, osUnknown, false));
    GetStats(&missed, &duped, &dropped, &counter);
    Add(new
	cOsdItem(cString::sprintf(tr
		(" Frames missed(%d) duped(%d) dropped(%d) total(%d)"), missed,
		duped, dropped, counter), osUnknown, false));

    SetCurrent(Get(current));		// restore selected menu entry
    Display();				// display build menu
}

/**
**	Soft device menu constructor.
*/
cSoftHdMenu::cSoftHdMenu(const char *title, int c0, int c1, int c2, int c3,
    int c4)
:cOsdMenu(title, c0, c1, c2, c3, c4)
{
    HotkeyState = HksInitial;

    Create();
}

/**
**	Soft device menu destructor.
*/
cSoftHdMenu::~cSoftHdMenu()
{
}

/**
**	Handle hot key commands.
**
**	@param code	numeric hot key code
*/
static void HandleHotkey(int code)
{
    switch (code) {
	case 10:			// disable pass-through
	    AudioPassthroughState = 0;
	    CodecSetAudioPassthrough(0);
	    Skins.QueueMessage(mtInfo, tr("pass-through disabled"));
	    break;
	case 11:			// enable pass-through
	    // note: you can't enable, without configured pass-through
	    AudioPassthroughState = 1;
	    CodecSetAudioPassthrough(ConfigAudioPassthrough);
	    Skins.QueueMessage(mtInfo, tr("pass-through enabled"));
	    break;
	case 12:			// toggle pass-through
	    AudioPassthroughState ^= 1;
	    if (AudioPassthroughState) {
		CodecSetAudioPassthrough(ConfigAudioPassthrough);
		Skins.QueueMessage(mtInfo, tr("pass-through enabled"));
	    } else {
		CodecSetAudioPassthrough(0);
		Skins.QueueMessage(mtInfo, tr("pass-through disabled"));
	    }
	    break;
	case 13:			// decrease audio delay
	    ConfigVideoAudioDelay -= 10;
	    VideoSetAudioDelay(ConfigVideoAudioDelay);
	    Skins.QueueMessage(mtInfo,
		cString::sprintf(tr("audio delay changed to %d"),
		    ConfigVideoAudioDelay));
	    break;
	case 14:			// increase audio delay
	    ConfigVideoAudioDelay += 10;
	    VideoSetAudioDelay(ConfigVideoAudioDelay);
	    Skins.QueueMessage(mtInfo,
		cString::sprintf(tr("audio delay changed to %d"),
		    ConfigVideoAudioDelay));
	    break;
	case 15:
	    ConfigAudioDownmix ^= 1;
	    fprintf(stderr, "toggle downmix\n");
	    CodecSetAudioDownmix(ConfigAudioDownmix);
	    if (ConfigAudioDownmix) {
		Skins.QueueMessage(mtInfo, tr("surround downmix enabled"));
	    } else {
		Skins.QueueMessage(mtInfo, tr("surround downmix disabled"));
	    }
	    ResetChannelId();
	    break;
	default:
	    esyslog(tr("[softhddev]: hot key %d is not supported\n"), code);
	    break;
    }
}

/**
**	Handle key event.
**
**	@param key	key event
*/
eOSState cSoftHdMenu::ProcessKey(eKeys key)
{
    eOSState state;

    //dsyslog("[softhddev]%s: %x\n", __FUNCTION__, key);

    switch (HotkeyState) {
	case HksInitial:		// initial state, waiting for hot key
	    if (key == kBlue) {
		HotkeyState = HksBlue;	// blue button
		return osContinue;
	    }
	    if (key == kRed) {
		HotkeyState = HksRed;	// red button
		return osContinue;
	    }
	    break;
	case HksBlue:			// blue and first number
	    if (k0 <= key && key <= k9) {
		HotkeyCode = key - k0;
		HotkeyState = HksBlue1;
		return osContinue;
	    }
	    HotkeyState = HksInitial;
	    break;
	case HksBlue1:			// blue and second number/enter
	    if (k0 <= key && key <= k9) {
		HotkeyCode *= 10;
		HotkeyCode += key - k0;
		HotkeyState = HksInitial;
		dsyslog("[softhddev]%s: hot-key %d\n", __FUNCTION__,
		    HotkeyCode);
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    if (key == kOk) {
		HotkeyState = HksInitial;
		dsyslog("[softhddev]%s: hot-key %d\n", __FUNCTION__,
		    HotkeyCode);
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    HotkeyState = HksInitial;
	case HksRed:			// red and first number
	    if (k0 <= key && key <= k9) {
		HotkeyCode = 100 + key - k0;
		HotkeyState = HksInitial;
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    HotkeyState = HksInitial;
	    break;
    }

    // call standard function
    state = cOsdMenu::ProcessKey(key);

    switch (state) {
	default:
	    Create();
	    break;
    }
    return state;
}

//////////////////////////////////////////////////////////////////////////////
//	cDevice
//////////////////////////////////////////////////////////////////////////////

class cSoftHdDevice:public cDevice
{
  public:
    cSoftHdDevice(void);
    virtual ~ cSoftHdDevice(void);

    virtual bool HasDecoder(void) const;
    virtual bool CanReplay(void) const;
    virtual bool SetPlayMode(ePlayMode);
    virtual void TrickSpeed(int, bool);
    virtual void Clear(void);
    virtual void Play(void);
    virtual void Freeze(void);
    virtual void Mute(void);
    virtual void StillPicture(const uchar *, int);
    virtual bool Poll(cPoller &, int = 0);
    virtual bool Flush(int = 0);
    virtual int64_t GetSTC(void);
    virtual void SetVideoDisplayFormat(eVideoDisplayFormat);
    virtual void SetVideoFormat(bool);
    virtual void GetVideoSize(int &, int &, double &);
    virtual void GetOsdSize(int &, int &, double &);
    virtual int PlayVideo(const uchar *, int);
    virtual int PlayAudio(const uchar *, int, uchar);
#ifdef USE_TS_VIDEO
    virtual int PlayTsVideo(const uchar *, int);
#endif
#if !defined(USE_AUDIO_THREAD) || !defined(NO_TS_AUDIO)
    virtual int PlayTsAudio(const uchar *, int);
#endif
    virtual void SetAudioChannelDevice(int);
    virtual int GetAudioChannelDevice(void);
    virtual void SetDigitalAudioDevice(bool);
    virtual void SetAudioTrackDevice(eTrackType);
    virtual void SetVolumeDevice(int);

// Image Grab facilities

    virtual uchar *GrabImage(int &, bool, int, int, int);

// SPU facilities
  private:
    cDvbSpuDecoder * spuDecoder;
  public:
    virtual cSpuDecoder * GetSpuDecoder(void);

  protected:
    virtual void MakePrimaryDevice(bool);
};

/**
**	Constructor device.
*/
cSoftHdDevice::cSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s\n", __FUNCTION__);
    spuDecoder = NULL;
}

/**
**	Destructor device.
*/
cSoftHdDevice::~cSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
    delete spuDecoder;
}

/**
**	Informs a device that it will be the primary device.
**
**	@param on	flag if becoming or loosing primary
*/
void cSoftHdDevice::MakePrimaryDevice(bool on)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, on);

    cDevice::MakePrimaryDevice(on);
    if (on) {
	new cSoftOsdProvider();
    }
}


/**
**	Get the device SPU decoder.
**
**	@returns a pointer to the device's SPU decoder (or NULL, if this
**	device doesn't have an SPU decoder)
*/
cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (!spuDecoder && IsPrimaryDevice()) {
	spuDecoder = new cDvbSpuDecoder();
    }
    return spuDecoder;
}


/**
**	Tells whether this device has a MPEG decoder.
*/
bool cSoftHdDevice::HasDecoder(void) const
{
    return true;
}

/**
**	Returns true if this device can currently start a replay session.
*/
bool cSoftHdDevice::CanReplay(void) const
{
    return true;
}

/**
**	Sets the device into the given play mode.
**
**	@param play_mode	new play mode (Audio/Video/External...)
*/
bool cSoftHdDevice::SetPlayMode(ePlayMode play_mode)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, play_mode);

    switch (play_mode) {
	case pmAudioVideo:
	    break;
	case pmAudioOnly:
	case pmAudioOnlyBlack:
	    break;
	case pmVideoOnly:
	    break;
	case pmNone:
	    break;
	default:
	    dsyslog("[softhddev] playmode not implemented... %d\n", play_mode);
	    break;
    }

    return::SetPlayMode(play_mode);
}

/**
**	Gets the current System Time Counter, which can be used to
**	synchronize audio, video and subtitles.
*/
int64_t cSoftHdDevice::GetSTC(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::GetSTC();
}

/**
**	Set trick play speed.
**
**	Every single frame shall then be displayed the given number of
**	times.
**
**	@param speed	trick speed
**	@param forward	flag forward direction
*/
void cSoftHdDevice::TrickSpeed(int speed, bool forward)
{
    dsyslog("[softhddev]%s: %d %d\n", __FUNCTION__, speed, forward);

    ::TrickSpeed(speed);
}

/**
**	Clears all video and audio data from the device.
*/
void cSoftHdDevice::Clear(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Clear();
    ::Clear();
}

/**
**	Sets the device into play mode (after a previous trick mode)
*/
void cSoftHdDevice::Play(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Play();
    ::Play();
}

/**
**	Puts the device into "freeze frame" mode.
*/
void cSoftHdDevice::Freeze(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
	fprintf(stderr, "[softhddev] Freeze\n");

    cDevice::Freeze();
    ::Freeze();
}

/**
**	Turns off audio while replaying.
*/
void cSoftHdDevice::Mute(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Mute();
    ::Mute();
}

/**
**	Display the given I-frame as a still picture.
**
**	@param data	pes or ts data of a frame
**	@param length	length of data area
*/
void cSoftHdDevice::StillPicture(const uchar * data, int length)
{
    dsyslog("[softhddev]%s: %s %p %d\n", __FUNCTION__,
	data[0] == 0x47 ? "ts" : "pes", data, length);

    if (data[0] == 0x47) {		// ts sync
	cDevice::StillPicture(data, length);
	return;
    }

    ::StillPicture(data, length);
}

/**
**	Check if the device is ready for further action.
**
**	@param poller		file handles (unused)
**	@param timeout_ms	timeout in ms to become ready
**
**	@retval true	if ready
**	@retval false	if busy
*/
bool cSoftHdDevice::Poll(
    __attribute__ ((unused)) cPoller & poller, int timeout_ms)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, timeout_ms);
//	fprintf(stderr, "[softhddev] timeout %dms \n", timeout_ms);

    return::Poll(timeout_ms);
}

/**
**	Flush the device output buffers.
**
**	@param timeout_ms	timeout in ms to become ready
*/
bool cSoftHdDevice::Flush(int timeout_ms)
{
    dsyslog("[softhddev]%s: %d ms\n", __FUNCTION__, timeout_ms);

    return::Flush(timeout_ms);
}

// ----------------------------------------------------------------------------

/**
**	Sets the video display format to the given one (only useful if this
**	device has an MPEG decoder).
*/
void cSoftHdDevice:: SetVideoDisplayFormat(eVideoDisplayFormat
    video_display_format)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, video_display_format);

    cDevice::SetVideoDisplayFormat(video_display_format);
#if 0
    static int last = -1;

    // called on every channel switch, no need to kill osd...
    if (last != video_display_format) {
	last = video_display_format;

	::VideoSetDisplayFormat(video_display_format);
	cSoftOsd::Dirty = 1;
    }
#endif
}

/**
**	Sets the output video format to either 16:9 or 4:3 (only useful
**	if this device has an MPEG decoder).
**
**	Should call SetVideoDisplayFormat.
**
**	@param video_format16_9	flag true 16:9.
*/
void cSoftHdDevice::SetVideoFormat(bool video_format16_9)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, video_format16_9);

    // FIXME: 4:3 / 16:9 video format not supported.

    SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
}

/**
**	Returns the width, height and video_aspect ratio of the currently
**	displayed video material.
**
**	@note the video_aspect is used to scale the subtitle.
*/
void cSoftHdDevice::GetVideoSize(int &width, int &height, double &video_aspect)
{
    ::GetScreenSize(&width, &height, &video_aspect);
}

/**
**	Returns the width, height and pixel_aspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
{
    ::GetScreenSize(&width, &height, &pixel_aspect);
}

// ----------------------------------------------------------------------------

/**
**	Play a audio packet.
**
**	@param data	exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
**	@param id	type of audio data this packet holds
*/
int cSoftHdDevice::PlayAudio(const uchar * data, int length, uchar id)
{
    //dsyslog("[softhddev]%s: %p %p %d %d\n", __FUNCTION__, this, data, length, id);

    return::PlayAudio(data, length, id);
}

void cSoftHdDevice::SetAudioTrackDevice(
    __attribute__ ((unused)) eTrackType type)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

void cSoftHdDevice::SetDigitalAudioDevice( __attribute__ ((unused)) bool on)
{
    //dsyslog("[softhddev]%s: %s\n", __FUNCTION__, on ? "true" : "false");
}

void cSoftHdDevice::SetAudioChannelDevice( __attribute__ ((unused))
    int audio_channel)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, audio_channel);
}

int cSoftHdDevice::GetAudioChannelDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
    return 0;
}

/**
**	Sets the audio volume on this device (Volume = 0...255).
**
**	@param volume	device volume
*/
void cSoftHdDevice::SetVolumeDevice(int volume)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, volume);

    ::SetVolumeDevice(volume);
}

// ----------------------------------------------------------------------------

/**
**	Play a video packet.
**
**	@param data		exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
*/
int cSoftHdDevice::PlayVideo(const uchar * data, int length)
{
    //dsyslog("[softhddev]%s: %p %d\n", __FUNCTION__, data, length);
    return::PlayVideo(data, length);
}

#ifdef USE_TS_VIDEO

/**
**	Play a TS video packet.
**
**	@param data		ts data buffer
**	@param length	ts packet length (188)
*/
int cSoftHdDevice::PlayTsVideo(const uchar * data, int length)
{
}

#endif

#if !defined(USE_AUDIO_THREAD) || !defined(NO_TS_AUDIO)

/**
**	Play a TS audio packet.
**
**	@param data		ts data buffer
**	@param length	ts packet length (188)
*/
int cSoftHdDevice::PlayTsAudio(const uchar * data, int length)
{
#ifndef NO_TS_AUDIO
    return::PlayTsAudio(data, length);
#else
    AudioPoller();

    return cDevice::PlayTsAudio(data, length);
#endif
}

#endif

/**
**	Grabs the currently visible screen image.
**
**	@param size	size of the returned data
**	@param jpeg	flag true, create JPEG data
**	@param quality	JPEG quality
**	@param width	number of horizontal pixels in the frame
**	@param height	number of vertical pixels in the frame
*/
uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int width,
    int height)
{
    dsyslog("[softhddev]%s: %d, %d, %d, %dx%d\n", __FUNCTION__, size, jpeg,
	quality, width, height);

    if (quality < 0) {			// caller should care, but fix it
	quality = 95;
    }

    return::GrabImage(&size, jpeg, quality, width, height);
}

/**
**	Call rgb to jpeg for C Plugin.
*/
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
    int width, int height)
{
    return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size,
	quality);
}

//////////////////////////////////////////////////////////////////////////////
//	cPlugin
//////////////////////////////////////////////////////////////////////////////

class cPluginSoftHdDevice:public cPlugin
{
  public:
    cPluginSoftHdDevice(void);
    virtual ~ cPluginSoftHdDevice(void);
    virtual const char *Version(void);
    virtual const char *Description(void);
    virtual const char *CommandLineHelp(void);
    virtual bool ProcessArgs(int, char *[]);
    virtual bool Initialize(void);
    virtual bool Start(void);
    virtual void Stop(void);
    virtual void Housekeeping(void);
    virtual void MainThreadHook(void);
    virtual const char *MainMenuEntry(void);
    virtual cOsdObject *MainMenuAction(void);
    virtual cMenuSetupPage *SetupMenu(void);
    virtual bool SetupParse(const char *, const char *);
    virtual bool Service(const char *, void * = NULL);
    virtual const char **SVDRPHelpPages(void);
    virtual cString SVDRPCommand(const char *, const char *, int &);
};

/**
**	Initialize any member variables here.
**
**	@note DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
**	VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
*/
cPluginSoftHdDevice::cPluginSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

/**
**	Clean up after yourself!
*/
cPluginSoftHdDevice::~cPluginSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::SoftHdDeviceExit();
}

/**
**	Return plugin version number.
**
**	@returns version number as constant string.
*/
const char *cPluginSoftHdDevice::Version(void)
{
    return VERSION;
}

/**
**	Return plugin short description.
**
**	@returns short description as constant string.
*/
const char *cPluginSoftHdDevice::Description(void)
{
    return tr(DESCRIPTION);
}

/**
**	Return a string that describes all known command line options.
**
**	@returns command line help as constant string.
*/
const char *cPluginSoftHdDevice::CommandLineHelp(void)
{
    return::CommandLineHelp();
}

/**
**	Process the command line arguments.
*/
bool cPluginSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::ProcessArgs(argc, argv);
}

/**
**	Initializes the DVB devices.
**
**	Must be called before accessing any DVB functions.
**
**	@returns true if any devices are available.
*/
bool cPluginSoftHdDevice::Initialize(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    MyDevice = new cSoftHdDevice();

    return true;
}

/**
**	 Start any background activities the plugin shall perform.
*/
bool cPluginSoftHdDevice::Start(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (!MyDevice->IsPrimaryDevice()) {
	isyslog("[softhddev] softhddevice %d is not the primary device!",
	    MyDevice->DeviceNumber());
	if (ConfigMakePrimary) {
	    // Must be done in the main thread
	    dsyslog("[softhddev] makeing softhddevice %d the primary device!",
		MyDevice->DeviceNumber());
	    DoMakePrimary = MyDevice->DeviceNumber() + 1;
	}
    }
    ::Start();

    return true;
}

/**
**	Shutdown plugin.  Stop any background activities the plugin is
**	performing.
*/
void cPluginSoftHdDevice::Stop(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::Stop();
}

/**
**	Perform any cleanup or other regular tasks.
*/
void cPluginSoftHdDevice::Housekeeping(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::Housekeeping();
}

/**
**	Create main menu entry.
*/
const char *cPluginSoftHdDevice::MainMenuEntry(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
**	Perform the action when selected from the main VDR menu.
*/
cOsdObject *cPluginSoftHdDevice::MainMenuAction(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return new cSoftHdMenu("SoftHdDevice");
}

/**
**	Called for every plugin once during every cycle of VDR's main program
**	loop.
*/
void cPluginSoftHdDevice::MainThreadHook(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (DoMakePrimary) {
	dsyslog("[softhddev]%s: switching primary device to %d\n",
	    __FUNCTION__, DoMakePrimary);
	cDevice::SetPrimaryDevice(DoMakePrimary);
	DoMakePrimary = 0;
    }

    ::MainThreadHook();
}

/**
**	Return our setup menu.
*/
cMenuSetupPage *cPluginSoftHdDevice::SetupMenu(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return new cMenuSetupSoft;
}

/**
**	Parse setup parameters
**
**	@param name	paramter name (case sensetive)
**	@param value	value as string
**
**	@returns true if the parameter is supported.
*/
bool cPluginSoftHdDevice::SetupParse(const char *name, const char *value)
{
    //dsyslog("[softhddev]%s: '%s' = '%s'\n", __FUNCTION__, name, value);

    if (!strcasecmp(name, "MakePrimary")) {
	ConfigMakePrimary = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "HideMainMenuEntry")) {
	ConfigHideMainMenuEntry = atoi(value);
	return true;
    }

    if (!strcasecmp(name, "SWDeinterlacer")) {
	VideoSetSWDeinterlacer(ConfigSWDeinterlacer = atoi(value));
	return true;
    }

    if (!strcasecmp(name, "AudioDelay")) {
	VideoSetAudioDelay(ConfigVideoAudioDelay = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioPassthrough")) {
	int i;

	i = atoi(value);
	AudioPassthroughState = i > 0;
	ConfigAudioPassthrough = abs(i);
	if (AudioPassthroughState) {
	    CodecSetAudioPassthrough(ConfigAudioPassthrough);
	} else {
	    CodecSetAudioPassthrough(0);
	}
	return true;
    }
    if (!strcasecmp(name, "AudioDownmix")) {
	CodecSetAudioDownmix(ConfigAudioDownmix = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioSoftvol")) {
	AudioSetSoftvol(ConfigAudioSoftvol = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioNormalize")) {
	ConfigAudioNormalize = atoi(value);
	AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
	return true;
    }
    if (!strcasecmp(name, "AudioMaxNormalize")) {
	ConfigAudioMaxNormalize = atoi(value);
	AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
	return true;
    }
    if (!strcasecmp(name, "AudioCompression")) {
	ConfigAudioCompression = atoi(value);
	AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
	return true;
    }
    if (!strcasecmp(name, "AudioMaxCompression")) {
	ConfigAudioMaxCompression = atoi(value);
	AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
	return true;
    }
    if (!strcasecmp(name, "AudioStereoDescent")) {
	ConfigAudioStereoDescent = atoi(value);
	AudioSetStereoDescent(ConfigAudioStereoDescent);
	return true;
    }
    if (!strcasecmp(name, "AudioBufferTime")) {
	ConfigAudioBufferTime = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioAutoAES")) {
	ConfigAudioAutoAES = atoi(value);
	AudioSetAutoAES(ConfigAudioAutoAES);
	return true;
    }
    if (!strcasecmp(name, "AudioEq")) {
	ConfigAudioEq = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand01b")) {
	SetupAudioEqBand[0] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand02b")) {
	SetupAudioEqBand[1] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand03b")) {
	SetupAudioEqBand[2] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand04b")) {
	SetupAudioEqBand[3] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand05b")) {
	SetupAudioEqBand[4] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand06b")) {
	SetupAudioEqBand[5] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand07b")) {
	SetupAudioEqBand[6] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand08b")) {
	SetupAudioEqBand[7] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand09b")) {
	SetupAudioEqBand[8] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand10b")) {
	SetupAudioEqBand[9] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand11b")) {
	SetupAudioEqBand[10] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand12b")) {
	SetupAudioEqBand[11] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand13b")) {
	SetupAudioEqBand[12] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand14b")) {
	SetupAudioEqBand[13] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand15b")) {
	SetupAudioEqBand[14] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand16b")) {
	SetupAudioEqBand[15] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand17b")) {
	SetupAudioEqBand[16] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand18b")) {
	SetupAudioEqBand[17] = atoi(value);
	AudioSetEq(SetupAudioEqBand, ConfigAudioEq);
	return true;
    }
    return false;
}

/**
**	Receive requests or messages.
**
**	@param id	unique identification string that identifies the
**			service protocol
**	@param data	custom data structure
*/
bool cPluginSoftHdDevice::Service(const char *id, void *data)
{
    //dsyslog("[softhddev]%s: id %s\n", __FUNCTION__, id);

    if (strcmp(id, ATMO_GRAB_SERVICE) == 0) {
	int width;
	int height;

	if (data == NULL) {
	    return true;
	}

	SoftHDDevice_AtmoGrabService_v1_0_t *r =
	    (SoftHDDevice_AtmoGrabService_v1_0_t *) data;
	if (r->structSize != sizeof(SoftHDDevice_AtmoGrabService_v1_0_t)
	    || r->analyseSize < 64 || r->analyseSize > 256
	    || r->clippedOverscan < 0 || r->clippedOverscan > 200) {
	    return false;
	}

	width = r->analyseSize * -1;	// Internal marker for Atmo grab service
	height = r->clippedOverscan;

	r->img = VideoGrabService(&r->imgSize, &width, &height);
	if (r->img == NULL) {
	    return false;
	}
	r->imgType = GRAB_IMG_RGBA_FORMAT_B8G8R8A8;
	r->width = width;
	r->height = height;
	return true;
    }

    if (strcmp(id, ATMO1_GRAB_SERVICE) == 0) {
	SoftHDDevice_AtmoGrabService_v1_1_t *r;

	if (!data) {
	    return true;
	}

	r = (SoftHDDevice_AtmoGrabService_v1_1_t *) data;
	r->img = VideoGrabService(&r->size, &r->width, &r->height);
	if (!r->img) {
	    return false;
	}

	return true;
    }

    return false;
}

//----------------------------------------------------------------------------
//	cPlugin SVDRP
//----------------------------------------------------------------------------

/**
**	SVDRP commands help text.
**	FIXME: translation?
*/
static const char *SVDRPHelpText[] = {
    "HOTK key\n" "    Execute hotkey.\n\n"
	"    key is the hotkey number, following are supported:\n"
	"    10: disable audio pass-through\n"
	"    11: enable audio pass-through\n"
	"    12: toggle audio pass-through\n"
	"    13: decrease audio delay by 10ms\n"
	"    14: increase audio delay by 10ms\n"
	"    15: toggle ac3 mixdown\n",
    NULL
};

/**
**	Return SVDRP commands help pages.
**
**	return a pointer to a list of help strings for all of the plugin's
**	SVDRP commands.
*/
const char **cPluginSoftHdDevice::SVDRPHelpPages(void)
{
    return SVDRPHelpText;
}

/**
**	Handle SVDRP commands.
**
**	@param command		SVDRP command
**	@param option		all command arguments
**	@param reply_code	reply code
*/
cString cPluginSoftHdDevice::SVDRPCommand(const char *command,
    const char *option, __attribute__ ((unused)) int &reply_code)
{
    if (!strcasecmp(command, "HOTK")) {
	int hotk;

	hotk = strtol(option, NULL, 0);
	HandleHotkey(hotk);
	return "hot-key executed";
    }

    return NULL;
}

VDRPLUGINCREATOR(cPluginSoftHdDevice);	// Don't touch this!
