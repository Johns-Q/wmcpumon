///
///	@file wmcpumon.c	@brief	CPU system monitor dockapp
///
///	Copyright (c) 2010 by Lutz Sammer.  All Rights Reserved.
///
///	Contributor(s):
///		Bitmap and design based on wmc2d.
///
///	This file is part of wmcpumon
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
////////////////////////////////////////////////////////////////////////////

/**
**	@mainpage
**
**	This is a small dockapp, that displays the following information about
**	the system:
**
**	@n
**	- Current CPU utilization of up to four CPU cores
**	- or current aggregates CPU utilization of all CPUs and cores
**	- Up to two minutes history of CPU utilization
**	- Current memory usage
**	- Current swap usage
**	- Can sleep while screensaver running
*/

////////////////////////////////////////////////////////////////////////////

#define SCREENSAVER			///< config support screensaver
#define MAX_CPUS 4			///< how many cpus are supported

////////////////////////////////////////////////////////////////////////////

#define _GNU_SOURCE			///< we need strchrnul
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <poll.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/shm.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/shape.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_pixel.h>
#ifdef SCREENSAVER
#include <xcb/screensaver.h>
#endif

#include "wmcpumon.xpm"

////////////////////////////////////////////////////////////////////////////

xcb_connection_t *Connection;		///< connection to X11 server
xcb_screen_t *Screen;			///< our screen
xcb_window_t Window;			///< our window
xcb_gcontext_t NormalGC;		///< normal graphic context
xcb_pixmap_t Pixmap;			///< our background pixmap

xcb_pixmap_t Image;			///< drawing data

#ifdef SCREENSAVER
int ScreenSaverEventId;			///< screen saver event ids
#endif

static int StartCpu;			///< first cpu nr. to use
static int Rate;			///< update rate in ms
static char WindowMode;			///< start in window mode
static char Logscale;			///< show cpu bar in logarithmic scale
static char AllCpus;			///< use aggregate numbers of all cpus
static char UseSleep;			///< use sleep while screensaver runs

extern void Timeout(void);		///< called from event loop

    /// logarithmic log10 table
static const unsigned char Log10[] = {
    0, 15, 23, 30, 34, 38, 42, 45, 47, 50, 52, 53, 55, 57, 58, 60, 61, 62,
    63, 65, 66, 67, 68, 69, 69, 70, 71, 72, 73, 73, 74, 75, 75, 76, 77, 77,
    78, 78, 79, 80, 80, 81, 81, 82, 82, 83, 83, 84, 84, 84, 85, 85, 86, 86,
    87, 87, 87, 88, 88, 88, 89, 89, 89, 90, 90, 90, 91, 91, 91, 92, 92, 92,
    93, 93, 93, 94, 94, 94, 94, 95, 95, 95, 95, 96, 96, 96, 96, 97, 97, 97,
    97, 98, 98, 98, 98, 99, 99, 99, 99, 99, 100,
};

////////////////////////////////////////////////////////////////////////////
//	XPM Stuff
////////////////////////////////////////////////////////////////////////////

