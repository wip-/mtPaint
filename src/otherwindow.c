/*	otherwindow.c
	Copyright (C) 2004-2013 Mark Tyler and Dmitry Groshev

	This file is part of mtPaint.

	mtPaint is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	mtPaint is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mtPaint in the file COPYING.
*/

#include "global.h"

#include "mygtk.h"
#include "memory.h"
#include "otherwindow.h"
#include "ani.h"
#include "png.h"
#include "mainwindow.h"
#include "viewer.h"
#include "inifile.h"
#include "canvas.h"
#include "layer.h"
#include "wu.h"
#include "channels.h"
#include "toolbar.h"
#include "csel.h"
#include "font.h"
#include "cpick.h"
#include "icons.h"
#include "vcode.h"

///	NEW IMAGE WINDOW

void reset_tools()
{
	if (!mem_img[mem_channel]) mem_channel = CHN_IMAGE; // Safety first
	pressed_select(FALSE); // To prevent automatic paste
	change_to_tool(DEFAULT_TOOL_ICON);

	init_istate(&mem_state, &mem_image);
	memset(channel_col_A, 255, NUM_CHANNELS);
	memset(channel_col_B, 0, NUM_CHANNELS);
	tool_opacity = 255;		// Set opacity to 100% to start with

	if (inifile_get_gboolean("zoomToggle", FALSE))
		can_zoom = 1;		// Always start at 100%

	update_stuff(UPD_RESET | UPD_ALL);
}

void do_new_chores(int undo)
{
	set_new_filename(layer_selected, NULL);
	if (layers_total) layers_notify_changed();

	// No reason to reset tools in undoable mode
	if (!undo) reset_tools();
	else update_stuff(UPD_ALL);
}

int do_new_one(int nw, int nh, int nc, png_color *pal, int bpp, int undo)
{
	int res = 0;

	nw = nw < MIN_WIDTH ? MIN_WIDTH : nw > MAX_WIDTH ? MAX_WIDTH : nw;
	nh = nh < MIN_HEIGHT ? MIN_HEIGHT : nh > MAX_HEIGHT ? MAX_HEIGHT : nh;
	mem_cols = nc < 2 ? 2 : nc > 256 ? 256 : nc;

	/* Check memory for undo */
	if (undo) undo = !undo_next_core(UC_CREATE | UC_GETMEM, nw, nh, bpp, CMASK_IMAGE);
	/* Create undo frame if requested */
	if (undo)
	{
		undo_next_core(UC_DELETE, nw, nh, bpp, CMASK_ALL);
		undo = !!(mem_img[CHN_IMAGE] = calloc(1, nw * nh * bpp));
	}
	/* Create image anew if all else fails */
	if (!undo)
	{
		res = mem_new( nw, nh, bpp, CMASK_IMAGE );
		if (res) memory_errors(1);	// Not enough memory!
	}
	/* *Now* prepare and update palette */
	if (pal) mem_pal_copy(mem_pal, pal);
	else mem_bw_pal(mem_pal, 0, nc - 1);
	update_undo(&mem_image);

	do_new_chores(undo);

	return (res);
}

static int clip_to_layer(int layer)
{
	image_info *img;
	image_state *state;
	int cmask, undo = undo_load;

	cmask = cmask_from(mem_clip.img);
	if (layer == layer_selected)
	{
		if (undo) undo = !undo_next_core(UC_CREATE | UC_GETMEM,
			mem_clip_w, mem_clip_h, mem_clip_bpp, cmask);
		if (undo) undo_next_core(UC_DELETE, mem_clip_w, mem_clip_h,
			mem_clip_bpp, CMASK_ALL);
		else mem_free_image(&mem_image, FREE_IMAGE);
		img = &mem_image;
		state = &mem_state;
	}
	else
	{
		img = &layer_table[layer].image->image_;
		state = &layer_table[layer].image->state_;
		*state = mem_state;
		mem_free_image(img, FREE_IMAGE);
		mem_pal_copy(img->pal, mem_pal);
		img->cols = mem_cols;
		img->trans = mem_xpm_trans;
	}
	if (!mem_alloc_image(AI_COPY, img, 0, 0, 0, 0, &mem_clip)) return (0);
	update_undo(img);
	state->channel = CHN_IMAGE;
	return (1);
}

typedef struct {
	int type, w, h, c, undo, im_type;
} newwin_dd;

static void create_new(newwin_dd *dt, void **wdata)
{
	GtkWidget *new_window = GET_REAL_WINDOW(wdata);
	png_color *pal;
	int im_type, new_window_type = dt->type;
	int nw, nh, nc, err = 1, bpp;


	run_query(wdata);
	im_type = dt->im_type;
	pal = im_type == 1 ? NULL : mem_pal_def;

	nw = dt->w; nh = dt->h; nc = dt->c;
	if (!new_window_type) undo_load = dt->undo;

	if (im_type == 4) /* Screenshot */
	{
#if GTK_MAJOR_VERSION == 1
		gdk_window_lower( main_window->window );
		gdk_window_lower( new_window->window );

		gdk_flush();
		handle_events();	// Wait for minimize

		sleep(1);		// Wait a second for screen to redraw
#else /* #if GTK_MAJOR_VERSION == 2 */
		gtk_window_set_transient_for( GTK_WINDOW(new_window), NULL );
		gdk_window_iconify( new_window->window );
		gdk_window_iconify( main_window->window );

		gdk_flush();
		handle_events(); 	// Wait for minimize

		g_usleep(400000);	// Wait 0.4 of a second for screen to redraw
#endif

		// Use current layer
		if (!new_window_type)
		{
			err = load_image(NULL, FS_PNG_LOAD, undo_load ?
				FT_PIXMAP | FTM_UNDO : FT_PIXMAP);
			if (err == 1)
			{
				do_new_chores(undo_load);
				notify_changed();
			}
		}
		// Add new layer
		else if (layer_add(0, 0, 1, 0, mem_pal_def, 0))
		{
			err = load_image(NULL, FS_LAYER_LOAD, FT_PIXMAP);
			if (err == 1) layer_show_new();
			else layer_delete(layers_total);
		}

#if GTK_MAJOR_VERSION == 2
		gdk_window_deiconify( main_window->window );
#endif
		gdk_window_raise( main_window->window );
	}

	if (im_type == 3) /* Clipboard */
	{
		// Use current layer
		if (!new_window_type)
		{
			err = import_clipboard(FS_PNG_LOAD);
			if ((err != 1) && mem_clipboard)
				err = clip_to_layer(layer_selected);
			if (err == 1)
			{
				do_new_chores(undo_load);
				notify_changed();
			}
		}
		// Add new layer
		else if (layer_add(0, 0, 1, 0, mem_pal_def, 0))
		{
			err = import_clipboard(FS_LAYER_LOAD);
			if ((err != 1) && mem_clipboard)
				err = clip_to_layer(layers_total);
			if (err == 1) layer_show_new();
			else layer_delete(layers_total);
		}
	}

	/* Fallthrough if error */
	if (err != 1) im_type = 0;

	/* RGB / Greyscale / Indexed */
	bpp = im_type == 0 ? 3 : 1;
	if (im_type > 2); // Successfully done above
	else if (new_window_type == 1) // Layer
		layer_new(nw, nh, bpp, nc, pal, CMASK_IMAGE);
	else // Image
	{
		/* Nothing to undo if image got deleted already */
		err = do_new_one(nw, nh, nc, pal, bpp, undo_load && mem_img[CHN_IMAGE]);
		if (err > 0)
		{
			/* System was unable to allocate memory for
			 * image, using 8x8 instead */
			nw = mem_width;
			nh = mem_height;  
		}

		inifile_set_gint32("lastnewWidth", nw );
		inifile_set_gint32("lastnewHeight", nh );
		inifile_set_gint32("lastnewCols", nc );
		inifile_set_gint32("lastnewType", im_type );
	}

	run_destroy(wdata);
}

#undef _
#define _(X) X

static char *newwin_txt[] = { _("24 bit RGB"), _("Greyscale"),
	_("Indexed Palette"), _("From Clipboard"), _("Grab Screenshot") };

#define WBbase newwin_dd
static void *newwin_code[] = {
	IF(type), WINDOWm(_("New Layer")), // modal
	UNLESS(type), WINDOWm(_("New Image")), // modal
	TABLE2(3),
	TSPIN(_("Width"), w, MIN_WIDTH, MAX_WIDTH),
	TSPIN(_("Height"), h, MIN_HEIGHT, MAX_HEIGHT),
	TSPIN(_("Colours"), c, 2, 256),
	WDONE,
	RPACK(newwin_txt, 5, 0, im_type),
	UNLESS(type), CHECK(_("Undoable"), undo),
	HSEPl(200),
	OKBOX(_("Create"), create_new, _("Cancel"), NULL),
	WSHOW
};
#undef WBbase

#undef _
#define _(X) __(X)

void generic_new_window(int type)	// 0=New image, 1=New layer
{
	newwin_dd tdata = { type, mem_width, mem_height, mem_cols, undo_load };
	int im_type = 3 - mem_img_bpp;

	if (!type)
	{
		if (check_for_changes() == 1) return;

		tdata.w = inifile_get_gint32("lastnewWidth", DEFAULT_WIDTH);
		tdata.h = inifile_get_gint32("lastnewHeight", DEFAULT_HEIGHT);
		tdata.c = inifile_get_gint32("lastnewCols", 256);
		im_type = inifile_get_gint32("lastnewType", 2);
		if ((im_type < 0) || (im_type > 2)) im_type = 0;
	}
	tdata.im_type = im_type;

	run_create(newwin_code, &tdata, sizeof(tdata));
}


///	PATTERN & BRUSH CHOOSER WINDOW

static GtkWidget *pat_window, *draw_pat;
static int pat_brush;
static unsigned char *mem_patch;

#define PAL_SLOT_SIZE 10

static unsigned char *render_color_grid(int w, int h, int cellsize, int channel)
{
	unsigned char *rgb, *tmp;
	int i, j, k, col, row;


	row = w * 3;
	rgb = calloc(1, h * row);
	if (!rgb) return (NULL);

	for (col = i = 0; i < h; i += cellsize)
	{
		tmp = rgb + i * row;
		for (j = 0; j < row; j += cellsize * 3 , col++)
		{
			if (channel == CHN_IMAGE) /* Palette as such */
			{
				if (col < mem_cols) /* Draw only existing colors */
				{
					tmp[j + 0] = mem_pal[col].red;
					tmp[j + 1] = mem_pal[col].green;
					tmp[j + 2] = mem_pal[col].blue;
				}
			}
			else if (channel >= 0) /* Utility */
			{
				k = channel_rgb[channel][0] * col;
				tmp[j + 0] = (k + (k >> 8) + 1) >> 8;
				k = channel_rgb[channel][1] * col;
				tmp[j + 1] = (k + (k >> 8) + 1) >> 8;
				k = channel_rgb[channel][2] * col;
				tmp[j + 2] = (k + (k >> 8) + 1) >> 8;
			}
			else /* Opacity */
			{
				tmp[j + 0] = tmp[j + 1] = tmp[j + 2] = col;
			}
			for (k = j + 3; k < j + cellsize * 3 - 3; k++)
				tmp[k] = tmp[k - 3];
		}
		for (j = i + 1; j < i + cellsize - 1; j++)
			memcpy(rgb + j * row, tmp, row);
	}
	return (rgb);
}

static gboolean delete_pat(GtkWidget *widget)
{
	gtk_widget_destroy(widget);
	if (pat_brush != CHOOSE_BRUSH) free(mem_patch);
	mem_patch = NULL;
	pat_window = NULL;

	return (FALSE);
}

static gboolean key_pat(GtkWidget *widget, GdkEventKey *event)
{
	/* xine-ui sends bogus keypresses so don't delete on this */
	if (!XINE_FAKERY(event->keyval)) delete_pat(widget);

	return (TRUE);
}

static gboolean click_pat(GtkWidget *widget, GdkEventButton *event)
{
	int pat_no, mx = event->x, my = event->y;


	if (pat_brush == CHOOSE_COLOR)
	{
		int ab;

		if (!(ab = event->button == 3) && (event->button != 1))
			return (FALSE); // Only left or right click
		pat_no = mx / PAL_SLOT_SIZE + 16 * (my / PAL_SLOT_SIZE);
		pat_no = pat_no < 0 ? 0 : pat_no >= mem_cols ? mem_cols - 1 : pat_no;
		mem_col_[ab] = pat_no;
		mem_col_24[ab] = mem_pal[pat_no];
		update_stuff(UPD_AB);
	}
	else if (event->button != 1) return (FALSE); // Left click only
	else if (pat_brush == CHOOSE_PATTERN)
	{
		pat_no = mx / (8 * 4 + 4) + PATTERN_GRID_W * (my / (8 * 4 + 4));
		pat_no = pat_no < 0 ? 0 : pat_no >= PATTERN_GRID_W * PATTERN_GRID_H ?
			PATTERN_GRID_W * PATTERN_GRID_H - 1 : pat_no;
		mem_tool_pat = pat_no;
		update_stuff(UPD_PAT);
	}
	else /* if (pat_brush == CHOOSE_BRUSH) */
	{
		pat_no = mx / (PATCH_WIDTH/9) + 9*( my / (PATCH_HEIGHT/9) );
		pat_no = pat_no < 0 ? 0 : pat_no > 80 ? 80 : pat_no;
		mem_set_brush(pat_no);
		change_to_tool(TTB_PAINT);
		update_stuff(UPD_BRUSH);
	}

	return (TRUE);
}

static gboolean expose_pat(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
	int w = (int)user_data;
	gdk_draw_rgb_image( draw_pat->window, draw_pat->style->black_gc,
		event->area.x, event->area.y, event->area.width, event->area.height,
		GDK_RGB_DITHER_NONE,
		mem_patch + 3 * (event->area.x + w * event->area.y), w * 3);
	return (TRUE);
}

void choose_pattern(int typ)	// Bring up pattern chooser (0) or brush (1)
{
	int w, h;

	if (pat_window) return; // Already displayed
	pat_brush = typ;
	pat_window = add_a_window(GTK_WINDOW_POPUP, _("Pattern Chooser"),
		GTK_WIN_POS_MOUSE, TRUE);
	gtk_container_set_border_width(GTK_CONTAINER(pat_window), 4);

	draw_pat = gtk_drawing_area_new();

	if (typ == CHOOSE_PATTERN)
	{
		mem_patch = render_patterns();
		w = PATTERN_GRID_W * (8 * 4 + 4);
		h = PATTERN_GRID_H * (8 * 4 + 4);
	}
	else if (typ == CHOOSE_BRUSH)
	{
		mem_patch = mem_brushes;
		w = PATCH_WIDTH;
		h = PATCH_HEIGHT;
	}
	else /* if (typ == CHOOSE_COLOR) */
	{
		w = h = 16 * PAL_SLOT_SIZE - 1;
		mem_patch = render_color_grid(w, w, PAL_SLOT_SIZE, CHN_IMAGE);
	}
	gtk_widget_set_usize(draw_pat, w, h);

	gtk_container_add(GTK_CONTAINER(pat_window), draw_pat);
	gtk_signal_connect(GTK_OBJECT(draw_pat), "expose_event",
		GTK_SIGNAL_FUNC(expose_pat), (gpointer)w);
	gtk_signal_connect(GTK_OBJECT(draw_pat), "button_press_event",
		GTK_SIGNAL_FUNC(click_pat), NULL);
	/* !!! Given the window is modal, this makes it closeable by button
	 * release anywhere over mtPaint's main window - WJ */
	gtk_signal_connect(GTK_OBJECT(pat_window), "button_release_event",
		GTK_SIGNAL_FUNC(delete_pat), NULL);
	gtk_signal_connect(GTK_OBJECT(pat_window), "key_press_event",
		GTK_SIGNAL_FUNC(key_pat), NULL);
	gtk_widget_set_events(draw_pat, GDK_ALL_EVENTS_MASK);

	gtk_widget_show_all(pat_window);
}


