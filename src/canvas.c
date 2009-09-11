/*	canvas.c
	Copyright (C) 2004-2006 Mark Tyler and Dmitry Groshev

	This file is part of mtPaint.

	mtPaint is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	mtPaint is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mtPaint in the file COPYING.
*/

#include <math.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "global.h"

#include "memory.h"
#include "png.h"
#include "mainwindow.h"
#include "otherwindow.h"
#include "mygtk.h"
#include "inifile.h"
#include "canvas.h"
#include "quantizer.h"
#include "viewer.h"
#include "layer.h"
#include "polygon.h"
#include "wu.h"
#include "prefs.h"
#include "ani.h"
#include "channels.h"
#include "toolbar.h"

GtkWidget *label_bar[STATUS_ITEMS];


float can_zoom = 1;				// Zoom factor 1..MAX_ZOOM
int margin_main_x=0, margin_main_y=0,		// Top left of image from top left of canvas
	margin_view_x=0, margin_view_y=0;
int zoom_flag = 0;
int fs_type = 0;
int perim_status = 0, perim_x = 0, perim_y = 0, perim_s = 2;		// Tool perimeter
int marq_status = MARQUEE_NONE,
	marq_x1 = -1, marq_y1 = -1, marq_x2 = -1, marq_y2 = -1;		// Selection marquee
int marq_drag_x = 0, marq_drag_y = 0;					// Marquee dragging offset
int line_status = LINE_NONE,
	line_x1 = 0, line_y1 = 0, line_x2 = 0, line_y2 = 0;		// Line tool
int poly_status = POLY_NONE;						// Polygon selection tool
int clone_x, clone_y;							// Clone offsets

int recent_files;					// Current recent files setting

gboolean show_paste,					// Show contents of clipboard while pasting
	col_reverse = FALSE,				// Painting with right button
	text_paste = FALSE,				// Are we pasting text?
	canvas_image_centre = TRUE,			// Are we centering the image?
	chequers_optimize = TRUE			// Are we optimizing the chequers for speed?
	;


///	STATUS BAR

void update_image_bar()
{
	char txt[64], txt2[16];

	toolbar_update_settings();		// Update A/B labels in settings toolbar

	if ( mem_img_bpp == 1 )
		sprintf(txt2, "%i", mem_cols);
	else
		sprintf(txt2, "RGB");

	snprintf(txt, 50, "%s %i x %i x %s", channames[mem_channel],
		mem_width, mem_height, txt2);

	if ( mem_img[CHN_ALPHA] || mem_img[CHN_SEL] || mem_img[CHN_MASK] )
	{
		strcat(txt, " + ");
		if ( mem_img[CHN_ALPHA] ) strcat(txt, "A");
		if ( mem_img[CHN_SEL] ) strcat(txt, "S");
		if ( mem_img[CHN_MASK] ) strcat(txt, "M");
	}

	if ( layers_total>0 )
	{
		sprintf(txt2, "  (%i/%i)", layer_selected, layers_total);
		strcat(txt, txt2);
	}
	if ( mem_xpm_trans>=0 )
	{
		sprintf(txt2, "  (T=%i)", mem_xpm_trans);
		strcat(txt, txt2);
	}
	strcat(txt, "  ");
	gtk_label_set_text( GTK_LABEL(label_bar[STATUS_GEOMETRY]), txt );
}

void update_sel_bar()			// Update selection stats on status bar
{
	char txt[64];
	int x1, y1, x2, y2;
	float lang = 0, llen = 1;

	if ( (tool_type == TOOL_SELECT || tool_type == TOOL_POLYGON) )
	{
		if ( marq_status > MARQUEE_NONE )
		{
			mtMIN( x1, marq_x1, marq_x2 )
			mtMIN( y1, marq_y1, marq_y2 )
			mtMAX( x2, marq_x1, marq_x2 )
			mtMAX( y2, marq_y1, marq_y2 )
			if ( status_on[STATUS_SELEGEOM] )
			{
				if ( x1==x2 )
				{
					if ( marq_y1 < marq_y2 ) lang = 180;
				}
				else
				{
					lang = 90 + 180*atan( ((float) marq_y1 - marq_y2) /
						(marq_x1 - marq_x2) ) / M_PI;
					if ( marq_x1 > marq_x2 )
						lang = lang - 180;
				}

				llen = sqrt( (x2-x1+1)*(x2-x1+1) + (y2-y1+1)*(y2-y1+1) );

				snprintf(txt, 60, "  %i,%i : %i x %i   %.1f' %.1f\"",
					x1, y1, x2-x1+1, y2-y1+1, lang, llen);
				gtk_label_set_text( GTK_LABEL(label_bar[STATUS_SELEGEOM]), txt );
			}
		}
		else
		{
			if ( tool_type == TOOL_POLYGON )
			{
				snprintf(txt, 60, "  (%i)", poly_points);
				if ( poly_status != POLY_DONE ) strcat(txt, "+");
				if ( status_on[STATUS_SELEGEOM] )
					gtk_label_set_text( GTK_LABEL(label_bar[STATUS_SELEGEOM]), txt );
			}
			else if ( status_on[STATUS_SELEGEOM] )
					gtk_label_set_text( GTK_LABEL(label_bar[STATUS_SELEGEOM]), "" );
		}
	}
	else if ( status_on[STATUS_SELEGEOM] )
			gtk_label_set_text( GTK_LABEL(label_bar[STATUS_SELEGEOM]), "" );
}

static void chan_txt_cat(char *txt, int chan, int x, int y)
{
	char txt2[8];

	if ( mem_img[chan] )
	{
		snprintf( txt2, 8, "%i", mem_img[chan][x + mem_width*y] );
		strcat(txt, txt2);
	}
}

void update_xy_bar(int x, int y)
{
	char txt[96];
	unsigned char pixel;
	png_color pixel24;
	int r, g, b;

	if ( x>=0 && y>= 0 )
	{
		if ( status_on[STATUS_CURSORXY] )
		{
			snprintf(txt, 60, "%i,%i", x, y);
			gtk_label_set_text( GTK_LABEL(label_bar[STATUS_CURSORXY]), txt );
		}

		if ( status_on[STATUS_PIXELRGB] )
		{
			if ( mem_img_bpp == 1 )
			{
				pixel = GET_PIXEL( x, y );
				r = mem_pal[pixel].red;
				g = mem_pal[pixel].green;
				b = mem_pal[pixel].blue;
				snprintf(txt, 60, "[%u] = {%i,%i,%i}", pixel, r, g, b);
			}
			else
			{
				pixel24 = get_pixel24( x, y );
				r = pixel24.red;
				g = pixel24.green;
				b = pixel24.blue;
				snprintf(txt, 60, "{%i,%i,%i}", r, g, b);
			}
			if ( mem_img[CHN_ALPHA] || mem_img[CHN_SEL] || mem_img[CHN_MASK] )
			{
				strcat(txt, " + {");
				chan_txt_cat(txt, CHN_ALPHA, x, y);
				strcat(txt, ",");
				chan_txt_cat(txt, CHN_SEL, x, y);
				strcat(txt, ",");
				chan_txt_cat(txt, CHN_MASK, x, y);
				strcat(txt, "}");
			}
			gtk_label_set_text( GTK_LABEL(label_bar[STATUS_PIXELRGB]), txt );
		}
	}
	else
	{
		if ( status_on[STATUS_CURSORXY] )
			gtk_label_set_text( GTK_LABEL(label_bar[STATUS_CURSORXY]), "" );
		if ( status_on[STATUS_PIXELRGB] )
			gtk_label_set_text( GTK_LABEL(label_bar[STATUS_PIXELRGB]), "" );
	}
}

static void update_undo_bar()
{
	char txt[32];

	if (status_on[STATUS_UNDOREDO])
	{
		sprintf(txt, "%i+%i", mem_undo_done, mem_undo_redo);
		gtk_label_set_text(GTK_LABEL(label_bar[STATUS_UNDOREDO]), txt);
	}
}

void init_status_bar()
{
	update_image_bar();
	if ( !status_on[STATUS_GEOMETRY] )
		gtk_label_set_text( GTK_LABEL(label_bar[STATUS_GEOMETRY]), "" );

	if ( status_on[STATUS_CURSORXY] )
		gtk_widget_set_usize(label_bar[STATUS_CURSORXY], 90, -2);
	else
	{
		gtk_widget_set_usize(label_bar[STATUS_CURSORXY], 0, -2);
		gtk_label_set_text( GTK_LABEL(label_bar[STATUS_CURSORXY]), "" );
	}

	if ( !status_on[STATUS_PIXELRGB] )
		gtk_label_set_text( GTK_LABEL(label_bar[STATUS_PIXELRGB]), "" );

	if ( !status_on[STATUS_SELEGEOM] )
		gtk_label_set_text( GTK_LABEL(label_bar[STATUS_SELEGEOM]), "" );

	if (status_on[STATUS_UNDOREDO])
	{	
		gtk_widget_set_usize(label_bar[STATUS_UNDOREDO], 50, -2);
		update_undo_bar();
	}
	else
	{
		gtk_widget_set_usize(label_bar[STATUS_UNDOREDO], 0, -2);
		gtk_label_set_text( GTK_LABEL(label_bar[STATUS_UNDOREDO]), "" );
	}
}


void commit_paste( gboolean undo )
{
	int fx, fy, fw, fh, fx2, fy2;		// Screen coords
	int mx = 0, my = 0;			// Mem coords
	int i, ofs, ua;
	unsigned char *image, *mask, *alpha = NULL;

	if ( marq_x1 < 0 ) mx = -marq_x1;
	if ( marq_y1 < 0 ) my = -marq_y1;

	mtMAX( fx, marq_x1, 0 )
	mtMAX( fy, marq_y1, 0 )
	mtMIN( fx2, marq_x2, mem_width-1 )
	mtMIN( fy2, marq_y2, mem_height-1 )

	fw = fx2 - fx + 1;
	fh = fy2 - fy + 1;

	ua = channel_dis[CHN_ALPHA];	// Ignore clipboard alpha if disabled

	mask = malloc(fw);
	if (!mask) return;	/* !!! Not enough memory */
	if ((mem_channel == CHN_IMAGE) && RGBA_mode && !mem_clip_alpha &&
		!ua && mem_img[CHN_ALPHA])
	{
		alpha = malloc(fw);
		if (!alpha) return;
		memset(alpha, channel_col_A[CHN_ALPHA], fw);
	}
	ua |= !mem_clip_alpha;

	if ( undo ) mem_undo_next(UNDO_PASTE);	// Do memory stuff for undo

	ofs = my * mem_clip_w + mx;
	image = mem_clipboard + ofs * mem_clip_bpp;

	for (i = 0; i < fh; i++)
	{
		row_protected(fx, fy + i, fw, mask);
		paste_pixels(fx, fy + i, fw, mask, image, ua ?
			alpha : mem_clip_alpha + ofs, mem_clip_mask ?
			mem_clip_mask + ofs : NULL, tool_opacity);
		image += mem_clip_w * mem_clip_bpp;
		ofs += mem_clip_w;
	}

	free(mask);
	free(alpha);

	update_menus();				// Update menu undo issues
	vw_update_area( fx, fy, fw, fh );
	gtk_widget_queue_draw_area( drawing_canvas,
			fx*can_zoom + margin_main_x, fy*can_zoom + margin_main_y,
			fw*can_zoom + 1, fh*can_zoom + 1);
}

void paste_prepare()
{
	poly_status = POLY_NONE;
	poly_points = 0;
	if ( tool_type != TOOL_SELECT && tool_type != TOOL_POLYGON )
	{
		perim_status = 0;
		clear_perim();
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(icon_buttons[DEFAULT_TOOL_ICON]), TRUE );
	}
	else
	{
		if ( marq_status != MARQUEE_NONE ) paint_marquee(0, marq_x1, marq_y1);
	}
}

void iso_trans( GtkMenuItem *menu_item, gpointer user_data, gint item )
{
	int i;

	i = mem_isometrics(item);

	if ( i==0 ) canvas_undo_chores();
	else
	{
		if ( i==-666 ) alert_box( _("Error"), _("The image is too large to transform."),
					_("OK"), NULL, NULL );
		else memory_errors(i);
	}
}

void create_pal_quantized(int dl)
{
	int i = 0;
	unsigned char newpal[3][256];

	mem_undo_next(UNDO_PAL);

	if ( dl==1 )
		i = dl1quant(mem_img[CHN_IMAGE], mem_width, mem_height, mem_cols, newpal);
	if ( dl==3 )
		i = dl3quant(mem_img[CHN_IMAGE], mem_width, mem_height, mem_cols, newpal);
	if ( dl==5 )
		i = wu_quant(mem_img[CHN_IMAGE], mem_width, mem_height, mem_cols, newpal);

	if ( i!=0 ) memory_errors(i);
	else
	{
		for ( i=0; i<mem_cols; i++ )
		{
			mem_pal[i].red = newpal[0][i];
			mem_pal[i].green = newpal[1][i];
			mem_pal[i].blue = newpal[2][i];
		}

		update_menus();
		init_pal();
	}
}

void pressed_create_dl1( GtkMenuItem *menu_item, gpointer user_data )
{	create_pal_quantized(1);	}

void pressed_create_dl3( GtkMenuItem *menu_item, gpointer user_data )
{	create_pal_quantized(3);	}

void pressed_create_wu( GtkMenuItem *menu_item, gpointer user_data )
{	create_pal_quantized(5);	}

