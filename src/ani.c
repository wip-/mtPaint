/*	ani.c
	Copyright (C) 2005-2013 Mark Tyler and Dmitry Groshev

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

#include <stdio.h>
#include <sys/types.h>
#include <errno.h>

#include "global.h"

#include "mygtk.h"
#include "memory.h"
#include "ani.h"
#include "png.h"
#include "mainwindow.h"
#include "otherwindow.h"
#include "canvas.h"
#include "viewer.h"
#include "layer.h"
#include "spawn.h"
#include "inifile.h"
#include "mtlib.h"
#include "wu.h"

///	GLOBALS

int	ani_frame1 = 1, ani_frame2 = 1, ani_gif_delay = 10, ani_play_state = FALSE,
	ani_timer_state = 0;



///	FORM VARIABLES

static GtkWidget *animate_window = NULL, *ani_prev_win,
	*ani_entry_path, *ani_entry_prefix,
	*ani_spin[5],			// start, end, delay
	*ani_text_pos, *ani_text_cyc,	// Text input widgets
	*ani_prev_slider		// Slider widget on preview area
	;

ani_cycle ani_cycle_table[MAX_CYC_SLOTS];

static int
	ani_layer_data[MAX_LAYERS + 1][4],	// x, y, opacity, visible
	ani_currently_selected_layer;
static char ani_output_path[PATHBUF], ani_file_prefix[ANI_PREFIX_LEN+2];
static gboolean ani_use_gif, ani_show_main_state;



static void ani_win_read_widgets();




static void ani_widget_changed()	// Widget changed so flag the layers as changed
{
	layers_changed = 1;
}



static void set_layer_from_slot( int layer, int slot )		// Set layer x, y, opacity from slot
{
	ani_slot *ani = layer_table[layer].image->ani_.pos + slot;
	layer_table[layer].x = ani->x;
	layer_table[layer].y = ani->y;
	layer_table[layer].opacity = ani->opacity;
}

static void set_layer_inbetween( int layer, int i, int frame, int effect )		// Calculate in between value for layer from slot i & (i+1) at given frame
{
	MT_Coor c[4], co_res, lenz;
	float p1, p2;
	int f0, f1, f2, f3, ii[4] = {i-1, i, i+1, i+2}, j;
	ani_slot *ani = layer_table[layer].image->ani_.pos;


	f1 = ani[i].frame;
	f2 = ani[i + 1].frame;

	if (i > 0) f0 = ani[i - 1].frame;
	else
	{
		f0 = f1;
		ii[0] = ii[1];
	}

	if ((i >= MAX_POS_SLOTS - 2) || !(f3 = ani[i + 2].frame))
	{
		f3 = f2;
		ii[3] = ii[2];
	}

		// Linear in between
	p1 = ( (float) (f2-frame) ) / ( (float) (f2-f1) );	// % of (i-1) slot
	p2 = 1-p1;						// % of i slot

	layer_table[layer].x = rint(p1 * ani[i].x + p2 * ani[i + 1].x);
	layer_table[layer].y = rint(p1 * ani[i].y + p2 * ani[i + 1].y);
	layer_table[layer].opacity = rint(p1 * ani[i].opacity +
		p2 * ani[i + 1].opacity);


	if ( effect == 1 )		// Interpolated smooth in between - use p2 value
	{
		for ( i=0; i<4; i++ ) c[i].z = 0;	// Unused plane
		lenz.x = f1 - f0;
		lenz.y = f2 - f1;			// Frames for each line
		lenz.z = f3 - f2;

		if ( lenz.x<1 ) lenz.x = 1;
		if ( lenz.y<1 ) lenz.y = 1;
		if ( lenz.z<1 ) lenz.z = 1;

		// Set up coords
		for ( j=0; j<4; j++ )
		{
			c[j].x = ani[ii[j]].x;
			c[j].y = ani[ii[j]].y;
		}
		co_res = MT_palin(p2, 0.35, c[0], c[1], c[2], c[3], lenz);

		layer_table[layer].x = co_res.x;
		layer_table[layer].y = co_res.y;
	}
}

static void ani_set_frame_state(int frame)
{
	int i, j, k, v, c, f, p;
	ani_slot *ani;
	ani_cycle *cc;
	unsigned char *cp;

// !!! Maybe make background x/y settable here too?
	for (k = 1; k <= layers_total; k++)
	{
		/* Set x, y, opacity for layer */
		ani = layer_table[k].image->ani_.pos;

		/* Find first frame in position list that excedes or equals 'frame' */
		for (i = 0; i < MAX_POS_SLOTS; i++)
		{
			/* End of list */
			if (ani[i].frame <= 0) break;
			/* Exact match or one exceding it found */
			if (ani[i].frame >= frame) break;
		}

		/* If no slots have been defined
		 * leave the layer x, y, opacity as now */
		if (ani[0].frame <= 0);
		/* All position slots < 'frame'
		 * Set layer pos/opac to last slot values */
		else if ((i >= MAX_POS_SLOTS) || !ani[i].frame)
			set_layer_from_slot(k, i - 1);
		/* If closest frame = requested frame, set all values to this
		 * ditto if i=0, i.e. no better matches exist */
		else if ((ani[i].frame == frame) || !i)
			set_layer_from_slot(k, i);
		/* i is currently pointing to slot that excedes 'frame',
		 * so in between this and the previous slot */
		else set_layer_inbetween(k, i - 1, frame, ani[i - 1].effect);

		/* Set visibility for layer by processing cycle table */
		/* !!! Table must be sorted by cycle # */
		v = -1; // Leave alone by default
		cp = layer_table[k].image->ani_.cycles;
		for (i = c = 0; i < MAX_CYC_ITEMS; i++ , c = j)
		{
			if (!(j = *cp++)) break; // Cycle # + 1
			p = *cp++;
			cc = ani_cycle_table + j - 1;
			if (!(f = cc->frame0)) continue; // Paranoia
			if (f > frame) continue; // Not yet active
			/* Special case for enabling/disabling en-masse */
			if (cc->frame1 == f)
			{
				if (j != c) v = !p; // Ignore entries after 1st
			}
			/* Inside a normal cycle */
			else if (cc->frame1 >= frame)
			{
				if (j != c) v = 0; // Hide initially
				v |= (frame - f) % cc->len == p; // Show if matched
			}
		}
		if (v >= 0) layer_table[k].visible = v;
	}
}