///	ADD COLOURS TO PALETTE WINDOW

static int do_add_cols(spin1_dd *dt, void **wdata)
{
	int i;

	run_query(wdata);
	i = dt->n[0];
	if (i != mem_cols)
	{
		spot_undo(UNDO_PAL);

		if (i > mem_cols) memset(mem_pal + mem_cols, 0,
			(i - mem_cols) * sizeof(png_color));

		mem_cols = i;
		update_stuff(UPD_PAL);
	}
	return (TRUE);
}

#undef _
#define _(X) X

void pressed_add_cols()
{
	static spin1_dd tdata = {
		{ _("Set Palette Size"), spin1_code, FW_FN(do_add_cols) },
		{ 256, 2, 256 } };
	run_create(filterwindow_code, &tdata, sizeof(tdata));
}

#undef _
#define _(X) __(X)

/* Generic code to handle UI needs of common image transform tasks */

static void do_filterwindow(filterwindow_dd *dt, void **wdata)
{
	if (dt->evt(dt, wdata)) run_destroy(wdata);
	update_stuff(UPD_IMG);
}

#undef _
#define _(X) X

#define WBbase filterwindow_dd
void *filterwindow_code[] = {
	WINDOWpm(name), // modal
	DEFW(300),
	HSEP,
	CALLp(code),
	HSEP,
	BORDER(OKBOX, 0),
	OKBOX(_("Apply"), do_filterwindow, _("Cancel"), NULL),
	WSHOW
};
#undef WBbase

#define WBbase spin1_dd
void *spin1_code[] = { SPINa(n), RET };
#undef WBbase

#undef _
#define _(X) __(X)

typedef struct {
	GtkWidget *cont;
	filter_hook func;
	gpointer data;
} filter_wrap;

void run_filter(GtkWidget *widget, gpointer user_data)
{
	filter_wrap *fw = gtk_object_get_user_data(GTK_OBJECT(widget));
	if (fw->func(fw->cont, fw->data)) destroy_dialog(widget);
	update_stuff(UPD_IMG);
}

void filter_window(gchar *title, GtkWidget *content, filter_hook filt, gpointer fdata, int istool)
{
	filter_wrap *fw;
	GtkWidget *win, *vbox;
	GtkWindowPosition pos = istool && !inifile_get_gboolean("centerSettings", TRUE) ?
		GTK_WIN_POS_MOUSE : GTK_WIN_POS_CENTER;

	win = add_a_window(GTK_WINDOW_TOPLEVEL, title, pos, TRUE);
	fw = bound_malloc(win, sizeof(filter_wrap));
	gtk_object_set_user_data(GTK_OBJECT(win), (gpointer)fw);
	gtk_window_set_default_size(GTK_WINDOW(win), 300, -1);

	fw->cont = content;
	fw->func = filt;
	fw->data = fdata;

	vbox = add_vbox(win);

	add_hseparator(vbox, -2, 10);
	pack5(vbox, content);
	add_hseparator(vbox, -2, 10);

	pack(vbox, OK_box(0, win, _("Apply"), GTK_SIGNAL_FUNC(run_filter),
		_("Cancel"), GTK_SIGNAL_FUNC(destroy_dialog)));

	gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(main_window));
	gtk_widget_show(win);
}

///	BACTERIA EFFECT

static int do_bacteria(spin1_dd *dt, void **wdata)
{
	run_query(wdata);
	spot_undo(UNDO_FILT);
	mem_bacteria(dt->n[0]);
	mem_undo_prepare();
	return (FALSE);
}

#undef _
#define _(X) X

void pressed_bacteria()
{
	static spin1_dd tdata = {
		{ _("Bacteria Effect"), spin1_code, FW_FN(do_bacteria) },
		{ 10, 1, 100 } };
	run_create(filterwindow_code, &tdata, sizeof(tdata));
}

#undef _
#define _(X) __(X)


///	SORT PALETTE COLOURS

int spal_mode;

typedef struct {
	int rgb, start[3], end[3];
} spal_dd;

static void spal_evt(spal_dd *dt, void **wdata, int what)
{
	int index1, index2, reverse;

	run_query(wdata);
	reverse = inifile_get_gboolean("palrevSort", FALSE);
	index1 = dt->start[0];
	index2 = dt->end[0];

	if (index1 != index2)
	{
		spot_undo(UNDO_XPAL);
		mem_pal_sort(spal_mode, index1, index2, reverse);
		mem_undo_prepare();
		update_stuff(UPD_TPAL);
	}
	if (what == op_EVT_OK) run_destroy(wdata);
}

#undef _
#define _(X) X

static char *spal_txt[] = {
	_("Hue"), _("Saturation"), _("Luminance"), _("Brightness"),
		_("Distance to A"),
	_("Red"), _("Green"), _("Blue"), _("Projection to A->B"),
		_("Frequency") };

#define WBbase spal_dd
static void *spal_code[] = {
	WINDOWm(_("Sort Palette Colours")), // modal
	TABLE2(2),
	TSPINa(_("Start Index"), start),
	TSPINa(_("End Index"), end),
	WDONE,
	IF(rgb), RPACKv(spal_txt, 9, 5, spal_mode),
	UNLESS(rgb), RPACKv(spal_txt, 10, 5, spal_mode),
	CHECKb(_("Reverse Order"), "palrevSort", FALSE),
	HSEPl(200),
	OKBOX(_("OK"), spal_evt, _("Cancel"), NULL),
	OKADD(_("Apply"), spal_evt),
	WSHOW
};
#undef WBbase

#undef _
#define _(X) __(X)

void pressed_sort_pal()
{
	spal_dd tdata = { mem_img_bpp == 3, { 0, 0, mem_cols - 1 },
		{ mem_cols - 1, 0, mem_cols - 1 } };
	run_create(spal_code, &tdata, sizeof(tdata));
}


///	BRIGHTNESS-CONTRAST-SATURATION WINDOW

#define BRCOSA_ITEMS 6
#define BRCOSA_POSTERIZE 3
#define BRCOSA_INDEX(i) (((i) == BRCOSA_POSTERIZE) && posterize_mode ? \
	BRCOSA_ITEMS : (i))

static int brcosa_values_default[BRCOSA_ITEMS + 1] = {0, 0, 0, 8, 100, 0, 256};

int mem_preview, mem_preview_clip, brcosa_auto;

int posterize_mode;	// bitwise/truncated/rounded

typedef struct {
	int rgb;
	int rgbclip, pflag;
	int allow[3];
	int c01[3 * 2];
	int values[BRCOSA_ITEMS];
	void **sss[BRCOSA_ITEMS]; // Spinsliders
	void **buttons[4]; // Preview/Reset/Apply/OK
	void **xtra; // "Details" area
	png_color pal[256];
} brcosa_dd;

static void brcosa_preview(brcosa_dd *dt, void *cause)
{
	int i, j, update = UPD_PAL;
	int do_pal = TRUE;	// palette processing

	mem_pal_copy(mem_pal, dt->pal);	// Get back normal palette

	for (i = 0; i < BRCOSA_ITEMS; i++)
		mem_bcsp.bcsp[i] = dt->values[i];
	mem_bcsp.pmode = posterize_mode;

	for (i = 0; i < 3; i++)
		mem_bcsp.allow[i] = dt->allow[i];

	if (mem_img_bpp == 3)
	{
		update = UPD_RENDER | UPD_PAL;
		do_pal = dt->pflag;
		// Unless user has just cleared toggle
		if (!do_pal && (cause != &dt->pflag)) update = UPD_RENDER;
	}
	if (do_pal)
	{
		j = dt->c01[0] > dt->c01[3] ? 3 : 0;
		transform_pal(mem_pal, dt->pal, dt->c01[j], dt->c01[j ^ 3]);
	}
	update_stuff(update);
}

static void brcosa_changed(brcosa_dd *dt, void **wdata, int what, void **where)
{
	brcosa_preview(dt, cmd_read(where, dt)); // Update everything
}

static void brcosa_moved(brcosa_dd *dt, void **wdata, int what, void **where)
{
	int i, state;
	void *cause = cmd_read(where, dt);

	if (brcosa_auto) brcosa_preview(dt, NULL);
	if (cause == &brcosa_auto) cmd_showhide(dt->buttons[0], !brcosa_auto);

	// Set 3 brcosa button as sensitive if the user has assigned changes
	for (state = i = 0; i < BRCOSA_ITEMS; i++)
		state |= dt->values[i] ^ brcosa_values_default[BRCOSA_INDEX(i)];
	for (i = 1; i < 4; i++) cmd_sensitive(dt->buttons[i], state);
}

static void brcosa_btn(brcosa_dd *dt, void **wdata, int what)
{
	unsigned char *mask, *mask0, *tmp;
	int i;

	mem_pal_copy(mem_pal, dt->pal);

	if (what != op_EVT_CANCEL) // OK/Apply
	{	// !!! Buttons disabled for default values
		spot_undo(UNDO_COL);

		brcosa_preview(dt, NULL); // This modifies palette
		while (mem_preview && (mem_img_bpp == 3)) // This modifies image
		{
			mask = malloc(mem_width);
			if (!mask) break;
			mask0 = NULL;
			if (!channel_dis[CHN_MASK]) mask0 = mem_img[CHN_MASK];
			tmp = mem_img[CHN_IMAGE];
			for (i = 0; i < mem_height; i++)
			{
				prep_mask(0, 1, mem_width, mask, mask0, tmp);
				do_transform(0, 1, mem_width, mask, tmp, tmp);
				if (mask0) mask0 += mem_width;
				tmp += mem_width * 3;
			}
			free(mask);
			break;
		}
		if (mem_preview_clip && (mem_img_bpp == 3) && (mem_clip_bpp == 3))
		{
			// This modifies clipboard
			do_transform(0, 1, mem_clip_w * mem_clip_h, NULL,
				mem_clipboard, mem_clipboard);
		}
		mem_undo_prepare();
	}

	// Disable preview for final update
	if (what != op_EVT_CLICK) mem_preview = mem_preview_clip = FALSE;

	update_stuff(UPD_PAL);

	if (what == op_EVT_CLICK) // Apply
	{
		// Reload palette and redo preview
		mem_pal_copy(dt->pal, mem_pal);
		brcosa_preview(dt, NULL);
	}
	else run_destroy(wdata); // OK/Cancel
}

static void click_brcosa_show_toggle(brcosa_dd *dt, void **wdata, int what,
	void **where)
{
	cmd_read(where, dt);
	cmd_showhide(dt->xtra, inifile_get_gboolean("transcol_show", FALSE));
}

static void click_brcosa_reset(brcosa_dd *dt)
{
	int i;

	mem_pal_copy(mem_pal, dt->pal);

	for (i = 0; i < BRCOSA_ITEMS; i++)
		cmd_set(dt->sss[i], brcosa_values_default[BRCOSA_INDEX(i)]);

	update_stuff(UPD_PAL);
}

static void brcosa_posterize_changed(brcosa_dd *dt, void **wdata, int what,
	void **where)
{
	int vvv[3];
	int i, v, oldv = posterize_mode;

	cmd_read(where, dt);
	if (!oldv ^ !posterize_mode) // From/to bitwise
	{
		cmd_read(dt->sss[BRCOSA_POSTERIZE], dt);
		v = dt->values[BRCOSA_POSTERIZE];
		if (oldv) // To bitwise
		{
			for (i = 0 , v -= 1; v ; i++ , v >>= 1);
			vvv[0] = i;
			vvv[1] = 1;
			vvv[2] = 8;
		}
		else // From bitwise
		{
			vvv[0] = 1 << v;
			vvv[1] = 2;
			vvv[2] = 256;
		}
		cmd_set3(dt->sss[BRCOSA_POSTERIZE], vvv);
	}
	else if (brcosa_auto) brcosa_preview(dt, NULL);
}

#undef _
#define _(X) X

static char *pos_txt[] = { _("Bitwise"), _("Truncated"), _("Rounded") };

#define WBbase brcosa_dd
static void *brcosa_code[] = {
	WPMOUSE, WINDOWm(_("Transform Colour")), NORESIZE,
	BORDER(TABLE, 10),
	TABLE2(6),
	REF(sss[4]), TSPINSLIDE(_("Gamma"), values[4], 20, 500),
	EVENT(CHANGE, brcosa_moved),
	REF(sss[0]), TSPINSLIDE(_("Brightness"), values[0], -255, 255),
	EVENT(CHANGE, brcosa_moved),
	REF(sss[1]), TSPINSLIDE(_("Contrast"), values[1], -100, 100),
	EVENT(CHANGE, brcosa_moved),
	REF(sss[2]), TSPINSLIDE(_("Saturation"), values[2], -100, 100),
	EVENT(CHANGE, brcosa_moved),
	REF(sss[5]), TSPINSLIDE(_("Hue"), values[5], -1529, 1529),
	EVENT(CHANGE, brcosa_moved),
	REF(sss[3]),
	UNLESSv(posterize_mode), TSPINSLIDE(_("Posterize"), values[3], 1, 8),
	IFv(posterize_mode), TSPINSLIDE(_("Posterize"), values[3], 2, 256),
	EVENT(CHANGE, brcosa_moved),
	WDONE,
///	MIDDLE SECTION
	HSEP,
	HBOX,
	CHECKb(_("Show Detail"), "transcol_show", FALSE),
	EVENT(CHANGE, click_brcosa_show_toggle), TRIGGER,
	WDONE,
	BORDER(TABLE, 0),
	REF(xtra), TABLEr(4, 4),
	TLLABEL(_("Posterize type"), 0, 0),
	TLOPTvle(pos_txt, 3, posterize_mode, brcosa_posterize_changed, 1, 0, 2),
	UNLESS(rgb), TLLABEL(_("Palette"), 0, 2),
	IFx(rgb, 1),
		TLCHECK(_("Palette"), pflag, 0, 2),
		EVENT(CHANGE, brcosa_changed),
		TLCHECKv(_("Image"), mem_preview, 0, 1),
		EVENT(CHANGE, brcosa_changed),
		IF(rgbclip),
			TLCHECKvl(_("Clipboard"), mem_preview_clip, 1, 1, 2),
		IF(rgbclip), EVENT(CHANGE, brcosa_changed),
	ENDIF(1),
	TLHBOXl(1, 2, 2),
	XSPINa(c01[0]), EVENT(CHANGE, brcosa_changed),
	XSPINa(c01[3]), EVENT(CHANGE, brcosa_changed),
	WDONE,
	TLCHECKv(_("Auto-Preview"), brcosa_auto, 0, 3),
	EVENT(CHANGE, brcosa_moved), TRIGGER,
	TLCHECK(_("Red"), allow[0], 1, 3), EVENT(CHANGE, brcosa_changed),
	TLCHECK(_("Green"), allow[1], 2, 3), EVENT(CHANGE, brcosa_changed),
	TLCHECK(_("Blue"), allow[2], 3, 3), EVENT(CHANGE, brcosa_changed),
	WDONE,
///	BOTTOM AREA
	HSEP,
	OKBOX0,
	BORDER(OKBTN, 4),
	CANCELBTN(_("Cancel"), brcosa_btn),
	REF(buttons[0]), OKNEXT(_("Preview"), brcosa_changed),
	REF(buttons[1]), OKNEXT(_("Reset"), click_brcosa_reset),
	REF(buttons[2]), OKNEXT(_("Apply"), brcosa_btn),
	REF(buttons[3]), OKBTN(_("OK"), brcosa_btn),
	WSHOW
};
#undef WBbase