/**
**	Convert XPM graphic to xcb_image.
**
**	@param connection	XCB connection to X11 server
**	@param colormap		window colormap
**	@param depth		image depth
**	@param transparent	pixel for transparent color
**	@param data		XPM graphic data
**	@param[out] mask	bitmap mask for transparent
**
**	@returns image create from the XPM data.
**
**	@warning supports only a subset of XPM formats.
*/
xcb_image_t *XcbXpm2Image(xcb_connection_t * connection,
    xcb_colormap_t colormap, uint8_t depth, uint32_t transparent,
    const char *const *data, uint8_t ** mask)
{
    // convert table: ascii hex nibble to binary
    static const uint8_t hex[128] =
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0,
	0, 10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    int w;
    int h;
    int colors;
    int bytes_per_color;
    char dummy;
    int i;
    xcb_alloc_color_cookie_t cookies[256];
    int color_to_pixel[256];
    uint32_t pixels[256];
    xcb_image_t *image;
    int mask_width;
    const char *line;
    int x;
    int y;

    if (sscanf(*data, "%d %d %d %d %c", &w, &h, &colors, &bytes_per_color,
	    &dummy) != 4) {
	fprintf(stderr, "unparsable XPM header\n");
	abort();
    }
    if (colors < 1 || colors > 255) {
	fprintf(stderr, "XPM: wrong number of colors %d\n", colors);
	abort();
    }
    if (bytes_per_color != 1) {
	fprintf(stderr, "%d-byte XPM files not supported\n", bytes_per_color);
	abort();
    }
    data++;

    //
    //	Read color table, send alloc color requests
    //
    for (i = 0; i < colors; i++) {
	int id;

	line = *data;
	id = *line++;
	color_to_pixel[id] = i;		// maps xpm color char to pixel
	cookies[i].sequence = 0;
	while (*line) {			// multiple choices for color
	    int r;
	    int g;
	    int b;
	    int n;
	    int type;

	    n = strspn(line, " \t");	// skip white space
	    type = line[n];
	    if (!type) {
		continue;		// whitespace upto end of line
	    }
	    if (type != 'c' && type != 'm') {
		fprintf(stderr, "unknown XPM pixel type '%c' in \"%s\"\n",
		    type, *data);
		abort();
	    }
	    line += n + 1;
	    n = strspn(line, " \t");	// skip white space
	    line += n;
	    if (!strncasecmp(line, "none", 4)) {
		line += 4;
		color_to_pixel[id] = -1;	// transparent
		continue;
	    } else if (*line != '#') {
		fprintf(stderr, "unparsable XPM color spec: \"%s\"\n", line);
		abort();
	    } else {
		line++;
		r = (hex[line[0] & 0xFF] << 4) | hex[line[1] & 0xFF];
		line += 2;
		g = (hex[line[0] & 0xFF] << 4) | hex[line[1] & 0xFF];
		line += 2;
		b = (hex[line[0] & 0xFF] << 4) | hex[line[1] & 0xFF];
		line += 2;
	    }

	    // 8bit rgb -> 16bit
	    r = (65535 * (r & 0xFF) / 255);
	    b = (65535 * (b & 0xFF) / 255);
	    g = (65535 * (g & 0xFF) / 255);

	    // FIXME: should i use _unchecked here?
	    if ((depth != 1 && type == 'c') || (depth == 1 && type == 'm')) {
		if (cookies[i].sequence) {
		    fprintf(stderr, "XPM multiple color spec: \"%s\"\n", line);
		    abort();
		}
		cookies[i] =
		    xcb_alloc_color_unchecked(connection, colormap, r, g, b);
	    }
	}
	data++;
    }

    //
    //	Fetch the replies
    //
    for (i = 0; i < colors; i++) {
	xcb_alloc_color_reply_t *reply;

	if (cookies[i].sequence) {
	    reply = xcb_alloc_color_reply(connection, cookies[i], NULL);
	    if (!reply) {
		fprintf(stderr, "unable to allocate XPM color\n");
		abort();
	    }
	    pixels[i] = reply->pixel;
	    free(reply);
	} else {
	    // transparent or error
	    pixels[i] = 0UL;
	}
	// printf("pixels(%d) %x\n", i, pixels[i]);
    }

    if (depth == 1) {
	transparent = 1;
    }

    image =
	xcb_image_create_native(connection, w, h,
	(depth == 1) ? XCB_IMAGE_FORMAT_XY_BITMAP : XCB_IMAGE_FORMAT_Z_PIXMAP,
	depth, NULL, 0L, NULL);
    if (!image) {			// failure
	return image;
    }
    //
    //	Allocate empty mask (if mask is requested)
    //
    mask_width = (w + 7) / 8;		// make gcc happy
    if (mask) {
	i = mask_width * h;
	*mask = malloc(i);
	if (!mask) {			// malloc failure
	    mask = NULL;
	} else {
	    memset(*mask, 255, i);
	}
    }

    for (y = 0; y < h; y++) {
	line = *data++;
	for (x = 0; x < w; x++) {
	    i = color_to_pixel[*line++ & 0xFF];
	    if (i == -1) {		// marks transparent
		xcb_image_put_pixel(image, x, y, transparent);
		if (mask) {
		    (*mask)[(y * mask_width) + (x >> 3)] &= (~(1 << (x & 7)));
		}
	    } else {
		xcb_image_put_pixel(image, x, y, pixels[i]);
	    }
	}
    }
    return image;
}

////////////////////////////////////////////////////////////////////////////