void pressed_invert( GtkMenuItem *menu_item, gpointer user_data )
{
	spot_undo(UNDO_INV);

	mem_invert();

	init_pal();
	update_all_views();
	gtk_widget_queue_draw( drawing_col_prev );
}

void pressed_edge_detect( GtkMenuItem *menu_item, gpointer user_data )
{
	spot_undo(UNDO_FILT);
	do_effect(0, 0);
	update_all_views();
}

int do_fx(GtkWidget *spin, gpointer fdata)
{
	int i;

	i = read_spin(spin);
	spot_undo(UNDO_FILT);
	do_effect((int)fdata, i);

	return TRUE;
}

void pressed_sharpen( GtkMenuItem *menu_item, gpointer user_data )
{
	GtkWidget *spin = add_a_spin(50, 1, 100);
	filter_window(_("Edge Sharpen"), spin, do_fx, (gpointer)(3), FALSE);
}

void pressed_soften( GtkMenuItem *menu_item, gpointer user_data )
{
	GtkWidget *spin = add_a_spin(50, 1, 100);
	filter_window(_("Edge Soften"), spin, do_fx, (gpointer)(4), FALSE);
}

void pressed_emboss( GtkMenuItem *menu_item, gpointer user_data )
{
	spot_undo(UNDO_FILT);
	do_effect(2, 0);
	update_all_views();
}

int do_gauss(GtkWidget *box, gpointer fdata)
{
	GtkWidget *spinX, *spinY, *toggleXY, *toggleGC;
	double radiusX, radiusY;

	spinX = ((GtkBoxChild*)GTK_BOX(box)->children->data)->widget;
	spinY = ((GtkBoxChild*)GTK_BOX(box)->children->next->data)->widget;
	toggleXY = ((GtkBoxChild*)GTK_BOX(box)->children->next->next->data)->widget;
	toggleGC = ((GtkBoxChild*)GTK_BOX(box)->children->next->next->next->data)->widget;

	gtk_spin_button_update(GTK_SPIN_BUTTON(spinX));
	radiusX = radiusY = gtk_spin_button_get_value_as_float(GTK_SPIN_BUTTON(spinX));
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggleXY)))
	{
		gtk_spin_button_update(GTK_SPIN_BUTTON(spinY));
		radiusY = gtk_spin_button_get_value_as_float(GTK_SPIN_BUTTON(spinY));
	}

	spot_undo(UNDO_DRAW);
	mem_gauss(radiusX, radiusY, gtk_toggle_button_get_active(
		GTK_TOGGLE_BUTTON(toggleGC)));

	return TRUE;
}

static void gauss_xy_click(GtkButton *button, GtkWidget *spin)
{
	gtk_widget_set_sensitive(spin,
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)));
}

void pressed_gauss(GtkMenuItem *menu_item, gpointer user_data)
{
	int i;
	GtkWidget *box, *spin, *check;

	box = gtk_vbox_new(FALSE, 5);
	gtk_widget_show(box);
	for (i = 0; i < 2; i++)
	{
		spin = add_a_spin(1, 0, 200);
		gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 0);
		gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 2);
	}
	gtk_widget_set_sensitive(spin, FALSE);
	check = add_a_toggle(_("Different X/Y"), box, FALSE);
	gtk_signal_connect(GTK_OBJECT(check), "clicked",
		GTK_SIGNAL_FUNC(gauss_xy_click), (gpointer)spin);
	check = add_a_toggle(_("Gamma corrected"), box,
		inifile_get_gboolean("defaultGamma", FALSE));
	if (mem_channel != CHN_IMAGE) gtk_widget_hide(check);
	filter_window(_("Gaussian Blur"), box, do_gauss, NULL, FALSE);
}

void pressed_convert_rgb( GtkMenuItem *menu_item, gpointer user_data )
{
	int i;

	i = mem_convert_rgb();

	if ( i!=0 ) memory_errors(i);
	else
	{
		if ( tool_type == TOOL_SELECT && marq_status >= MARQUEE_PASTE )
			pressed_select_none( NULL, NULL );
				// If the user is pasting, lose it!

		update_menus();
		init_pal();
		gtk_widget_queue_draw( drawing_canvas );
		gtk_widget_queue_draw( drawing_col_prev );
	}
}

void pressed_greyscale( GtkMenuItem *menu_item, gpointer user_data )
{
	spot_undo(UNDO_COL);

	mem_greyscale();

	init_pal();
	update_all_views();
	gtk_widget_queue_draw( drawing_col_prev );
}

void rot_im(int dir)
{
	if ( mem_image_rot(dir) == 0 )
	{
		check_marquee();
		canvas_undo_chores();
	}
	else alert_box( _("Error"), _("Not enough memory to rotate image"), _("OK"), NULL, NULL );
}

void pressed_rotate_image_clock( GtkMenuItem *menu_item, gpointer user_data )
{	rot_im(0);	}

void pressed_rotate_image_anti( GtkMenuItem *menu_item, gpointer user_data )
{	rot_im(1);	}

void rot_sel(int dir)
{
	if ( mem_sel_rot(dir) == 0 )
	{
		check_marquee();
		gtk_widget_queue_draw( drawing_canvas );
	}
	else	alert_box( _("Error"), _("Not enough memory to rotate clipboard"), _("OK"), NULL, NULL );
}

void pressed_rotate_sel_clock( GtkMenuItem *menu_item, gpointer user_data )
{	rot_sel(0);	}

void pressed_rotate_sel_anti( GtkMenuItem *menu_item, gpointer user_data )
{	rot_sel(1);	}

int do_rotate_free(GtkWidget *box, gpointer fdata)
{
	GtkWidget *spin = ((GtkBoxChild*)GTK_BOX(box)->children->data)->widget;
	int j, smooth = 0, gcor = 0;
	double angle;

	gtk_spin_button_update(GTK_SPIN_BUTTON(spin));
	angle = gtk_spin_button_get_value_as_float(GTK_SPIN_BUTTON(spin));

	if (mem_img_bpp == 3)
	{
		GtkWidget *gch = ((GtkBoxChild*)GTK_BOX(box)->children->next->data)->widget;
		GtkWidget *check = ((GtkBoxChild*)GTK_BOX(box)->children->next->next->data)->widget;
		gcor = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gch));
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)))
			smooth = 1;
	}
	j = mem_rotate_free(angle, smooth, gcor);
	if (!j) canvas_undo_chores();
	else
	{
		if (j == -5) alert_box(_("Error"),
			_("The image is too large for this rotation."),
			_("OK"), NULL, NULL);
		else memory_errors(j);
	}

	return TRUE;
}

void pressed_rotate_free( GtkMenuItem *menu_item, gpointer user_data )
{
	GtkWidget *box, *spin = add_a_spin(45, -360, 360);
	box = gtk_vbox_new(FALSE, 5);
	gtk_widget_show(box);
	gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 0);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 2);
	if (mem_img_bpp == 3)
	{
		add_a_toggle(_("Gamma corrected"), box,
			inifile_get_gboolean("defaultGamma", FALSE));
		add_a_toggle(_("Smooth"), box, TRUE);
	}
	filter_window(_("Free Rotate"), box, do_rotate_free, NULL, FALSE);
}


void mask_ab(int v)
{
	int i;

	if ( mem_clip_mask == NULL )
	{
		i = mem_clip_mask_init(v ^ 255);
		if ( i != 0 )
		{
			memory_errors(1);	// Not enough memory
			return;
		}
	}
	mem_clip_mask_set(v);
	gtk_widget_queue_draw( drawing_canvas );
}

void pressed_clip_unmask()
{	mask_ab(255);	}

void pressed_clip_mask()
{	mask_ab(0);	}

void pressed_clip_alphamask()
{
	unsigned char *old_mask = mem_clip_mask;
	int i, j = mem_clip_w * mem_clip_h, k;

	if (!mem_clipboard || !mem_clip_alpha) return;

	mem_clip_mask = mem_clip_alpha;
	mem_clip_alpha = NULL;

	if (old_mask)
	{
		for (i = 0; i < j; i++)
		{
			k = old_mask[i] * mem_clip_mask[i];
			mem_clip_mask[i] = (k + (k >> 8) + 1) >> 8;
		}
		free(old_mask);
	}

	gtk_widget_queue_draw( drawing_canvas );
}

void pressed_clip_alpha_scale()
{
	if (!mem_clipboard || (mem_clip_bpp != 3)) return;
	if (!mem_clip_mask) mem_clip_mask_init(255);
	if (!mem_clip_mask) return;

	if (mem_scale_alpha(mem_clipboard, mem_clip_mask,
		mem_clip_w, mem_clip_h, TRUE)) return;

	gtk_widget_queue_draw( drawing_canvas );
}

void pressed_clip_mask_all()
{
	int i;

	i = mem_clip_mask_init(0);
	if ( i != 0 )
	{
		memory_errors(1);	// Not enough memory
		return;
	}
	gtk_widget_queue_draw( drawing_canvas );
}

void pressed_clip_mask_clear()
{
	if ( mem_clip_mask != NULL )
	{
		mem_clip_mask_clear();
		gtk_widget_queue_draw( drawing_canvas );
	}
}

void pressed_flip_image_v( GtkMenuItem *menu_item, gpointer user_data )
{
	int i;
	unsigned char *temp;

	temp = malloc(mem_width * mem_img_bpp);
	if (!temp) return; /* Not enough memory for temp buffer */
	spot_undo(UNDO_XFORM);
	for (i = 0; i < NUM_CHANNELS; i++)
	{
		if (!mem_img[i]) continue;
		mem_flip_v(mem_img[i], temp, mem_width, mem_height, BPP(i));
	}
	free(temp);
	update_all_views();
}

void pressed_flip_image_h( GtkMenuItem *menu_item, gpointer user_data )
{
	int i;

	spot_undo(UNDO_XFORM);
	for (i = 0; i < NUM_CHANNELS; i++)
	{
		if (!mem_img[i]) continue;
		mem_flip_h(mem_img[i], mem_width, mem_height, BPP(i));
	}
	update_all_views();
}

void pressed_flip_sel_v( GtkMenuItem *menu_item, gpointer user_data )
{
	unsigned char *temp;

	temp = malloc(mem_clip_w * mem_clip_bpp);
	if (!temp) return; /* Not enough memory for temp buffer */
	mem_flip_v(mem_clipboard, temp, mem_clip_w, mem_clip_h, mem_clip_bpp);
	if (mem_clip_mask) mem_flip_v(mem_clip_mask, temp, mem_clip_w, mem_clip_h, 1);
	if (mem_clip_alpha) mem_flip_v(mem_clip_alpha, temp, mem_clip_w, mem_clip_h, 1);
	gtk_widget_queue_draw( drawing_canvas );
}

void pressed_flip_sel_h( GtkMenuItem *menu_item, gpointer user_data )
{
	mem_flip_h(mem_clipboard, mem_clip_w, mem_clip_h, mem_clip_bpp);
	if (mem_clip_mask) mem_flip_h(mem_clip_mask, mem_clip_w, mem_clip_h, 1);
	if (mem_clip_alpha) mem_flip_h(mem_clip_alpha, mem_clip_w, mem_clip_h, 1);
	gtk_widget_queue_draw( drawing_canvas );
}

void paste_init()
{
	marq_status = MARQUEE_PASTE;
	cursor_corner = -1;
	update_sel_bar();
	update_menus();
	gtk_widget_queue_draw( drawing_canvas );
}

void pressed_paste( GtkMenuItem *menu_item, gpointer user_data )
{
	paste_prepare();
	marq_x1 = mem_clip_x;
	marq_y1 = mem_clip_y;
	marq_x2 = mem_clip_x + mem_clip_w - 1;
	marq_y2 = mem_clip_y + mem_clip_h - 1;
	paste_init();
}

void pressed_paste_centre( GtkMenuItem *menu_item, gpointer user_data )
{
	int canz = can_zoom;
	GtkAdjustment *hori, *vert;

	if ( canz<1 ) canz = 1;

	hori = gtk_scrolled_window_get_hadjustment( GTK_SCROLLED_WINDOW(scrolledwindow_canvas) );
	vert = gtk_scrolled_window_get_vadjustment( GTK_SCROLLED_WINDOW(scrolledwindow_canvas) );

	if ( hori->page_size > mem_width*can_zoom ) mem_icx = 0.5;
	else mem_icx = ( hori->value + hori->page_size/2 ) / (mem_width*can_zoom);

	if ( vert->page_size > mem_height*can_zoom ) mem_icy = 0.5;
	else mem_icy = ( vert->value + vert->page_size/2 ) / (mem_height*can_zoom);

	paste_prepare();
	align_size( can_zoom );
	marq_x1 = mem_width * mem_icx - mem_clip_w/2;
	marq_y1 = mem_height * mem_icy - mem_clip_h/2;
	marq_x2 = marq_x1 + mem_clip_w - 1;
	marq_y2 = marq_y1 + mem_clip_h - 1;
	paste_init();
}