#define ANI_CYC_ROWLEN (MAX_CYC_ITEMS * 2 + 1)
#define ANI_CYC_TEXT_MAX (128 + MAX_CYC_ITEMS * 10)

/* Convert cycle header & layers list to text */
static void ani_cyc_sprintf(char *txt, ani_cycle *chead, unsigned char *cdata)
{
	int j, l, b;
	char *tmp = txt + sprintf(txt, "%i\t%i\t", chead->frame0, chead->frame1);

	l = *cdata++;
	if (!l); // Empty group
	else if (chead->frame0 == chead->frame1) // Batch toggle group
	{
		while (TRUE)
		{
			tmp += sprintf(tmp, "%s%i", cdata[0] ? "-" : "", cdata[1]);
			if (--l <= 0) break;
			*tmp++ = ',';
			cdata += 2;
		}
	}
	else // Regular cycle
	{
		b = -1;
		while (TRUE)
		{
			j = cdata[0] - b;
			while (--j > 0) *tmp++ = '0' , *tmp++ = ',';
			b = cdata[0];
			if (--l <= 0) break;
			if ((cdata[2] == b) && (j >= 0)) *tmp++ = '(';
			tmp += sprintf(tmp, "%i%s,", cdata[1],
				(cdata[2] != b) && (j < 0) ? ")" : "");
			cdata += 2;
		}
		tmp += sprintf(tmp, "%i%s", cdata[1], j < 0 ? ")" : "");
		j = chead->len - cdata[0];
		while (--j > 0) *tmp++ = ',' , *tmp++ = '0';
	}
	strcpy(tmp, "\n");
}

/* Parse text into cycle header & layers list */
static int ani_cyc_sscanf(char *txt, ani_cycle *chead, unsigned char *cdata)
{
	char *tail;
	unsigned char *cntp;
	int i, j, l, f, f0, f1, b;

	while (*txt && (*txt < 32)) txt++;	// Skip non ascii chars
	if (!*txt) return (FALSE);
	f0 = f1 = -1; i = 0;	// Default state if invalid
	l = 0; sscanf(txt, "%i\t%i\t%n", &f0, &f1, &l);
	chead->frame0 = f0;
	chead->frame1 = f1;
	chead->len = *cdata = 0;
	if (!l) return (TRUE); // Invalid cycle is a cycle still
	txt += l;

	cntp = cdata++;
	f = b = 0;
	while (cdata - cntp < ANI_CYC_ROWLEN)
	{
		while (*txt && (*txt <= 32)) txt++;	// Skip whitespace etc
		if (*txt == '(')
		{
			b = 1; txt++;
			while (*txt && (*txt <= 32)) txt++;	// Skip whitespace etc
		}
		j = strtol(txt, &tail, 0);
		if (tail - txt <= 0) break; // No number there
		if (f0 == f1) if ((f = j < 0)) j = -j; // Batch toggle group
		if ((j < 0) || (j > MAX_LAYERS)) j = 0; // Out of range
		*cdata++ = f; // Position
		*cdata++ = j; // Layer
		cntp[0]++;
		txt = tail;
		while (*txt && (*txt <= 32)) txt++;	// Skip whitespace etc
		if (*txt == ')')
		{
			b = 0; txt++;
			while (*txt && (*txt <= 32)) txt++;	// Skip whitespace etc
		}
		if (*txt++ != ',') break;	// Stop if no comma
		f += 1 - b;
	}
	if (*cntp) chead->len = *(cdata - 2) + 1;

	return (TRUE);
}

static int cmp_frames(const void *f1, const void *f2)
{
	int n = (int)((unsigned char *)f1)[0] - ((unsigned char *)f2)[0];
	return (n ? n :
		(int)((unsigned char *)f1)[1] - ((unsigned char *)f2)[1]);
}

/* Assemble cycles info from per-layer lists into per-cycle ones */
static void ani_cyc_get(unsigned char *buf)
{
	unsigned char *ptr, *cycles;
	int i, j, k, l;

	memset(buf, 0, MAX_CYC_SLOTS * ANI_CYC_ROWLEN); // Clear
	buf -= ANI_CYC_ROWLEN; // 1-based
	for (i = 1; i <= layers_total; i++)
	{
		cycles = layer_table[i].image->ani_.cycles;
		for (j = 0; j < MAX_CYC_ITEMS; j++)
		{
			if (!(k = *cycles++)) break; // Cycle # + 1
			l = *cycles++; // Position
			if ((k > MAX_CYC_SLOTS) || (l >= MAX_CYC_ITEMS))
				continue;
			ptr = buf + ANI_CYC_ROWLEN * k;
			if (ptr[0] >= MAX_CYC_ITEMS) continue;
			k = ptr[0]++;
			ptr += k * 2 + 1;
			ptr[0] = l; // Position
			ptr[1] = i; // Layer
		}
	}
	for (i = 1; i <= MAX_CYC_SLOTS; i++) // Sort by position & layer
	{
		ptr = buf + ANI_CYC_ROWLEN * i;
		if (*ptr > 1) qsort(ptr + 1, *ptr, 2, cmp_frames);
	}
}

/* Distribute out cycles info from per-cycle to per-layer lists */
static void ani_cyc_put(unsigned char *buf)
{
	unsigned char *ptr, *cycles;
	int i, j, k, l, cnt[MAX_LAYERS + 1];

	memset(cnt, 0, sizeof(cnt)); // Clear
	buf -= ANI_CYC_ROWLEN; // 1-based
	for (i = 1; i <= MAX_CYC_SLOTS; i++)
	{
		ptr = buf + ANI_CYC_ROWLEN * i;
		j = *ptr++;
		while (j-- > 0)
		{
			l = *ptr++; // Position
			k = *ptr++; // Layer
			if (!k || (k > layers_total)) continue;
			if (cnt[k] >= MAX_CYC_ITEMS) continue;
			cycles = layer_table[k].image->ani_.cycles + cnt[k]++ * 2;
			cycles[0] = i; // Cycle # + 1
			cycles[1] = l; // Position
		}
	}
	for (i = 0; i <= layers_total; i++) // Mark end of list
		if (cnt[i] < MAX_CYC_ITEMS)
			layer_table[i].image->ani_.cycles[cnt[i] * 2] = 0;
}