/**
**	Create pixmap.
**
**	@param data		XPM data
**	@param[out] mask	Pixmap for data
**
**	@returns pixmap created from data.
*/
xcb_pixmap_t CreatePixmap(const char *const *data, xcb_pixmap_t * mask)
{
    xcb_pixmap_t pixmap;
    uint8_t *bitmap;
    xcb_image_t *image;

    image =
	XcbXpm2Image(Connection, Screen->default_colormap, Screen->root_depth,
	0UL, data, mask ? &bitmap : NULL);
    if (!image) {
	fprintf(stderr, "Can't create image\n");
	abort();
    }
    if (mask) {
	*mask =
	    xcb_create_pixmap_from_bitmap_data(Connection, Window, bitmap,
	    image->width, image->height, 1, 0, 0, NULL);
	free(bitmap);
    }
    // now get data from image and build a pixmap...
    pixmap = xcb_generate_id(Connection);
    xcb_create_pixmap(Connection, Screen->root_depth, pixmap, Window,
	image->width, image->height);
    xcb_image_put(Connection, pixmap, NormalGC, image, 0, 0, 0);

    xcb_image_destroy(image);

    return pixmap;
}

////////////////////////////////////////////////////////////////////////////

/**
**	Loop
*/
void Loop(void)
{
    struct pollfd fds[1];
    xcb_generic_event_t *event;
    int n;
    int delay;

    fds[0].fd = xcb_get_file_descriptor(Connection);
    fds[0].events = POLLIN | POLLPRI;

    delay = Rate;
    for (;;) {
	// wait for events or timeout
	if ((n = poll(fds, 1, delay)) < 0) {
	    return;
	}
	if (n) {
	    if (fds[0].revents & (POLLIN | POLLPRI)) {
		if ((event = xcb_poll_for_event(Connection))) {

		    switch (event->response_type &
			XCB_EVENT_RESPONSE_TYPE_MASK) {
			case XCB_EXPOSE:
			    // collapse multi expose
			    if (!((xcb_expose_event_t *) event)->count) {
				xcb_clear_area(Connection, 0, Window, 0, 0, 64,
				    64);
				// flush the request
				xcb_flush(Connection);
			    }
			    break;
			case XCB_DESTROY_NOTIFY:
			    return;
			case 0:
			    printf("error %x\n", event->response_type);
			    // error_code
			    break;
			default:
#ifdef SCREENSAVER
			    if (XCB_EVENT_RESPONSE_TYPE(event) ==
				ScreenSaverEventId) {
				xcb_screensaver_notify_event_t *sse;

				sse = (xcb_screensaver_notify_event_t *) event;
				if (sse->code == XCB_SCREENSAVER_STATE_ON) {
				    // screensave on, stop updates
				    delay = -1;
				} else {
				    // screensave off, resume updates
				    delay = Rate;
				    xcb_copy_area(Connection, Image, Pixmap,
					NormalGC, 6, 6, 6, 6, 49, 39);
				    xcb_copy_area(Connection, Image, Pixmap,
					NormalGC, 65, 57, 35, 22, 21, 7);
				    xcb_clear_area(Connection, 0, Window, 6, 6,
					49, 39);
				    xcb_flush(Connection);
				}
				break;
			    }
#endif
			    printf("unknown %x\n", event->response_type);
			    // Unknown event type, ignore it
			    break;
		    }

		    free(event);
		} else {
		    // No event, can happen, but we must check for close
		    if (xcb_connection_has_error(Connection)) {
			return;
		    }
		}
	    }
	} else {
	    Timeout();
	}
    }
}