void do_the_copy(int op)
{
	int x1 = marq_x1, y1 = marq_y1;
	int x2 = marq_x2, y2 = marq_y2;
	int x, y, w, h, bpp, ofs, delta, len;
	int i;

	mtMIN( x, x1, x2 )
	mtMIN( y, y1, y2 )

	w = x1 - x2;
	h = y1 - y2;

	if ( w < 0 ) w = -w;
	if ( h < 0 ) h = -h;

	w++; h++;

	if ( op == 1 )		// COPY
	{
		bpp = MEM_BPP;
		free(mem_clipboard);		// Lose old clipboard
		free(mem_clip_alpha);		// Lose old clipboard alpha
		mem_clip_mask_clear();		// Lose old clipboard mask
		mem_clip_alpha = NULL;
		if (mem_channel == CHN_IMAGE)
		{
			if (mem_img[CHN_ALPHA] && !channel_dis[CHN_ALPHA])
				mem_clip_alpha = malloc(w * h);
			if (mem_img[CHN_SEL] && !channel_dis[CHN_SEL])
				mem_clip_mask = malloc(w * h);
		}
		mem_clipboard = malloc(w * h * bpp);
		text_paste = FALSE;
	
		if (!mem_clipboard)
		{
			free(mem_clip_alpha);
			mem_clip_mask_clear();
			alert_box( _("Error"), _("Not enough memory to create clipboard"),
					_("OK"), NULL, NULL );
		}
		else
		{
			mem_clip_bpp = bpp;
			mem_clip_x = x;
			mem_clip_y = y;
			mem_clip_w = w;
			mem_clip_h = h;

			/* Current channel */
			ofs = (y * mem_width + x) * bpp;
			delta = 0;
			len = w * bpp;
			for (i = 0; i < h; i++)
			{
				memcpy(mem_clipboard + delta,
					mem_img[mem_channel] + ofs, len);
				ofs += mem_width * bpp;
				delta += len;
			}

			/* Utility channels */
			if (mem_clip_alpha)
			{
				ofs = y * mem_width + x;
				delta = 0;
				for (i = 0; i < h; i++)
				{
					memcpy(mem_clip_alpha + delta,
						mem_img[CHN_ALPHA] + ofs, w);
					ofs += mem_width;
					delta += w;
				}
			}

			/* Selection channel */
			if (mem_clip_mask)
			{
				ofs = y * mem_width + x;
				delta = 0;
				for (i = 0; i < h; i++)
				{
					memcpy(mem_clip_mask + delta,
						mem_img[CHN_SEL] + ofs, w);
					ofs += mem_width;
					delta += w;
				}
			}
		}
	}
	if ( op == 2 )		// CLEAR area
	{
		f_rectangle( x, y, w, h );
	}
	if ( op == 3 )		// Remember new coords for copy while pasting
	{
		mem_clip_x = x;
		mem_clip_y = y;
	}

	update_menus();
}

void pressed_outline_rectangle( GtkMenuItem *menu_item, gpointer user_data )
{
	int x, y, w, h, x2, y2;

	spot_undo(UNDO_DRAW);

	if ( tool_type == TOOL_POLYGON )
	{
		poly_outline();
	}
	else
	{
		mtMIN( x, marq_x1, marq_x2 )
		mtMIN( y, marq_y1, marq_y2 )
		mtMAX( x2, marq_x1, marq_x2 )
		mtMAX( y2, marq_y1, marq_y2 )
		w = abs(marq_x1 - marq_x2) + 1;
		h = abs(marq_y1 - marq_y2) + 1;

		if ( 2*tool_size >= w || 2*tool_size >= h )
			f_rectangle( x, y, w, h );
		else
		{
			f_rectangle( x, y, w, tool_size );				// TOP
			f_rectangle( x, y + tool_size, tool_size, h - 2*tool_size );	// LEFT
			f_rectangle( x, y2 - tool_size + 1, w, tool_size );		// BOTTOM
			f_rectangle( x2 - tool_size + 1,
				y + tool_size, tool_size, h - 2*tool_size );		// RIGHT
		}
	}

	update_all_views();
}

void pressed_fill_ellipse( GtkMenuItem *menu_item, gpointer user_data )
{
	spot_undo(UNDO_DRAW);
	f_ellipse( marq_x1, marq_y1, marq_x2, marq_y2 );
	update_all_views();
}

void pressed_outline_ellipse( GtkMenuItem *menu_item, gpointer user_data )
{
	spot_undo(UNDO_DRAW);
	o_ellipse( marq_x1, marq_y1, marq_x2, marq_y2, tool_size );
	update_all_views();
}

void pressed_fill_rectangle( GtkMenuItem *menu_item, gpointer user_data )
{
	spot_undo(UNDO_DRAW);
	if ( tool_type == TOOL_SELECT ) do_the_copy(2);
	if ( tool_type == TOOL_POLYGON ) poly_paint();
	update_all_views();
}

void pressed_cut( GtkMenuItem *menu_item, gpointer user_data )
{			// Copy current selection to clipboard and then fill area with current pattern
	do_the_copy(1);
	spot_undo(UNDO_DRAW);
	if ( tool_type == TOOL_SELECT ) do_the_copy(2);
	if ( tool_type == TOOL_POLYGON )
	{
		poly_mask();
		poly_paint();
	}

	update_all_views();
}

void pressed_lasso( GtkMenuItem *menu_item, gpointer user_data )
{
	do_the_copy(1);
	if ( mem_clipboard == NULL ) return;		// No memory so bail out
	poly_mask();
	poly_lasso();
	pressed_paste_centre( NULL, NULL );
}

void pressed_lasso_cut( GtkMenuItem *menu_item, gpointer user_data )
{
	pressed_lasso( menu_item, user_data );
	if ( mem_clipboard == NULL ) return;		// No memory so bail out
	spot_undo(UNDO_DRAW);
	poly_lasso_cut();
}

void pressed_copy( GtkMenuItem *menu_item, gpointer user_data )
{			// Copy current selection to clipboard
	if ( tool_type == TOOL_POLYGON )
	{
		do_the_copy(1);
		poly_mask();
	}
	if ( tool_type == TOOL_SELECT )
	{
		if ( marq_status >= MARQUEE_PASTE ) do_the_copy(3);
		else do_the_copy(1);
	}
}

/* !!! Add support for channel-specific option sets !!! */
void update_menus()			// Update edit/undo menu
{
	int i, j;

	update_undo_bar();

	if ( mem_img_bpp == 1 )
	{
		men_item_state( menu_only_indexed, TRUE );
		men_item_state( menu_only_24, FALSE );
	}
	if ( mem_img_bpp == 3 )
	{
		men_item_state( menu_only_indexed, FALSE );
		men_item_state( menu_only_24, TRUE );
	}

	if ( mem_img_bpp == 3 && mem_clipboard != NULL && mem_clip_bpp )
		men_item_state( menu_alphablend, TRUE );
	else	men_item_state( menu_alphablend, FALSE );

	if ( marq_status == MARQUEE_NONE )
	{
		men_item_state( menu_need_selection, FALSE );
		men_item_state( menu_crop, FALSE );
		if ( poly_status == POLY_DONE )
		{
			men_item_state( menu_lasso, TRUE );
			men_item_state( menu_need_marquee, TRUE );
		}
		else
		{
			men_item_state( menu_lasso, FALSE );
			men_item_state( menu_need_marquee, FALSE );
		}
	}
	else
	{
		if ( poly_status != POLY_DONE ) men_item_state( menu_lasso, FALSE );

		men_item_state( menu_need_marquee, TRUE );

		if ( marq_status >= MARQUEE_PASTE )	// If we are pasting disallow copy/cut/crop
		{
			men_item_state( menu_need_selection, FALSE );
			men_item_state( menu_crop, FALSE );
		}
		else	men_item_state( menu_need_selection, TRUE );

		// Only offer the crop option if the user hasn't selected everything
		if ((marq_status <= MARQUEE_DONE) &&
			((abs(marq_x1 - marq_x2) < mem_width - 1) ||
			(abs(marq_y1 - marq_y2) < mem_height - 1)))
			men_item_state( menu_crop, TRUE );
		else men_item_state( menu_crop, FALSE );
	}

	if ( mem_clipboard == NULL ) men_item_state( menu_need_clipboard, FALSE );
	else
	{
		if (mem_clip_bpp == MEM_BPP) men_item_state( menu_need_clipboard, TRUE );
		else men_item_state( menu_need_clipboard, FALSE );
			// Only allow pasting if the image is the same type as the clipboard
	}

	if ( mem_undo_done == 0 ) men_item_state( menu_undo, FALSE );
	else men_item_state( menu_undo, TRUE );

	if ( mem_undo_redo == 0 ) men_item_state( menu_redo, FALSE );
	else  men_item_state( menu_redo, TRUE );

	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_chann_x[mem_channel]), TRUE);

	for (i = j = 0; i < NUM_CHANNELS; i++)	// Enable/disable channel enable/disable
	{
		if (mem_img[i]) j++;
		gtk_widget_set_sensitive(menu_chan_dis[i], !!mem_img[i]);
	}
	if (j > 1) men_item_state(menu_chan_del, TRUE);
	else men_item_state(menu_chan_del, FALSE);
}

void canvas_undo_chores()
{
	gtk_widget_set_usize( drawing_canvas, mem_width*can_zoom, mem_height*can_zoom );
	update_all_views();				// redraw canvas widget
	update_menus();
	init_pal();
	gtk_widget_queue_draw( drawing_col_prev );
}

void check_undo_paste_bpp()
{
	if (marq_status >= MARQUEE_PASTE && (mem_clip_bpp != MEM_BPP))
		pressed_select_none( NULL, NULL );

	if ( tool_type == TOOL_SMUDGE && mem_img_bpp == 1 )
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(icon_buttons[DEFAULT_TOOL_ICON]), TRUE );
		// User is smudging and undo/redo to an indexed image - reset tool
}

void main_undo( GtkMenuItem *menu_item, gpointer user_data )
{
	mem_undo_backward();
	check_undo_paste_bpp();
	canvas_undo_chores();
}

void main_redo( GtkMenuItem *menu_item, gpointer user_data )
{
	mem_undo_forward();
	check_undo_paste_bpp();
	canvas_undo_chores();
}

/* Save palette to a file */
static int save_pal(char *file_name, ls_settings *settings)		
{
	FILE *fp;
	int i;

	if ((fp = fopen(file_name, "w")) == NULL) return -1;

	if (settings->ftype == FT_GPL)		// .gpl file
	{
		fprintf(fp, "GIMP Palette\nName: mtPaint\nColumns: 16\n#\n");
		for (i = 0; i < settings->colors; i++)
			fprintf(fp, "%3i %3i %3i\tUntitled\n",
				settings->pal[i].red, settings->pal[i].green,
				settings->pal[i].blue);
	}

	if (settings->ftype == FT_TXT)		// .txt file
	{
		fprintf(fp, "%i\n", settings->colors);
		for (i = 0; i < settings->colors; i++)
			fprintf(fp, "%i,%i,%i\n",
				settings->pal[i].red, settings->pal[i].green,
				settings->pal[i].blue);
	}

	fclose( fp );

	return 0;
}

int load_pal(char *file_name)			// Load palette file
{
	int i;
	png_color new_mem_pal[256];

	i = mem_load_pal( file_name, new_mem_pal );

	if ( i < 0 ) return i;		// Failure

	spot_undo(UNDO_PAL);

	mem_pal_copy( mem_pal, new_mem_pal );
	mem_cols = i;

	update_all_views();
	init_pal();
	gtk_widget_queue_draw(drawing_col_prev);

	return 0;
}


void update_cols()
{
	if (!mem_img[CHN_IMAGE]) return;	// Only do this if we have an image

	update_image_bar();
	mem_pat_update();

	if ( marq_status >= MARQUEE_PASTE && text_paste )
	{
		render_text( drawing_col_prev );
		check_marquee();
		gtk_widget_queue_draw( drawing_canvas );
	}

	gtk_widget_queue_draw( drawing_col_prev );
}

void init_pal()					// Initialise palette after loading
{
	mem_pal_init();				// Update palette RGB on screen
	gtk_widget_set_usize( drawing_palette, PALETTE_WIDTH, 4+mem_cols*16 );

	update_cols();
	gtk_widget_queue_draw( drawing_palette );
}

#if GTK_MAJOR_VERSION == 2
void cleanse_txt( char *out, char *in )		// Cleans up non ASCII chars for GTK+2
{
	char *c;

	c = g_locale_to_utf8( (gchar *) in, -1, NULL, NULL, NULL );
	if ( c == NULL )
	{
		sprintf(out, "Error in cleanse_txt using g_*_to_utf8");
	}
	else
	{
		strcpy( out, c );
		g_free(c);
	}
}
#endif

void set_new_filename( char *fname )
{
	strncpy( mem_filename, fname, 250 );
	update_titlebar();
}

static int populate_channel(char *filename)
{
	int ftype, res = -1;

	ftype = detect_image_format(filename);
	if (ftype < 0) return (-1); /* Silently fail if no file */

	/* !!! No other formats for now */
	if (ftype == FT_PNG) res = load_image(filename, FS_CHANNEL_LOAD, ftype);

	/* Successful */
	if (res == 1) canvas_undo_chores();

	/* Not enough memory available */
	else if (res == FILE_MEM_ERROR) memory_errors(1);

	/* Unspecified error */
	else alert_box(_("Error"), _("Invalid channel file."), _("OK"), NULL, NULL);

	return (res == 1 ? 0 : -1);
}