#define ANI_POS_TEXT_MAX 256

static void ani_pos_sprintf(char *txt, ani_slot *ani)
{
	sprintf(txt, "%i\t%i\t%i\t%i\t%i\n",
		ani->frame, ani->x, ani->y, ani->opacity, ani->effect);
}

static int ani_pos_sscanf(char *txt, ani_slot *ani)
{
	ani_slot data = { -1, -1, -1, -1, -1 };

	while (*txt && (*txt < 32)) txt++;	// Skip non ascii chars
	if (!*txt) return (FALSE);
	// !!! Ignoring parse errors
	sscanf(txt, "%i\t%i\t%i\t%i\t%i", &data.frame, &data.x, &data.y,
		&data.opacity, &data.effect);
	*ani = data;
	return (TRUE);
}




static void ani_read_layer_data()		// Read current layer x/y/opacity data to table
{
	int i;

	for ( i=0; i<=MAX_LAYERS; i++ )
	{
		ani_layer_data[i][0] = layer_table[i].x;
		ani_layer_data[i][1] = layer_table[i].y;
		ani_layer_data[i][2] = layer_table[i].opacity;
		ani_layer_data[i][3] = layer_table[i].visible;
	}
}

static void ani_write_layer_data()		// Write current layer x/y/opacity data from table
{
	int i;

	for ( i=0; i<=MAX_LAYERS; i++ )
	{
		layer_table[i].x       = ani_layer_data[i][0];
		layer_table[i].y       = ani_layer_data[i][1];
		layer_table[i].opacity = ani_layer_data[i][2];
		layer_table[i].visible = ani_layer_data[i][3];
	}
}


static char *text_edit_widget_get(GtkWidget *w)		// Get text string from input widget
		// WARNING memory allocated for this so lose it with g_free(txt)
{
#if GTK_MAJOR_VERSION == 1
	return gtk_editable_get_chars( GTK_EDITABLE(w), 0, -1 );
#endif
#if GTK_MAJOR_VERSION == 2
	GtkTextIter begin, end;
	GtkTextBuffer *buffer = GTK_TEXT_VIEW(w)->buffer;

	gtk_text_buffer_get_start_iter( buffer, &begin );
	gtk_text_buffer_get_end_iter( buffer, &end );
	return gtk_text_buffer_get_text( buffer, &begin, &end, -1 );
#endif
}

static void empty_text_widget(GtkWidget *w)	// Empty the text widget
{
#if GTK_MAJOR_VERSION == 1
	gtk_text_set_point( GTK_TEXT(w), 0 );
	gtk_text_forward_delete( GTK_TEXT(w), gtk_text_get_length(GTK_TEXT(w)) );
#endif
#if GTK_MAJOR_VERSION == 2
	gtk_text_buffer_set_text( GTK_TEXT_VIEW(w)->buffer, "", 0 );
#endif
}

static void ani_cyc_refresh_txt()		// Refresh the text in the cycle text widget
{
	char txt[ANI_CYC_TEXT_MAX];
	unsigned char buf[MAX_CYC_SLOTS * ANI_CYC_ROWLEN];
	int i;
#if GTK_MAJOR_VERSION == 2
	GtkTextIter iter;

	g_signal_handlers_block_by_func(GTK_TEXT_VIEW(ani_text_cyc)->buffer,
		GTK_SIGNAL_FUNC(ani_widget_changed), NULL);
#endif
	empty_text_widget(ani_text_cyc);	// Clear the text in the widget

	ani_cyc_get(buf);
	for (i = 0; i < MAX_CYC_SLOTS; i++)
	{
		if (!ani_cycle_table[i].frame0) break;
		ani_cyc_sprintf(txt, ani_cycle_table + i,
			buf + ANI_CYC_ROWLEN * i);
#if GTK_MAJOR_VERSION == 1
		gtk_text_insert (GTK_TEXT (ani_text_cyc), NULL, NULL, NULL, txt, -1);
#endif
#if GTK_MAJOR_VERSION == 2
		gtk_text_buffer_get_end_iter( GTK_TEXT_VIEW(ani_text_cyc)->buffer, &iter );
		gtk_text_buffer_insert( GTK_TEXT_VIEW(ani_text_cyc)->buffer, &iter, txt, -1 );
#endif
	}

#if GTK_MAJOR_VERSION == 2
	g_signal_handlers_unblock_by_func(GTK_TEXT_VIEW(ani_text_cyc)->buffer,
		GTK_SIGNAL_FUNC(ani_widget_changed), NULL);
	// We have to switch off then back on or it looks like the user changed it
#endif
}

static void ani_pos_refresh_txt()		// Refresh the text in the position text widget
{
	char txt[ANI_POS_TEXT_MAX];
	int i = ani_currently_selected_layer, j;
	ani_slot *ani;
#if GTK_MAJOR_VERSION == 2
	GtkTextIter iter;

	g_signal_handlers_block_by_func(GTK_TEXT_VIEW(ani_text_pos)->buffer,
		GTK_SIGNAL_FUNC(ani_widget_changed), NULL);
#endif

	empty_text_widget(ani_text_pos);	// Clear the text in the widget

	if ( i > 0 )		// Must no be for background layer or negative => PANIC!
	{
		for (j = 0; j < MAX_POS_SLOTS; j++)
		{
			ani = layer_table[i].image->ani_.pos + j;
			if (ani->frame <= 0) break;
			// Add a line if one exists
			ani_pos_sprintf(txt, ani);
#if GTK_MAJOR_VERSION == 1
			gtk_text_insert (GTK_TEXT (ani_text_pos), NULL, NULL, NULL, txt, -1);
#endif
#if GTK_MAJOR_VERSION == 2
			gtk_text_buffer_get_end_iter( GTK_TEXT_VIEW(ani_text_pos)->buffer, &iter );
			gtk_text_buffer_insert( GTK_TEXT_VIEW(ani_text_pos)->buffer, &iter, txt,
				strlen(txt) );

#endif
		}
	}
#if GTK_MAJOR_VERSION == 2
	g_signal_handlers_unblock_by_func(GTK_TEXT_VIEW(ani_text_pos)->buffer,
		GTK_SIGNAL_FUNC(ani_widget_changed), NULL);
	// We have to switch off then back on or it looks like the user changed it
#endif
}