/**
**	Init
**
**	@param argc	number of arguments
**	@param argv	arguments vector
**
**	@returns 0 if success, -1 for failure
*/
int Init(int argc, char *const argv[])
{
    const char *display_name;
    xcb_connection_t *connection;
    xcb_screen_iterator_t iter;
    int screen_nr;
    xcb_screen_t *screen;
    xcb_gcontext_t normal;
    uint32_t mask;
    uint32_t values[3];
    xcb_pixmap_t pixmap;
    xcb_window_t window;
    xcb_size_hints_t size_hints;
    xcb_wm_hints_t wm_hints;
    int i;
    int n;
    char *s;

    display_name = getenv("DISPLAY");

    //	Open the connection to the X server.
    //	use the DISPLAY environment variable as the default display name
    connection = xcb_connect(NULL, &screen_nr);
    if (!connection || xcb_connection_has_error(connection)) {
	fprintf(stderr, "Can't connect to X11 server on %s\n", display_name);
	return -1;
    }
    //	Get the requested screen number
    iter = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (i=0; i<screen_nr; ++i) {
	xcb_screen_next(&iter);
    }
    screen = iter.data;

    //	Create normal graphic context
    normal = xcb_generate_id(connection);
    mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    values[0] = screen->white_pixel;
    values[1] = screen->black_pixel;
    values[2] = 0;
    xcb_create_gc(connection, normal, screen->root, mask, values);

    pixmap = xcb_generate_id(connection);
    xcb_create_pixmap(connection, screen->root_depth, pixmap, screen->root, 64,
	64);

    //	Create the window
    window = xcb_generate_id(connection);

    mask = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK;
    values[0] = pixmap;
    values[1] = XCB_EVENT_MASK_EXPOSURE;

    xcb_create_window(connection,	// Connection
	XCB_COPY_FROM_PARENT,		// depth (same as root)
	window,				// window Id
	screen->root,			// parent window
	0, 0,				// x, y
	64, 64,				// width, height
	0,				// border_width
	XCB_WINDOW_CLASS_INPUT_OUTPUT,	// class
	screen->root_visual,		// visual
	mask, values);			// mask, values

    // XSetWMNormalHints
    size_hints.flags = 0;		// FIXME: bad lib design
    // xcb_size_hints_set_position(&size_hints, 0, 0, 0);
    // xcb_size_hints_set_size(&size_hints, 0, 64, 64);
    xcb_size_hints_set_min_size(&size_hints, 64, 64);
    xcb_size_hints_set_max_size(&size_hints, 64, 64);
    xcb_set_wm_normal_hints(connection, window, &size_hints);

    // XSetClassHint from xc/lib/X11/SetHints.c
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, WM_CLASS,
	STRING, 8, strlen("wmcpumon,wmcpumon"), "wmcpumon\0wmcpumon");

    xcb_set_wm_name(connection, window, STRING, strlen("wmcpumon"),
	"wmcpumon");
    xcb_set_wm_icon_name(connection, window, STRING, strlen("wmcpumon"),
	"wmcpumon");

    // XSetWMHints
    wm_hints.flags = 0;
    //xcb_wm_hints_set_icon_window(&wm_hints, IconWindow);
    xcb_wm_hints_set_icon_pixmap(&wm_hints, pixmap);
    xcb_wm_hints_set_window_group(&wm_hints, window);
    xcb_wm_hints_set_withdrawn(&wm_hints);
    if (WindowMode) {
	xcb_wm_hints_set_normal(&wm_hints);
    }
    xcb_set_wm_hints(connection, window, &wm_hints);

    // XSetCommand (see xlib source)
    for (n = i = 0; i < argc; ++i) {	// length of string prop
	n += strlen(argv[i]) + 1;
    }
    s = alloca(n);
    for (n = i = 0; i < argc; ++i) {	// copy string prop
	strcpy(s + n, argv[i]);
	n += strlen(s + n) + 1;
    }
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, WM_COMMAND,
	STRING, 8, n, s);

#ifdef SCREENSAVER
    //
    //	Prepare screensaver notify.
    //
    if (UseSleep) {
	const xcb_query_extension_reply_t *reply_screensaver;

	reply_screensaver =
	    xcb_get_extension_data(connection, &xcb_screensaver_id);
	if (reply_screensaver) {
	    ScreenSaverEventId =
		reply_screensaver->first_event + XCB_SCREENSAVER_NOTIFY;

	    xcb_screensaver_select_input(connection, window,
		XCB_SCREENSAVER_EVENT_NOTIFY_MASK);
	}
    }
#endif

    //	Map the window on the screen
    xcb_map_window(connection, window);

    //	Make sure commands are sent
    xcb_flush(connection);

    Connection = connection;
    Screen = screen;
    Window = window;
    NormalGC = normal;
    Pixmap = pixmap;

    return 0;
}

/**
**	Exit
*/
void Exit(void)
{
    xcb_destroy_window(Connection, Window);
    Window = 0;

    xcb_free_pixmap(Connection, Pixmap);

    if (Image) {
	xcb_free_pixmap(Connection, Image);
    }

    xcb_disconnect(Connection);
    Connection = NULL;
}