#undef _
#define _(X) __(X)

void pressed_brcosa()
{
	brcosa_dd tdata = { mem_img_bpp == 3,
		mem_clipboard && (mem_clip_bpp == 3), FALSE,
		{ TRUE, TRUE, TRUE },
		{ 0, 0, mem_cols - 1, mem_cols - 1, 0, mem_cols - 1 } };
	int i;

	memcpy(tdata.values, brcosa_values_default, sizeof(tdata.values));
	tdata.values[BRCOSA_POSTERIZE] =
		brcosa_values_default[BRCOSA_INDEX(BRCOSA_POSTERIZE)];
	mem_pal_copy(tdata.pal, mem_pal);	// Remember original palette

	/* !!! In case a redraw happens inside run_create() */
	for (i = 0; i < BRCOSA_ITEMS; i++)
		mem_bcsp.bcsp[i] = brcosa_values_default[i];

	mem_preview = TRUE;	// Enable live preview in RGB mode

	run_create(brcosa_code, &tdata, sizeof(tdata));
}


///	RESIZE/RESCALE WINDOWS

typedef struct {
	int mode, rgb;
	int fix, gamma;
	int w, h, x, y;
	void **spin[4]; // w, h, x, y
	void **book;
} sisca_dd;


static void sisca_moved(sisca_dd *dt, void **wdata, int what, void **where)
{
	int w, h, nw, idx;

	idx = cmd_read(where, dt) != &dt->h; // _other_ spin index: w/h
	if (!dt->fix) return;
	w = dt->w; h = dt->h;
	if (!idx)
	{
		nw = (h * mem_width * 2 + mem_height) / (mem_height * 2);
		nw = nw < 1 ? 1 : nw > MAX_WIDTH ? MAX_WIDTH : nw;
		if (nw == w) return;
	}
	else
	{
		nw = (w * mem_height * 2 + mem_width) / (mem_width * 2);
		nw = nw < 1 ? 1 : nw > MAX_HEIGHT ? MAX_HEIGHT : nw;
		if (nw == h) return;
	}
	cmd_set(dt->spin[idx], nw); /* Other coordinate */
}

static void alert_same_geometry()
{
	alert_box(_("Error"), _("New geometry is the same as now - nothing to do."), NULL);
}

static int scale_mode = 6;
static int resize_mode = 0;
static int boundary_mode = BOUND_MIRROR;
int sharper_reduce;

static void click_sisca_ok(sisca_dd *dt, void **wdata)
{
	int nw, nh, ox, oy, res, scale_type = 0, gcor = FALSE;

	run_query(wdata);
	nw = dt->w; nh = dt->h; ox = dt->x; oy = dt->y;

	if (!((nw ^ mem_width) | (nh ^ mem_height) | ox | oy))
	{
		alert_same_geometry();
		return;
	}

	if (dt->mode)
	{
		if (mem_img_bpp == 3)
		{
			scale_type = scale_mode;
			gcor = dt->gamma;
		}
		res = mem_image_scale(nw, nh, scale_type, gcor, sharper_reduce,
			boundary_mode);
	}
	else res = mem_image_resize(nw, nh, ox, oy, resize_mode);

	if (!res)
	{
		update_stuff(UPD_GEOM);
		run_destroy(wdata);
	}
	else memory_errors(res);
}

void memory_errors(int type)
{
	if ( type == 1 )
		alert_box(_("Error"), _("The operating system cannot allocate the memory for this operation."), NULL);
	if ( type == 2 )
		alert_box(_("Error"), _("You have not allocated enough memory in the Preferences window for this operation."), NULL);
}

static void click_sisca_centre(sisca_dd *dt, void **wdata)
{
	int nw = dt->w, nh = dt->h; // !!! sisca_moved() keeps these updated

	cmd_set(dt->spin[2], (nw - mem_width) / 2);
	cmd_set(dt->spin[3], (nh - mem_height) / 2);
}

#undef _
#define _(X) X

static char *bound_modes[] = { _("Mirror"), _("Tile"), _("Void") };
static char *resize_modes[] = { _("Clear"), _("Tile"), _("Mirror tile"), NULL };
static char *scale_modes[] = { 
	_("Nearest Neighbour"),
	_("Bilinear / Area Mapping"),
	_("Bicubic"),
	_("Bicubic edged"),
	_("Bicubic better"),
	_("Bicubic sharper"),
	_("Blackman-Harris"),
	NULL
};

#define WBbase sisca_dd
static void *sisca_code[] = {
	IF(mode), WINDOWm(_("Scale Canvas")),
	UNLESS(mode), WINDOWm(_("Resize Canvas")),
	TABLE(3, 3), // !!! in fact 5 rows in resize mode
	BORDER(TLLABEL, 0),
	TLLABEL(_("Width     "), 1, 0), TLLABEL(_("Height    "), 2, 0),
	TLLABEL(_("Original      "), 0, 1), TLNOSPIN(w, 1, 1), TLNOSPIN(h, 2, 1),
	TLLABEL(_("New"), 0, 2),
	REF(spin[0]), TLSPIN(w, 1, MAX_WIDTH, 1, 2), EVENT(CHANGE, sisca_moved),
	REF(spin[1]), TLSPIN(h, 1, MAX_HEIGHT, 2, 2), EVENT(CHANGE, sisca_moved),
	UNLESSx(mode, 1),
		TLLABEL(_("Offset"), 0, 3),
		REF(spin[2]), TLSPIN(x, -MAX_WIDTH, MAX_WIDTH, 1, 3),
		REF(spin[3]), TLSPIN(y, -MAX_HEIGHT, MAX_HEIGHT, 2, 3),
		TLBUTTON(_("Centre"), click_sisca_centre, 0, 4),
	ENDIF(1),
	WDONE,
	HSEP,
	IF(rgb), HBOX, IF(rgb), VBOX,
	CHECK(_("Fix Aspect Ratio"), fix), EVENT(CHANGE, sisca_moved),
	IFx(rgb, 1),
		CHECK(_("Gamma corrected"), gamma),
		WDONE, EVBOX,
		BOOKBTN(_("Settings"), book),
		WDONE, WDONE,
		HSEP,
		REF(book), PLAINBOOK, // pages 0 and 1 are both stacked
		RPACKv(scale_modes, 0, 0, scale_mode),
		HSEP,
		WDONE, // page 0
		CHECKv(_("Sharper image reduction"), sharper_reduce),
		BORDER(FRBOX, 0),
		FRPACKv(_("Boundary extension"), bound_modes, 3, 0,
			boundary_mode),
		WDONE, // page 1
	ENDIF(1),
	UNLESSx(mode, 1),
		HSEP,
		RPACKv(resize_modes, 0, 0, resize_mode),
		HSEP,
	ENDIF(1),
	OKBOX(_("OK"), click_sisca_ok, _("Cancel"), NULL),
	WSHOW
};
#undef WBbase

#undef _
#define _(X) __(X)

void pressed_scale_size(int mode)
{
	sisca_dd tdata = { mode, mode && (mem_img_bpp == 3), TRUE, use_gamma,
		mem_width, mem_height, 0, 0 };
	run_create(sisca_code, &tdata, sizeof(tdata));
}


///	PALETTE EDITOR WINDOW

enum {
	CHOOK_CANCEL = 0,
	CHOOK_PREVIEW,
	CHOOK_OK,
	CHOOK_SELECT,
	CHOOK_SET,
	CHOOK_UNVIEW,
	CHOOK_CHANGE
};

typedef struct {
	unsigned char rgb[768]; // everything needs it
	unsigned char opac[256]; // only EDIT_OVERLAYS uses it
	int AB[2][NUM_CHANNELS]; // for EDIT_AB
	int csrange, csinv, csmode; // for EDIT_CSEL
	int cgrid, tgrid, grid_min, tgrid_dx, tgrid_dy; // for GRID
} csdata_dd;

typedef struct colsel_dd colsel_dd;
typedef void (*colsel_fn)(colsel_dd *dt, int what);

struct colsel_dd {
	colsel_fn select;
	char *name, **cnames;
	int opflag, mpflag;
	int cnt, idx;

	int preview;
	int rflag; // recursion

	int color[2];
	void **clist, **csel;

	int is_pal;
	int n0, n1, scale;
	void **fbutton, **fspin, **tspin;

	int is_AB;
	int pos_AB, v_AB[NUM_CHANNELS];
	void **pspin_AB, **spin_AB[NUM_CHANNELS];

	int is_csel;
	void **fspin_csel;

	int is_grid;

	csdata_dd v, v0; // primary and backup
};


/* Put current color in list into selector */
static void colsel_show_idx(colsel_dd *dt)
{
	unsigned char *c = dt->v.rgb + dt->idx * 3;
	int color[4];

	color[2] = color[0] = MEM_2_INT(c, 0);
	color[3] = color[1] = dt->v.opac[dt->idx];
	dt->rflag = TRUE; // prevent recursion
	cmd_set4(dt->csel, color);
	dt->rflag = FALSE;
}

static void color_refresh(colsel_dd *dt)
{
	colsel_show_idx(dt);
	cmd_repaint(dt->clist);
}


static void do_allcol(csdata_dd *v)
{
	unsigned char *rgb = v->rgb;
	int i;

	for (i = 0; i < mem_cols; i++)
	{
		mem_pal[i].red = rgb[0];
		mem_pal[i].green = rgb[1];
		mem_pal[i].blue = rgb[2];
		rgb += 3;
	}
	update_stuff(UPD_PAL);
}

static void do_allover(csdata_dd *v)
{
	unsigned char *rgb = v->rgb;
	int i;

	for (i = 0; i < NUM_CHANNELS; i++)
	{
		channel_rgb[i][0] = rgb[0];
		channel_rgb[i][1] = rgb[1];
		channel_rgb[i][2] = rgb[2];
		channel_opacity[i] = v->opac[i];
		rgb += 3;
	}
	update_stuff(UPD_RENDER);
}

static void do_AB(csdata_dd *v)
{
	png_color *A0, *B0;
	A0 = mem_img_bpp == 1 ? &mem_pal[mem_col_A] : &mem_col_A24;
	B0 = mem_img_bpp == 1 ? &mem_pal[mem_col_B] : &mem_col_B24;

	A0->red = v->rgb[0]; A0->green = v->rgb[1]; A0->blue = v->rgb[2];
	B0->red = v->rgb[3]; B0->green = v->rgb[4]; B0->blue = v->rgb[5];
	update_stuff(mem_img_bpp == 1 ? UPD_PAL : UPD_AB);
}

static void set_csel(csdata_dd *v)
{
	unsigned char *rgb = v->rgb;
	csel_data->center = MEM_2_INT(rgb, 0);
	csel_data->limit = MEM_2_INT(rgb, 3);
	csel_preview = MEM_2_INT(rgb, 6);
/* !!! Disabled for now - later will be opacity */
//	csel_preview_a = v->opac[2];
	csel_data->mode = v->csmode;
	csel_data->range = v->csrange / 100.0;
	csel_data->invert = v->csinv;
}

static void set_grid(csdata_dd *v)
{
	unsigned char *rgb = v->rgb;
	int i;

	for (i = 0; i < GRID_MAX; i++)
	{
		grid_rgb[i] = MEM_2_INT(rgb, 0);
		rgb += 3;
	}
	color_grid = v->cgrid;
	show_tile_grid = v->tgrid;
	mem_grid_min = v->grid_min;
	tgrid_dx = v->tgrid_dx;
	tgrid_dy = v->tgrid_dy;
}

static void select_colour(colsel_dd *dt, int what)
{
	switch (what)
	{
	case CHOOK_UNVIEW: /* Disable preview */
	case CHOOK_CANCEL: /* Cancel */
		do_allcol(&dt->v0);
		break;
	case CHOOK_SET: /* Set */
		if (!dt->preview) break;
	case CHOOK_PREVIEW: /* Preview */
		do_allcol(&dt->v);
		break;
	case CHOOK_OK: /* OK */
		do_allcol(&dt->v0);
		spot_undo(UNDO_PAL);
		do_allcol(&dt->v);
		break;
	}
}

static void click_colour(colsel_dd *dt, void **wdata, int what, void **where,
	colorlist_ext *xdata)
{
	/* Middle click sets "from" */
	if (xdata->button == 2) cmd_set(dt->fspin, xdata->idx);

	/* Right click sets "to" */
	if (xdata->button == 3) cmd_set(dt->tspin, xdata->idx);
}

static void set_range_spin(colsel_dd *dt, void **wdata, int what, void **where)
{
	cmd_set(origin_slot(where) == dt->fbutton ? dt->fspin : dt->tspin, dt->idx);
}

static void make_cscale(colsel_dd *dt, void **wdata)
{
	int i, n, start, stop, start0, mode;
	unsigned char *c0, *c1, *lc;
	double d;

	run_query(wdata);
	mode = dt->scale;
	start = start0 = dt->n0;
	stop = dt->n1;

	if (mode <= 2) /* RGB/sRGB/HSV */
	{
		if (start > stop) { i = start; start = stop; stop = i; }
		if (stop < start + 2) return;
	}
	else if (stop == start) return; /* Gradient */

	c0 = lc = dt->v.rgb + start * 3;
	c1 = dt->v.rgb + stop * 3;
	d = n = stop - start;

	switch (mode)
	{
	case 0: /* RGB */
	{
		double r0, g0, b0, dr, dg, db;

		dr = ((int)c1[0] - c0[0]) / d;
		r0 = c0[0];
		dg = ((int)c1[1] - c0[1]) / d;
		g0 = c0[1];
		db = ((int)c1[2] - c0[2]) / d;
		b0 = c0[2];

		for (i = 1; i < n; i++)
		{
			lc += 3;
			lc[0] = rint(r0 + dr * i);
			lc[1] = rint(g0 + dg * i);
			lc[2] = rint(b0 + db * i);
		}
		break;
	}
	case 1: /* sRGB */
	{
		double r0, g0, b0, dr, dg, db, rr, gg, bb;

		dr = (gamma256[c1[0]] - (r0 = gamma256[c0[0]])) / d;
		dg = (gamma256[c1[1]] - (g0 = gamma256[c0[1]])) / d;
		db = (gamma256[c1[2]] - (b0 = gamma256[c0[2]])) / d;

		for (i = 1; i < n; i++)
		{
			lc += 3;
			rr = r0 + dr * i;
			lc[0] = UNGAMMA256(rr);
			gg = g0 + dg * i;
			lc[1] = UNGAMMA256(gg);
			bb = b0 + db * i;
			lc[2] = UNGAMMA256(bb);
		}
		break;
	}
	case 2: /* HSV */
	{
		int t;
		double h0, dh, s0, ds, v0, dv, hsv[6], hh, ss, vv;

		rgb2hsv(c0, hsv + 0);
		rgb2hsv(c1, hsv + 3);
		/* Grey has no hue */
		if (hsv[1] == 0.0) hsv[0] = hsv[3];
		if (hsv[4] == 0.0) hsv[3] = hsv[0];

		/* Always go from 1st to 2nd hue in ascending order */
		t = start == start0 ? 0 : 3;
		if (hsv[t] > hsv[t ^ 3]) hsv[t] -= 6.0;

		dh = (hsv[3] - hsv[0]) / d;
		h0 = hsv[0];
		ds = (hsv[4] - hsv[1]) / d;
		s0 = hsv[1];
		dv = (hsv[5] - hsv[2]) / d;
		v0 = hsv[2];

		for (i = 1; i < n; i++)
		{
			vv = v0 + dv * i;
			ss = vv - vv * (s0 + ds * i);
			hh = h0 + dh * i;
			if (hh < 0.0) hh += 6.0;
			t = hh;
			hh = (hh - t) * (vv - ss);
			if (t & 1) { vv -= hh; hh += vv; }
			else hh += ss;
			t >>= 1;
			lc += 3;
			lc[t] = rint(vv);
			lc[(t + 1) % 3] = rint(hh);
			lc[(t + 2) % 3] = rint(ss);
		}
		break;
	}
	default: /* Gradient */
	{
		int j, op, c[NUM_CHANNELS + 3];

		j = start < stop ? 1 : -1;
		for (i = 0; i != n + j; i += j , lc += j * 3)
		{
			op = grad_value(c, 0, i / d);
			/* Zero opacity - empty slot */
			if (!op) lc[0] = lc[1] = lc[2] = 0;
			else
			{
				lc[0] = (c[0] + 128) >> 8;
				lc[1] = (c[1] + 128) >> 8;
				lc[2] = (c[2] + 128) >> 8;
			}
		}
		break;
	}
	}
	color_refresh(dt);
	select_colour(dt, CHOOK_SET);
}