void ani_init()			// Initialize variables/arrays etc. before loading or on startup
{
	int j;

	ani_frame1 = 1;
	ani_frame2 = 100;
	ani_gif_delay = 10;

	for (j = 0; j <= layers_total; j++)
		memset(&layer_table[j].image->ani_, 0, sizeof(ani_info));
	memset(ani_cycle_table, 0, sizeof(ani_cycle_table));

	strcpy(ani_output_path, "frames");
	strcpy(ani_file_prefix, "f");

	ani_use_gif = TRUE;
}



///	EXPORT ANIMATION FRAMES WINDOW


static void ani_win_set_pos()
{
	win_restore_pos(animate_window, "ani", 0, 0, 200, 200);
}

static void ani_fix_pos()
{
	ani_read_layer_data();
	layers_notify_changed();
}

static void ani_but_save()
{
	ani_win_read_widgets();
	ani_write_layer_data();
	layer_press_save();
}

static gboolean delete_ani()
{
	win_store_pos(animate_window, "ani");
	ani_win_read_widgets();
	gtk_widget_destroy(animate_window);
	animate_window = NULL;
	ani_write_layer_data();
	layers_pastry_cut = FALSE;

	show_layers_main = ani_show_main_state;
	update_stuff(UPD_ALLV);
	return (FALSE);
}


static void ani_parse_store_positions()		// Read current positions in text input and store
{
	char *txt, *tx, *tmp;
	ani_slot *ani = layer_table[ani_currently_selected_layer].image->ani_.pos;
	int i;

	tmp = tx = text_edit_widget_get(ani_text_pos);
	for (i = 0; i < MAX_POS_SLOTS; i++)
	{
		if (!(txt = tmp)) break;
		tmp = strchr(txt, '\n');
		if (tmp) *tmp++ = '\0';
		if (!ani_pos_sscanf(txt, ani + i)) break;
	}
	if (i < MAX_POS_SLOTS) ani[i].frame = 0;	// End delimeter

	g_free(tx);
}

static void ani_parse_store_cycles()		// Read current cycles in text input and store
{
	unsigned char buf[MAX_CYC_SLOTS * ANI_CYC_ROWLEN];
	char *txt, *tx, *tmp;
	int i;

	tmp = tx = text_edit_widget_get(ani_text_cyc);
	memset(buf, 0, sizeof(buf));
	for (i = 0; i < MAX_CYC_SLOTS; i++)
	{
		if (!(txt = tmp)) break;
		tmp = strchr(txt, '\n');
		if (tmp) *tmp++ = '\0';
		if (!ani_cyc_sscanf(txt, ani_cycle_table + i,
			buf + ANI_CYC_ROWLEN * i)) break;
	}
	if (i < MAX_CYC_SLOTS) ani_cycle_table[i].frame0 = 0;	// End delimeter
	ani_cyc_put(buf);

	g_free(tx);
}

static void ani_win_read_widgets()		// Read all widgets and set up relevant variables
{
	int	a = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(ani_spin[0]) ),
		b = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(ani_spin[1]) );


	ani_gif_delay = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(ani_spin[2]) );

	ani_parse_store_positions();
	ani_parse_store_cycles();
	ani_pos_refresh_txt();		// Update 2 text widgets
	ani_cyc_refresh_txt();

	ani_frame1 = a < b ? a : b;
	ani_frame2 = a < b ? b : a;
	gtkncpy(ani_output_path, gtk_entry_get_text(GTK_ENTRY(ani_entry_path)), PATHBUF);
	gtkncpy(ani_file_prefix, gtk_entry_get_text(GTK_ENTRY(ani_entry_prefix)), ANI_PREFIX_LEN + 1);
	// GIF toggle is automatically set by callback
}


static gboolean ani_play_timer_call()
{
	int i;

	if ( ani_play_state == 0 )
	{
		ani_timer_state = 0;
		return FALSE;			// Stop animating
	}
	else
	{
		i = ADJ2INT(SPINSLIDE_ADJUSTMENT(ani_prev_slider)) + 1;
		if (i > ani_frame2) i = ani_frame1;
		mt_spinslide_set_value(ani_prev_slider, i);
		return TRUE;
	}
}

static void ani_play_start()
{
	if (!ani_play_state)
	{
		ani_play_state = 1;
		if (!ani_timer_state) ani_timer_state = threads_timeout_add(
			ani_gif_delay * 10, ani_play_timer_call, NULL);
	}
}

static void ani_play_stop()
{
	ani_play_state = 0;
}


///	PREVIEW WINDOW CALLBACKS

static void ani_but_playstop(GtkToggleButton *togglebutton, gpointer user_data)
{
	if (gtk_toggle_button_get_active(togglebutton)) ani_play_start();
	else ani_play_stop();
}

static void ani_frame_slider_moved(GtkAdjustment *adjustment, gpointer user_data)
{
	int x = 0, y = 0, w = mem_width, h = mem_height;

	ani_set_frame_state(ADJ2INT(adjustment));

	if (layer_selected)
	{
		x = layer_table[0].x - layer_table[layer_selected].x;
		y = layer_table[0].y - layer_table[layer_selected].y;
		w = layer_table[0].image->image_.width;
		h = layer_table[0].image->image_.height;
	}

	vw_update_area(x, y, w, h);	// Update only the area we need
}