int do_a_load( char *fname )
{
	char mess[512], real_fname[300];
	int res, i, ftype;


	if ((fname[0] != DIR_SEP)
#ifdef WIN32
		&& (fname[1] != ':')
#endif
	)
	{
		getcwd(real_fname, 256);
		i = strlen(real_fname);
		real_fname[i] = DIR_SEP;
		real_fname[i + 1] = 0;
		strncat(real_fname, fname, 256);
	}
	else strncpy(real_fname, fname, 256);

	ftype = detect_image_format(real_fname);
	if ((ftype < 0) || (ftype == FT_NONE))
	{
		alert_box(_("Error"), ftype < 0 ? _("Cannot open file") :
			_("Unsupported file format"), _("OK"), NULL, NULL);
		return (1);
	}

	set_image(FALSE);

	switch (ftype)
	{
	case FT_PNG:
	case FT_GIF:
	case FT_JPEG:
	case FT_TIFF:
		res = load_image(real_fname, FS_PNG_LOAD, ftype);
		break;
	case FT_BMP: res = load_bmp( real_fname ); break;
	case FT_XPM: res = load_xpm( real_fname ); break;
	case FT_XBM: res = load_xbm( real_fname ); break;
	case FT_LAYERS1: res = load_layers(real_fname); break;
	default: res = -1; break;
	}

	if ( res<=0 )				// Error loading file
	{
		if (res == NOT_INDEXED)
		{
			snprintf(mess, 500, _("Image is not indexed: %s"), fname);
			alert_box( _("Error"), mess, ("OK"), NULL, NULL );
		} else
			if (res == TOO_BIG)
			{
				snprintf(mess, 500, _("File is too big, must be <= to width=%i height=%i : %s"), MAX_WIDTH, MAX_HEIGHT, fname);
				alert_box( _("Error"), mess, _("OK"), NULL, NULL );
			} else
			{
				alert_box( _("Error"), _("Unable to load file"),
					_("OK"), NULL, NULL );
			}
		goto fail;
	}

	if ( res == FILE_LIB_ERROR )
		alert_box( _("Error"), _("The file import library had to terminate due to a problem with the file (possibly corrupt image data or a truncated file). I have managed to load some data as the header seemed fine, but I would suggest you save this image to a new file to ensure this does not happen again."), _("OK"), NULL, NULL );

	if ( res == FILE_MEM_ERROR ) memory_errors(1);		// Image was too large for OS

/* !!! Not here - move to success-only path! */
	if (ftype != FT_LAYERS1)
	{
		register_file(real_fname);
		set_new_filename(real_fname);

		/* To prevent automatic paste following a file load when enabling
		 * "Changing tool commits paste" via preferences */
		pressed_select_none(NULL, NULL);
		reset_tools();
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(icon_buttons[DEFAULT_TOOL_ICON]), TRUE );
		if ( layers_total>0 )
			layers_notify_changed(); // We loaded an image into the layers, so notify change
	}
	else
	{
		register_file(real_fname);		// Update recently used file list
//		if ( layers_window == NULL ) pressed_layers( NULL, NULL );
		if ( !view_showing ) view_show();
			// We have just loaded a layers file so display view & layers window if not up
		update_menus();
	}

	if ( res>0 )
	{
		update_all_views();					// Show new image
		update_image_bar();

		gtk_adjustment_value_changed( gtk_scrolled_window_get_hadjustment(
			GTK_SCROLLED_WINDOW(scrolledwindow_canvas) ) );
		gtk_adjustment_value_changed( gtk_scrolled_window_get_vadjustment(
			GTK_SCROLLED_WINDOW(scrolledwindow_canvas) ) );
				// These 2 are needed to synchronize the scrollbars & image view
		res = 1;
	}
fail:	set_image(TRUE);
	return (res != 1);
}



///	FILE SELECTION WINDOW

int check_file( char *fname )		// Does file already exist?  Ask if OK to overwrite
{
	char mess[512];

	if ( valid_file(fname) == 0 )
	{
		snprintf(mess, 500, _("File: %s already exists. Do you want to overwrite it?"), fname);
		if ( alert_box( _("File Found"), mess, _("NO"), _("YES"), NULL ) != 2 ) return 1;
	}

	return 0;
}


static void change_image_format(GtkMenuItem *menuitem, GtkWidget *box)
{
	static int flags[] = {FF_TRANS, FF_COMPR, FF_SPOT, FF_SPOT, 0};
	GList *chain = GTK_BOX(box)->children->next->next;
	int i, ftype;

	ftype = (int)gtk_object_get_user_data(GTK_OBJECT(menuitem));
	/* Hide/show name/value widget pairs */
	for (i = 0; flags[i]; i++)
	{
		if (file_formats[ftype].flags & flags[i])
		{
			gtk_widget_show(((GtkBoxChild*)chain->data)->widget);
			gtk_widget_show(((GtkBoxChild*)chain->next->data)->widget);
		}
		else
		{
			gtk_widget_hide(((GtkBoxChild*)chain->data)->widget);
			gtk_widget_hide(((GtkBoxChild*)chain->next->data)->widget);
		}
		chain = chain->next->next;
	}
}

static void image_widgets(GtkWidget *box, char *name, int mode)
{
	GtkWidget *opt, *menu, *item, *label, *spin;
	int i, j, k, mask = FF_IDX;
	char *ext = strrchr(name, '.');

	ext = ext ? ext + 1 : "";
	switch (mode)
	{
	default: return;
	case FS_CHANNEL_SAVE: if (mem_channel != CHN_IMAGE) break;
	case FS_PNG_SAVE: mask = mem_img_bpp == 3 ? FF_RGB : mem_cols <= 2 ?
		FF_BW | FF_IDX : FF_IDX;
		break;
	case FS_COMPOSITE_SAVE: mask = FF_RGB;
	}

	/* Create controls (!!! two widgets per value - used in traversal) */
	label = gtk_label_new(_("File Format"));
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 5);
	opt = gtk_option_menu_new();
	gtk_box_pack_start(GTK_BOX(box), opt, FALSE, FALSE, 5);

	label = gtk_label_new(_("Transparency index"));
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 5);
	spin = add_a_spin(mem_xpm_trans, -1, mem_cols - 1);
	gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 5);

	label = gtk_label_new(_("JPEG Save Quality (100=High)"));
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 5);
	spin = add_a_spin(mem_jpeg_quality, 0, 100);
	gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 5);

	label = gtk_label_new(_("Hotspot at X ="));
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 5);
	spin = add_a_spin(mem_xbm_hot_x, -1, mem_width - 1);
	gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 5);

	label = gtk_label_new(_("Y ="));
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 5);
	spin = add_a_spin(mem_xbm_hot_y, -1, mem_height - 1);
	gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 5);

	gtk_widget_show_all(box);

	menu = gtk_menu_new();
	for (i = j = k = 0; i < NUM_FTYPES; i++)
	{
		if (!(file_formats[i].flags & mask)) continue;
		if (!strncasecmp(ext, file_formats[i].ext, LONGEST_EXT) ||
			(file_formats[i].ext2[0] &&
			!strncasecmp(ext, file_formats[i].ext2, LONGEST_EXT)))
			j = k;
		item = gtk_menu_item_new_with_label(file_formats[i].name);
		gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)i);
		gtk_signal_connect(GTK_OBJECT(item), "activate",
			GTK_SIGNAL_FUNC(change_image_format), (gpointer)box);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
		k++;
  	}
	gtk_widget_show_all(menu);
	gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);

	gtk_option_menu_set_history(GTK_OPTION_MENU(opt), j);

	gtk_signal_emit_by_name(GTK_OBJECT(g_list_nth_data(
		GTK_MENU_SHELL(menu)->children, j)), "activate", (gpointer)box);
}

static void palette_widgets(GtkWidget *box, char *name, int mode)
{
	GtkWidget *opt, *menu, *item, *label;
	int i, j, k;
	char *ext = strrchr(name, '.');

	ext = ext ? ext + 1 : "";

	label = gtk_label_new(_("File Format"));
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 10);
	opt = gtk_option_menu_new();
	gtk_box_pack_start(GTK_BOX(box), opt, FALSE, FALSE, 10);

	menu = gtk_menu_new();
	for (i = j = k = 0; i < NUM_FTYPES; i++)
	{
		if (!(file_formats[i].flags & FF_PALETTE)) continue;
		if (!strncasecmp(ext, file_formats[i].ext, LONGEST_EXT) ||
			(file_formats[i].ext2[0] &&
			!strncasecmp(ext, file_formats[i].ext2, LONGEST_EXT)))
			j = k;
		item = gtk_menu_item_new_with_label(file_formats[i].name);
		gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)i);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
		k++;
  	}
	gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
	gtk_option_menu_set_history(GTK_OPTION_MENU(opt), j);

	gtk_widget_show_all(box);
}

static GtkWidget *ls_settings_box(char *name, int mode)
{
	GtkWidget *box, *label;

	box = gtk_hbox_new(FALSE, 0);
	gtk_object_set_user_data(GTK_OBJECT(box), (gpointer)mode);

	switch (mode) /* Only save operations need settings */
	{
	case FS_PNG_SAVE:
	case FS_CHANNEL_SAVE:
	case FS_COMPOSITE_SAVE:
		image_widgets(box, name, mode);
		break;
	case FS_LAYER_SAVE: /* !!! No selectable layer file format yet */
		break;
	case FS_EXPORT_GIF: /* !!! No selectable formats yet */
		label = gtk_label_new(_("Animation delay"));
		gtk_widget_show(label);
		gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
		label = add_a_spin(preserved_gif_delay, 1, MAX_DELAY);
		gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 10);
		break;
	case FS_PALETTE_SAVE:
		palette_widgets(box, name, mode);
		break;
	default: /* Give a hidden empty box */
		return (box);
	}

	gtk_widget_show(box);
	return (box);
}

static int selected_file_type(GtkWidget *box)
{
	GtkWidget *opt;

	opt = BOX_CHILD(box, 1);
	opt = gtk_option_menu_get_menu(GTK_OPTION_MENU(opt));
	if (!opt) return (FT_NONE);
	opt = gtk_menu_get_active(GTK_MENU(opt));
	return ((int)gtk_object_get_user_data(GTK_OBJECT(opt)));
}

void init_ls_settings(ls_settings *settings, GtkWidget *box)
{
	int xmode;

	memset(settings, 0, sizeof(ls_settings));
	settings->ftype = FT_NONE;

	/* Read in settings */
	if (box)
	{
		xmode = (int)gtk_object_get_user_data(GTK_OBJECT(box));
		settings->mode = xmode;
		switch (xmode)
		{
		case FS_PNG_SAVE:
		case FS_CHANNEL_SAVE:
		case FS_COMPOSITE_SAVE:
			settings->ftype = selected_file_type(box);
			settings->xpm_trans = read_spin(BOX_CHILD(box, 3));
			settings->jpeg_quality = read_spin(BOX_CHILD(box, 5));
			settings->hot_x = read_spin(BOX_CHILD(box, 7));
			settings->hot_y = read_spin(BOX_CHILD(box, 9));
			break;
		case FS_LAYER_SAVE: /* Nothing to do yet */
			break;
		case FS_EXPORT_GIF: /* No formats yet */
			settings->gif_delay = read_spin(BOX_CHILD(box, 1));
			break;
		case FS_PALETTE_SAVE:
			settings->ftype = selected_file_type(box);
			break;
		case FS_EXPORT_UNDO:
		case FS_EXPORT_UNDO2:
			settings->ftype = FT_PNG;
		default: /* Use defaults */
			box = NULL;
			break;
		}
	}

	/* Set defaults */
	if (!box)
	{
		settings->xpm_trans = mem_xpm_trans;
		settings->jpeg_quality = mem_jpeg_quality;
		settings->hot_x = mem_xbm_hot_x;
		settings->hot_y = mem_xbm_hot_y;
		settings->gif_delay = preserved_gif_delay;
	}

	/* Default expansion of xpm_trans */
	settings->rgb_trans = settings->xpm_trans < 0 ? -1 :
		PNG_2_INT(mem_pal[settings->xpm_trans]);
}

static void store_ls_settings(ls_settings *settings)
{
	guint32 fflags = file_formats[settings->ftype].flags;

	switch (settings->mode)
	{
	case FS_PNG_SAVE:
	case FS_CHANNEL_SAVE:
	case FS_COMPOSITE_SAVE:
		if (fflags & FF_TRANS)
			mem_xpm_trans = settings->xpm_trans;
		if (fflags & FF_COMPR)
		{
			mem_jpeg_quality = settings->jpeg_quality;
			inifile_set_gint32("jpegQuality", mem_jpeg_quality);
		}
		if (fflags & FF_SPOT)
		{
			mem_xbm_hot_x = settings->hot_x;
			mem_xbm_hot_y = settings->hot_y;
		}
		break;
	case FS_EXPORT_GIF:
		preserved_gif_delay = settings->gif_delay;
		break;
	}
}

static gboolean fs_destroy(GtkWidget *fs)
{
	int x, y, width, height;

	gdk_window_get_size(fs->window, &width, &height);
	gdk_window_get_root_origin(fs->window, &x, &y);

	inifile_set_gint32("fs_window_x", x);
	inifile_set_gint32("fs_window_y", y);
	inifile_set_gint32("fs_window_w", width);
	inifile_set_gint32("fs_window_h", height);

	gtk_window_set_transient_for(GTK_WINDOW(fs), NULL);
	gtk_widget_destroy(fs);

	return FALSE;
}