static void select_overlay(colsel_dd *dt, int what)
{
	char txt[64];
	int i, j;

	switch (what)
	{
	case CHOOK_UNVIEW: /* Disable preview */
	case CHOOK_CANCEL: /* Cancel */
		do_allover(&dt->v0);	// Restore original values
		break;
	case CHOOK_SET: /* Set */
		if (!dt->preview) break;
	case CHOOK_PREVIEW: /* Preview */
		do_allover(&dt->v);
		break;
	case CHOOK_OK: /* OK */
		do_allover(&dt->v);
		for (i = 0; i < NUM_CHANNELS; i++)	// Save all settings to ini file
		{
			for (j = 0; j < 4; j++)
			{
				sprintf(txt, "overlay%i%i", i, j);
				inifile_set_gint32(txt, j < 3 ? channel_rgb[i][j] :
					channel_opacity[i]);
			}
		}
		break;
	}
}

static void select_AB(colsel_dd *dt, int what)
{
	int i, *v;

	switch (what)
	{
	case CHOOK_UNVIEW: /* Disable preview */
	case CHOOK_CANCEL: /* Cancel */
		do_AB(&dt->v0);
		break;
	case CHOOK_OK: /* OK */
		do_AB(&dt->v0);
		spot_undo(UNDO_PAL);
		for (i = CHN_ALPHA; i < NUM_CHANNELS; i++)
		{
			channel_col_A[i] = dt->v.AB[0][i];
			channel_col_B[i] = dt->v.AB[1][i];
		}
		do_AB(&dt->v);
		break;
	case CHOOK_SET: /* Set */
		if (!dt->preview) break;
	case CHOOK_PREVIEW: /* Preview */
		do_AB(&dt->v);
		break;
	case CHOOK_SELECT: /* Select */
		v = dt->v.AB[dt->idx];
		for (i = CHN_ALPHA; i < NUM_CHANNELS; i++)
			cmd_set(dt->spin_AB[i], v[i]);
		break;
	}
}

static void AB_spin_moved(colsel_dd *dt, void **wdata, int what, void **where)
{
	int *w = cmd_read(where, dt);
	dt->v.AB[dt->idx][w - dt->v_AB] = *w;
}

static void posterize_AB(colsel_dd *dt, void **wdata)
{
	static const int posm[8] = {0, 0xFF00, 0x5500, 0x2480, 0x1100,
				 0x0840, 0x0410, 0x0204};
	unsigned char *lc = dt->v.rgb;
	int i, pm, ps;

	cmd_read(dt->pspin_AB, dt);
	inifile_set_gint32("posterizeInt", ps = dt->pos_AB);
	if (ps >= 8) return;
	pm = posm[ps]; ps = 8 - ps;

	for (i = 0; i < 6; i++) lc[i] = ((lc[i] >> ps) * pm) >> 8;
	color_refresh(dt);
	select_AB(dt, CHOOK_SET);
}

static void select_csel(colsel_dd *dt, int what)
{
	int old_over = csel_overlay;

	switch (what)
	{
	case CHOOK_CANCEL: /* Cancel */
		set_csel(&dt->v0);
		csel_reset(csel_data);
	case CHOOK_UNVIEW: /* Disable preview */
		csel_overlay = 0;
		if (old_over) update_stuff(UPD_RENDER);
		break;
	case CHOOK_PREVIEW: /* Preview */
		csel_overlay = 1;
	case CHOOK_CHANGE: /* Range/mode/invert controls changed */
		if (!csel_overlay) break; // No preview
	case CHOOK_OK: /* OK */
		set_csel(&dt->v);
		csel_reset(csel_data);
		if (what == CHOOK_OK)
		{
			csel_overlay = 0;
			update_stuff(UPD_RENDER | UPD_MODE);
		}
		else update_stuff(UPD_RENDER);
		break;
	case CHOOK_SET: /* Set */
		set_csel(&dt->v);
		if (dt->idx == 1) /* Limit color changed */
		{
			// This triggers CHOOK_CHANGE which will redraw
			cmd_set(dt->fspin_csel, rint(csel_eval(csel_data->mode,
				csel_data->center, csel_data->limit) * 100));
		}
		else if (csel_overlay)
		{
			/* Center color changed */
			if (!dt->idx) csel_reset(csel_data);
			/* Else, overlay color changed */
			update_stuff(UPD_RENDER);
		}
		break;
	}
}

static void csel_controls_changed(colsel_dd *dt, void **wdata, int what, void **where)
{
	if (cmd_read(where, dt) == &dt->v.csmode)
	{
		set_csel(&dt->v);
		// This triggers CHOOK_CHANGE which will redraw
		cmd_set(dt->fspin_csel, rint(csel_eval(csel_data->mode,
			csel_data->center, csel_data->limit) * 100));
	}
	else select_csel(dt, CHOOK_CHANGE);
}

static void select_grid(colsel_dd *dt, int what)
{
	switch (what)
	{
	case CHOOK_UNVIEW: /* Disable preview */
	case CHOOK_CANCEL: /* Cancel */
		// Restore original values
		set_grid(&dt->v0);
		break;
	case CHOOK_CHANGE: /* Grid controls changed */
	case CHOOK_SET: /* Set */
		if (!dt->preview) return;
	case CHOOK_PREVIEW: /* Preview */
	case CHOOK_OK: /* OK */
		set_grid(&dt->v);
		break;
	default: return;
	}
	update_stuff(UPD_RENDER);
}

static void grid_controls_changed(colsel_dd *dt, void **wdata, int what, void **where)
{
	cmd_read(where, dt);
	select_grid(dt, CHOOK_CHANGE);
}


static void colsel_evt(colsel_dd *dt, void **wdata, int what, void **where)
{
	void *cause = NULL;

	if (dt->rflag) return; // skip recursive calls
	if (what == op_EVT_CHANGE) cause = cmd_read(where, dt);

	if (what == op_EVT_SELECT) // COLORLIST, self-updating
	{
		colsel_show_idx(dt); // Set selected color
		dt->select(dt, CHOOK_SELECT);
	}
	else if (cause == &dt->color)
	{
		/* Opacity */
		dt->v.opac[dt->idx] = dt->color[1];
		/* Color; will update the RGB array */
		cmd_set2(dt->clist, dt->idx, dt->color[0]);
		dt->select(dt, CHOOK_SET);
	}
	else if (cause == &dt->preview)
		dt->select(dt, dt->preview ? CHOOK_PREVIEW : CHOOK_UNVIEW);
	else if (what == op_EVT_OK)
	{
		run_query(wdata); // for final updating, and for extra part
		dt->select(dt, CHOOK_OK);
		run_destroy(wdata);
	}
	else if (what == op_EVT_CANCEL)
	{
		dt->select(dt, CHOOK_CANCEL);
		run_destroy(wdata);
	}
}

#undef _
#define _(X) X

static char *scales_txt[] = { _("RGB"), _("sRGB"), _("HSV"), _("Gradient") };
static char *AB_txt[] = { "A", "B", NULL };
static char *csel_txt[] = { _("Centre"), _("Limit"), _("Preview"), NULL };
static char *csel_modes[] = { _("Sphere"), _("Angle"), _("Cube"), NULL };
static char *grid_txt[GRID_MAX + 1] = { _("Opaque"), _("Border"),
	_("Transparent"), _("Tile "), _("Segment"), NULL };
/* !!! "Tile " has a trailing space to be distinct from "Tile" in "Resize Canvas" */

#if NUM_CHANNELS > CHN_MASK + 1
#error "Not all channels listed in dialog"
#endif

#define WBbase colsel_dd
static void *colsel_code[] = {
	IF(mpflag), WPMOUSE,
	WINDOWpm(name),
	REF(clist),
	IFx(is_pal, 1), // long-list form - for now only palette needs it
		XHBOXb(5, 0),
		COLORLISTN(cnt, idx, v.rgb, colsel_evt, click_colour),
		XVBOXb(5, 5),
	ENDIF(1),
	UNLESSx(is_pal, 1), // short-list form (6 items or less)
		XVBOXb(5, 5),
		XHBOXb(10, 0),
		COLORLIST(cnames, idx, v.rgb, colsel_evt, NULL),
	ENDIF(1),
	TRIGGER, // colorlist SELECT
	REF(csel),
	IF(opflag), TCOLOR(color), // with opacity
	UNLESS(opflag), COLOR(color), // solid colors
	EVENT(CHANGE, colsel_evt),
	UNLESS(is_pal), WDONE,
	/* Task-specific part */
	BORDER(TABLE, 0),
	IFx(is_pal, 1),
		BORDER(SPIN, 0),
		HBOX,
		MLABEL(_("Scale")),
		REF(fbutton), BUTTON(_("From"), set_range_spin),
		REF(fspin), SPIN(n0, 0, 255),
		BUTTON(_("To"), set_range_spin),
		REF(tspin), SPIN(n1, 0, 255),
		XOPT(scales_txt, 4, scale),
		BUTTON(_("Create"), make_cscale),
		WDONE,
	ENDIF(1),
	IFx(is_AB, 1),
		HBOX,
		BUTTON(_("Posterize"), posterize_AB),
		REF(pspin_AB), SPIN(pos_AB, 1, 8),
		WDONE,
		TABLE(2, 3),
		REF(spin_AB[CHN_ALPHA]),
		HTSPINSLIDE(_("Alpha"), v_AB[CHN_ALPHA], 0, 255),
		EVENT(CHANGE, AB_spin_moved),
		REF(spin_AB[CHN_SEL]),
		HTSPINSLIDE(_("Selection"), v_AB[CHN_SEL], 0, 255),
		EVENT(CHANGE, AB_spin_moved),
		REF(spin_AB[CHN_MASK]),
		HTSPINSLIDE(_("Mask"), v_AB[CHN_MASK], 0, 255),
		EVENT(CHANGE, AB_spin_moved),
		WDONE,
	ENDIF(1),
	IFx(is_csel, 1),
		HBOX,
		MLABEL(_("Range")),
		REF(fspin_csel),
		FSPIN(v.csrange, 0, 76500), EVENT(CHANGE, csel_controls_changed),
		CHECK(_("Inverse"), v.csinv), EVENT(CHANGE, csel_controls_changed),
		RPACKe(csel_modes, 0, 1, v.csmode, csel_controls_changed),
		WDONE,
	ENDIF(1),
	IFx(is_grid, 1),
		BORDER(SPIN, 0),
		TABLE(6, 2),
		TLCHECK(_("Smart grid"), v.cgrid, 0, 0),
		EVENT(CHANGE, grid_controls_changed),
		TLCHECKl(_("Tile grid"), v.tgrid, 1, 0, 2),
		EVENT(CHANGE, grid_controls_changed),
		TLLABEL(_("Minimum grid zoom"), 0, 1),
		TLXSPIN(v.grid_min, 2, 12, 1, 1),
		EVENT(CHANGE, grid_controls_changed),
		TLLABEL(_("Tile width"), 2, 1),
		TLXSPIN(v.tgrid_dx, 2, MAX_WIDTH, 3, 1),
		EVENT(CHANGE, grid_controls_changed),
		TLLABEL(_("Tile height"), 4, 1),
		TLXSPIN(v.tgrid_dy, 2, MAX_HEIGHT, 5, 1),
		EVENT(CHANGE, grid_controls_changed),
		WDONE,
	ENDIF(1),
	BORDER(OKBOX, 0),
	EOKBOX(_("OK"), colsel_evt, _("Cancel"), colsel_evt),
	OKTOGGLE(_("Preview"), preview, colsel_evt),
	WSHOW
};
#undef WBbase