static gboolean ani_but_preview_close()
{
	ani_play_stop();				// Stop animation playing if necessary

	win_store_pos(ani_prev_win, "ani_prev");
	gtk_widget_destroy( ani_prev_win );

	if ( animate_window != NULL )
	{
		ani_win_set_pos();
		gtk_widget_show (animate_window);
	}
	else
	{
		ani_write_layer_data();
		layers_pastry_cut = FALSE;
		update_stuff(UPD_ALLV);
	}
	return (FALSE);
}

void ani_but_preview()
{
	GtkWidget *hbox3, *button;
	GtkAccelGroup* ag = gtk_accel_group_new();


	if ( animate_window != NULL )
	{
		/* We need to remember this as we are hiding it */
		win_store_pos(animate_window, "ani");
		ani_win_read_widgets();		// Get latest values for the preview
	}
	else	ani_read_layer_data();

	if ( !view_showing ) view_show();	// If not showing, show the view window

	ani_prev_win = add_a_window( GTK_WINDOW_TOPLEVEL,
			_("Animation Preview"), GTK_WIN_POS_NONE, TRUE );
	gtk_container_set_border_width(GTK_CONTAINER(ani_prev_win), 5);

	win_restore_pos(ani_prev_win, "ani_prev", 0, 0, 200, -1);

	hbox3 = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox3);
	gtk_container_add (GTK_CONTAINER (ani_prev_win), hbox3);

	pack(hbox3, sig_toggle_button(_("Play"), FALSE, NULL,
		GTK_SIGNAL_FUNC(ani_but_playstop)));
	ani_play_state = FALSE;			// Stopped

	ani_prev_slider = mt_spinslide_new(-2, -2);
	xpack(hbox3, widget_align_minsize(ani_prev_slider, 200, -2));
	mt_spinslide_set_range(ani_prev_slider, ani_frame1, ani_frame2);
	mt_spinslide_set_value(ani_prev_slider, ani_frame1);
	mt_spinslide_connect(ani_prev_slider,
		GTK_SIGNAL_FUNC(ani_frame_slider_moved), NULL);

	if ( animate_window == NULL )	// If called via the menu have a fix button
	{
		button = add_a_button( _("Fix"), 5, hbox3, FALSE );
		gtk_signal_connect(GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(ani_fix_pos), NULL);
	}

	button = add_a_button( _("Close"), 5, hbox3, FALSE );
	gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(ani_but_preview_close), NULL);
	gtk_widget_add_accelerator (button, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);

	gtk_signal_connect (GTK_OBJECT (ani_prev_win), "delete_event",
			GTK_SIGNAL_FUNC(ani_but_preview_close), NULL);

	gtk_window_set_transient_for( GTK_WINDOW(ani_prev_win), GTK_WINDOW(main_window) );
	gtk_widget_show (ani_prev_win);
	gtk_window_add_accel_group(GTK_WINDOW (ani_prev_win), ag);

	if ( animate_window != NULL ) gtk_widget_hide (animate_window);
	else
	{
		layers_pastry_cut = TRUE;
		update_stuff(UPD_ALLV);
	}

	gtk_adjustment_value_changed(SPINSLIDE_ADJUSTMENT(ani_prev_slider));
}

static void create_frames_ani()
{
	image_info *image;
	ls_settings settings;
	png_color pngpal[256], *trans;
	unsigned char *layer_rgb, *irgb = NULL;
	char output_path[PATHBUF], *command, *wild_path;
	int a, b, k, i, tr, cols, layer_w, layer_h, npt, l = 0;


	ani_win_read_widgets();

	gtk_widget_hide(animate_window);

	ani_write_layer_data();
	layer_press_save();		// Save layers data file

	command = strrchr(layers_filename, DIR_SEP);
	if (command) l = command - layers_filename + 1;
	wjstrcat(output_path, PATHBUF, layers_filename, l, ani_output_path, NULL);
	l = strlen(output_path);

	if (!ani_output_path[0]); // Reusing layers file directory
#ifdef WIN32
	else if (mkdir(output_path))
#else
	else if (mkdir(output_path, 0777))
#endif
	{
		if ( errno != EEXIST )
		{
			alert_box(_("Error"), _("Unable to create output directory"), NULL);
			goto failure;			// Failure to create directory
		}
	}

		// Create output path and pointer for first char of filename

	a = ani_frame1 < ani_frame2 ? ani_frame1 : ani_frame2;
	b = ani_frame1 < ani_frame2 ? ani_frame2 : ani_frame1;

	image = layer_selected ? &layer_table[0].image->image_ : &mem_image;

	layer_w = image->width;
	layer_h = image->height;
	layer_rgb = malloc( layer_w * layer_h * 3);	// Primary layer image for RGB version

	if (!layer_rgb)
	{
		memory_errors(1);
		goto failure;
	}

	/* Prepare settings */
	init_ls_settings(&settings, NULL);
	settings.mode = FS_COMPOSITE_SAVE;
	settings.width = layer_w;
	settings.height = layer_h;
	settings.colors = 256;
	settings.silent = TRUE;
	if (ani_use_gif)
	{
		irgb = malloc(layer_w * layer_h);	// Resulting indexed image
		if (!irgb)
		{
			free(layer_rgb);
			memory_errors(1);
			goto failure;
		}
		settings.ftype = FT_GIF;
		settings.img[CHN_IMAGE] = irgb;
		settings.bpp = 1;
		settings.pal = pngpal;
	}
	else
	{
		settings.ftype = FT_PNG;
		settings.img[CHN_IMAGE] = layer_rgb;
		settings.bpp = 3;
		/* Background transparency */
		settings.xpm_trans = tr = image->trans;
		settings.rgb_trans = tr < 0 ? -1 : PNG_2_INT(image->pal[tr]);
	}

	progress_init(_("Creating Animation Frames"), 1);
	for ( k=a; k<=b; k++ )			// Create each frame and save it as a PNG or GIF image
	{
		if (progress_update(b == a ? 0.0 : (k - a) / (float)(b - a)))
			break;

		ani_set_frame_state(k);		// Change layer positions
		view_render_rgb( layer_rgb, 0, 0, layer_w, layer_h, 1 );	// Render layer

		snprintf(output_path + l, PATHBUF - l, DIR_SEP_STR "%s%05d.%s",
			ani_file_prefix, k, ani_use_gif ? "gif" : "png");

		if ( ani_use_gif )	// Prepare palette
		{
			cols = mem_cols_used_real(layer_rgb, layer_w, layer_h, 258, 0);
							// Count colours in image

			if ( cols <= 256 )	// If <=256 convert directly
				mem_cols_found(pngpal);	// Get palette
			else			// If >256 use Wu to quantize
			{
				cols = 256;
				if (wu_quant(layer_rgb, layer_w, layer_h, cols,
					pngpal)) goto failure2;
			}

			// Create new indexed image
			if (mem_dumb_dither(layer_rgb, irgb, pngpal,
				layer_w, layer_h, cols, FALSE)) goto failure2;

			settings.xpm_trans = -1;	// Default is no transparency
			if (image->trans >= 0)	// Background has transparency
			{
				trans = image->pal + image->trans;
				npt = PNG_2_INT(*trans);
				for (i = 0; i < cols; i++)
				{	// Does it exist in the composite frame?
					if (PNG_2_INT(pngpal[i]) != npt) continue;
					// Transparency found so note it
					settings.xpm_trans = i;
					break;
				}
			}
		}

		if (save_image(output_path, &settings) < 0)
		{
			alert_box(_("Error"), _("Unable to save image"), NULL);
			goto failure2;
		}
	}

	if ( ani_use_gif )	// all GIF files created OK so lets give them to gifsicle
	{
		wild_path = wjstrcat(NULL, 0, output_path, l,
			DIR_SEP_STR, ani_file_prefix, "?????.gif", NULL);
		snprintf(output_path + l, PATHBUF - l, DIR_SEP_STR "%s.gif",
			ani_file_prefix);

		run_def_action(DA_GIF_CREATE, wild_path, output_path, ani_gif_delay);
		run_def_action(DA_GIF_PLAY, output_path, NULL, 0);
		free(wild_path);
	}

failure2:
	progress_end();
	free( layer_rgb );

failure:
	free( irgb );

	gtk_widget_show(animate_window);
}