static gint fs_ok(GtkWidget *fs)
{
	ls_settings settings;
	GtkWidget *xtra;
	char fname[256], mess[512], gif_nam[256], gif_nam2[320], *c, *ext, *ext2;
	int i, j;

	/* Pick up extra info */
	xtra = GTK_WIDGET(gtk_object_get_user_data(GTK_OBJECT(fs)));
	init_ls_settings(&settings, xtra);

	/* Needed to show progress in Windows GTK+2 */
	gtk_window_set_modal(GTK_WINDOW(fs), FALSE);

	/* Better aesthetics? */
	gtk_widget_hide(fs);

	/* File extension */
	strncpy(fname, gtk_entry_get_text(GTK_ENTRY(
		GTK_FILE_SELECTION(fs)->selection_entry)), 256);
	c = strrchr(fname, '.');
	while (TRUE)
	{
		/* Cut the extension off */
		if ((settings.mode == FS_CLIP_FILE) ||
			(settings.mode == FS_EXPORT_UNDO) ||
			(settings.mode == FS_EXPORT_UNDO2))
		{
			if (!c) break;
			*c = '\0';
		}
		/* Modify the file extension if needed */
		else
		{
			ext = file_formats[settings.ftype].ext;
			if (!ext[0]) break;
		
			if (c) /* There is an extension */
			{
				/* Same extension? */
				if (!strncasecmp(c + 1, ext, 256)) break;
				/* Alternate extension? */
				ext2 = file_formats[settings.ftype].ext2;
				if (ext2[0] && !strncasecmp(c + 1, ext2, 256))
					break;
				/* Truncate */
				*c = '\0';
			}
			i = strlen(fname);
			j = strlen(ext);
			if (i + j + 1 > 250) break; /* Too long */
			fname[i] = '.';
			strncpy(fname + i + 1, ext, j + 1);
		}
		gtk_entry_set_text(GTK_ENTRY(
			GTK_FILE_SELECTION(fs)->selection_entry), fname);
		break;
	}

	/* Get filename the proper way */
#if GTK_MAJOR_VERSION == 2
	c = g_locale_from_utf8((gchar *)gtk_file_selection_get_filename(
		GTK_FILE_SELECTION(fs)), -1, NULL, NULL, NULL);
	if (c)
	{
		strncpy(fname, c, 250);
		g_free(c);
	}
	else
#endif
	strncpy(fname, gtk_file_selection_get_filename(GTK_FILE_SELECTION(fs)), 250);


	switch (settings.mode)
	{
	case FS_PNG_LOAD:
		if (do_a_load(fname) == 1) goto redo;
		break;
	case FS_PNG_SAVE:
		if (check_file(fname)) goto redo;
		store_ls_settings(&settings);	// Update data in memory
		if (gui_save(fname, &settings) < 0) goto redo;
		if (layers_total > 0)
		{
			/* Filename has changed so layers file needs re-saving to be correct */
			if (strcmp(fname, mem_filename)) layers_notify_changed();
		}
		set_new_filename(fname);
		update_image_bar();	// Update transparency info
		update_all_views();	// Redraw in case transparency changed
		break;
	case FS_PALETTE_LOAD:
		if (load_pal(fname))
		{
			snprintf(mess, 500, _("File: %s invalid - palette not updated"), fname);
			alert_box( _("Error"), mess, _("OK"), NULL, NULL );
			goto redo;
		}
		else notify_changed();
		break;
	case FS_PALETTE_SAVE:
		if (check_file(fname)) goto redo;
		settings.pal = mem_pal;
		settings.colors = mem_cols;
		if (save_pal(fname, &settings) < 0) goto redo_name;
		break;
	case FS_CLIP_FILE:
		if (clipboard_entry)
			gtk_entry_set_text(GTK_ENTRY(clipboard_entry), fname);
		break;
	case FS_EXPORT_UNDO:
	case FS_EXPORT_UNDO2:
		if (export_undo(fname, &settings))
			alert_box( _("Error"), _("Unable to export undo images"),
				_("OK"), NULL, NULL );
		break;
	case FS_EXPORT_ASCII:
		if (check_file(fname)) goto redo;
		if (export_ascii(fname))
			alert_box( _("Error"), _("Unable to export ASCII file"),
				_("OK"), NULL, NULL );
		break;
	case FS_LAYER_SAVE:
		if (check_file(fname)) goto redo;
		if (save_layers(fname) != 1) goto redo;
		break;
	case FS_GIF_EXPLODE:
		c = strrchr( preserved_gif_filename, DIR_SEP );
		if ( c == NULL ) c = preserved_gif_filename;
		else c++;
		snprintf(gif_nam, 250, "%s%c%s", fname, DIR_SEP, c);
		snprintf(mess, 500,
			"gifsicle -U --explode \"%s\" -o \"%s\"",
			preserved_gif_filename, gif_nam );
//printf("%s\n", mess);
		gifsicle(mess);
		strncat( gif_nam, ".???", 250 );
		wild_space_change( gif_nam, gif_nam2, 315 );
		snprintf(mess, 500,
			"mtpaint -g %i %s &",
			preserved_gif_delay, gif_nam2 );
//printf("%s\n", mess);
		gifsicle(mess);
		break;
	case FS_EXPORT_GIF:
		if (check_file(fname)) goto redo;
		store_ls_settings(&settings);	// Update data in memory
		snprintf(gif_nam, 250, "%s", mem_filename);
		wild_space_change( gif_nam, gif_nam2, 315 );
		for (i = strlen(gif_nam2) - 1; (i >= 0) && (gif_nam2[i] != DIR_SEP); i--)
		{
			if ((unsigned char)(gif_nam2[i] - '0') <= 9) gif_nam2[i] = '?';
		}
						
		snprintf(mess, 500, "%s -d %i %s -o \"%s\"",
			GIFSICLE_CREATE, settings.gif_delay, gif_nam2, fname);
//printf("%s\n", mess);
		gifsicle(mess);

#ifndef WIN32
		snprintf(mess, 500, "gifview -a \"%s\" &", fname );
		gifsicle(mess);
//printf("%s\n", mess);
#endif

		break;
	case FS_CHANNEL_LOAD:
		if (populate_channel(fname)) goto redo;
		break;
	case FS_CHANNEL_SAVE:
		if (check_file(fname)) goto redo;
		settings.img[CHN_IMAGE] = mem_img[mem_channel];
		settings.width = mem_width;
		settings.height = mem_height;
		if (mem_channel == CHN_IMAGE)
		{
			settings.pal = mem_pal;
			settings.bpp = mem_img_bpp;
			settings.colors = mem_cols;
		}
		else
		{
			settings.pal = NULL; /* Greyscale one 'll be created */
			settings.bpp = 1;
			settings.colors = 256;
			settings.xpm_trans = -1;
		}
		if (save_image(fname, &settings)) goto redo_name;
		break;
	case FS_COMPOSITE_SAVE:
		if (check_file(fname)) goto redo;
		if (layer_save_composite(fname, &settings)) goto redo_name;
		break;
	}

	update_menus();

	fs_destroy(fs);

	return FALSE;
redo_name:
	snprintf(mess, 500, _("Unable to save file: %s"), fname);
	alert_box( _("Error"), mess, _("OK"), NULL, NULL );
redo:
	gtk_widget_show(fs);
	gtk_window_set_modal(GTK_WINDOW(fs), TRUE);
	return FALSE;
}

void file_selector(int action_type)
{
	char *title = NULL, txt[300], txt2[300];
	GtkWidget *fs, *xtra;


	switch (action_type)
	{
	case FS_PNG_LOAD:
		title = _("Load Image File");
		if (layers_total == 0)
		{
			if (check_for_changes() == 1) return;
		}
		else if (check_layers_for_changes() == 1) return;
		break;
	case FS_PNG_SAVE:
		title = _("Save Image File");
		break;
	case FS_PALETTE_LOAD:
		title = _("Load Palette File");
		break;
	case FS_PALETTE_SAVE:
		title = _("Save Palette File");
		break;
	case FS_CLIP_FILE:
		title = _("Select Clipboard File");
		break;
	case FS_EXPORT_UNDO:
		title = _("Export Undo Images");
		break;
	case FS_EXPORT_UNDO2:
		title = _("Export Undo Images (reversed)");
		break;
	case FS_EXPORT_ASCII:
		title = _("Export ASCII Art");
		break;
	case FS_LAYER_SAVE:
		title = _("Save Layer Files");
		break;
	case FS_GIF_EXPLODE:
		title = _("Import GIF animation - Choose frames directory");
		break;
	case FS_EXPORT_GIF:
		title = _("Export GIF animation");
		break;
	case FS_CHANNEL_LOAD:
		title = _("Load Channel");
		break;
	case FS_CHANNEL_SAVE:
		title = _("Save Channel");
		break;
	case FS_COMPOSITE_SAVE:
		title = _("Save Composite Image");
		break;
	}

	fs = gtk_file_selection_new(title);

	gtk_window_set_modal(GTK_WINDOW(fs), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(fs),
		inifile_get_gint32("fs_window_w", 550),
		inifile_get_gint32("fs_window_h", 500));
	gtk_widget_set_uposition(fs,
		inifile_get_gint32("fs_window_x", 0),
		inifile_get_gint32("fs_window_y", 0));

	if ((action_type == FS_EXPORT_UNDO) || (action_type == FS_EXPORT_UNDO2)
		 || (action_type == FS_GIF_EXPLODE))
	{
//		gtk_widget_set_sensitive( GTK_WIDGET(GTK_FILE_SELECTION(fs)->file_list), FALSE );
		gtk_widget_hide(GTK_WIDGET(GTK_FILE_SELECTION(fs)->file_list));
		if (action_type == FS_GIF_EXPLODE)
			gtk_widget_hide(GTK_WIDGET(GTK_FILE_SELECTION(fs)->selection_entry));
	}

	gtk_signal_connect_object(GTK_OBJECT(GTK_FILE_SELECTION(fs)->ok_button),
		"clicked", GTK_SIGNAL_FUNC(fs_ok), GTK_OBJECT(fs));

	gtk_signal_connect_object(GTK_OBJECT(GTK_FILE_SELECTION(fs)->cancel_button),
		"clicked", GTK_SIGNAL_FUNC(fs_destroy), GTK_OBJECT(fs));

	gtk_signal_connect_object(GTK_OBJECT(fs),
		"delete_event", GTK_SIGNAL_FUNC(fs_destroy), GTK_OBJECT(fs));

	if ((action_type == FS_PNG_SAVE) && strcmp(mem_filename, _("Untitled")))
		strncpy( txt, mem_filename, 256 );	// If we have a filename and saving
	else if ((action_type == FS_LAYER_SAVE) &&
		strcmp(layers_filename, _("Untitled")))
		strncpy(txt, layers_filename, 256);
	else if (action_type == FS_LAYER_SAVE)
	{
		strncpy(txt, inifile_get("last_dir", "/"), 256);
		strncat(txt, "layers.txt", 256);
	}
	else strncpy(txt, inifile_get("last_dir", "/"), 256);	// Default

#if GTK_MAJOR_VERSION == 2
	cleanse_txt( txt2, txt );		// Clean up non ASCII chars
#else
	strcpy( txt2, txt );
#endif
	gtk_file_selection_set_filename(GTK_FILE_SELECTION(fs), txt2);

	xtra = ls_settings_box(txt2, action_type);
	gtk_box_pack_start(GTK_BOX(GTK_FILE_SELECTION(fs)->main_vbox), xtra,
		FALSE, TRUE, 0);
	gtk_object_set_user_data(GTK_OBJECT(fs), xtra);

	gtk_widget_show(fs);
	gtk_window_set_transient_for(GTK_WINDOW(fs), GTK_WINDOW(main_window));
	gdk_window_raise(fs->window);	// Needed to ensure window is at the top
}

void align_size( float new_zoom )		// Set new zoom level
{
	GtkAdjustment *hori, *vert;
	int nv_h = 0, nv_v = 0;			// New positions of scrollbar

	if ( zoom_flag != 0 ) return;		// Needed as we could be called twice per iteration

	if ( new_zoom<MIN_ZOOM ) new_zoom = MIN_ZOOM;
	if ( new_zoom>MAX_ZOOM ) new_zoom = MAX_ZOOM;

	if ( new_zoom != can_zoom )
	{
		zoom_flag = 1;
		hori = gtk_scrolled_window_get_hadjustment( GTK_SCROLLED_WINDOW(scrolledwindow_canvas) );
		vert = gtk_scrolled_window_get_vadjustment( GTK_SCROLLED_WINDOW(scrolledwindow_canvas) );

		if ( mem_ics == 0 )
		{
			if ( hori->page_size > mem_width*can_zoom ) mem_icx = 0.5;
			else mem_icx = ( hori->value + ((float) hori->page_size )/2 )
				/ (mem_width*can_zoom);

			if ( vert->page_size > mem_height*can_zoom ) mem_icy = 0.5;
			else mem_icy = ( vert->value + ((float) vert->page_size )/2 )
				/ (mem_height*can_zoom);
		}
		mem_ics = 0;

		can_zoom = new_zoom;

		if ( hori->page_size < mem_width*can_zoom )
			nv_h = mt_round( mem_width*can_zoom*mem_icx - ((float)hori->page_size)/2 );
		else	nv_h = 0;

		if ( vert->page_size < mem_height*can_zoom )
			nv_v = mt_round( mem_height*can_zoom*mem_icy - ((float)vert->page_size)/2 );
		else	nv_v = 0;

		hori->value = nv_h;
		hori->upper = mt_round(mem_width*can_zoom);
		vert->value = nv_v;
		vert->upper = mt_round(mem_height*can_zoom);

#if GTK_MAJOR_VERSION == 1
		gtk_adjustment_value_changed( hori );
		gtk_adjustment_value_changed( vert );
#endif
		gtk_widget_set_usize( drawing_canvas, mem_width*can_zoom, mem_height*can_zoom );

		update_image_bar();
		zoom_flag = 0;
		vw_focus_view();		// View window position may need updating
		toolbar_zoom_update();
	}
}