////////////////////////////////////////////////////////////////////////////
//	App Stuff
////////////////////////////////////////////////////////////////////////////

// ------------------------------------------------------------------------- //
// /proc/stat

    ///
    /// collected data from /proc/stat
    /// @see /usr/src/linux/Documentation/filesystems/proc.txt
    ///
struct cpu_info
{
    uint64_t Idle;			///< time cpu idle
    uint64_t Used;			///< time cpu used
    int Load;				///< cpu load
    int AvgLoad;			///< avg cpu load
    int OldLoadSize;			///< old cpu load bar size
    int OldAvgLoadSize;			///< old avg cpu load bar size
} CpuInfo[MAX_CPUS];			///< cached cpu informations
int Cpus;				///< number of cpus

/**
**	Read stat.
**
**	@returns -1 if failures.
**
**	FIXME: keep fd open!
*/
int GetStat(void)
{
    int fd;
    int n;
    char buf[1024];
    int cpu;
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
    uint64_t total;

    n = -1;
    if ((fd = open("/proc/stat", O_RDONLY)) >= 0) {
	n = read(fd, buf, sizeof(buf));
	if (n > 0) {
	    const char *s;

	    buf[n] = '\0';
	    n = 0;
	    if (AllCpus) {
		n = sscanf(buf,
		    "cpu   %" PRIu64 "%" PRIu64 " %" PRIu64 " %" PRIu64 "",
		    &user, &nice, &system, &idle);

		total =
		    user + nice + system + idle - CpuInfo[0].Used -
		    CpuInfo[0].Idle;

		CpuInfo[0].Load = 0;
		if (total) {
		    CpuInfo[0].Load =
			(100 * (user + nice + system - CpuInfo[0].Used))
			/ total;
		}
		CpuInfo[0].Idle = idle;
		CpuInfo[0].Used = user + nice + system;
		Cpus = 1;
	    } else {

		// skip the first total cpu line
		for (s = buf;;) {
		    if (!(s = strchr(s + 6, '\n'))) {
			break;
		    }
		    ++s;		// skip newline

		    // each line is "cpuN values\n"
		    n = sscanf(s,
			"cpu%u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
			&cpu, &user, &nice, &system, &idle);
		    if (n != 5) {
			Cpus = cpu + 1;
			break;
		    }
		    cpu -= StartCpu;
		    if (cpu < 0) {
			continue;
		    }

		    total =
			user + nice + system + idle - CpuInfo[cpu].Used -
			CpuInfo[cpu].Idle;

		    CpuInfo[cpu].Load = 0;
		    if (total) {
			CpuInfo[cpu].Load =
			    (100 * (user + nice + system - CpuInfo[cpu].Used))
			    / total;
		    }
		    CpuInfo[cpu].Idle = idle;
		    CpuInfo[cpu].Used = user + nice + system;

		    //printf("cpu %d %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %d\n", cpu, user, nice, system,
		    //	idle, CpuInfo[cpu].Load);
		    if (cpu == MAX_CPUS - 1) {	// no more cpus are supported
			Cpus = MAX_CPUS;
			break;
		    }
		}
	    }
	}
	close(fd);
    }
    return n;
}

// ------------------------------------------------------------------------- //
// /proc/meminfo

static uint32_t MemTotal;		///< cached memory info total memory
static uint32_t MemFree;		///< cached memory info free memory
static uint32_t Cached;			///< cached memory info cached memory
static uint32_t SwapFree;		///< cached memory info free swap
static uint32_t SwapTotal;		///< cached memory info total swap

/**
**	Read meminfo.
**
**	@returns -1 if failures.
**
**	FIXME: keep fd open!
*/
int GetMeminfo(void)
{
    int fd;
    int n;
    char buf[1024];

    n = -1;
    if ((fd = open("/proc/meminfo", O_RDONLY)) >= 0) {
	n = read(fd, buf, sizeof(buf));
	if (n > 0) {
	    const char *s;

	    buf[n] = '\0';
	    n = 0;
	    for (s = buf; n < 5;) {
		// each line is "name: value kb\n"
		if (!strncmp(s, "MemTotal:", 8)) {
		    MemTotal = atol(s + 9);
		    ++n;
		} else if (!strncmp(s, "MemFree:", 7)) {
		    MemFree = atol(s + 8);
		    ++n;
		} else if (!strncmp(s, "Cached:", 6)) {
		    Cached = atol(s + 7);
		    ++n;
		} else if (!strncmp(s, "SwapTotal:", 10)) {
		    SwapTotal = atol(s + 11);
		    ++n;
		} else if (!strncmp(s, "SwapFree:", 9)) {
		    SwapFree = atol(s + 10);
		    ++n;
		} else {
		    // printf("%8.8s\n", s);
		}
		if (!(s = strchr(s + 6, '\n'))) {
		    break;
		}
		++s;			// skip newline
	    }
	}
	close(fd);
    }
    return n;
}