void pressed_remove_key_frames()
{
	int i, j;

	i = alert_box(_("Warning"), _("Do you really want to clear all of the position and cycle data for all of the layers?"),
		_("No"), _("Yes"), NULL);
	if ( i==2 )
	{
		for (j = 0; j <= layers_total; j++)
			memset(&layer_table[j].image->ani_, 0, sizeof(ani_info));
		memset(ani_cycle_table, 0, sizeof(ani_cycle_table));
	}
}

static void ani_set_key_frame(int key)		// Set key frame postions & cycles as per current layers
{
	unsigned char buf[MAX_CYC_SLOTS * ANI_CYC_ROWLEN];
	unsigned char *cp;
	ani_slot *ani;
	ani_cycle *cc;
	int i, j, k, l;


// !!! Maybe make background x/y settable here too?
	for ( k=1; k<=layers_total; k++ )	// Add current position for each layer
	{
		ani = layer_table[k].image->ani_.pos;
		// Find first occurence of 0 or frame # < 'key'
		for ( i=0; i<MAX_POS_SLOTS; i++ )
		{
			if (ani[i].frame > key || ani[i].frame == 0) break;
		}

		if ( i>=MAX_POS_SLOTS ) i=MAX_POS_SLOTS-1;

		//  Shift remaining data down a slot
		for ( j=MAX_POS_SLOTS-1; j>i; j-- )
		{
			ani[j] = ani[j - 1];
		}

		//  Enter data for the current state
		ani[i].frame = key;
		ani[i].x = layer_table[k].x;
		ani[i].y = layer_table[k].y;
		ani[i].opacity = layer_table[k].opacity;
		ani[i].effect = 0;			// No effect
	}

	// Find first occurence of 0 or frame # < 'key'
	for ( i=0; i<MAX_CYC_SLOTS; i++ )
	{
		if ( ani_cycle_table[i].frame0 > key ||
			ani_cycle_table[i].frame0 == 0 )
				break;
	}

	if ( i>=MAX_CYC_SLOTS ) i=MAX_CYC_SLOTS-1;

	// Shift remaining data down a slot
	l = MAX_CYC_SLOTS - 1 - i;
	ani_cyc_get(buf);
	cp = buf + ANI_CYC_ROWLEN * i;
	memmove(cp + ANI_CYC_ROWLEN, cp, l * ANI_CYC_ROWLEN);
	cc = ani_cycle_table + i;
	memmove(cc + 1, cc, l * sizeof(ani_cycle));

	// Enter data for the current state
	l = layers_total;
	if (l > MAX_CYC_ITEMS) l = MAX_CYC_ITEMS;
	cc->frame0 = cc->frame1 = key;
	cc->len = *cp++ = l;
	for (j = 1; j <= l; j++)
	{
		*cp++ = !layer_table[j].visible; // Position
		*cp++ = j; // Layer
	}

	// Write back
	ani_cyc_put(buf);
}

static void ani_tog_gif(GtkToggleButton *togglebutton, gpointer user_data)
{
	ani_use_gif = gtk_toggle_button_get_active(togglebutton);
	ani_widget_changed();
}

static void ani_layer_select( GtkList *list, GtkWidget *widget )
{
	int j = layers_total - gtk_list_child_position(list, widget);

// !!! Allow background here when/if added to the list
	if ( j<1 || j>layers_total ) return;		// Item not found

	if ( ani_currently_selected_layer != -1 )	// Only if not first click
	{
		ani_parse_store_positions();		// Parse & store text inputs
	}

	ani_currently_selected_layer = j;
	ani_pos_refresh_txt();				// Refresh the text in the widget
}

static int do_set_key_frame(GtkWidget *spin, gpointer fdata)
{
	int i;

	i = read_spin(spin);
	ani_set_key_frame(i);
	layers_notify_changed();

	return TRUE;
}

void pressed_set_key_frame()
{
	GtkWidget *spin = add_a_spin(ani_frame1, ani_frame1, ani_frame2);
	filter_window(_("Set Key Frame"), spin, do_set_key_frame, NULL, FALSE);
}