void square_continuous( int nx, int ny, int *minx, int *miny, int *xw, int *yh )
{
	if ( tool_size == 1 )
	{
		put_pixel( nx, ny );
	}
	else
	{
		if ( tablet_working )	// Needed to fill in possible gap when size changes
		{
			f_rectangle( tool_ox - tool_size/2, tool_oy - tool_size/2,
					tool_size, tool_size );
		}
		if ( ny > tool_oy )		// Down
		{
			h_para( tool_ox - tool_size/2,
				tool_oy - tool_size/2 + tool_size - 1,
				nx - tool_size/2,
				ny - tool_size/2 + tool_size - 1,
				tool_size );
		}
		if ( nx > tool_ox )		// Right
		{
			v_para( tool_ox - tool_size/2 + tool_size - 1,
				tool_oy - tool_size/2,
				nx - tool_size/2 + tool_size -1,
				ny - tool_size/2,
				tool_size );
		}
		if ( ny < tool_oy )		// Up
		{
			h_para( tool_ox - tool_size/2,
				tool_oy - tool_size/2,
				nx - tool_size/2,
				ny - tool_size/2,
				tool_size );
		}
		if ( nx < tool_ox )		// Left
		{
			v_para( tool_ox - tool_size/2,
				tool_oy - tool_size/2,
				nx - tool_size/2,
				ny - tool_size/2,
				tool_size );
		}
	}
}

void vertical_continuous( int nx, int ny, int *minx, int *miny, int *xw, int *yh )
{
	int	ax = tool_ox, ay = tool_oy - tool_size/2,
		bx = nx, by = ny - tool_size/2, vlen = tool_size;

	int mny, may;

	if ( ax == bx )		// Simple vertical line required
	{
		mtMIN( ay, tool_oy - tool_size/2, ny - tool_size/2 )
		mtMAX( by, tool_oy - tool_size/2 + tool_size - 1, ny - tool_size/2 + tool_size - 1 )
		vlen = by - ay + 1;
		if ( ay < 0 )
		{
			vlen = vlen + ay;
			ay = 0;
		}
		if ( by >= mem_height )
		{
			vlen = vlen - ( mem_height - by + 1 );
			by = mem_height - 1;
		}

		if ( vlen <= 1 )
		{
			ax = bx; ay = by;
			put_pixel( bx, by );
		}
		else
		{
			sline( ax, ay, bx, by );

			mtMIN( *minx, ax, bx )
			mtMIN( *miny, ay, by )
			*xw = abs( ax - bx ) + 1;
			*yh = abs( ay - by ) + 1;
		}
	}
	else			// Parallelogram with vertical left and right sides required
	{
		v_para( ax, ay, bx, by, tool_size );

		mtMIN( *minx, ax, bx )
		*xw = abs( ax - bx ) + 1;

		mtMIN( mny, ay, by )
		mtMAX( may, ay + tool_size - 1, by + tool_size - 1 )

		mtMAX( mny, mny, 0 )
		mtMIN( may, may, mem_height )

		*miny = mny;
		*yh = may - mny + 1;
	}
}

void horizontal_continuous( int nx, int ny, int *minx, int *miny, int *xw, int *yh )
{
	int ax = tool_ox - tool_size/2, ay = tool_oy,
		bx = nx - tool_size/2, by = ny, hlen = tool_size;

	int mnx, max;

	if ( ay == by )		// Simple horizontal line required
	{
		mtMIN( ax, tool_ox - tool_size/2, nx - tool_size/2 )
		mtMAX( bx, tool_ox - tool_size/2 + tool_size - 1, nx - tool_size/2 + tool_size - 1 )
		hlen = bx - ax + 1;
		if ( ax < 0 )
		{
			hlen = hlen + ax;
			ax = 0;
		}
		if ( bx >= mem_width )
		{
			hlen = hlen - ( mem_width - bx + 1 );
			bx = mem_width - 1;
		}

		if ( hlen <= 1 )
		{
			ax = bx; ay = by;
			put_pixel( bx, by );
		}
		else
		{
			sline( ax, ay, bx, by );

			mtMIN( *minx, ax, bx )
			mtMIN( *miny, ay, by )
			*xw = abs( ax - bx ) + 1;
			*yh = abs( ay - by ) + 1;
		}
	}
	else			// Parallelogram with horizontal top and bottom sides required
	{
		h_para( ax, ay, bx, by, tool_size );

		mtMIN( *miny, ay, by )
		*yh = abs( ay - by ) + 1;

		mtMIN( mnx, ax, bx )
		mtMAX( max, ax + tool_size - 1, bx + tool_size - 1 )

		mtMAX( mnx, mnx, 0 )
		mtMIN( max, max, mem_width )

		*minx = mnx;
		*xw = max - mnx + 1;
	}
}

void update_all_views()				// Update whole canvas on all views
{
	if ( view_showing && vw_drawing ) gtk_widget_queue_draw( vw_drawing );
	if ( drawing_canvas ) gtk_widget_queue_draw( drawing_canvas );
}


void stretch_poly_line(int x, int y)			// Clear old temp line, draw next temp line
{
	if ( poly_points>0 && poly_points<MAX_POLY )
	{
		if ( line_x1 != x || line_y1 != y )	// This check reduces flicker
		{
			repaint_line(0);
			paint_poly_marquee();

			line_x1 = x;
			line_y1 = y;
			line_x2 = poly_mem[poly_points-1][0];
			line_y2 = poly_mem[poly_points-1][1];

			repaint_line(2);
		}
	}
}

static void poly_conclude()
{
	repaint_line(0);
	if ( poly_points < 3 )
	{
		poly_status = POLY_NONE;
		poly_points = 0;
		update_all_views();
		update_sel_bar();
	}
	else
	{
		poly_status = POLY_DONE;
		poly_init();			// Set up polgon stats
		marq_x1 = poly_min_x;
		marq_y1 = poly_min_y;
		marq_x2 = poly_max_x;
		marq_y2 = poly_max_y;
		update_menus();			// Update menu/icons

		paint_poly_marquee();
		update_sel_bar();
	}
}

static void poly_add_po( int x, int y )
{
	repaint_line(0);
	poly_add(x, y);
	if ( poly_points >= MAX_POLY ) poly_conclude();
	paint_poly_marquee();
	update_sel_bar();
}