void colour_selector(int cs_type)
{
	colsel_dd tdata, *dt;
	unsigned char *rgb = tdata.v.rgb;
	void **res;
	int i;

	memset(&tdata, 0, sizeof(tdata));
	if (cs_type >= COLSEL_EDIT_ALL)
	{
		tdata.select = select_colour;
		tdata.name = _("Palette Editor");
		tdata.mpflag = TRUE;
		tdata.cnt = mem_cols;
		tdata.idx = cs_type - COLSEL_EDIT_ALL;
		tdata.is_pal = TRUE;

		for (i = 0; i < mem_cols; i++)
		{
			rgb[0] = mem_pal[i].red;
			rgb[1] = mem_pal[i].green;
			rgb[2] = mem_pal[i].blue;
			// Opacity (unused)
//			tdata.v.opac[i] = 255;
			rgb += 3;
		}
	}
	else if (cs_type == COLSEL_OVERLAYS)
	{
		tdata.select = select_overlay;
		tdata.name = _("Configure Overlays");
		tdata.cnames = channames_;
		tdata.opflag = TRUE;
		tdata.cnt = NUM_CHANNELS;

		for (i = 0; i < NUM_CHANNELS; i++)
		{
			tdata.v.opac[i] = channel_opacity[i];
			rgb[0] = channel_rgb[i][0];
			rgb[1] = channel_rgb[i][1];
			rgb[2] = channel_rgb[i][2];
			rgb += 3;
		}
	}
	else if (cs_type == COLSEL_EDIT_AB)
	{
		png_color *A0 = &mem_col_A24, *B0 = &mem_col_B24;

		tdata.select = select_AB;
		tdata.name = _("Colour Editor");
		tdata.cnames = AB_txt;
		tdata.mpflag = TRUE; // at cursor
		tdata.cnt = 2;
		tdata.is_AB = TRUE;

		tdata.pos_AB = inifile_get_gint32("posterizeInt", 1);

		if (mem_img_bpp == 1)
			A0 = &mem_pal[mem_col_A] , B0 = &mem_pal[mem_col_B];

		rgb[0] = A0->red; rgb[1] = A0->green; rgb[2] = A0->blue;
		rgb[3] = B0->red; rgb[4] = B0->green; rgb[5] = B0->blue;
		for (i = CHN_ALPHA; i < NUM_CHANNELS; i++)
		{
			tdata.v.AB[0][i] = tdata.v_AB[i] = channel_col_A[i];
			tdata.v.AB[1][i] = channel_col_B[i];
		}
		// Opacity (unused)
//		tdata.v.opac[0] = tdata.v.opac[1] = 255;
	}
	else if (cs_type == COLSEL_EDIT_CSEL)
	{
		if (!csel_data)
		{
			csel_init();
			if (!csel_data) return;
		}

		tdata.select = select_csel;
		tdata.name = _("Colour-Selective Mode");
		tdata.cnames = csel_txt;
		tdata.cnt = 3;
		tdata.is_csel = TRUE;

		tdata.v.csrange = rint(csel_data->range * 100);
		tdata.v.csinv = csel_data->invert;
		tdata.v.csmode = csel_data->mode;

		rgb[0] = INT_2_R(csel_data->center);
		rgb[1] = INT_2_G(csel_data->center);
		rgb[2] = INT_2_B(csel_data->center);
		rgb[3] = INT_2_R(csel_data->limit);
		rgb[4] = INT_2_G(csel_data->limit);
		rgb[5] = INT_2_B(csel_data->limit);
		rgb[6] = INT_2_R(csel_preview);
		rgb[7] = INT_2_G(csel_preview);
		rgb[8] = INT_2_B(csel_preview);
		// Opacity (unused)
//		tdata.v.opac[0] = tdata.v.opac[1] = 255;
//		tdata.v.opac[2] = csel_preview_a;
	}
	else if (cs_type == COLSEL_GRID)
	{
		tdata.select = select_grid;
		tdata.name = _("Configure Grid");
		tdata.cnames = grid_txt;
		tdata.cnt = GRID_MAX;
		tdata.is_grid = TRUE;

		tdata.v.cgrid = color_grid;
		tdata.v.tgrid = show_tile_grid;
		tdata.v.grid_min = mem_grid_min;
		tdata.v.tgrid_dx = tgrid_dx;
		tdata.v.tgrid_dy = tgrid_dy;

		for (i = 0; i < GRID_MAX; i++)
		{
			rgb[0] = INT_2_R(grid_rgb[i]);
			rgb[1] = INT_2_G(grid_rgb[i]);
			rgb[2] = INT_2_B(grid_rgb[i]);
			// Opacity (unused)
//			tdata.v.opac[i] = 255;
			rgb += 3;
		}
	}

	tdata.v0 = tdata.v; // Save original values
	res = run_create(colsel_code, &tdata, sizeof(tdata));
	dt = GET_DDATA(res);
	cmd_scroll(dt->clist, dt->idx); // scroll in current position
}

#undef _
#define _(X) __(X)

///	QUANTIZE WINDOW

#define QUAN_EXACT   0
#define QUAN_CURRENT 1
#define QUAN_PNN     2
#define QUAN_WU      3
#define QUAN_MAXMIN  4
#define QUAN_MAX     5

#define DITH_NONE	0
#define DITH_FS		1
#define DITH_STUCKI	2
#define DITH_ORDERED	3
#define DITH_DUMBFS	4
#define DITH_OLDDITHER	5
#define DITH_OLDSCATTER	6
#define DITH_MAX	7

typedef struct {
	int pflag;
	int cols, cols0;
	int err;
	char **qtxt;
	void **dith, **colspin, **errspin;
	void **book, **qbook;
} quantize_dd;

/* Quantization & dither settings - persistent */
static int quantize_mode = -1, dither_mode = -1;
static int quantize_tp;
static int dither_cspace = CSPACE_SRGB, dither_dist = DIST_L2, dither_limit;
static int dither_scan = TRUE, dither_8b, dither_sel;
static int dither_pfract[2] = { 100, 85 };

static void click_quantize_radio(quantize_dd *dt, void **wdata)
{
	int vvv[3] = { 1, 1, 256 }, n = quantize_mode; // self-updating

	cmd_set(dt->qbook, (n == QUAN_PNN) || (n == QUAN_WU) ? 2 :
		n == QUAN_CURRENT ? 1 : 0);

	if (n == QUAN_EXACT) vvv[1] = vvv[2] = dt->cols0;
	else if (n == QUAN_CURRENT) vvv[2] = mem_cols;
	cmd_read(dt->colspin, dt);
	vvv[0] = dt->cols; // !!! gets bounded when setting
	cmd_set3(dt->colspin, vvv);

	/* No dither for exact conversion */
	if (!dt->pflag) cmd_sensitive(dt->dith, n != QUAN_EXACT);
}

static void click_quantize_ok(quantize_dd *dt, void **wdata)
{
	int i, dither, new_cols, have_image = !dt->pflag, err = 0;
	int quantize_cols = dt->cols0, efrac = 0;
	png_color newpal[256];
	unsigned char *old_image = mem_img[CHN_IMAGE];

	/* Dithering filters */
	/* Floyd-Steinberg dither */
	static short fs_dither[16] =
		{ 16,  0, 0, 0, 7, 0,  0, 3, 5, 1, 0,  0, 0, 0, 0, 0 };
	/* Stucki dither */
	static short s_dither[16] =
		{ 42,  0, 0, 0, 8, 4,  2, 4, 8, 4, 2,  1, 2, 4, 2, 1 };

	run_query(wdata);
	dither = quantize_mode != QUAN_EXACT ? dither_mode : DITH_NONE;
	new_cols = dt->cols;
	if (have_image) /* Work on image */
		dither_pfract[dither_sel ? 1 : 0] = efrac = dt->err;

	run_destroy(wdata);

	/* Paranoia */
	if ((quantize_mode >= QUAN_MAX) || (dither >= DITH_MAX)) return;
	if (!have_image && (quantize_mode == QUAN_CURRENT)) return;

	i = undo_next_core(UC_NOCOPY, mem_width, mem_height,
		have_image ? 1 : mem_img_bpp, have_image ? CMASK_IMAGE : CMASK_NONE);
	if (i)
	{
		memory_errors(i);
		return;
	}

	switch (quantize_mode)
	{
	case QUAN_EXACT: /* Use image colours */
		new_cols = quantize_cols;
		mem_cols_found(newpal);
		if (have_image) err = mem_convert_indexed();
		dither = DITH_MAX;
		break;
	default:
	case QUAN_CURRENT: /* Use current palette */
		break;
	case QUAN_PNN: /* PNN quantizer */
		err = pnnquan(old_image, mem_width, mem_height, new_cols, newpal);
		break;
	case QUAN_WU: /* Wu quantizer */
		err = wu_quant(old_image, mem_width, mem_height, new_cols, newpal);
		break;
	case QUAN_MAXMIN: /* Max-Min quantizer */
		err = maxminquan(old_image, mem_width, mem_height, new_cols, newpal);
		break;
	}

	if (err) dither = DITH_MAX;
	else if (quantize_mode != QUAN_CURRENT)
	{
		memcpy(mem_pal, newpal, new_cols * sizeof(*mem_pal));
		mem_cols = new_cols;
	}
	else if (quantize_tp) mem_cols = new_cols; // Truncate palette

	if (!have_image) /* Palette only */
	{
		if (err < 0) memory_errors(1);
		update_stuff(UPD_PAL | CF_MENU);
		return;
	}

	switch (dither)
	{
	case DITH_NONE:
	case DITH_FS:
	case DITH_STUCKI:
		err = mem_dither(old_image, new_cols, dither == DITH_NONE ?
			NULL : dither == DITH_FS ? fs_dither : s_dither,
			dither_cspace, dither_dist, dither_limit, dither_sel,
			dither_scan, dither_8b, efrac * 0.01);
		break;

// !!! No code yet - temporarily disabled !!!
//	case DITH_ORDERED:

	case DITH_DUMBFS:
		err = mem_dumb_dither(old_image, mem_img[CHN_IMAGE],
			mem_pal, mem_width, mem_height, new_cols, TRUE);
		break;
	case DITH_OLDDITHER:
		err = mem_quantize(old_image, new_cols, 2);
		break;
	case DITH_OLDSCATTER:
		err = mem_quantize(old_image, new_cols, 3);
		break;
	case DITH_MAX: /* Stay silent unless a memory error happened */
		err = err < 0;
		break;
	}
	if (err) memory_errors(1);

	/* Image was converted */
	mem_col_A = mem_cols > 1 ? 1 : 0;
	mem_col_B = 0;
	update_stuff(UPD_2IDX);
}

static void choose_selective(quantize_dd *dt, void **wdata, int what, void **where)
{
	int i = dither_sel;

	cmd_read(where, dt);

	/* Selectivity state toggled */
	if ((i = !i) ^ !dither_sel)
	{
		cmd_read(dt->errspin, dt);
		dither_pfract[i ^ 1] = dt->err;
		cmd_set(dt->errspin, dither_pfract[i]);
	}
}

#undef _
#define _(X) X

static char *quan_txt[] = { _("Exact Conversion"), _("Use Current Palette"),
	_("PNN Quantize (slow, better quality)"),
	_("Wu Quantize (fast)"),
	_("Max-Min Quantize (best for small palettes and dithering)"), NULL };
static char *dith_txt[] = { _("None"), _("Floyd-Steinberg"), _("Stucki"),
// !!! "Ordered" not done yet !!!
	/* _("Ordered") */ "",
	_("Floyd-Steinberg (quick)"), _("Dithered (effect)"),
	_("Scattered (effect)"), NULL };
static char *clamp_txt[] = { _("Gamut"), _("Weakly"), _("Strongly") };
static char *err_txt[] = { _("Off"), _("Separate/Sum"), _("Separate/Split"),
	_("Length/Sum"), _("Length/Split"), NULL };
static char *dist_txt[] = { _("Largest (Linf)"), _("Sum (L1)"), _("Euclidean (L2)") };

#define WBbase quantize_dd
static void *quantize_code[] = {
	IF(pflag), WINDOWm(_("Create Quantized")),
	UNLESS(pflag), WINDOWm(_("Convert To Indexed")),
	BORDER(FRBOX, 0),
	HBOXb(5, 10), MLABEL(_("Indexed Colours To Use")),
	REF(colspin), XSPIN(cols, 1, 256),
	UNLESS(pflag), BOOKBTN(_("Settings"), book),
	WDONE,
	REF(book), UNLESS(pflag), PLAINBOOK,
	/* Main page - Palette frame */
	FVBOX(_("Palette")),
	RPACKDve(qtxt, 0, quantize_mode, click_quantize_radio), TRIGGER,
	REF(qbook), PLAINBOOKn(3),
	WDONE, // empty page 0
	CHECKv(_("Truncate palette"), quantize_tp), WDONE, // page 1
	CHECKv(_("Diameter based weighting"), quan_sqrt), WDONE, // page 2
	WDONE,
	UNLESSx(pflag, 1),
		/* Main page - Dither frame */
		REF(dith),
		FRPACKv(_("Dither"), dith_txt, 0, DITH_MAX / 2, dither_mode),
		WDONE,
		/* Settings page */
		FRPACKv(_("Colour space"), cspnames_, NUM_CSPACES, 1, dither_cspace),
		FRPACKv(_("Difference measure"), dist_txt, 3, 1, dither_dist),
		FRPACKv(_("Reduce colour bleed"), clamp_txt, 3, 1, dither_limit),
		CHECKv(_("Serpentine scan"), dither_scan),
		TABLE2(2),
		REF(errspin), TSPIN(_("Error propagation, %"), err, 0, 100),
		TLLABEL(_("Selective error propagation"), 0, 1),
		TLOPTve(err_txt, 0, dither_sel, choose_selective, 1, 1),
		WDONE,
		CHECKv(_("Full error precision"), dither_8b),
		WDONE,
	ENDIF(1),
	/* OK / Cancel */
	BORDER(OKBOX, 0),
	OKBOX(_("OK"), click_quantize_ok, _("Cancel"), NULL),
	WSHOW
};
#undef WBbase

#undef _
#define _(X) __(X)

void pressed_quantize(int palette)
{
	char *qnames[sizeof(quan_txt) / sizeof(quan_txt[0])];
	quantize_dd tdata;

	tdata.pflag = palette;
	tdata.cols = tdata.cols0 = mem_cols_used(257);
	tdata.qtxt = qnames;

	memcpy(qnames, quan_txt, sizeof(qnames));
	/* No exact transfer if too many colours */
	if (tdata.cols > 256) qnames[QUAN_EXACT] = "";
	if (palette) qnames[QUAN_CURRENT] = "";
	if ((quantize_mode < 0) || !qnames[quantize_mode][0]) // Use default mode
	{
		quantize_mode = palette || (tdata.cols > 256) ? QUAN_WU : QUAN_EXACT;
		if (!palette) dither_mode = -1; // Reset dither too
	}

	if (!palette)
	{
		if (dither_mode < 0) dither_mode = tdata.cols > 256 ?
			DITH_DUMBFS : DITH_NONE;
		tdata.err = dither_pfract[dither_sel ? 1 : 0];
	}

	run_create(quantize_code, &tdata, sizeof(tdata));
}

///	GRADIENT WINDOW

static int grad_channel;
static grad_info grad_temps[NUM_CHANNELS];
static grad_map grad_tmaps[NUM_CHANNELS + 1];
static grad_store grad_tbytes;

static GtkWidget *grad_window;
static GtkWidget *grad_spin_len, *grad_spin_rep, *grad_spin_ofs;
static GtkWidget *grad_opt_type, *grad_opt_bound;
static GtkWidget *grad_ss_pre;
static GtkWidget *grad_opt_gtype, *grad_opt_otype;
static GtkWidget *grad_check_grev, *grad_check_orev;

static unsigned char grad_pad[GRAD_POINTS * 3], grad_mpad[GRAD_POINTS];
static int grad_cnt, grad_ofs, grad_slot, grad_mode;

static GtkWidget *grad_ed_cs, *grad_ed_opt, *grad_ed_ss, *grad_ed_tog, *grad_ed_pm;
static GtkWidget *grad_ed_len, *grad_ed_bar[16], *grad_ed_left, *grad_ed_right;

#define SLOT_SIZE 15

#define PPAD_SLOT 11
#define PPAD_XSZ 32
#define PPAD_YSZ 8
#define PPAD_WIDTH(X) (PPAD_XSZ * (X) - 1)
#define PPAD_HEIGHT(X) (PPAD_YSZ * (X) - 1)

static void palette_pad_set(int value)
{
	wjpixmap_move_cursor(grad_ed_pm, (value % PPAD_XSZ) * PPAD_SLOT,
		(value / PPAD_XSZ) * PPAD_SLOT);
}