static GtkWidget *ani_text(GtkWidget **textptr)
{
	GtkWidget *scroll, *text;

#if GTK_MAJOR_VERSION == 1
	text = gtk_text_new(NULL, NULL);
	gtk_text_set_editable(GTK_TEXT(text), TRUE);

	gtk_signal_connect(GTK_OBJECT(text), "changed",
			GTK_SIGNAL_FUNC(ani_widget_changed), NULL);

	scroll = gtk_scrolled_window_new(NULL, GTK_TEXT(text)->vadj);
#else /* #if GTK_MAJOR_VERSION == 2 */
	GtkTextBuffer *texbuf = gtk_text_buffer_new(NULL);

	text = gtk_text_view_new_with_buffer(texbuf);

	g_signal_connect(texbuf, "changed", GTK_SIGNAL_FUNC(ani_widget_changed), NULL);

	scroll = gtk_scrolled_window_new(GTK_TEXT_VIEW(text)->hadjustment,
		GTK_TEXT_VIEW(text)->vadjustment);
#endif
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), text);

	*textptr = text;
	return (scroll);
}

void pressed_animate_window()
{
	GtkWidget *table, *label, *button, *notebook1, *scrolledwindow;
	GtkWidget /**ani_toggle_gif,*/ *ani_list_layers, *list_data;
	GtkWidget *hbox4, *hbox2, *vbox1, *vbox3, *vbox4;
	GtkAccelGroup* ag = gtk_accel_group_new();
	char txt[PATHTXT];
	int i;


	if ( layers_total < 1 )					// Only background layer available
	{
		alert_box(_("Error"), _("You must have at least 2 layers to create an animation"), NULL);
		return;
	}

	if (!layers_filename[0])
	{
		alert_box(_("Error"), _("You must save your layers file before creating an animation"), NULL);
		return;
	}

	delete_layers_window();	// Lose the layers window if its up

	ani_read_layer_data();

	ani_currently_selected_layer = -1;

	animate_window = add_a_window( GTK_WINDOW_TOPLEVEL, _("Configure Animation"),
					GTK_WIN_POS_NONE, TRUE );

	ani_win_set_pos();

	vbox1 = add_vbox(animate_window);

	notebook1 = xpack(vbox1, gtk_notebook_new());
	gtk_container_set_border_width(GTK_CONTAINER(notebook1), 5);

	vbox4 = add_new_page(notebook1, _("Output Files"));
	table = xpack(vbox4, gtk_table_new(5, 3, FALSE));

	label = add_to_table( _("Start frame"), table, 0, 0, 5 );
	add_to_table( _("End frame"), table, 1, 0, 5 );

	add_to_table( _("Delay"), table, 2, 0, 5 );
	add_to_table( _("Output path"), table, 3, 0, 5 );
	add_to_table( _("File prefix"), table, 4, 0, 5 );

	ani_spin[0] = spin_to_table(table, 0, 1, 5, ani_frame1, 1, MAX_FRAME);	// Start
	ani_spin[1] = spin_to_table(table, 1, 1, 5, ani_frame2, 1, MAX_FRAME);	// End
	ani_spin[2] = spin_to_table(table, 2, 1, 5, ani_gif_delay, 1, MAX_DELAY);	// Delay

	ani_entry_path = gtk_entry_new_with_max_length(PATHBUF);
	to_table(ani_entry_path, table, 3, 1, 0);
	gtkuncpy(txt, ani_output_path, PATHTXT);
	gtk_entry_set_text(GTK_ENTRY(ani_entry_path), txt);

	ani_entry_prefix = gtk_entry_new_with_max_length(ANI_PREFIX_LEN);
	to_table(ani_entry_prefix, table, 4, 1, 0);
	gtkuncpy(txt, ani_file_prefix, PATHTXT);
	gtk_entry_set_text(GTK_ENTRY(ani_entry_prefix), txt);

	track_updates(GTK_SIGNAL_FUNC(ani_widget_changed),
		ani_spin[0], ani_spin[1], ani_spin[2],
		ani_entry_path, ani_entry_prefix, NULL);

//	ani_toggle_gif =
	pack(vbox4, sig_toggle(_("Create GIF frames"),
		ani_use_gif, NULL, GTK_SIGNAL_FUNC(ani_tog_gif)));

///	LAYERS TABLES

	hbox4 = gtk_hbox_new(FALSE, 0);
	label = gtk_label_new(_("Positions"));
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook1), hbox4, label);

	scrolledwindow = pack(hbox4, gtk_scrolled_window_new(NULL, NULL));
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow),
			GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	ani_list_layers = gtk_list_new ();
	gtk_signal_connect( GTK_OBJECT(ani_list_layers), "select_child",
			GTK_SIGNAL_FUNC(ani_layer_select), NULL );
	gtk_scrolled_window_add_with_viewport (GTK_SCROLLED_WINDOW (scrolledwindow), ani_list_layers);

	gtk_widget_set_usize (ani_list_layers, 150, -2);
	gtk_container_set_border_width (GTK_CONTAINER (ani_list_layers), 5);

// !!! Maybe allow background here too, for x/y?
	for ( i=layers_total; i>0; i-- )
	{
		hbox2 = gtk_hbox_new( FALSE, 3 );

		list_data = gtk_list_item_new();
		gtk_container_add( GTK_CONTAINER(ani_list_layers), list_data );
		gtk_container_add( GTK_CONTAINER(list_data), hbox2 );

		sprintf(txt, "%i", i);					// Layer number
		label = pack(hbox2, gtk_label_new(txt));
		gtk_widget_set_usize (label, 40, -2);
		gtk_misc_set_alignment( GTK_MISC(label), 0.5, 0.5 );

		label = xpack(hbox2, gtk_label_new(layer_table[i].name)); // Layer name
		gtk_misc_set_alignment( GTK_MISC(label), 0, 0.5 );
	}

	vbox3 = xpack(hbox4, gtk_vbox_new(FALSE, 0));
	xpack(vbox3, ani_text(&ani_text_pos));