void tool_action(int event, int x, int y, int button, gdouble pressure)
{
	int minx = -1, miny = -1, xw = -1, yh = -1;
	int i, j, k, rx, ry, sx, sy;
	int ox, oy, off1, off2, o_size = tool_size, o_flow = tool_flow, o_opac = tool_opacity, n_vs[3];
	int xdo, ydo, px, py, todo, oox, ooy;	// Continuous smudge stuff
	float rat;
	gboolean first_point = FALSE, paint_action = FALSE;

	if ( tool_fixx > -1 ) x = tool_fixx;
	if ( tool_fixy > -1 ) y = tool_fixy;

	if ( (button == 1 || button == 3) && (tool_type <= TOOL_SPRAY) )
		paint_action = TRUE;

	if ( tool_type <= TOOL_SHUFFLE ) tint_mode[2] = button;

	if ( pen_down == 0 )
	{
		first_point = TRUE;
		if ( button == 3 && paint_action && !tint_mode[0] )
		{
			col_reverse = TRUE;
			mem_swap_cols();
		}
	}
	else if ( tool_ox == x && tool_oy == y ) return;	// Only do something with a new point

	if ( (button == 1 || paint_action) && tool_type != TOOL_FLOOD &&
		tool_type != TOOL_SELECT && tool_type != TOOL_POLYGON )
	{
		if ( !(tool_type == TOOL_LINE && line_status == LINE_NONE) )
		{
			mem_undo_next(UNDO_TOOL);	// Do memory stuff for undo
		}
	}

	if ( tablet_working )
	{
		pressure = (pressure - 0.2)/0.8;
		mtMIN( pressure, pressure, 1)
		mtMAX( pressure, pressure, 0)

		n_vs[0] = tool_size;
		n_vs[1] = tool_flow;
		n_vs[2] = tool_opacity;
		for ( i=0; i<3; i++ )
		{
			if ( tablet_tool_use[i] )
			{
				if ( tablet_tool_factor[i] > 0 )
					n_vs[i] *= (1 + tablet_tool_factor[i] * (pressure - 1));
				else
					n_vs[i] *= (0 - tablet_tool_factor[i] * (1 - pressure));
				mtMAX( n_vs[i], n_vs[i], 1 )
			}
		}
		tool_size = n_vs[0];
		tool_flow = n_vs[1];
		tool_opacity = n_vs[2];
	}

	minx = x - tool_size/2;
	miny = y - tool_size/2;
	xw = tool_size;
	yh = tool_size;

	if ( paint_action && !first_point && mem_continuous && tool_size == 1 &&
		tool_type < TOOL_SPRAY && ( abs(x - tool_ox) > 1 || abs(y - tool_oy ) > 1 ) )
	{		// Single point continuity
		sline( tool_ox, tool_oy, x, y );

		mtMIN( minx, tool_ox, x )
		mtMIN( miny, tool_oy, y )
		xw = abs( tool_ox - x ) + 1;
		yh = abs( tool_oy - y ) + 1;
	}
	else
	{
		if ( mem_continuous && !first_point && (button == 1 || button == 3) )
		{
			mtMIN( minx, tool_ox, x )
			mtMAX( xw, tool_ox, x )
			xw = xw - minx + tool_size;
			minx = minx - tool_size/2;

			mtMIN( miny, tool_oy, y )
			mtMAX( yh, tool_oy, y )
			yh = yh - miny + tool_size;
			miny = miny - tool_size/2;

			mem_boundary( &minx, &miny, &xw, &yh );
		}
		if ( tool_type == TOOL_SQUARE && paint_action )
		{
			if ( !mem_continuous || first_point )
				f_rectangle( minx, miny, xw, yh );
			else
			{
				square_continuous(x, y, &minx, &miny, &xw, &yh);
			}
		}
		if ( tool_type == TOOL_CIRCLE  && paint_action )
		{
			if ( mem_continuous && !first_point )
			{
				tline( tool_ox, tool_oy, x, y, tool_size );
			}
			f_circle( x, y, tool_size );
		}
		if ( tool_type == TOOL_HORIZONTAL && paint_action )
		{
			if ( !mem_continuous || first_point || tool_size == 1 )
			{
				for ( j=0; j<tool_size; j++ )
				{
					rx = x - tool_size/2 + j;
					ry = y;
					IF_IN_RANGE( rx, ry ) put_pixel( rx, ry );
				}
			}
			else	horizontal_continuous(x, y, &minx, &miny, &xw, &yh);
		}
		if ( tool_type == TOOL_VERTICAL && paint_action )
		{
			if ( !mem_continuous || first_point || tool_size == 1 )
			{
				for ( j=0; j<tool_size; j++ )
				{
					rx = x;
					ry = y - tool_size/2 + j;
					IF_IN_RANGE( rx, ry ) put_pixel( rx, ry );
				}
			}
			else	vertical_continuous(x, y, &minx, &miny, &xw, &yh);
		}
		if ( tool_type == TOOL_SLASH && paint_action )
		{
			if ( mem_continuous && !first_point && tool_size > 1 )
				g_para( x + (tool_size-1)/2, y - tool_size/2,
					x + (tool_size-1)/2 - (tool_size - 1),
					y - tool_size/2 + tool_size - 1,
					tool_ox - x, tool_oy - y );
			else for ( j=0; j<tool_size; j++ )
			{
				rx = x + (tool_size-1)/2 - j;
				ry = y - tool_size/2 + j;
				IF_IN_RANGE( rx, ry ) put_pixel( rx, ry );
			}
		}
		if ( tool_type == TOOL_BACKSLASH && paint_action )
		{
			if ( mem_continuous && !first_point && tool_size > 1 )
				g_para( x - tool_size/2, y - tool_size/2,
					x - tool_size/2 + tool_size - 1,
					y - tool_size/2 + tool_size - 1,
					tool_ox - x, tool_oy - y );
			else for ( j=0; j<tool_size; j++ )
			{
				rx = x - tool_size/2 + j;
				ry = y - tool_size/2 + j;
				IF_IN_RANGE( rx, ry ) put_pixel( rx, ry );
			}
		}
		if ( tool_type == TOOL_SPRAY && paint_action )
		{
			for ( j=0; j<tool_flow; j++ )
			{
				rx = x - tool_size/2 + rand() % tool_size;
				ry = y - tool_size/2 + rand() % tool_size;
				IF_IN_RANGE( rx, ry ) put_pixel( rx, ry );
			}
		}
		if ( tool_type == TOOL_SHUFFLE && button == 1 )
		{
			for ( j=0; j<tool_flow; j++ )
			{
				rx = x - tool_size/2 + rand() % tool_size;
				ry = y - tool_size/2 + rand() % tool_size;
				sx = x - tool_size/2 + rand() % tool_size;
				sy = y - tool_size/2 + rand() % tool_size;
				IF_IN_RANGE( rx, ry ) IF_IN_RANGE( sx, sy )
				{
			/* !!! Or do something for partial mask too? !!! */
					if (!pixel_protected(rx, ry) &&
						!pixel_protected(sx, sy))
					{
						off1 = rx + ry * mem_width;
						off2 = sx + sy * mem_width;
						if ((mem_channel == CHN_IMAGE) &&
							RGBA_mode && mem_img[CHN_ALPHA])
						{
							px = mem_img[CHN_ALPHA][off1];
							py = mem_img[CHN_ALPHA][off2];
							mem_img[CHN_ALPHA][off1] = py;
							mem_img[CHN_ALPHA][off2] = px;
						}
						k = MEM_BPP;
						off1 *= k; off2 *= k;
						for (i = 0; i < k; i++)
						{
							px = mem_img[mem_channel][off1];
							py = mem_img[mem_channel][off2];
							mem_img[mem_channel][off1++] = py;
							mem_img[mem_channel][off2++] = px;
						}
					}
				}
			}
		}
		if ( tool_type == TOOL_FLOOD && button == 1 )
		{
			/* Flood fill shouldn't start on masked points */
			if (pixel_protected(x, y) < 255)
			{
				j = get_pixel(x, y);
				k = mem_channel != CHN_IMAGE ? channel_col_A[mem_channel] :
					mem_img_bpp == 1 ? mem_col_A : PNG_2_INT(mem_col_A24);
				if (j != k) /* And never start on colour A */
				{
					spot_undo(UNDO_DRAW);
					flood_fill(x, y, j);
					update_all_views();
				}
			}
		}
		if ( tool_type == TOOL_SMUDGE && button == 1 )
		{
			if ( !first_point && (tool_ox!=x || tool_oy!=y) )
			{
				if ( mem_continuous )
				{
					xdo = tool_ox - x;
					ydo = tool_oy - y;
					mtMAX( todo, abs(xdo), abs(ydo) )
					oox = tool_ox;
					ooy = tool_oy;

					for ( i=1; i<=todo; i++ )
					{
						rat = ((float) i ) / todo;
						px = mt_round(tool_ox + (x - tool_ox) * rat);
						py = mt_round(tool_oy + (y - tool_oy) * rat);
						mem_smudge(oox, ooy, px, py);
						oox = px;
						ooy = py;
					}
				}
				else mem_smudge(tool_ox, tool_oy, x, y);
			}
		}
		if ( tool_type == TOOL_CLONE && button == 1 )
		{
			if ( first_point || (!first_point && (tool_ox!=x || tool_oy!=y)) )
			{
				mem_clone( x+clone_x, y+clone_y, x, y );
			}
		}
	}

	if ( tool_type == TOOL_LINE )
	{
		if ( button == 1 )
		{
			line_x1 = x;
			line_y1 = y;
			if ( line_status == LINE_NONE )
			{
				line_x2 = x;
				line_y2 = y;
			}

			// Draw circle at x, y
			if ( line_status == LINE_LINE )
			{
				if ( tool_size > 1 )
				{
					int oldmode = mem_undo_opacity;
					mem_undo_opacity = TRUE;
					f_circle( line_x1, line_y1, tool_size );
					f_circle( line_x2, line_y2, tool_size );
					// Draw tool_size thickness line from 1-2
					tline( line_x1, line_y1, line_x2, line_y2, tool_size );
					mem_undo_opacity = oldmode;
				}
				else sline( line_x1, line_y1, line_x2, line_y2 );

				mtMIN( minx, line_x1, line_x2 )
				mtMIN( miny, line_y1, line_y2 )
				minx = minx - tool_size/2;
				miny = miny - tool_size/2;
				xw = abs( line_x2 - line_x1 ) + 1 + tool_size;
				yh = abs( line_y2 - line_y1 ) + 1 + tool_size;

				line_x2 = line_x1;
				line_y2 = line_y1;
				line_status = LINE_START;
			}
			if ( line_status == LINE_NONE ) line_status = LINE_START;
		}
		else stop_line();	// Right button pressed so stop line process
	}

	if ( tool_type == TOOL_SELECT || tool_type == TOOL_POLYGON )
	{
		if ( marq_status == MARQUEE_PASTE )		// User wants to drag the paste box
		{
			if ( x>=marq_x1 && x<=marq_x2 && y>=marq_y1 && y<=marq_y2 )
			{
				marq_status = MARQUEE_PASTE_DRAG;
				marq_drag_x = x - marq_x1;
				marq_drag_y = y - marq_y1;
			}
		}
		if ( marq_status == MARQUEE_PASTE_DRAG && ( button == 1 || button == 13 || button == 2 ) )
		{	// User wants to drag the paste box
			ox = marq_x1;
			oy = marq_y1;
			paint_marquee(0, x - marq_drag_x, y - marq_drag_y);
			marq_x1 = x - marq_drag_x;
			marq_y1 = y - marq_drag_y;
			marq_x2 = marq_x1 + mem_clip_w - 1;
			marq_y2 = marq_y1 + mem_clip_h - 1;
			paint_marquee(1, ox, oy);
		}
		if ( (marq_status == MARQUEE_PASTE_DRAG || marq_status == MARQUEE_PASTE ) &&
			(((button == 3) && (event == GDK_BUTTON_PRESS)) ||
			((button == 13) && (event == GDK_MOTION_NOTIFY))))
		{	// User wants to commit the paste
			commit_paste(TRUE);
		}
		if ( tool_type == TOOL_SELECT && button == 3 && (marq_status == MARQUEE_DONE ) )
		{
			pressed_select_none(NULL, NULL);
			set_cursor();
		}
		if ( tool_type == TOOL_SELECT && button == 1 && (marq_status == MARQUEE_NONE ||
			marq_status == MARQUEE_DONE) )		// Starting a selection
		{
			if ( marq_status == MARQUEE_DONE )
			{
				paint_marquee(0, marq_x1-mem_width, marq_y1-mem_height);
				i = close_to(x, y);
				if ( (i%2) == 0 )
				{	mtMAX(marq_x1, marq_x1, marq_x2)	}
				else
				{	mtMIN(marq_x1, marq_x1, marq_x2)	}
				if ( (i/2) == 0 )
				{	mtMAX(marq_y1, marq_y1, marq_y2)	}
				else
				{	mtMIN(marq_y1, marq_y1, marq_y2)	}
				set_cursor();
			}
			else
			{
				marq_x1 = x;
				marq_y1 = y;
			}
			marq_x2 = x;
			marq_y2 = y;
			marq_status = MARQUEE_SELECTING;
			paint_marquee(1, marq_x1-mem_width, marq_y1-mem_height);
		}
		else
		{
			if ( marq_status == MARQUEE_SELECTING )		// Continuing to make a selection
			{
				paint_marquee(0, marq_x1-mem_width, marq_y1-mem_height);
				marq_x2 = x;
				marq_y2 = y;
				paint_marquee(1, marq_x1-mem_width, marq_y1-mem_height);
			}
		}
	}

	if ( tool_type == TOOL_POLYGON )
	{
		if ( poly_status == POLY_NONE && marq_status == MARQUEE_NONE )
		{
			if (button)		// Start doing something
			{
				if ( button == 1 )
					poly_status = POLY_SELECTING;
				else
					poly_status = POLY_DRAGGING;
			}
		}
		if ( poly_status == POLY_SELECTING )
		{
			if ( button == 1 )
				poly_add_po(x, y);	// Add another point to polygon
			else if ( button == 3 )
				poly_conclude();	// Stop adding points
		}
		if ( poly_status == POLY_DRAGGING )
		{
			if (event == GDK_BUTTON_RELEASE)
				poly_conclude();	// Stop forming polygon
			else poly_add_po(x, y);		// Add another point to polygon
		}
	}

	if ( tool_type != TOOL_SELECT && tool_type != TOOL_POLYGON )
	{
		if ( minx<0 )
		{
			xw = xw + minx;
			minx = 0;
		}

		if ( miny<0 )
		{
			yh = yh + miny;
			miny = 0;
		}
		if ( can_zoom<1 )
		{
			xw = xw + mt_round(1/can_zoom) + 1;
			yh = yh + mt_round(1/can_zoom) + 1;
		}
		if ( (minx+xw) > mem_width ) xw = mem_width - minx;
		if ( (miny+yh) > mem_height ) yh = mem_height - miny;
		if ( tool_type != TOOL_FLOOD && (button == 1 || paint_action) &&
			minx>-1 && miny>-1 && xw>-1 && yh>-1)
		{
			gtk_widget_queue_draw_area( drawing_canvas,
				margin_main_x + minx*can_zoom, margin_main_y + miny*can_zoom,
				xw*can_zoom + 1, yh*can_zoom + 1);
			vw_update_area( minx, miny, xw+1, yh+1 );
		}
	}
	tool_ox = x;	// Remember the coords just used as they are needed in continuous mode
	tool_oy = y;

	if ( tablet_working )
	{
		tool_size = o_size;
		tool_flow = o_flow;
		tool_opacity = o_opac;
	}
}

void check_marquee()		// Check marquee boundaries are OK - may be outside limits via arrow keys
{
	int i;

	if ( marq_status >= MARQUEE_PASTE )
	{
		mtMAX( marq_x1, marq_x1, 1-mem_clip_w )
		mtMAX( marq_y1, marq_y1, 1-mem_clip_h )
		mtMIN( marq_x1, marq_x1, mem_width-1 )
		mtMIN( marq_y1, marq_y1, mem_height-1 )
		marq_x2 = marq_x1 + mem_clip_w - 1;
		marq_y2 = marq_y1 + mem_clip_h - 1;
		return;
	}
	/* Selection mode in operation */
	if ((marq_status != MARQUEE_NONE) || (poly_status == POLY_DONE))
	{
		mtMAX( marq_x1, marq_x1, 0 )
		mtMAX( marq_y1, marq_y1, 0 )
		mtMAX( marq_x2, marq_x2, 0 )
		mtMAX( marq_y2, marq_y2, 0 )
		mtMIN( marq_x1, marq_x1, mem_width-1 )
		mtMIN( marq_y1, marq_y1, mem_height-1 )
		mtMIN( marq_x2, marq_x2, mem_width-1 )
		mtMIN( marq_y2, marq_y2, mem_height-1 )
	}
	if ( tool_type == TOOL_POLYGON && poly_points > 0 )
	{
		for ( i=0; i<poly_points; i++ )
		{
			mtMIN( poly_mem[i][0], poly_mem[i][0], mem_width-1 )
			mtMIN( poly_mem[i][1], poly_mem[i][1], mem_height-1 )
		}
	}
}

int vc_x1, vc_y1, vc_x2, vc_y2;			// Visible canvas
GtkAdjustment *hori, *vert;

void get_visible()
{
	GtkAdjustment *hori, *vert;

	hori = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolledwindow_canvas) );
	vert = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolledwindow_canvas) );

	vc_x1 = hori->value;
	vc_y1 = vert->value;
	vc_x2 = hori->value + hori->page_size - 1;
	vc_y2 = vert->value + vert->page_size - 1;
}

void clip_area( int *rx, int *ry, int *rw, int *rh )		// Clip area to visible canvas
{
	if ( *rx<vc_x1 )
	{
		*rw = *rw + (*rx - vc_x1);
		*rx = vc_x1;
	}
	if ( *ry<vc_y1 )
	{
		*rh = *rh + (*ry - vc_y1);
		*ry = vc_y1;
	}
	if ( *rx + *rw > vc_x2 ) *rw = vc_x2 - *rx + 1;
	if ( *ry + *rh > vc_y2 ) *rh = vc_y2 - *ry + 1;
}

void update_paste_chunk( int x1, int y1, int x2, int y2 )
{
	int ux1, uy1, ux2, uy2;

	get_visible();

	mtMAX( ux1, vc_x1, x1 )
	mtMAX( uy1, vc_y1, y1 )
	mtMIN( ux2, vc_x2, x2 )
	mtMIN( uy2, vc_y2, y2 )

	mtMIN( ux2, ux2, mem_width*can_zoom - 1 )
	mtMIN( uy2, uy2, mem_height*can_zoom - 1 )

	if ( ux1 <= ux2 && uy1 <= uy2 )		// Only repaint if on visible canvas
		repaint_paste( ux1, uy1, ux2, uy2 );
}

void paint_poly_marquee()			// Paint polygon marquee
{
	int i, j, last = poly_points-1, co[2];
	
	GdkPoint xy[MAX_POLY+1];

	check_marquee();

	if ( tool_type == TOOL_POLYGON && poly_points > 1 )
	{
		if ( poly_status == POLY_DONE ) last++;		// Join 1st & last point if finished
		for ( i=0; i<=last; i++ )
		{
			for ( j=0; j<2; j++ )
			{
				co[j] = poly_mem[ i % (poly_points) ][j];
				co[j] = mt_round(co[j] * can_zoom + can_zoom/2);
						// Adjust for zoom
			}
			xy[i].x = margin_main_x + co[0];
			xy[i].y = margin_main_y + co[1];
		}
		gdk_draw_lines( drawing_canvas->window, dash_gc, xy, last+1 );
	}
}