static void palette_pad_draw(GtkWidget *widget, gpointer user_data)
{
	unsigned char *rgb;
	int w, h, col, cellsize = PPAD_SLOT;

	if (!wjpixmap_pixmap(widget)) return;
	w = PPAD_WIDTH(cellsize);
	h = PPAD_HEIGHT(cellsize);
	rgb = render_color_grid(w, h, cellsize, (int)user_data);
	if (!rgb) return;
	palette_pad_set(0);
	wjpixmap_draw_rgb(widget, 0, 0, w, h, rgb, w * 3);
	col = (cellsize >> 1) - 1;
	wjpixmap_set_cursor(widget, xbm_ring4_bits, xbm_ring4_mask_bits,
		xbm_ring4_width, xbm_ring4_height,
		xbm_ring4_x_hot - col, xbm_ring4_y_hot - col, TRUE);

	free(rgb);
}

static void grad_edit_set_rgb(GtkWidget *selection, gpointer user_data);
static void palette_pad_select(int i, int mode)
{
	palette_pad_set(i);
	/* Indexed / utility / opacity */
	if (!mode)
	{
		/* !!! Signal needs be sent even if value stays the same */
		GtkAdjustment *adj = SPINSLIDE_ADJUSTMENT(grad_ed_ss);
		adj->value = i;
		gtk_adjustment_value_changed(adj);
	}
	else if (i < mem_cols) /* Valid RGB */
	{
		cpick_set_colour(grad_ed_cs, PNG_2_INT(mem_pal[i]), 255);
// !!! GTK+2 emits "color_changed" when setting color, others need explicit call
#if GTK_MAJOR_VERSION == 1 || defined U_CPICK_MTPAINT
		grad_edit_set_rgb(grad_ed_cs, NULL);
#endif
	}
}

static gboolean palette_pad_click(GtkWidget *widget, GdkEventButton *event,
	gpointer user_data)
{
	int x = event->x, y = event->y;

	gtk_widget_grab_focus(widget);
	/* Only single clicks */
	if (event->type != GDK_BUTTON_PRESS) return (TRUE);
	x /= PPAD_SLOT; y /= PPAD_SLOT;
	palette_pad_select(y * PPAD_XSZ + x, (int)user_data);
	return (TRUE);
}

static gboolean palette_pad_key(GtkWidget *widget, GdkEventKey *event,
	gpointer user_data)
{
	int x, y, dx, dy;

	if (!arrow_key(event, &dx, &dy, 0)) return (FALSE);
	wjpixmap_cursor(widget, &x, &y);
	x = x / PPAD_SLOT + dx; y = y / PPAD_SLOT + dy;
	y = y < 0 ? 0 : y >= PPAD_YSZ ? PPAD_YSZ - 1 : y;
	y = y * PPAD_XSZ + x;
	y = y < 0 ? 0 : y > 255 ? 255 : y;
	palette_pad_select(y, (int)user_data);
#if GTK_MAJOR_VERSION == 1
	/* Return value alone doesn't stop GTK1 from running other handlers */
	gtk_signal_emit_stop_by_name(GTK_OBJECT(widget), "key_press_event");
#endif
	return (TRUE);
}

static GtkWidget *grad_interp_menu(int value, int allow_const, GtkSignalFunc handler)
{
	char *interp[] = { _("RGB"), _("sRGB"), _("HSV"), _("Backward HSV"),
		_("Constant") };

	return (wj_option_menu(interp, allow_const ? 5 : 4, value, NULL, handler));
}

static void click_grad_edit_ok(GtkWidget *widget)
{
	int idx = (grad_channel == CHN_IMAGE) && (mem_img_bpp == 3) ? 0 :
		grad_channel + 1;

	if (grad_mode < 0) /* Opacity */
	{
		memcpy(grad_tbytes + GRAD_CUSTOM_OPAC(idx), grad_pad, GRAD_POINTS);
		memcpy(grad_tbytes + GRAD_CUSTOM_OMAP(idx), grad_mpad, GRAD_POINTS);
		grad_tmaps[idx].coplen = grad_cnt;
	}
	else /* Gradient */
	{
		memcpy(grad_tbytes + GRAD_CUSTOM_DATA(idx), grad_pad,
			idx ? GRAD_POINTS : GRAD_POINTS * 3);
		memcpy(grad_tbytes + GRAD_CUSTOM_DMAP(idx), grad_mpad, GRAD_POINTS);
		grad_tmaps[idx].cvslen = grad_cnt;
	}
	gtk_widget_destroy(widget);
}

static void grad_load_slot(int slot)
{
	if (slot >= grad_cnt) /* Empty slot */
	{
		grad_slot = slot;
		return;
	}
	grad_slot = -1; /* Block circular signal calls */

	if (!grad_mode) /* RGB */
	{
		unsigned char *gp = grad_pad + slot * 3;
		int rgb = MEM_2_INT(gp, 0);
		cpick_set_colour(grad_ed_cs, rgb, 255);
		cpick_set_colour_previous(grad_ed_cs, rgb, 255);

		gtk_option_menu_set_history(GTK_OPTION_MENU(grad_ed_opt),
			grad_mpad[slot]);
	}
	else /* Indexed / utility / opacity */
	{
		mt_spinslide_set_value(grad_ed_ss, grad_pad[slot]);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(grad_ed_tog),
			grad_mpad[slot] == GRAD_TYPE_CONST);
		palette_pad_set(grad_pad[slot]);
	}
	grad_slot = slot;
}

static void grad_redraw_slots(int idx0, int idx1)
{
	if (idx0 < grad_ofs) idx0 = grad_ofs;
	if (++idx1 > grad_ofs + 16) idx1 = grad_ofs + 16;
	for (; idx0 < idx1; idx0++)
		gtk_widget_queue_draw(GTK_BIN(grad_ed_bar[idx0 - grad_ofs])->child);
}

static void grad_edit_set_mode()
{
	int mode, ix0 = grad_slot;

	if (grad_slot < 0) return; /* Blocked */
	if (!grad_mode) mode = wj_option_menu_get_history(grad_ed_opt);
	else mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(grad_ed_tog)) ?
		GRAD_TYPE_CONST : GRAD_TYPE_RGB;
	grad_mpad[grad_slot] = mode;

	if (grad_cnt <= grad_slot)
	{
		ix0 = grad_cnt;
		grad_cnt = grad_slot + 1;
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(grad_ed_len), grad_cnt);
	}
	grad_redraw_slots(ix0, grad_slot);
}

static void grad_edit_set_rgb(GtkWidget *selection, gpointer user_data)
{
	int i, rgb;

	if (grad_slot < 0) return; /* Blocked */
	rgb = cpick_get_colour(selection, NULL);
	i = grad_slot * 3;
	grad_pad[i++] = INT_2_R(rgb);
	grad_pad[i++] = INT_2_G(rgb);
	grad_pad[i  ] = INT_2_B(rgb);
	grad_edit_set_mode();
}

static void grad_edit_move_slide(GtkAdjustment *adj, gpointer user_data)
{
	if (grad_slot < 0) return; /* Blocked */
	palette_pad_set(grad_pad[grad_slot] = ADJ2INT(adj));
	grad_edit_set_mode();
}

static void grad_edit_length(GtkAdjustment *adj, gpointer user_data)
{
	int l0 = grad_cnt;
	grad_cnt = ADJ2INT(adj);
	if (l0 < grad_cnt) grad_redraw_slots(l0, grad_cnt - 1);
	else if (l0 > grad_cnt) grad_redraw_slots(grad_cnt, l0 - 1);
}

static void grad_edit_scroll(GtkButton *button, gpointer user_data)
{
	int dir = (int)user_data;
	
	grad_ofs += dir;
	gtk_widget_set_sensitive(grad_ed_left, !!grad_ofs);
	gtk_widget_set_sensitive(grad_ed_right, grad_ofs < GRAD_POINTS - 16);
	grad_redraw_slots(0, GRAD_POINTS);
}

static void grad_edit_slot(GtkWidget *btn, gpointer user_data)
{
	if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn))) return;
	grad_load_slot((int)user_data + grad_ofs);
}

static gboolean grad_draw_slot(GtkWidget *widget, GdkEventExpose *event,
	gpointer idx)
{
	unsigned char rgb[SLOT_SIZE * 2 * 3];
	int i, n = (int)idx + grad_ofs, mode = n >= grad_cnt ? -2 : grad_mode;

	switch (mode)
	{
	default: /* Error */
	case -2: /* Empty */
		rgb[0] = rgb[1] = rgb[2] = 178;
		break;
	case -1: /* Opacity */
		rgb[0] = rgb[1] = rgb[2] = grad_pad[n];
		break;
	case 0: /* RGB */
		rgb[0] = grad_pad[n * 3 + 0];
		rgb[1] = grad_pad[n * 3 + 1];
		rgb[2] = grad_pad[n * 3 + 2];
		break;
	case CHN_IMAGE + 1: /* Indexed */
		i = grad_pad[n];
		if (i >= mem_cols) rgb[0] = rgb[1] = rgb[2] = 0;
		else
		{
			rgb[0] = mem_pal[i].red;
			rgb[1] = mem_pal[i].green;
			rgb[2] = mem_pal[i].blue;
		}
		break;
	case CHN_ALPHA + 1: /* Alpha */
	case CHN_SEL + 1: /* Selection */
	case CHN_MASK + 1: /* Mask */
		i = channel_rgb[mode - 1][0] * grad_pad[n];
		rgb[0] = (i + (i >> 8) + 1) >> 8;
		i = channel_rgb[mode - 1][1] * grad_pad[n];
		rgb[1] = (i + (i >> 8) + 1) >> 8;
		i = channel_rgb[mode - 1][2] * grad_pad[n];
		rgb[2] = (i + (i >> 8) + 1) >> 8;
		break;
	}
	for (i = 3; i < SLOT_SIZE * 2 * 3; i++) rgb[i] = rgb[i - 3];
	if (mode == -2) /* Empty slot - show that */
		memset(rgb, 128, SLOT_SIZE * 3);

	gdk_draw_rgb_image(widget->window, widget->style->black_gc,
		0, 0, SLOT_SIZE, SLOT_SIZE, GDK_RGB_DITHER_NONE,
		rgb + SLOT_SIZE * 3, -3);

	return (TRUE);
}

static void grad_edit(GtkWidget *widget, gpointer user_data)
{
	GtkWidget *win, *mainbox, *hbox, *hbox2, *pix, *cs, *ss, *sw, *btn;
	int i, idx, opac = (int)user_data != 0;

	idx = (grad_channel == CHN_IMAGE) && (mem_img_bpp == 3) ? 0 :
		grad_channel + 1;

	/* Copy to temp */
	if (opac)
	{
		memcpy(grad_pad, grad_tbytes + GRAD_CUSTOM_OPAC(idx), GRAD_POINTS);
		memcpy(grad_mpad, grad_tbytes + GRAD_CUSTOM_OMAP(idx), GRAD_POINTS);
		grad_cnt = grad_tmaps[idx].coplen;
	}
	else
	{
		memcpy(grad_pad, grad_tbytes + GRAD_CUSTOM_DATA(idx),
			idx ? GRAD_POINTS : GRAD_POINTS * 3);
		memcpy(grad_mpad, grad_tbytes + GRAD_CUSTOM_DMAP(idx), GRAD_POINTS);
		grad_cnt = grad_tmaps[idx].cvslen;
	}
	if (grad_cnt < 2) grad_cnt = 2;
	grad_ofs = grad_slot = 0;
	grad_mode = opac ? -1 : idx;

	win = add_a_window(GTK_WINDOW_TOPLEVEL, _("Edit Gradient"),
		GTK_WIN_POS_CENTER, TRUE);
	mainbox = gtk_vbox_new(FALSE, 5);
	gtk_container_set_border_width(GTK_CONTAINER(mainbox), 5);
	gtk_container_add(GTK_CONTAINER(win), mainbox);

	/* Palette pad */

	pix = grad_ed_pm = pack(mainbox, wjpixmap_new(PPAD_WIDTH(PPAD_SLOT),
		PPAD_HEIGHT(PPAD_SLOT)));
	gtk_signal_connect(GTK_OBJECT(pix), "realize",
		GTK_SIGNAL_FUNC(palette_pad_draw),
		(gpointer)(opac ? -1 : grad_channel));
	gtk_signal_connect(GTK_OBJECT(pix), "button_press_event",
		GTK_SIGNAL_FUNC(palette_pad_click), (gpointer)!grad_mode);
	gtk_signal_connect(GTK_OBJECT(pix), "key_press_event",
		GTK_SIGNAL_FUNC(palette_pad_key), (gpointer)!grad_mode);
	add_hseparator(mainbox, -2, 10);

	/* Editor widgets */

	if (!grad_mode) /* RGB */
	{
		grad_ed_cs = cs = pack(mainbox, cpick_create());
		cpick_set_opacity_visibility( cs, FALSE );

		gtk_signal_connect(GTK_OBJECT(cs), "color_changed",
			GTK_SIGNAL_FUNC(grad_edit_set_rgb), NULL);
		grad_ed_opt = sw = grad_interp_menu(0, TRUE,
			GTK_SIGNAL_FUNC(grad_edit_set_mode));
	}
	else /* Indexed / utility / opacity */
	{
		grad_ed_ss = ss = pack(mainbox, mt_spinslide_new(-2, -2));
		mt_spinslide_set_range(ss, 0, grad_mode == CHN_IMAGE + 1 ?
			mem_cols - 1 : 255);
		mt_spinslide_connect(ss, GTK_SIGNAL_FUNC(grad_edit_move_slide), NULL);
		grad_ed_tog = sw = sig_toggle(_("Constant"), FALSE, NULL,
			GTK_SIGNAL_FUNC(grad_edit_set_mode));
	}
	hbox = pack(mainbox, gtk_hbox_new(TRUE, 5));
	xpack(hbox, sw);
	hbox2 = xpack(hbox, gtk_hbox_new(FALSE, 5));
	pack(hbox2, gtk_label_new(_("Points:")));
	grad_ed_len = sw = pack(hbox2, add_a_spin(grad_cnt, 2, GRAD_POINTS));
	spin_connect(sw, GTK_SIGNAL_FUNC(grad_edit_length), NULL);

	/* Gradient bar */

	hbox2 = pack(mainbox, gtk_hbox_new(TRUE, 0));
	grad_ed_left = btn = xpack(hbox2, gtk_button_new());
	gtk_container_add(GTK_CONTAINER(btn), gtk_arrow_new(GTK_ARROW_LEFT,
		GTK_SHADOW_NONE));
	gtk_widget_set_sensitive(btn, FALSE);
	gtk_signal_connect(GTK_OBJECT(btn), "clicked",
		GTK_SIGNAL_FUNC(grad_edit_scroll), (gpointer)(-1));
	btn = NULL;
	for (i = 0; i < 16; i++)
	{
		grad_ed_bar[i] = btn = xpack(hbox2, gtk_radio_button_new_from_widget(
			GTK_RADIO_BUTTON_0(btn)));
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(btn), FALSE);
		gtk_signal_connect(GTK_OBJECT(btn), "toggled",
			GTK_SIGNAL_FUNC(grad_edit_slot), (gpointer)i);
		sw = gtk_drawing_area_new();
		gtk_container_add(GTK_CONTAINER(btn), sw);
		gtk_widget_set_usize(sw, SLOT_SIZE, SLOT_SIZE);
		gtk_signal_connect(GTK_OBJECT(sw), "expose_event",
			GTK_SIGNAL_FUNC(grad_draw_slot), (gpointer)i);
	}
	grad_ed_right = btn = xpack(hbox2, gtk_button_new());
	gtk_container_add(GTK_CONTAINER(btn), gtk_arrow_new(GTK_ARROW_RIGHT,
		GTK_SHADOW_NONE));
	gtk_signal_connect(GTK_OBJECT(btn), "clicked",
		GTK_SIGNAL_FUNC(grad_edit_scroll), (gpointer)1);

	pack(mainbox, OK_box(0, win, _("OK"), GTK_SIGNAL_FUNC(click_grad_edit_ok),
		_("Cancel"), GTK_SIGNAL_FUNC(gtk_widget_destroy)));