/**
**	Get memory used.
**
**	@returns the amount of memory used in procent (0-100).
*/
int GetMemory(void)
{
    return (MemTotal - MemFree - Cached) / (MemTotal / 100);
}

/**
**	Get swap used.
**
**	@returns the amount of swap used in procent (0-100); -1 no swap.
*/
int GetSwap(void)
{
    if (SwapTotal) {
	return (SwapTotal - SwapFree) / (SwapTotal / 100);
    }
    return -1;
}

// ------------------------------------------------------------------------- //

/**
**	Draw CPU graphs.
**
**	@param loops		How many loops was collected
*/
void DrawCpuGraphs(int loops)
{
    int n;
    int c;
    int y;
    int o;
    int r;

    //
    //	    copy area to the left
    //
    xcb_copy_area(Connection, Pixmap, Pixmap, NormalGC, 7, 6, 6, 6, 49, 39);

    y = 6;
    o = 40 / Cpus;
    r = 40 % Cpus;
    for (c = 0; c < Cpus; ++c) {
	if (!r--) {			// no remainder reduce
	    --o;
	}

	n = CpuInfo[c].AvgLoad / loops;
	if (Logscale) {
	    n = Log10[n];
	}
	// draw graph
	n = o - ((o * n) / 100);
	// draw only if size has changed
	if (n != CpuInfo[c].OldAvgLoadSize) {
	    CpuInfo[c].OldAvgLoadSize = n;
	    if (n) {
		xcb_copy_area(Connection, Image, Pixmap, NormalGC, 55, y, 55,
		    y, 1, n);
	    }
	    if (n != o) {
		xcb_copy_area(Connection, Image, Pixmap, NormalGC, 64, 0, 55,
		    y + n, 1, o - n);
	    }
	}
	CpuInfo[c].AvgLoad = 0;
	y += o + 1;
    }
}

/**
**	Draw CPU bar
*/
void DrawCpuBar(void)
{
    int n;
    int c;
    int y;
    int o;
    int r;

    GetStat();

    y = 6;
    o = 40 / Cpus;
    r = 40 % Cpus;
    for (c = 0; c < Cpus; ++c) {
	if (!r--) {			// no remainder reduce
	    --o;
	}

	n = CpuInfo[c].Load;
	CpuInfo[c].AvgLoad += n;
	if (Logscale) {
	    n = Log10[n];
	}
	// draw graph
	n = o - ((o * n) / 100);

	// draw only if size has changed
	if (n != CpuInfo[c].OldLoadSize) {
	    CpuInfo[c].OldLoadSize = n;
	    if (n) {
		xcb_copy_area(Connection, Image, Pixmap, NormalGC, 56, y, 56,
		    y, 3, n);
	    }
	    if (n != o) {
		xcb_copy_area(Connection, Image, Pixmap, NormalGC,
		    65 - 3 + Cpus * 3, n, 56, y + n, 3, o - n);
	    }
	}
	y += o + 1;
    }
}

/**
**	Draw memory information.
*/
void DrawMemGraphs(void)
{
    int p;
    int n;
    static int old_m;
    static int old_s;

    GetMeminfo();			// cache /proc/meminfo

    p = GetMemory();
    // copy memory usage bar
    n = (23 * p) / 100;
    if (n != old_m) {			// only draw, if changed
	old_m = n;
	if (n) {
	    xcb_copy_area(Connection, Image, Pixmap, NormalGC, 64, 40, 6, 50,
		n, 8);
	}
	// clear unused are at the end
	if (23 - n) {
	    xcb_copy_area(Connection, Image, Pixmap, NormalGC, 6 + n, 50,
		6 + n, 50, 23 - n, 8);
	}
    }

    p = GetSwap();
    if (p != old_s) {			// only draw, if changed
	old_s = p;
	if (p >= 0) {
	    // copy memory usage bar
	    n = (23 * p) / 100;
	    if (n >= 0) {
		xcb_copy_area(Connection, Image, Pixmap, NormalGC, 64, 40, 35,
		    50, n, 8);
	    }
	    // clear unused are at the end
	    if (23 - n) {
		xcb_copy_area(Connection, Image, Pixmap, NormalGC, 35 + n, 50,
		    35 + n, 50, 23 - n, 8);
	    }
	} else {
	    xcb_copy_area(Connection, Image, Pixmap, NormalGC, 64, 48, 35, 50,
		23, 8);
	}
    }
}