void paint_marquee(int action, int new_x, int new_y)
{
	int x1, y1, x2, y2;
	int x, y, w, h, offx = 0, offy = 0;
	int rx, ry, rw, rh, canz = can_zoom, zerror = 0;
	int i, j, new_x2 = new_x + (marq_x2-marq_x1), new_y2 = new_y + (marq_y2-marq_y1);
	char *rgb;

	if ( canz<1 )
	{
		canz = 1;
		zerror = 2;
	}

	check_marquee();
	x1 = marq_x1*can_zoom; y1 = marq_y1*can_zoom;
	x2 = marq_x2*can_zoom; y2 = marq_y2*can_zoom;

	mtMIN( x, x1, x2 )
	mtMIN( y, y1, y2 )
	w = x1 - x2;
	h = y1 - y2;

	if ( w < 0 ) w = -w;
	if ( h < 0 ) h = -h;

	w = w + canz;
	h = h + canz;

	get_visible();

	if ( action == 0 )		// Clear marquee
	{
		j = marq_status;
		marq_status = 0;
		if ( j >= MARQUEE_PASTE && show_paste )
		{
			if ( new_x != marq_x1 || new_y != marq_y1 )
			{	// Only do something if there is a change
				if (	new_x2 < marq_x1 || new_x > marq_x2 ||
					new_y2 < marq_y1 || new_y > marq_y2	)
						repaint_canvas( margin_main_x + x, margin_main_y + y,
							w, h );	// Remove completely
				else
				{
					if ( new_x != marq_x1 )
					{	// Horizontal shift
						if ( new_x < marq_x1 )	// LEFT
						{
							ry = y; rh = h + zerror;
							rx = (new_x2 + 1) * can_zoom;
							rw = (marq_x2 - new_x2) * can_zoom + zerror;
						}
						else			// RIGHT
						{
							ry = y; rx = x; rh = h + zerror;
							rw = (new_x - marq_x1) * can_zoom + zerror;
						}
						clip_area( &rx, &ry, &rw, &rh );
						repaint_canvas( margin_main_x + rx, margin_main_y + ry,
							rw, rh );
					}
					if ( new_y != marq_y1 )
					{	// Vertical shift
						if ( new_y < marq_y1 )	// UP
						{
							rx = x; rw = w + zerror;
							ry = (new_y2 + 1) * can_zoom;
							rh = (marq_y2 - new_y2) * can_zoom + zerror;
						}
						else			// DOWN
						{
							rx = x; ry = y; rw = w + zerror;
							rh = (new_y - marq_y1) * can_zoom + zerror;
						}
						clip_area( &rx, &ry, &rw, &rh );
						repaint_canvas( margin_main_x + rx, margin_main_y + ry,
							rw, rh );
					}
				}
			}
		}
		else
		{
			repaint_canvas( margin_main_x + x, margin_main_y + y, 1, h );
			repaint_canvas(	margin_main_x + x+w-1-zerror/2, margin_main_y + y, 1+zerror, h );
			repaint_canvas(	margin_main_x + x, margin_main_y + y, w, 1 );
			repaint_canvas(	margin_main_x + x, margin_main_y + y+h-1-zerror/2, w, 1+zerror );
				// zerror required here to stop artifacts being left behind while dragging
				// a selection at the right/bottom edges
		}
		marq_status = j;
	}
	if ( action == 1 || action == 11 )		// Draw marquee
	{
		mtMAX( j, w, h )
		rgb = grab_memory( j*3, 255 );

		if ( marq_status >= MARQUEE_PASTE )
		{
			if ( action == 1 && show_paste )
			{	// Display paste RGB, only if not being called from repaint_canvas
				if ( new_x != marq_x1 || new_y != marq_y1 )
				{	// Only do something if there is a change in position
					update_paste_chunk( x1+1, y1+1,
						x2 + canz-2, y2 + canz-2 );
				}
			}
			for ( i=0; i<j; i++ )
			{
				rgb[ 0 + 3*i ] = 255 * ((i/3) % 2);
				rgb[ 1 + 3*i ] = 255 * ((i/3) % 2);
				rgb[ 2 + 3*i ] = 255;
			}
		}
		else
		{
			for ( i=0; i<j; i++ )
			{
				rgb[ 0 + 3*i ] = 255;
				rgb[ 1 + 3*i ] = 255 * ((i/3) % 2);
				rgb[ 2 + 3*i ] = 255 * ((i/3) % 2);
			}
		}

		rx = x; ry = y; rw = w; rh = h;
		clip_area( &rx, &ry, &rw, &rh );

		if ( rx != x ) offx = 3*( abs(rx - x) );
		if ( ry != y ) offy = 3*( abs(ry - y) );

		if ( (rx + rw) >= mem_width*can_zoom ) rw = mem_width*can_zoom - rx;
		if ( (ry + rh) >= mem_height*can_zoom ) rh = mem_height*can_zoom - ry;

		if ( x >= vc_x1 )
		{
			gdk_draw_rgb_image (drawing_canvas->window, drawing_canvas->style->black_gc,
				margin_main_x + rx, margin_main_y + ry,
				1, rh, GDK_RGB_DITHER_NONE, rgb + offy, 3 );
		}

		if ( (x+w-1) <= vc_x2 && (x+w-1) < mem_width*can_zoom )
		{
			gdk_draw_rgb_image (drawing_canvas->window, drawing_canvas->style->black_gc,
				margin_main_x + rx+rw-1, margin_main_y + ry,
				1, rh, GDK_RGB_DITHER_NONE, rgb + offy, 3 );
		}

		if ( y >= vc_y1 )
		{
			gdk_draw_rgb_image (drawing_canvas->window, drawing_canvas->style->black_gc,
				margin_main_x + rx, margin_main_y + ry,
				rw, 1, GDK_RGB_DITHER_NONE, rgb + offx, 3*j );
		}

		if ( (y+h-1) <= vc_y2 && (y+h-1) < mem_height*can_zoom )
		{
			gdk_draw_rgb_image (drawing_canvas->window, drawing_canvas->style->black_gc,
				margin_main_x + rx, margin_main_y + ry+rh-1,
				rw, 1, GDK_RGB_DITHER_NONE, rgb + offx, 3*j );
		}

		free(rgb);
	}
}

int close_to( int x1, int y1 )		// Which corner of selection is coordinate closest to?
{
	return ((x1 + x1 <= marq_x1 + marq_x2 ? 0 : 1) +
		(y1 + y1 <= marq_y1 + marq_y2 ? 0 : 2));
}


void repaint_line(int mode)			// Repaint or clear line on canvas
{
	png_color pcol;
	int i, j, pixy = 1, xdo, ydo, px, py, todo, todor;
	int minx, miny, xw, yh, canz = can_zoom, canz2 = 1;
	int lx1, ly1, lx2, ly2,
		ax=-1, ay=-1, bx, by, aw, ah;
	float rat;
	char *rgb;
	gboolean do_redraw = FALSE;

	if ( canz<1 )
	{
		canz = 1;
		canz2 = mt_round(1/can_zoom);
	}
	pixy = canz*canz;
	lx1 = line_x1;
	ly1 = line_y1;
	lx2 = line_x2;
	ly2 = line_y2;

	xdo = abs(lx2 - lx1);
	ydo = abs(ly2 - ly1);
	mtMAX( todo, xdo, ydo )

	mtMIN( minx, lx1, lx2 )
	mtMIN( miny, ly1, ly2 )
	minx = minx * canz;
	miny = miny * canz;
	xw = (xdo + 1)*canz;
	yh = (ydo + 1)*canz;
	get_visible();
	clip_area( &minx, &miny, &xw, &yh );

	mtMIN( lx1, lx1 / canz2, mem_width / canz2 - 1 )
	mtMIN( ly1, ly1 / canz2, mem_height / canz2 - 1 )
	mtMIN( lx2, lx2 / canz2, mem_width / canz2 - 1 )
	mtMIN( ly2, ly2 / canz2, mem_height / canz2 - 1 )
	todo = todo / canz2;

	if ( todo == 0 ) todor = 1; else todor = todo;
	rgb = grab_memory( pixy*3, 255 );

	for ( i=0; i<=todo; i++ )
	{
		rat = ((float) i ) / todor;
		px = mt_round(lx1 + (lx2 - lx1) * rat);
		py = mt_round(ly1 + (ly2 - ly1) * rat);

		if ( (px+1)*canz > vc_x1 && (py+1)*canz > vc_y1 &&
			px*canz <= vc_x2 && py*canz <= vc_y2 )
		{
			if ( mode == 2 )
			{
				pcol.red   = 255*( (todo-i)/4 % 2 );
				pcol.green = pcol.red;
				pcol.blue  = pcol.red;
			}
			if ( mode == 1 )
			{
				pcol.red   = mem_col_pat24[     3*((px % 8) + 8*(py % 8)) ];
				pcol.green = mem_col_pat24[ 1 + 3*((px % 8) + 8*(py % 8)) ];
				pcol.blue  = mem_col_pat24[ 2 + 3*((px % 8) + 8*(py % 8)) ];
			}

			if ( mode == 0 )
			{
				if ( ax<0 )	// 1st corner of repaint rectangle
				{
					ax = px;
					ay = py;
				}
				do_redraw = TRUE;
			}
			else
			{
				for ( j=0; j<pixy; j++ )
				{
					rgb[ 3*j ] = pcol.red;
					rgb[ 1 + 3*j ] = pcol.green;
					rgb[ 2 + 3*j ] = pcol.blue;
				}
				gdk_draw_rgb_image (drawing_canvas->window, drawing_canvas->style->black_gc,
					margin_main_x + px*canz, margin_main_y + py*canz,
					canz, canz,
					GDK_RGB_DITHER_NONE, rgb, 3*canz );
			}
		}
		else
		{
			if ( ax>=0 && mode==0 ) do_redraw = TRUE;
		}
		if ( do_redraw )
		{
			do_redraw = FALSE;
			bx = px;	// End corner
			by = py;
			aw = canz * (1 + abs(bx-ax));	// Width of rectangle on canvas
			ah = canz * (1 + abs(by-ay));
			if ( aw>16 || ah>16 || i==todo )
			{ // Commit canvas clear if >16 pixels or final pixel of this line
				mtMIN( ax, ax, bx )
				mtMIN( ay, ay, by )
				repaint_canvas( margin_main_x + ax*canz, margin_main_y + ay*canz,
					aw, ah );
				ax = -1;
			}
		}
	}
	free(rgb);
}

void men_item_visible( GtkWidget *menu_items[], gboolean state )
{	// Show or hide menu items
	int i = 0;
	while ( menu_items[i] != NULL )
	{
		if ( state )
			gtk_widget_show( menu_items[i] );
		else
			gtk_widget_hide( menu_items[i] );
		i++;
	}
}

void update_recent_files()			// Update the menu items
{
	char txt[64], *t, txt2[600];
	int i, count;

	if ( recent_files == 0 ) men_item_visible( menu_recent, FALSE );
	else
	{
		for ( i=0; i<=MAX_RECENT; i++ )			// Show or hide items
		{
			if ( i <= recent_files )
				gtk_widget_show( menu_recent[i] );
			else
				gtk_widget_hide( menu_recent[i] );
		}
		count = 0;
		for ( i=1; i<=recent_files; i++ )		// Display recent filenames
		{
			sprintf( txt, "file%i", i );

			t = inifile_get( txt, "." );
			if ( strlen(t) < 2 )
				gtk_widget_hide( menu_recent[i] );	// Hide if empty
			else
			{
#if GTK_MAJOR_VERSION == 2
				cleanse_txt( txt2, t );		// Clean up non ASCII chars
#else
				strcpy( txt2, t );
#endif
				gtk_label_set_text( GTK_LABEL( GTK_MENU_ITEM(
					menu_recent[i] )->item.bin.child ) , txt2 );
				count++;
			}
		}
		if ( count == 0 ) gtk_widget_hide( menu_recent[0] );	// Hide separator if not needed
	}
}

void register_file( char *filename )		// Called after successful load/save
{
	char txt[280], *c;
	int i, f;

	c = strrchr( filename, DIR_SEP );
	if (c != NULL)
	{
		txt[0] = c[1];
		c[1] = 0;
	}
	inifile_set("last_dir", filename);		// Strip off filename
	if (c != NULL) c[1] = txt[0];

	// Is it already in used file list?  If so shift relevant filenames down and put at top.
	i = 1;
	f = 0;
	while ( i<MAX_RECENT && f==0 )
	{
		sprintf( txt, "file%i", i );
		c = inifile_get( txt, "." );
		if ( strcmp( filename, c ) == 0 ) f = 1;	// Filename found in list
		else i++;
	}
	if ( i>1 )			// If file is already most recent, do nothing
	{
		while ( i>1 )
		{
			sprintf( txt, "file%i", i-1 );
			sprintf( txt+100, "file%i", i );
			inifile_set( txt+100,
				inifile_get( txt, "" )
				);

			i--;
		}
		inifile_set("file1", filename);		// Strip off filename
	}

	update_recent_files();
}

void scroll_wheel( int x, int y, int d )		// Scroll wheel action from mouse
{
	if (d == 1) zoom_in();
	else zoom_out();
}

void create_default_image()			// Create default new image
{
	int	nw = inifile_get_gint32("lastnewWidth", DEFAULT_WIDTH ),
		nh = inifile_get_gint32("lastnewHeight", DEFAULT_HEIGHT ),
		nc = inifile_get_gint32("lastnewCols", 256 ),
		nt = inifile_get_gint32("lastnewType", 2 ),
		bpp = 1;

	if ( nt == 0 || nt>2 ) bpp = 3;
	do_new_one( nw, nh, nc, nt, bpp );
}