#ifndef U_CPICK_MTPAINT
	grad_load_slot(0);
#endif
	gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(grad_window));
	gtk_widget_show_all(win);
#ifdef U_CPICK_MTPAINT
	grad_load_slot(0);
#endif

#if GTK_MAJOR_VERSION == 1
	gtk_widget_queue_resize(win); /* Re-render sliders */
#endif
}

#define NUM_GTYPES 7
#define NUM_OTYPES 3
static const char gtmap[NUM_GTYPES * 2] = { GRAD_TYPE_RGB, 1, GRAD_TYPE_RGB, 2,
	GRAD_TYPE_SRGB, 2, GRAD_TYPE_HSV, 2, GRAD_TYPE_BK_HSV, 2,
	GRAD_TYPE_CONST, 3, GRAD_TYPE_CUSTOM, 3 };
static const char opmap[NUM_OTYPES] = { GRAD_TYPE_RGB, GRAD_TYPE_CONST,
	GRAD_TYPE_CUSTOM };

static void grad_reset_menu(int mode, int bpp)
{
	GList *items = GTK_MENU_SHELL(gtk_option_menu_get_menu(
		GTK_OPTION_MENU(grad_opt_gtype)))->children;
	char f = bpp == 1 ? 1 : 2;
	int i, j;

	for (j = NUM_GTYPES - 1; j >= 0; j--)
	{
		if ((gtmap[j * 2] == mode) && (gtmap[j * 2 + 1] & f)) break;
	}

	for (; items; items = items->next)
	{
		i = (int)gtk_object_get_user_data(GTK_OBJECT(items->data));
		widget_showhide(GTK_WIDGET(items->data), gtmap[2 * i + 1] & f);
	}
	gtk_option_menu_set_history(GTK_OPTION_MENU(grad_opt_gtype), j);
}

static void store_channel_gradient(int channel)
{
	grad_info *grad = grad_temps + channel;
	grad_map *gmap = grad_tmaps + channel + 1;

	if (channel < 0) return;
	if ((channel == CHN_IMAGE) && (mem_img_bpp == 3)) gmap = grad_tmaps;

	grad->len = read_spin(grad_spin_len);
	grad->gmode = wj_option_menu_get_history(grad_opt_type) + GRAD_MODE_LINEAR;
	grad->rep = read_spin(grad_spin_rep);
	grad->rmode = wj_option_menu_get_history(grad_opt_bound);
	grad->ofs = read_spin(grad_spin_ofs);

	gmap->gtype = gtmap[2 * wj_option_menu_get_history(grad_opt_gtype)];
	gmap->grev = !!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(grad_check_grev));
	gmap->otype = opmap[wj_option_menu_get_history(grad_opt_otype)];
	gmap->orev = !!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(grad_check_orev));
}

static void show_channel_gradient(int channel)
{
	grad_info *grad = grad_temps + channel;
	grad_map *gmap;
	int i, idx = channel + 1, bpp = BPP(channel);

	if (bpp == 3) --idx;
	gmap = grad_tmaps + idx;

	gtk_spin_button_set_value(GTK_SPIN_BUTTON(grad_spin_len), grad->len);
	gtk_option_menu_set_history(GTK_OPTION_MENU(grad_opt_type),
		grad->gmode - GRAD_MODE_LINEAR);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(grad_spin_rep), grad->rep);
	gtk_option_menu_set_history(GTK_OPTION_MENU(grad_opt_bound), grad->rmode);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(grad_spin_ofs), grad->ofs);

	grad_reset_menu(gmap->gtype, bpp);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(grad_check_grev), gmap->grev);
	for (i = NUM_OTYPES - 1; (i >= 0) && (opmap[i] != gmap->otype); i--);
	gtk_option_menu_set_history(GTK_OPTION_MENU(grad_opt_otype), i);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(grad_check_orev), gmap->orev);
}

static void click_grad_apply(GtkWidget *widget)
{
	int i;

	store_channel_gradient(grad_channel);
	memcpy(gradient, grad_temps, sizeof(grad_temps));
	memcpy(graddata, grad_tmaps, sizeof(grad_tmaps));
	memcpy(gradbytes, grad_tbytes, sizeof(grad_tbytes));

	grad_opacity = mt_spinslide_get_value(grad_ss_pre);

	for (i = 0; i < NUM_CHANNELS; i++) grad_update(gradient + i);
	for (i = 0; i <= NUM_CHANNELS; i++) gmap_setup(graddata + i, gradbytes, i);
	update_stuff(UPD_GRAD);
}

static void click_grad_ok(GtkWidget *widget)
{
	click_grad_apply(widget);
	gtk_widget_destroy(widget);
}

static void grad_channel_changed(GtkToggleButton *widget, gpointer user_data)
{
	if ((int)user_data == grad_channel) return;
	if (!gtk_toggle_button_get_active(widget)) return;
	store_channel_gradient(grad_channel);
	grad_channel = -1;
	show_channel_gradient((int)user_data);
	grad_channel = (int)user_data;
}

static void grad_selector_box(GtkWidget *box, char **mtext, int op)
{
	GtkWidget *vbox, *hbox, *menu, *rev, *btn;

	vbox = gtk_vbox_new(FALSE, 0);
	add_with_frame_x(box, op ? _("Opacity") : _("Gradient"), vbox, 5, TRUE);
	menu = pack(vbox, wj_option_menu(mtext, -1, 0, NULL, NULL));
	gtk_container_set_border_width(GTK_CONTAINER(menu), 5);
	hbox = pack(vbox, gtk_hbox_new(TRUE, 0));
	rev = add_a_toggle(_("Reverse"), hbox, FALSE);
	btn = add_a_button(_("Edit Custom"), 5, hbox, TRUE);
	gtk_signal_connect(GTK_OBJECT(btn), "clicked",
		GTK_SIGNAL_FUNC(grad_edit), (gpointer)op);
	if (op)
	{
		grad_opt_otype = menu;
		grad_check_orev = rev;
	}
	else
	{
		grad_opt_gtype = menu;
		grad_check_grev = rev;
	}
}

void gradient_setup(int mode)
{
	char *gtypes[] = {_("Linear"), _("Bilinear"), _("Radial"), _("Square"),
		_("Angular"), _("Conical")};
	char *rtypes[] = {_("None"), _("Level"), _("Repeat"), _("Mirror")};
	char *gradtypes[] = {_("A to B"), _("A to B (RGB)"), _("A to B (sRGB)"),
		_("A to B (HSV)"), _("A to B (backward HSV)"), _("A only"),
		_("Custom"), NULL};
	char *optypes[] = {_("Current to 0"), _("Current only"), _("Custom"), NULL};
	GtkWidget *win, *mainbox, *hbox, *table, *align;
	GtkWindowPosition pos = !mode && !inifile_get_gboolean("centerSettings", TRUE) ?
		GTK_WIN_POS_MOUSE : GTK_WIN_POS_CENTER;

	memcpy(grad_temps, gradient, sizeof(grad_temps));
	memcpy(grad_tmaps, graddata, sizeof(grad_tmaps));
	memcpy(grad_tbytes, gradbytes, sizeof(grad_tbytes));
	grad_channel = mem_channel;

	grad_window = win = add_a_window(GTK_WINDOW_TOPLEVEL,
		_("Configure Gradient"), pos, TRUE);
	mainbox = add_vbox(win);

	/* Channel box */

	hbox = wj_radio_pack(allchannames, 4, 1, mem_channel, NULL,
		GTK_SIGNAL_FUNC(grad_channel_changed));
	add_with_frame(mainbox, _("Channel"), hbox);

	/* Setup block */

	table = add_a_table(3, 4, 5, mainbox);
	add_to_table(_("Length"), table, 0, 0, 5);
	grad_spin_len = spin_to_table(table, 0, 1, 5, 0, 0, MAX_GRAD);
	add_to_table(_("Repeat length"), table, 1, 0, 5);
	grad_spin_rep = spin_to_table(table, 1, 1, 5, 0, 0, MAX_GRAD);
	add_to_table(_("Offset"), table, 2, 0, 5);
	grad_spin_ofs = spin_to_table(table, 2, 1, 5, 0, -MAX_GRAD, MAX_GRAD);
	add_to_table(_("Gradient type"), table, 0, 2, 5);
	grad_opt_type = wj_option_menu(gtypes, 6, 0, NULL, NULL);
	to_table(grad_opt_type, table, 0, 3, 5);
	add_to_table(_("Extension type"), table, 1, 2, 5);
	grad_opt_bound = wj_option_menu(rtypes, 4, 0, NULL, NULL);
	to_table(grad_opt_bound, table, 1, 3, 5);
	add_to_table(_("Preview opacity"), table, 2, 2, 5);
	grad_ss_pre = mt_spinslide_new(-1, -1);
	mt_spinslide_set_range(grad_ss_pre, 0, 255);
	mt_spinslide_set_value(grad_ss_pre, grad_opacity);
	/* !!! Box derivatives can't have their "natural" size set directly */
	align = widget_align_minsize(grad_ss_pre, 200, -2);
	to_table(align, table, 2, 3, 5);

	/* Select page */

	hbox = pack(mainbox, gtk_hbox_new(TRUE, 0));
	grad_selector_box(hbox, gradtypes, 0);
	grad_selector_box(hbox, optypes, 1);

	/* Cancel / Apply / OK */

	align = pack(mainbox, OK_box(0, win, _("OK"), GTK_SIGNAL_FUNC(click_grad_ok),
		_("Cancel"), GTK_SIGNAL_FUNC(gtk_widget_destroy)));
	OK_box_add(align, _("Apply"), GTK_SIGNAL_FUNC(click_grad_apply));

	/* Fill in values */

	gtk_widget_show_all(mainbox);
	show_channel_gradient(mem_channel);

	gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(main_window));
	gtk_widget_show(win);

#if GTK_MAJOR_VERSION == 1
	/* Re-render sliders, adjust option menus */
	gtk_widget_queue_resize(win);
#endif
}

/// GRADIENT PICKER

static int pickg_grad = GRAD_TYPE_RGB, pickg_cspace = CSPACE_LXN;

static int do_pick_gradient(GtkWidget *table, gpointer fdata)
{
	unsigned char buf[256];
	int len;

	pickg_grad = wj_option_menu_get_history(table_slot(table, 0, 1));
	pickg_cspace = wj_option_menu_get_history(table_slot(table, 1, 1));

	len = mem_pick_gradient(buf, pickg_cspace, pickg_grad);

	mem_clip_new(len, 1, 1, CMASK_IMAGE, FALSE);
	if (mem_clipboard) memcpy(mem_clipboard, buf, len);

	update_stuff(UPD_XCOPY);
	pressed_paste(TRUE);

	return TRUE;
}

void pressed_pick_gradient()
{
	GtkWidget *table = gtk_table_new(2, 2, FALSE);

	gtk_container_set_border_width(GTK_CONTAINER(table), 5);
	to_table(grad_interp_menu(pickg_grad, FALSE, NULL), table, 0, 1, 5);
	to_table(wj_option_menu(cspnames, NUM_CSPACES, pickg_cspace, NULL, NULL),
		table, 1, 1, 5);
	add_to_table(_("Gradient"), table, 0, 0, 5);
	add_to_table(_("Colour space"), table, 1, 0, 5);
	gtk_widget_show_all(table);
	filter_window(_("Pick Gradient"), table, do_pick_gradient, NULL, FALSE);
}

/// SKEW WINDOW

typedef struct {
	GtkWidget *angle[2], *ofs[2], *dist[2], *gc;
	int angles, lock;
} skew_widgets;

static int skew_mode = 6;

static void click_skew_ok(GtkWidget *widget, gpointer user_data)
{
	skew_widgets *sw = gtk_object_get_user_data(GTK_OBJECT(widget));
	double xskew, yskew;
	int res, ftype = 0, gcor = FALSE;

	if ((sw->angles & 1) || GTK_WIDGET_HAS_FOCUS(sw->angle[0]))
		xskew = tan(read_float_spin(sw->angle[0]) * (M_PI / 180.0));
	else xskew = read_float_spin(sw->ofs[0]) / (double)read_spin(sw->dist[0]);
	if ((sw->angles & 2) || GTK_WIDGET_HAS_FOCUS(sw->angle[1]))
		yskew = tan(read_float_spin(sw->angle[1]) * (M_PI / 180.0));
	else yskew = read_float_spin(sw->ofs[1]) / (double)read_spin(sw->dist[1]);

	if (!xskew && !yskew)
	{
		alert_same_geometry();
		return;
	}

	if (mem_img_bpp == 3)
	{
		ftype = skew_mode;
		gcor = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sw->gc));
	}

	res = mem_skew(xskew, yskew, ftype, gcor);
	if (!res)
	{
		update_stuff(UPD_GEOM);
		destroy_dialog(widget);
	}
	else memory_errors(res);
}

static void skew_moved(GtkAdjustment *adjustment, gpointer user_data)
{
	skew_widgets *sw = user_data;
	int i;

	if (sw->lock) return; // Avoid recursion
	sw->lock = TRUE;

	for (i = 0; i < 2; i++)
	{
		/* Offset for angle */
		if (adjustment == GTK_SPIN_BUTTON(sw->angle[i])->adjustment)
		{
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw->ofs[i]),
				ADJ2INT(GTK_SPIN_BUTTON(sw->dist[i])->adjustment) *
				tan(adjustment->value * (M_PI / 180.0)));
			sw->angles |= 1 << i;
		}
		/* Angle for offset */
		else if ((adjustment == GTK_SPIN_BUTTON(sw->ofs[i])->adjustment) ||
			(adjustment == GTK_SPIN_BUTTON(sw->dist[i])->adjustment))
		{
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw->angle[i]),
				atan(GTK_SPIN_BUTTON(sw->ofs[i])->adjustment->value /
				ADJ2INT(GTK_SPIN_BUTTON(sw->dist[i])->adjustment)) *
				(180.0 / M_PI));
			sw->angles &= ~(1 << i);
		}
	}

	sw->lock = FALSE;
}

static GtkWidget *filter_pack(int idx, int *var)
{
	char *fnames[sizeof(scale_modes) / sizeof(scale_modes[0])];
	int i;

	for (i = 0; scale_modes[i]; i++) fnames[i] = _(scale_modes[i]);
	fnames[i] = NULL;
	fnames[1] = _("Bilinear");
	return (wj_radio_pack(fnames, -1, 0, idx, var, NULL));
}