// ------------------------------------------------------------------------- //

/**
**	Timeout call back.
*/
void Timeout(void)
{
    static int loops;

    //
    // Update everything
    //
    if (++loops == 10) {		// graph is slower redrawn
	DrawCpuGraphs(loops);
	DrawMemGraphs();
	loops = 0;
    }
    DrawCpuBar();

    // FIXME: not the complete area need to be redraw!!!
    xcb_clear_area(Connection, 0, Window, 0, 0, 64, 64);
    // flush the request
    xcb_flush(Connection);
}

/**
**	Prepare our graphic data.
*/
void PrepareData(void)
{
    xcb_pixmap_t shape;

    Image = CreatePixmap((void *)wmcpumon_xpm, &shape);
    // Copy background part
    xcb_copy_area(Connection, Image, Pixmap, NormalGC, 0, 0, 0, 0, 64, 64);
    if (shape) {
	xcb_shape_mask(Connection, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
	    Window, 0, 0, shape);
	xcb_free_pixmap(Connection, shape);
    }

    Timeout();
}

// ------------------------------------------------------------------------- //

/**
**	Print version.
*/
static void PrintVersion(void)
{
    printf("wmcpumon CPU system monitor dockapp Version " VERSION
#ifdef GIT_REV
	"(GIT-" GIT_REV ")"
#endif
	", (c) 2010 by Lutz Sammer\n"
	"\tLicense AGPLv3: GNU Affero General Public License version 3\n");
}

/**
**	Print usage.
*/
static void PrintUsage(void)
{
    printf("Usage: wmcpumon [-a] [-c n] [-l] [-r rate] [-s] [-w]\n"
	"\t-a\tdisplay the aggregate numbers of all cores\n"
	"\t-c n\tfirst cpu to use (to monitor more than 4 cores)\n"
	"\t-l\tuse a logarithmic scale\n"
	"\t-r rate\trefresh rate (in milliseconds, default 250 ms)\n"
	"\t-s\tsleep while screen-saver is running or video blanked\n"
	"\t-w\tStart in window mode\n" "Only idiots print usage on stderr!\n");
}

/**
**	Main entry point.
**
**	@param argc	number of arguments
**	@param argv	arguments vector
*/
int main(int argc, char *const argv[])
{
    Rate = 250;				// 250 ms default update rate

    //
    //	Parse arguments.
    //
    for (;;) {
	switch (getopt(argc, argv, "h?-ac:lr:sw")) {
	    case 'a':			// all cpus
		AllCpus = 1;
		continue;
	    case 'c':			// cpu
		StartCpu = atoi(optarg);
		break;
	    case 'l':			// logarithmic scale
		Logscale = 1;
		continue;
	    case 'r':			// update rate
		Rate = atoi(optarg);
		continue;
	    case 's':			// sleep while screensaver running
		UseSleep = 1;
		continue;
	    case 'w':			// window mode
		WindowMode = 1;
		continue;

	    case EOF:
		break;
	    case '?':
	    case 'h':			// help usage
		PrintVersion();
		PrintUsage();
		return 0;
	    case '-':
		fprintf(stderr, "We need no long options\n");
		PrintUsage();
		return -1;
	    case ':':
		PrintVersion();
		fprintf(stderr, "Missing argument for option '%c'\n", optopt);
		return -1;
	    default:
		PrintVersion();
		fprintf(stderr, "Unkown option '%c'\n", optopt);
		return -1;
	}
	break;
    }
    if (optind < argc) {
	PrintVersion();
	while (optind < argc) {
	    fprintf(stderr, "Unhandled argument '%s'\n", argv[optind++]);
	}
	return -1;
    }

    Init(argc, argv);

    PrepareData();
    Loop();
    Exit();

    return 0;
}