///	CYCLES TAB

	vbox3 = add_new_page(notebook1, _("Cycling"));
	xpack(vbox3, ani_text(&ani_text_cyc));

	ani_cyc_refresh_txt();

///	MAIN BUTTONS

	hbox2 = pack(vbox1, gtk_hbox_new(FALSE, 0));

	button = add_a_button(_("Close"), 5, hbox2, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(delete_ani), NULL);
	gtk_widget_add_accelerator (button, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);

	button = add_a_button(_("Save"), 5, hbox2, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(ani_but_save), NULL);

	button = add_a_button(_("Preview"), 5, hbox2, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(ani_but_preview), NULL);

	button = add_a_button(_("Create Frames"), 5, hbox2, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(create_frames_ani), NULL);

	gtk_signal_connect_object (GTK_OBJECT (animate_window), "delete_event",
		GTK_SIGNAL_FUNC (delete_ani), NULL);

	ani_show_main_state = show_layers_main;	// Remember old state
	show_layers_main = FALSE;		// Don't show all layers in main window - too messy

	gtk_window_set_transient_for( GTK_WINDOW(animate_window), GTK_WINDOW(main_window) );

	gtk_list_select_item( GTK_LIST(ani_list_layers), 0 );

	gtk_widget_show_all(animate_window);
	gtk_window_add_accel_group(GTK_WINDOW (animate_window), ag);

	layers_pastry_cut = TRUE;
	update_stuff(UPD_ALLV);
}



///	FILE HANDLING

void ani_read_file( FILE *fp )			// Read data from layers file already opened
{
	unsigned char buf[MAX_CYC_SLOTS * ANI_CYC_ROWLEN];
	char tin[2048];
	int i, j, k, tot;

	ani_init();
	do
	{
		if (!fgets(tin, 2000, fp)) return;		// BAILOUT - invalid line
		string_chop( tin );
	} while ( strcmp( tin, ANIMATION_HEADER ) != 0 );	// Look for animation header

	i = read_file_num(fp, tin);
	if ( i<0 ) return;				// BAILOUT - invalid #
	ani_frame1 = i;

	i = read_file_num(fp, tin);
	if ( i<0 ) return;				// BAILOUT - invalid #
	ani_frame2 = i;

	if (!fgets(tin, 2000, fp)) return;		// BAILOUT - invalid line
	string_chop( tin );
	strncpy0(ani_output_path, tin, PATHBUF);

	if (!fgets(tin, 2000, fp)) return;		// BAILOUT - invalid #
	string_chop( tin );
	strncpy0(ani_file_prefix, tin, ANI_PREFIX_LEN + 1);

	i = read_file_num(fp, tin);
	if ( i<0 )
	{
		ani_use_gif = FALSE;
		ani_gif_delay = -i;
	}
	else
	{
		ani_use_gif = TRUE;
		ani_gif_delay = i;
	}

///	CYCLE DATA

	i = read_file_num(fp, tin);
	if ( i<0 || i>MAX_CYC_SLOTS ) return;			// BAILOUT - invalid #

	tot = i;
	memset(buf, 0, sizeof(buf));
	for ( j=0; j<tot; j++ )					// Read each cycle line
	{
		if (!fgets(tin, 2000, fp)) break;		// BAILOUT - invalid line

		ani_cyc_sscanf(tin, ani_cycle_table + j,
			buf + ANI_CYC_ROWLEN * j);
	}
	if ( j<MAX_CYC_SLOTS ) ani_cycle_table[j].frame0 = 0;	// Mark end
	ani_cyc_put(buf);

///	POSITION DATA

	for ( k=0; k<=layers_total; k++ )
	{
		i = read_file_num(fp, tin);
		if ( i<0 || i>MAX_POS_SLOTS ) return;			// BAILOUT - invalid #

		tot = i;
		for ( j=0; j<tot; j++ )					// Read each position line
		{
			if (!fgets(tin, 2000, fp)) break;		// BAILOUT - invalid line
			ani_pos_sscanf(tin, layer_table[k].image->ani_.pos + j);
		}
		if ( j<MAX_POS_SLOTS )
			layer_table[k].image->ani_.pos[j].frame = 0;	// Mark end
	}
}

void ani_write_file( FILE *fp )			// Write data to layers file already opened
{
	char txt[ANI_CYC_TEXT_MAX];
	unsigned char buf[MAX_CYC_SLOTS * ANI_CYC_ROWLEN];
	int gifcode = ani_gif_delay, i, j, k;

	if ( layers_total == 0 ) return;	// No layers memory allocated so bail out


	if ( !ani_use_gif ) gifcode = -gifcode;

	// HEADER

	fprintf( fp, "%s\n", ANIMATION_HEADER );
	fprintf( fp, "%i\n%i\n%s\n%s\n%i\n", ani_frame1, ani_frame2,
			ani_output_path, ani_file_prefix, gifcode );

	// CYCLE INFO

	// Count number of cycles, and output this data (if any)
	for ( i=0; i<MAX_CYC_SLOTS; i++ )
	{
		if (!ani_cycle_table[i].frame0) break;	// Bail out at 1st 0
	}

	fprintf( fp, "%i\n", i );

	ani_cyc_get(buf);
	for ( k=0; k<i; k++ )
	{
		ani_cyc_sprintf(txt, ani_cycle_table + k,
			buf + ANI_CYC_ROWLEN * k);
		fputs(txt, fp);
	}

	// POSITION INFO

	// NOTE - we are saving data for layer 0 even though its never used during animation.
	// This is because the user may shift this layer up/down and bring it into play

	for ( k=0; k<=layers_total; k++ )		// Write position table for each layer
	{
		ani_slot *ani = layer_table[k].image->ani_.pos;
		for ( i=0; i<MAX_POS_SLOTS; i++ )	// Count how many lines are in the table
		{
			if (ani[i].frame == 0) break;
		}
		fprintf( fp, "%i\n", i );		// Number of position lines for this layer
		for (j = 0; j < i; j++)
		{
			ani_pos_sprintf(txt, ani + j);
			fputs(txt, fp);
		}
	}
}