void pressed_skew()
{
	GtkWidget *skew_window, *vbox, *table;
	skew_widgets *sw;


	skew_window = add_a_window(GTK_WINDOW_TOPLEVEL, _("Skew"), GTK_WIN_POS_CENTER, TRUE);
	sw = bound_malloc(skew_window, sizeof(skew_widgets));
	gtk_object_set_user_data(GTK_OBJECT(skew_window), (gpointer)sw);
	vbox = add_vbox(skew_window);

	table = add_a_table(3, 4, 5, vbox);
	add_to_table(_("Angle"), table, 0, 1, 0);
	add_to_table(_("Offset"), table, 0, 2, 0);
	add_to_table(_("At distance"), table, 0, 3, 0);
	add_to_table(_("Horizontal "), table, 1, 0, 0);
	add_to_table(_("Vertical"), table, 2, 0, 0);
	sw->angle[0] = float_spin_to_table(table, 1, 1, 5, 0, -89.99, 89.99);
	sw->angle[1] = float_spin_to_table(table, 2, 1, 5, 0, -89.99, 89.99);
	sw->ofs[0] = float_spin_to_table(table, 1, 2, 5, 0, -MAX_WIDTH, MAX_WIDTH);
	sw->ofs[1] = float_spin_to_table(table, 2, 2, 5, 0, -MAX_HEIGHT, MAX_HEIGHT);
	sw->dist[0] = spin_to_table(table, 1, 3, 5, mem_height, 1, MAX_HEIGHT);
	sw->dist[1] = spin_to_table(table, 2, 3, 5, mem_width, 1, MAX_WIDTH);
	spin_connect(sw->angle[0], GTK_SIGNAL_FUNC(skew_moved), (gpointer)sw);
	spin_connect(sw->ofs[0], GTK_SIGNAL_FUNC(skew_moved), (gpointer)sw);
	spin_connect(sw->dist[0], GTK_SIGNAL_FUNC(skew_moved), (gpointer)sw);
	spin_connect(sw->angle[1], GTK_SIGNAL_FUNC(skew_moved), (gpointer)sw);
	spin_connect(sw->ofs[1], GTK_SIGNAL_FUNC(skew_moved), (gpointer)sw);
	spin_connect(sw->dist[1], GTK_SIGNAL_FUNC(skew_moved), (gpointer)sw);
	add_hseparator(vbox, -2, 10);

	if (mem_img_bpp == 3)
	{
		sw->gc = pack(vbox, gamma_toggle());
		add_hseparator(vbox, -2, 10);
		xpack(vbox, filter_pack(skew_mode, &skew_mode));
		add_hseparator(vbox, -2, 10);
	}

	pack(vbox, OK_box(5, skew_window, _("OK"), GTK_SIGNAL_FUNC(click_skew_ok),
		_("Cancel"), GTK_SIGNAL_FUNC(gtk_widget_destroy)));
	gtk_window_set_transient_for(GTK_WINDOW(skew_window), GTK_WINDOW(main_window));
	gtk_widget_show_all(skew_window);
}

/// TRACING IMAGE WINDOW

typedef struct {
	GtkWidget *wspin, *hspin, *xspin, *yspin, *zspin, *opt, *tog;
	int src, x, y, scale, state;
} bkg_widgets;

static void bkg_update_widgets(bkg_widgets *bw)
{
	int w = 0, h = 0;

	bw->src = wj_option_menu_get_history(bw->opt);
	switch (bw->src)
	{
	case 0: w = bkg_w; h = bkg_h; break;
	case 1: break;
	case 2: w = mem_width; h = mem_height; break;
	case 3: if (mem_clipboard) w = mem_clip_w , h = mem_clip_h;
		break;
	}

	spin_set_range(bw->wspin, w, w);
	spin_set_range(bw->hspin, h, h);

	bw->x = read_spin(bw->xspin);
	bw->y = read_spin(bw->yspin);
	bw->scale = read_spin(bw->zspin);
	bw->state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bw->tog));
}

static void click_bkg_apply(GtkWidget *widget)
{
	bkg_widgets *bw = gtk_object_get_user_data(GTK_OBJECT(widget));

	bkg_update_widgets(bw);
	bkg_x = bw->x;
	bkg_y = bw->y;
	bkg_scale = bw->scale;
	bkg_flag = bw->state;
	if (!config_bkg(bw->src)) memory_errors(1);
	update_stuff(UPD_RENDER);
}

static void click_bkg_option(GtkMenuItem *menuitem, gpointer user_data)
{
	bkg_update_widgets(user_data);
}

static void click_bkg_ok(GtkWidget *widget)
{
	click_bkg_apply(widget);
	gtk_widget_destroy(widget);
}

void bkg_setup()
{
	char *srcs[4] = { _("Unchanged"), _("None"), _("Image"), _("Clipboard") };
	GtkWidget *win, *vbox, *table, *hbox;
	bkg_widgets *bw;


	win = add_a_window(GTK_WINDOW_TOPLEVEL, _("Tracing Image"), GTK_WIN_POS_CENTER, TRUE);
	bw = bound_malloc(win, sizeof(bkg_widgets));
	gtk_object_set_user_data(GTK_OBJECT(win), (gpointer)bw);

	bw->src = 0;
	bw->state = bkg_flag;

	vbox = add_vbox(win);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);

	table = add_a_table(4, 3, 5, vbox);
	add_to_table(_("Source"), table, 0, 0, 5);
	add_to_table(_("Size"), table, 1, 0, 5);
	add_to_table(_("Origin"), table, 2, 0, 5);
	add_to_table(_("Relative scale"), table, 3, 0, 5);
	bw->opt = wj_option_menu(srcs, 4, 0, bw, GTK_SIGNAL_FUNC(click_bkg_option));
	to_table_l(bw->opt, table, 0, 1, 2, 5);
	bw->wspin = spin_to_table(table, 1, 1, 5, 0, 0, 0);
	bw->hspin = spin_to_table(table, 1, 2, 5, 0, 0, 0);
	GTK_WIDGET_UNSET_FLAGS(bw->wspin, GTK_CAN_FOCUS);
	GTK_WIDGET_UNSET_FLAGS(bw->hspin, GTK_CAN_FOCUS);
	bw->xspin = spin_to_table(table, 2, 1, 5, bkg_x, -MAX_WIDTH, MAX_WIDTH);
	bw->yspin = spin_to_table(table, 2, 2, 5, bkg_y, -MAX_HEIGHT, MAX_HEIGHT);
	bw->zspin = spin_to_table(table, 3, 1, 5, bkg_scale, 1, MAX_ZOOM);

	bw->tog = add_a_toggle(_("Display"), vbox, bkg_flag);

	add_hseparator(vbox, -2, 10);

	/* Cancel / Apply / OK */

	hbox = pack(vbox, OK_box(5, win, _("OK"), GTK_SIGNAL_FUNC(click_bkg_ok),
		_("Cancel"), GTK_SIGNAL_FUNC(gtk_widget_destroy)));
	OK_box_add(hbox, _("Apply"), GTK_SIGNAL_FUNC(click_bkg_apply));
	bkg_update_widgets(bw);
	gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(main_window));
	gtk_widget_show_all(win);
}

/// SEGMENTATION WINDOW

seg_state *seg_preview;

typedef struct {
	char ids[3]; // For binding updaters
	GtkWidget *win, *tspin, *pbutton;
	seg_state *s;
	int vars[2]; // Colorspace and distance
	int progress;
	int step; // Calculation step of 2nd phase
} seg_widgets;

static int seg_cspace = CSPACE_LXN, seg_dist = DIST_LINF;
static int seg_rank = 4, seg_minsize = 1;
static guint seg_idle;

static GtkWidget *cspace_frame(int idx, gpointer var, GtkSignalFunc handler)
{
	return (add_with_frame(NULL, _("Colour space"),
		wj_radio_pack(cspnames, NUM_CSPACES, 1, idx, var, handler)));
}

static GtkWidget *difference_frame(int idx, gpointer var, GtkSignalFunc handler)
{
	char *dnames[3];
	int i;

	for (i = 0; i < 3; i++) dnames[i] = _(dist_txt[i]);
	return (add_with_frame(NULL, _("Difference measure"),
		wj_radio_pack(dnames, 3, 1, idx, var, handler)));
}

/* Change colorspace or distance measure, causing full recalculation */
static void seg_mode_toggled(GtkWidget *btn, gpointer idx)
{
	seg_widgets *sw;
	char *cp;

	if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn))) return;
	cp = gtk_object_get_user_data(GTK_OBJECT(btn->parent));
	sw = (seg_widgets *)(cp - *cp);
	if (sw->vars[(int)*cp] == (int)idx) return;

	sw->vars[(int)*cp] = (int)idx;
	mem_seg_prepare(sw->s, mem_img[CHN_IMAGE], mem_width, mem_height,
		sw->progress, sw->vars[0], sw->vars[1]);
	/* Disable preview if cancelled, change threshold if not */
	if (!sw->s->phase) gtk_toggle_button_set_active(
		GTK_TOGGLE_BUTTON(sw->pbutton), FALSE);
	else gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw->tspin),
		mem_seg_threshold(sw->s));
}

/* Do phase 2 (segmentation) in the background */
static gboolean seg_process_idle(seg_widgets *sw)
{
	if (seg_preview && (sw->s->phase == 1))
	{
#define SEG_STEP 100000
		sw->step = mem_seg_process_chunk(sw->step, SEG_STEP, sw->s);
#undef SEG_STEP
		if (!(sw->s->phase & 2)) return (TRUE); // Not yet completed
		gdk_window_set_cursor(sw->win->window, NULL);
		seg_idle = 0; // In case update_stuff() ever calls main loop
		update_stuff(UPD_RENDER);
	}
	seg_idle = 0;
	return (FALSE);
}

/* Change segmentation limits, causing phase 2 restart */
static void seg_spin_changed(GtkAdjustment *adjustment, char *cp)
{
	seg_widgets *sw = (seg_widgets *)(cp - *cp);
	seg_state *s = sw->s;

	if (!*cp) s->threshold = adjustment->value;
	else *(*cp == 1 ? &s->minrank : &s->minsize) = ADJ2INT(adjustment);
	s->phase &= 1; // Need phase 2 rerun
	sw->step = 0; // Restart phase 2 afresh
	if (seg_preview)
	{
		if (sw->progress) gdk_window_set_cursor(sw->win->window, busy_cursor);
		if (!seg_idle) seg_idle = threads_idle_add_priority(
			GTK_PRIORITY_REDRAW + 5, (GtkFunction)seg_process_idle, sw);
	}
}

/* Finish all calculations (preparation and segmentation) */
static int seg_process(seg_widgets *sw)
{
	/* Run phase 1 if necessary */
	if (!sw->s->phase) mem_seg_prepare(sw->s, mem_img[CHN_IMAGE],
		mem_width, mem_height, sw->progress, sw->vars[0], sw->vars[1]);
	/* Run phase 2 if possible & necessary */
	if (sw->s->phase == 1) mem_seg_process(sw->s);
	/* Return whether job is done */
	return (sw->s->phase > 1);
}

/* Toggle interactive preview */
static void seg_preview_toggle(GtkToggleButton *button, GtkWidget *window)
{
	seg_state *oldp = seg_preview;
	seg_widgets *sw = NULL;


	if ((button && button->active) ^ !oldp) return; // Nothing to do
	if (window) sw = gtk_object_get_user_data(GTK_OBJECT(window));
	if (seg_idle) gtk_idle_remove(seg_idle);
	seg_idle = 0;
	if (oldp) // Disable
	{
// !!! Maybe better to add & use an integrated progressbar?
		if (sw) gdk_window_set_cursor(sw->win->window, NULL);
		seg_preview = NULL;
	}
	else // Enable
	{
		/* Do segmentation conspicuously at first */
		if (!seg_process(sw)) gtk_toggle_button_set_active(button, FALSE);
		else seg_preview = sw->s;
	}
	if (oldp != seg_preview) update_stuff(UPD_RENDER);
}

static gboolean seg_cancel(GtkWidget *window)
{
	seg_widgets *sw = gtk_object_get_user_data(GTK_OBJECT(window));

	seg_preview_toggle(NULL, NULL);	// Clear the preview if enabled
	destroy_dialog(window);
	free(sw->s);
	free(sw);
	return (FALSE);
}

static void seg_ok(GtkWidget *window)
{
	seg_widgets *sw = gtk_object_get_user_data(GTK_OBJECT(window));

	/* First, disable preview */
	seg_preview_toggle(NULL, NULL);

	/* Then, update parameters */
	update_window_spin(window);
	seg_cspace = sw->vars[0];
	seg_dist = sw->vars[1];
	seg_rank = sw->s->minrank;
	seg_minsize = sw->s->minsize;

	/* Now, finish segmentation & render results */
	if (seg_process(sw))
	{
		spot_undo(UNDO_FILT);
		mem_seg_render(mem_img[CHN_IMAGE], sw->s);
		mem_undo_prepare();
		update_stuff(UPD_IMG);
	}

	seg_cancel(window);
}

void pressed_segment()
{
	GtkWidget *seg_window, *mainbox, *table, *hbox, *spin;
	seg_widgets *sw;
	seg_state *s;
	int progress = 0, sz = mem_width * mem_height;


	if (sz == 1) return; /* 1 pixel in image is trivial - do nothing */
	if (sz >= 1024 * 1024) progress = SEG_PROGRESS;

	s = mem_seg_prepare(NULL, mem_img[CHN_IMAGE], mem_width, mem_height,
		progress, seg_cspace, seg_dist);
	if (!s)
	{
		memory_errors(1);
		return;
	}
	if (!s->phase) return; // Terminated by user
	s->threshold = mem_seg_threshold(s);

	sw = calloc(1, sizeof(seg_widgets));
	sw->ids[1] = 1; sw->ids[2] = 2;
	sw->s = s;
	sw->progress = progress;

	sw->win = seg_window = add_a_window(GTK_WINDOW_TOPLEVEL,
		_("Segment Image"), GTK_WIN_POS_CENTER, TRUE);
	gtk_object_set_user_data(GTK_OBJECT(seg_window), sw);
	mainbox = add_vbox(seg_window);

	pack(mainbox, cspace_frame(sw->vars[0] = seg_cspace, sw->ids + 0,
		GTK_SIGNAL_FUNC(seg_mode_toggled)));
	pack(mainbox, difference_frame(sw->vars[1] = seg_dist, sw->ids + 1,
		GTK_SIGNAL_FUNC(seg_mode_toggled)));

	table = add_a_table(3, 2, 5, mainbox);
	add_to_table(_("Threshold"), table, 0, 0, 5);
	sw->tspin = float_spin_to_table(table, 0, 1, 5, s->threshold, 0, 5000.0);
	spin_connect(sw->tspin, GTK_SIGNAL_FUNC(seg_spin_changed), sw->ids + 0);
	add_to_table(_("Level"), table, 1, 0, 5);
	spin = spin_to_table(table, 1, 1, 5, s->minrank = seg_rank, 0, 32);
	spin_connect(spin, GTK_SIGNAL_FUNC(seg_spin_changed), sw->ids + 1);
	add_to_table(_("Minimum size"), table, 2, 0, 5);
	spin = spin_to_table(table, 2, 1, 5, s->minsize = seg_minsize, 1, sz);
	spin_connect(spin, GTK_SIGNAL_FUNC(seg_spin_changed), sw->ids + 2);

	hbox = pack(mainbox, OK_box(0, seg_window, _("Apply"), GTK_SIGNAL_FUNC(seg_ok),
		_("Cancel"), GTK_SIGNAL_FUNC(seg_cancel)));
	sw->pbutton = OK_box_add_toggle(hbox, _("Preview"), GTK_SIGNAL_FUNC(seg_preview_toggle));

	gtk_window_set_transient_for(GTK_WINDOW(seg_window), GTK_WINDOW(main_window));
	gtk_widget_show(seg_window);
}
