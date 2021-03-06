/*	vcode.c
	Copyright (C) 2013 Dmitry Groshev

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
#include "inifile.h"
#include "png.h"
#include "mainwindow.h"
#include "cpick.h"
#include "vcode.h"

/* Make code not compile if it cannot work */
typedef char Opcodes_Too_Long[2 * (op_BOR_LAST <= WB_OPMASK) - 1];

/// V-CODE ENGINE

/* Max V-code subroutine nesting */
#define CALL_DEPTH 16
/* Max container widget nesting */
#define CONT_DEPTH 128

#define GET_OP(S) ((int)*(void **)(S)[1] & WB_OPMASK)

enum {
	pk_NONE = 0,
	pk_PACK,
	pk_PACKp,
	pk_XPACK,
	pk_PACKEND,
	pk_TABLE,
	pk_TABLEx,
	pk_TABLEp,
	pk_TABLE2,
	pk_TABLE2x
};
#define pk_MASK    0xFF
#define pkf_FRAME 0x100
#define pkf_STACK 0x200

/* From event to its originator */
void **origin_slot(void **slot)
{
	while (((int)*(void **)slot[1] & WB_OPMASK) >= op_EVT_0) slot -= 2;
	return (slot);
}

/* !!! Warning: handlers should not access datastore after window destruction!
 * GTK+ refs objects for signal duration, but no guarantee every other toolkit
 * will behave alike - WJ */

static void get_evt_1(GtkObject *widget, gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];

	((evt_fn)desc[1])(GET_DDATA(base), base, (int)desc[0] & WB_OPMASK, slot);
}

/* Handler for wj_radio_pack() */
static void get_evt_wjr(GtkWidget *btn, gpointer idx)
{
	void **slot, **base, **desc, **orig, *v;
	char *d;

	if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn))) return;
	slot = gtk_object_get_user_data(GTK_OBJECT(btn->parent));
	base = slot[0];
	d = GET_DDATA(base);
	/* Update the value */
	orig = origin_slot(slot);
	desc = orig[1];
	v = desc[1];
	if ((int)desc[0] & WB_FFLAG) v = d + (int)v;
	*(int *)v = (int)idx;
	/* Call the handler */
	desc = slot[1];
	((evt_fn)desc[1])(d, base, (int)desc[0] & WB_OPMASK, slot);
}

static void **add_click(void **r, void **res, void **pp, GtkWidget *widget,
	GtkWidget *window)
{
	if (pp[1])
	{
		r[0] = res;
		r[1] = pp;
		gtk_signal_connect(GTK_OBJECT(widget), "clicked",
			GTK_SIGNAL_FUNC(get_evt_1), r);
		r += 2;
	}
	// default to destructor
	else if (window) gtk_signal_connect_object(GTK_OBJECT(widget), "clicked",
		GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(window));
	return (r);
}

static void **skip_if(void **pp)
{
	int lp, mk;
	void **ifcode;

	ifcode = pp + 1 + (lp = (int)*pp >> 16);
	if (lp > 1) // skip till corresponding ENDIF
	{
		mk = (int)pp[2];
		while ((((int)*ifcode & WB_OPMASK) != op_ENDIF) ||
			((int)ifcode[1] != mk))
			ifcode += 1 + ((int)*ifcode >> 16);
	}
	return (ifcode + 1 + ((int)*ifcode >> 16));
}

/* Trigger events which need triggering */
static void trigger_things(void **wdata)
{
	char *data = GET_DDATA(wdata);
	void **slot, **desc;

	for (wdata = GET_WINDOW(wdata); wdata[1]; wdata += 2)
	{
		if (GET_OP(wdata) != op_TRIGGER) continue;
		slot = wdata - 2;
		desc = slot[1];
		((evt_fn)desc[1])(data, slot[0], (int)desc[0] & WB_OPMASK, slot);
	}
}

/* Predict how many _slots_ a V-code sequence could need */
// !!! With GCC inlining this, weird size fluctuations can happen
static int predict_size(void **ifcode, char *ddata)
{
	void **v, **pp, *rstack[CALL_DEPTH], **rp = rstack;
	int op, n = 2 + 1; // safety margin and WTAGS slot

	while (TRUE)
	{
		op = (int)*ifcode++;
		ifcode = (pp = ifcode) + (op >> 16);
		n += WB_GETREF(op);
		op &= WB_OPMASK;
		if (op <= op_WSHOW) break; // End
		// Subroutine call/return
		if (op == op_RET) ifcode = *--rp;
		else if (op == op_CALLp)
		{
			*rp++ = ifcode;
			v = *pp;
			if ((int)*(pp - 1) & WB_FFLAG)
				v = (void *)(ddata + (int)v);
			ifcode = *v;
		}
	}
	return (n);
}

// !!! And with inlining this, same problem
void table_it(GtkWidget *table, GtkWidget *it, int wh, int pad, int pack)
{
	int row = wh & 255, column = (wh >> 8) & 255, l = (wh >> 16) + 1;
	gtk_table_attach(GTK_TABLE(table), it, column, column + l, row, row + 1,
		pack == pk_TABLEx ? GTK_EXPAND | GTK_FILL : GTK_FILL, 0,
		pack == pk_TABLEp ? pad : 0, pad);
}

/* Find where unused rows/columns start */
static int next_table_level(GtkWidget *table, int h)
{
	GList *item;
	int y, n = 0;
	for (item = GTK_TABLE(table)->children; item; item = item->next)
	{
		y = h ? ((GtkTableChild *)item->data)->right_attach :
			((GtkTableChild *)item->data)->bottom_attach;
		if (n < y) n = y;
	}
	return (n);
}

/* Try to avoid scrolling - request full size of contents */
static void scroll_max_size_req(GtkWidget *widget, GtkRequisition *requisition,
	gpointer user_data)
{
	GtkWidget *child = GTK_BIN(widget)->child;

	if (child && GTK_WIDGET_VISIBLE(child))
	{
		GtkRequisition wreq;
		int n, border = GTK_CONTAINER(widget)->border_width * 2;

		gtk_widget_get_child_requisition(child, &wreq);
		n = wreq.width + border;
		if (requisition->width < n) requisition->width = n;
		n = wreq.height + border;
		if (requisition->height < n) requisition->height = n;
	}
}

/* Toggle notebook pages */
static void toggle_vbook(GtkToggleButton *button, gpointer user_data)
{
	gtk_notebook_set_page(**(void ***)user_data,
		!!gtk_toggle_button_get_active(button));
}

//	COLORLIST widget

typedef struct {
	unsigned char *col;
	int cnt, *idx;
	int scroll;
} colorlist_data;

// !!! ref to RGB[3]
static gboolean col_expose(GtkWidget *widget, GdkEventExpose *event,
	unsigned char *col)
{
	GdkGCValues sv;

	gdk_gc_get_values(widget->style->black_gc, &sv);
	gdk_rgb_gc_set_foreground(widget->style->black_gc, MEM_2_INT(col, 0));
	gdk_draw_rectangle(widget->window, widget->style->black_gc, TRUE,
		event->area.x, event->area.y, event->area.width, event->area.height);
	gdk_gc_set_foreground(widget->style->black_gc, &sv.foreground);

	return (TRUE);
}

static gboolean colorlist_click(GtkWidget *widget, GdkEventButton *event,
	gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];
	colorlist_ext xdata;

	if (event->type == GDK_BUTTON_PRESS)
	{
		xdata.idx = (int)gtk_object_get_user_data(GTK_OBJECT(widget));
		xdata.button = event->button;
		((evtx_fn)desc[1])(GET_DDATA(base), base,
			(int)desc[0] & WB_OPMASK, slot, &xdata);
	}

	/* Let click processing continue */
	return (FALSE);
}

static void colorlist_select(GtkList *list, GtkWidget *widget, gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));

	/* Update the value */
	*(dt->idx) = (int)gtk_object_get_user_data(GTK_OBJECT(widget));
	/* Call the handler */
	if (desc[1]) ((evt_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot);
}

static void colorlist_map_scroll(GtkWidget *list, colorlist_data *dt)
{	
	GtkAdjustment *adj;
	int idx = dt->scroll - 1;

	dt->scroll = 0;
	if (idx < 0) return;
	adj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(list->parent->parent));
	if (adj->upper > adj->page_size)
	{
		float f = adj->upper * (idx + 0.5) / dt->cnt - adj->page_size * 0.5;
		adj->value = f < 0.0 ? 0.0 : f > adj->upper - adj->page_size ?
			adj->upper - adj->page_size : f;
		gtk_adjustment_value_changed(adj);
	}
}

// !!! And with inlining this, problem also
GtkWidget *colorlist(GtkWidget *box, int *idx, char *ddata, void **pp,
	void **r)
{
	GtkWidget *scroll, *list, *item, *col, *label;
	colorlist_data *dt;
	void *v, **cslot = NULL;
	char txt[64], *t, **sp = NULL;
	int i, cnt = 0;

	scroll = pack(box, gtk_scrolled_window_new(NULL, NULL));
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	list = gtk_list_new();

	// Allocate datablock
	dt = bound_malloc(list, sizeof(colorlist_data));
	v = ddata + (int)pp[3];
	if (((int)pp[0] & WB_OPMASK) == op_COLORLIST) // array of names
	{
		sp = *(char ***)v;
		while (sp[cnt]) cnt++;
	}
	else cnt = *(int *)v; // op_COLORLISTN - number
	dt->cnt = cnt;
	dt->col = (void *)(ddata + (int)pp[2]); // palette
	dt->idx = idx;

	if (pp[7]) cslot = r , r += 2; // !!! ext-event handler goes first

	gtk_object_set_user_data(GTK_OBJECT(list), dt); // know thy descriptor
	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), list);
	gtk_widget_show_all(scroll);

	for (i = 0; i < cnt; i++)
	{
		item = gtk_list_item_new();
		gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)i);
		if (cslot) gtk_signal_connect(GTK_OBJECT(item), "button_press_event",
			GTK_SIGNAL_FUNC(colorlist_click), cslot);
		gtk_container_add(GTK_CONTAINER(list), item);

		box = gtk_hbox_new(FALSE, 3);
		gtk_widget_show(box);
		gtk_container_set_border_width(GTK_CONTAINER(box), 3);
		gtk_container_add(GTK_CONTAINER(item), box);

		col = pack(box, gtk_drawing_area_new());
		gtk_drawing_area_size(GTK_DRAWING_AREA(col), 20, 20);
		gtk_signal_connect(GTK_OBJECT(col), "expose_event",
			GTK_SIGNAL_FUNC(col_expose), dt->col + i * 3);

		/* Name or index */
		if (sp) t = _(sp[i]);
		else sprintf(t = txt, "%d", i);
		label = xpack(box, gtk_label_new(t));
		gtk_misc_set_alignment(GTK_MISC(label), 0.0, 1.0);

		gtk_widget_show_all(item);
	}
	gtk_list_set_selection_mode(GTK_LIST(list), GTK_SELECTION_BROWSE);
	/* gtk_list_select_*() don't work in GTK_SELECTION_BROWSE mode */
	gtk_container_set_focus_child(GTK_CONTAINER(list),
		GTK_WIDGET(g_list_nth(GTK_LIST(list)->children, *idx)->data));
	gtk_signal_connect(GTK_OBJECT(list), "select_child",
		GTK_SIGNAL_FUNC(colorlist_select), r);
	gtk_signal_connect_after(GTK_OBJECT(list), "map",
		GTK_SIGNAL_FUNC(colorlist_map_scroll), dt);

	return (list);
}

static void colorlist_scroll_in(GtkWidget *list, int idx)
{
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));
	dt->scroll = idx + 1;
	if (GTK_WIDGET_MAPPED(list)) colorlist_map_scroll(list, dt);
}

static void colorlist_set_color(GtkWidget *list, int idx, int v)
{
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));
	unsigned char *rgb = dt->col + idx * 3;
	GdkColor c;

	c.pixel = 0;
	c.red   = (rgb[0] = INT_2_R(v)) * 257;
	c.green = (rgb[1] = INT_2_G(v)) * 257;
	c.blue  = (rgb[2] = INT_2_B(v)) * 257;
	// In case of some really ancient system with indexed display mode
	gdk_colormap_alloc_color(gdk_colormap_get_system(), &c, FALSE, TRUE);
	/* Redraw the item displaying the color */
	gtk_widget_queue_draw(
		GTK_WIDGET(g_list_nth(GTK_LIST(list)->children, idx)->data));
}

#if U_NLS

/* Translate array of strings */
static int n_trans(char **dest, char **src, int n)
{
	int i;
	for (i = 0; (i != n) && src[i]; i++) dest[i] = _(src[i]);
	return (i);
}

#endif

/* V-code is really simple-minded; it can do 0-tests but no arithmetics, and
 * naturally, can inline only constants. Everything else must be prepared either
 * in global variables, or in fields of "ddata" structure.
 * Parameters of codes should be arrayed in fixed order:
 * result location first; frame name last; table location, or name in table,
 * directly before it (last if no frame name) */

#define DEF_BORDER 5
#define GET_BORDER(T) (borders[op_BOR_##T - op_BOR_0] + DEF_BORDER)

void **run_create(void **ifcode, void *ddata, int ddsize)
{
	static const int scrollp[3] = { GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC,
		GTK_POLICY_ALWAYS };
#if U_NLS
	char *tc[256];
#endif
#if GTK_MAJOR_VERSION == 1
	int have_sliders = FALSE;
#endif
	int have_spins = FALSE;
	char txt[PATHTXT];
	int borders[op_BOR_LAST - op_BOR_0], wpos = GTK_WIN_POS_CENTER;
	GtkWidget *wstack[CONT_DEPTH], **wp = wstack + CONT_DEPTH;
	GtkWidget *tw, *window = NULL, *widget = NULL;
	GtkAccelGroup* ag = NULL;
	void *rstack[CALL_DEPTH], **rp = rstack;
	void *v, **pp, **tagslot = NULL, **r = NULL, **res = NULL;
	int ld, dsize;
	int n, op, lp, ref, pk, cw, tpad = 0;


	// Allocation size
	ld = (ddsize + sizeof(void *) - 1) / sizeof(void *);
	dsize = (ld + 2 + predict_size(ifcode, ddata) * 2) * sizeof(void *);

	// Border sizes are DEF_BORDER-based
	memset(borders, 0, sizeof(borders));

	while (TRUE)
	{
		op = (int)*ifcode++;
		ifcode = (pp = ifcode) + (lp = op >> 16);
		ref = WB_GETREF(op);
		pk = cw = 0;
		v = lp ? pp[0] : NULL;
		if (op & WB_FFLAG) v = (void *)((char *)ddata + (int)v);
		switch (op &= WB_OPMASK)
		{
		/* Terminate */
		case op_WEND: case op_WSHOW:
			/* Terminate the list */
			*r++ = NULL;
			*r++ = NULL;

			/* Make WTAGS descriptor */
			*tagslot = r;
			*r++ = WBh(WTAGS, 1);
			*r++ = (void *)have_spins; // only this for now

			/* !!! For now, done unconditionally */
			gtk_window_set_transient_for(GTK_WINDOW(window),
				GTK_WINDOW(main_window));
#if GTK_MAJOR_VERSION == 1
			/* To make Smooth theme engine render sliders properly */
			if (have_sliders) gtk_signal_connect_after(
				GTK_OBJECT(window), "show",
				GTK_SIGNAL_FUNC(gtk_widget_queue_resize), NULL);
#endif
			/* Trigger remembered events */
			trigger_things(res);
			/* Display */
			if (op == op_WSHOW) gtk_widget_show(window);
			/* Return anchor position */
			return (res);
		/* Done with a container */
		case op_WDONE: ++wp; continue;
		/* Create a toplevel window, bind datastore to it, and
		 * put a vertical box inside it */
		case op_WINDOWpm:
			v = *(char **)v; // dereference & fallthrough
		case op_WINDOW: case op_WINDOWm:
			window = add_a_window(GTK_WINDOW_TOPLEVEL, _(v),
				wpos, op != op_WINDOW);
			res = bound_malloc(window, dsize);

			memcpy(res, ddata, ddsize); // Copy datastruct
			ddata = res; // And switch to using it
			r = res += ld; // Anchor after it
			*r++ = ddata; // Store struct ref at anchor
			*r++ = window; // Store window ref right next to it
			*r++ = pp - 1; // And slot ref after it
			*r++ = window; // Store window again for WTAGS
			tagslot = r; // Remember the location
			*r++ = NULL; // No WTAGS descriptor yet
			*--wp = add_vbox(window);
			continue;
		/* Add a notebook page */
		case op_PAGE:
			--wp; wp[0] = widget = add_new_page(wp[1], _(v));
			break;
		/* Add a table */
		case op_TABLE:
			--wp; wp[0] = widget = add_a_table((int)v & 0xFFFF,
				(int)v >> 16, GET_BORDER(TABLE), wp[1]);
			break;
		/* Add a box */
		case op_TLHBOX:
			if (lp < 2) v = NULL; // reserve the one parameter
		case op_VBOX: case op_XVBOX: case op_EVBOX:
		case op_HBOX: case op_XHBOX:
			widget = (op < op_HBOX ? gtk_vbox_new :
				gtk_hbox_new)(FALSE, (int)v & 255);
			gtk_widget_show(widget);
			cw = (int)v >> 8;
// !!! Padding = 0
			tpad = 0;
			pk = pk_PACK | pkf_STACK;
			if ((op == op_XVBOX) || (op == op_XHBOX))
				pk = pk_XPACK | pkf_STACK;
			if (op == op_TLHBOX) pk = pk_TABLE | pkf_STACK;
			if (op == op_EVBOX) pk = pk_PACKEND | pkf_STACK;
			break;
		/* Add a framed vertical box */
		case op_FVBOX:
			widget = gtk_vbox_new(FALSE, lp > 1 ? (int)v : 0);
			gtk_widget_show(widget);
			cw = GET_BORDER(FRBOX);
			pk = pk_PACK | pkf_FRAME | pkf_STACK;
			break;
		/* Add a scrolled window */
		case op_SCROLL:
			widget = gtk_scrolled_window_new(NULL, NULL);
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget),
				scrollp[(int)v & 255], scrollp[(int)v >> 8]);
			gtk_widget_show(widget);
			pk = pk_XPACK | pkf_STACK;
			break;
		/* Put a notebook into scrolled window */
		case op_SNBOOK:
			tw = *wp++; // unstack the box
			--wp; wp[0] = widget = gtk_notebook_new();
			gtk_notebook_set_tab_pos(GTK_NOTEBOOK(widget), GTK_POS_LEFT);
//			gtk_notebook_set_scrollable(GTK_NOTEBOOK(wp[0]), TRUE);
			gtk_scrolled_window_add_with_viewport(
				GTK_SCROLLED_WINDOW(tw), widget);
			tw = GTK_BIN(tw)->child;
			gtk_viewport_set_shadow_type(GTK_VIEWPORT(tw), GTK_SHADOW_NONE);
			gtk_widget_show_all(tw);
			vport_noshadow_fix(tw);
			break;
		/* Add a plain notebook (2 pages for now) */
		case op_PLAINBOOK:
		{
			// !!! no extra args
			int n = v ? (int)v : 2; // 2 pages by default
			// !!! All pages go onto stack, with #0 on top
			wp -= n;
			widget = pack(wp[n], plain_book(wp, n));
			break;
		}
		/* Add a toggle button for controlling 2-paged notebook */
		case op_BOOKBTN:
			widget = sig_toggle_button(_(pp[1]), FALSE, v,
				GTK_SIGNAL_FUNC(toggle_vbook));
			pk = pk_PACK;
			break;
		/* Add a horizontal line */
		case op_HSEP:
// !!! Height = 10
			add_hseparator(wp[0], lp ? (int)v : -2, 10);
			continue; // !!! nonreferable: does not return widget
		/* Add a label */
		case op_MLABEL: case op_TLLABEL:
			widget = gtk_label_new(_(v));
			gtk_widget_show(widget);
			pk = pk_PACK;
			if (op == op_MLABEL) break; // Multiline label
			gtk_label_set_justify(GTK_LABEL(widget), GTK_JUSTIFY_LEFT);
			gtk_misc_set_alignment(GTK_MISC(widget), 0.0, 0.5);
			tpad = GET_BORDER(TLLABEL);
			pk = pk_TABLEp;
			break;
		/* Add a non-spinning spin to table slot */
		case op_TLNOSPIN:
		{
			int n = *(int *)v;
			widget = add_a_spin(n, n, n);
			GTK_WIDGET_UNSET_FLAGS(widget, GTK_CAN_FOCUS);
			tpad = GET_BORDER(SPIN);
			pk = pk_TABLE;
			break;
		}
		/* Add a spin, fill from field/var */
		case op_SPIN: case op_XSPIN:
		case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
			widget = add_a_spin(*(int *)v, (int)pp[1], (int)pp[2]);
			have_spins = TRUE;
			tpad = GET_BORDER(SPIN);
			pk = pk_TABLE2;
			if (op == op_TLSPIN) pk = pk_TABLE;
			if (op == op_TLXSPIN) pk = pk_TABLEx;
// !!! Padding = 0
			if (op == op_XSPIN) pk = pk_XPACK;
			if (op == op_SPIN) pk = pk_PACKp;
			break;
		/* Add float spin, fill from field/var */
		case op_FSPIN:
			widget = add_float_spin(*(int *)v / 100.0,
				(int)pp[1] / 100.0, (int)pp[2] / 100.0);
			have_spins = TRUE;
// !!! Padding = 0
			pk = pk_PACK;
			break;
		/* Add a spin, fill from array */
		case op_SPINa: case op_XSPINa: case op_TSPINa:
		{
			int *xp = v;
			widget = add_a_spin(xp[0], xp[1], xp[2]);
			have_spins = TRUE;
			tpad = GET_BORDER(SPIN);
			pk = pk_TABLE2;
// !!! Padding = 0
			if (op == op_XSPINa) pk = pk_XPACK;
			if (op == op_SPINa) pk = pk_PACKp;
			break;
		}
		/* Add a named spinslider to table, fill from field */
		case op_TSPINSLIDE:
// !!! Padding = 0
			tpad = 0;
// !!! Width = 255 Height = 20
			widget = mt_spinslide_new(255, 20);
			mt_spinslide_set_range(widget, (int)pp[1], (int)pp[2]);
			mt_spinslide_set_value(widget, *(int *)v);
#if GTK_MAJOR_VERSION == 1
			have_sliders = TRUE;
#endif
			have_spins = TRUE;
			pk = pk_TABLE2x;
			break;
		/* Add a named spinslider to table horizontally, fill from field */
		case op_HTSPINSLIDE:
		{
			GtkWidget *label;
			int x;
// !!! Width & height unset
			widget = mt_spinslide_new(-1, -1);
			mt_spinslide_set_range(widget, (int)pp[1], (int)pp[2]);
			mt_spinslide_set_value(widget, *(int *)v);
#if GTK_MAJOR_VERSION == 1
			have_sliders = TRUE;
#endif
			have_spins = TRUE;

			x = next_table_level(wp[0], TRUE);
			label = gtk_label_new(_(pp[--lp]));
			gtk_widget_show(label);
			gtk_misc_set_alignment(GTK_MISC(label), 1.0 / 3.0, 0.5);
// !!! Padding = 0
			to_table(label, wp[0], 0, x, 0);
			gtk_table_attach(GTK_TABLE(wp[0]), widget, x, x + 1,
				1, 2, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
			break;
		}
		/* Add a named checkbox, fill from field/var */
		case op_CHECK: case op_TLCHECK:
			widget = sig_toggle(_(pp[1]), *(int *)v, NULL, NULL);
// !!! Padding = 0
			tpad = 0;
			pk = pk_PACK;
			if (op >= op_TLCHECK) pk = pk_TABLE;
			break;
		/* Add a named checkbox, fill from inifile */
		case op_CHECKb:
			widget = sig_toggle(_(pp[2]), inifile_get_gboolean(v,
				(int)pp[1]), NULL, NULL);
			pk = pk_PACK;
			break;
		/* Add a (self-reading) pack of radio buttons for field/var */
		case op_RPACK: case op_RPACKD: case op_FRPACK:
		{
			char **src = pp[1];
			int nh = (int)pp[2];
			int n = nh >> 8;
			if (op == op_RPACKD) n = -1 ,
				src = *(char ***)((char *)ddata + (int)pp[1]);
			if (!n) n = -1;
#if U_NLS
			n = n_trans(tc, src, n);
			src = tc;
#endif
			widget = wj_radio_pack(src, n, nh & 255, *(int *)v,
				ref > 1 ? r + 2 : v, // self-update by default
				ref > 1 ? GTK_SIGNAL_FUNC(get_evt_wjr) : NULL);
			*r++ = widget;
			*r++ = pp - 1;
			if (ref > 1) *r++ = res , *r++ = pp + 3; // event
			ref = 0;
			pk = pk_XPACK;
			if (op == op_FRPACK)
			{
				cw = GET_BORDER(FRBOX);
				pk = pk_PACK | pkf_FRAME;
			}
			break;
		}
		/* Add an option menu for field/var */
		case op_OPT: case op_XOPT: case op_TLOPT:
		{
			char **src = pp[1];
			int n = (int)pp[2];
			if (!n) n = -1;
#if U_NLS
			n = n_trans(tc, src, n);
			src = tc;
#endif
			widget = wj_option_menu(src, n, *(int *)v,
				ref > 1 ? r + 2 : NULL,
				ref > 1 ? GTK_SIGNAL_FUNC(get_evt_1) : NULL);
			*r++ = widget;
			*r++ = pp - 1;
			if (ref > 1) *r++ = res , *r++ = pp + 3; // event
			ref = 0;
// !!! Padding = 0
			tpad = 0;
			pk = op == op_TLOPT ? pk_TABLE : pk_PACK;
// !!! Border = 4
			if (op == op_XOPT) cw = 4 , pk = pk_XPACK;
			break;
		}
		/* Add a path box, fill from var/inifile */
		case op_PATH: case op_PATHs:
			widget = mt_path_box(_(pp[1]), wp[0], _(pp[2]),
				(int)pp[3]);
			gtkuncpy(txt, op == op_PATHs ? inifile_get(v, "") : v,
				PATHTXT);
			gtk_entry_set_text(GTK_ENTRY(widget), txt);
			break;
		/* Add a color picker box, w/field array, & leave unfilled(!) */
		case op_COLOR: case op_TCOLOR:
			widget = cpick_create();
			cpick_set_opacity_visibility(widget, op == op_TCOLOR);
			gtk_widget_show(widget);
			have_spins = TRUE;
			pk = pk_PACK;
			break;
		/* Add a colorlist box, fill from fields */
		case op_COLORLIST: case op_COLORLISTN:
		{
			r[0] = widget = colorlist(wp[0], v, ddata, pp - 1, r + 2);
			r[1] = pp - 1;
			r += 2;
			if (pp[6]) *r++ = res , *r++ = pp + 5; // click event
			*r++ = res; *r++ = pp + 3; // select event
			continue;
		}
		/* Add a box with "OK"/"Cancel", or something like */
		case op_OKBOX: case op_EOKBOX:
		{
			GtkWidget *ok_bt, *cancel_bt, *box;

			ag = gtk_accel_group_new();
 			gtk_window_add_accel_group(GTK_WINDOW(window), ag);

			--wp; wp[0] = widget = gtk_hbox_new(TRUE, 0);
			gtk_container_set_border_width(GTK_CONTAINER(widget),
				GET_BORDER(OKBOX));
			gtk_widget_show(widget);
			box = widget;
			if (op == op_EOKBOX) // clustered to right side
			{
				box = gtk_hbox_new(FALSE, 0);
				gtk_widget_show(box);
// !!! Min width = 260
				pack_end(box, widget_align_minsize(widget, 260, -1));
			}
			pack(wp[1], box);
			if (ref < 2) break; // empty box for separate buttons
			*r++ = widget;
			*r++ = pp - 1;

			ok_bt = cancel_bt = gtk_button_new_with_label(_(v));
			gtk_container_set_border_width(GTK_CONTAINER(ok_bt),
				GET_BORDER(OKBTN));
			gtk_widget_show(ok_bt);
			/* OK-event */
			r = add_click(r, res, pp + 2, ok_bt, window);
			if (pp[1])
			{
				cancel_bt = xpack(widget,
					gtk_button_new_with_label(_(pp[1])));
				gtk_container_set_border_width(
					GTK_CONTAINER(cancel_bt), GET_BORDER(OKBTN));
				gtk_widget_show(cancel_bt);
				/* Cancel-event */
				r = add_click(r, res, pp + 4, cancel_bt, window);
			}
			xpack(widget, ok_bt);

			gtk_widget_add_accelerator(cancel_bt, "clicked", ag,
				GDK_Escape, 0, (GtkAccelFlags)0);
			gtk_widget_add_accelerator(ok_bt, "clicked", ag,
				GDK_Return, 0, (GtkAccelFlags)0);
			gtk_widget_add_accelerator(ok_bt, "clicked", ag,
				GDK_KP_Enter, 0, (GtkAccelFlags)0);
			delete_to_click(window, cancel_bt);
			continue;
		}
		/* Add a clickable button to OK-box */
		case op_OKBTN: case op_CANCELBTN: case op_OKADD: case op_OKNEXT:
		{
			*r++ = widget = xpack(wp[0],
				gtk_button_new_with_label(_(v)));
			*r++ = pp - 1;
			gtk_container_set_border_width(GTK_CONTAINER(widget),
				GET_BORDER(OKBTN));
			if (op == op_OKADD) gtk_box_reorder_child(GTK_BOX(wp[0]),
				widget, 1);
			gtk_widget_show(widget);
			if (op == op_OKBTN)
			{
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_Return, 0, (GtkAccelFlags)0);
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_KP_Enter, 0, (GtkAccelFlags)0);
			}
			else if (op == op_CANCELBTN)
			{
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_Escape, 0, (GtkAccelFlags)0);
				delete_to_click(window, widget);
			}
			/* Click-event */
			r = add_click(r, res, pp + 1, widget, window);
			continue;
		}
		/* Add a toggle button to OK-box */
		case op_OKTOGGLE:
			*r++ = widget = xpack(wp[0],
				gtk_toggle_button_new_with_label(_(pp[1])));
			*r++ = pp - 1;
			gtk_container_set_border_width(GTK_CONTAINER(widget),
				GET_BORDER(OKBTN));
			gtk_box_reorder_child(GTK_BOX(wp[0]), widget, 1);
			gtk_widget_show(widget);
			if (pp[3])
			{
				gtk_signal_connect(GTK_OBJECT(widget), "toggled",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				*r++ = res;
				*r++ = pp + 2;
			}
			continue;
		/* Add a clickable button to table slot */
		case op_BUTTON: case op_TLBUTTON:
			*r++ = widget = gtk_button_new_with_label(_(v));
			*r++ = pp - 1;
			gtk_widget_show(widget);
			/* Click-event */
			r = add_click(r, res, pp + 1, widget, NULL);
			ref = 0;
// !!! Padding = 5
			tpad = 5;
			pk = pk_TABLEp;
			if (op == op_TLBUTTON) break;
// !!! Border = 4
			cw = 4;
			pk = pk_XPACK;
			break;
		/* Call a function */
		case op_EXEC:
			r = ((ext_fn)v)(r, &wp);
			continue;
		/* Call a V-code subroutine, indirect from field/var */
// !!! Maybe add opcode for direct call too
		case op_CALLp:
			*rp++ = ifcode;
			ifcode = *(void **)v;
			continue;
		/* Return from V-code subroutine */
		case op_RET:
			ifcode = *--rp;
			continue;
		/* Skip next token(s) if/unless field/var is unset */
		case op_IF: case op_UNLESS:
			if (!*(int *)v ^ (op != op_IF))
				ifcode = skip_if(pp - 1);
			continue;
		/* Store a reference to whatever is next into field */
		case op_REF:
			*(void **)v = r;
			continue;
		/* Make toplevel window shrinkable */
		case op_MKSHRINK:
			gtk_window_set_policy(GTK_WINDOW(window), TRUE, TRUE, FALSE);
			continue;
		/* Make toplevel window non-resizable */
		case op_NORESIZE:
			gtk_window_set_policy(GTK_WINDOW(window), FALSE, FALSE, TRUE);
			continue;
		/* Make scrolled window request max size */
		case op_WANTMAX:
			gtk_signal_connect(GTK_OBJECT(widget), "size_request",
				GTK_SIGNAL_FUNC(scroll_max_size_req), NULL);
			continue;
		/* Set default width for window */
		case op_DEFW:
			gtk_window_set_default_size(GTK_WINDOW(window), (int)v, -1);
			continue;
		/* Make toplevel window be positioned at mouse */
		case op_WPMOUSE: wpos = GTK_WIN_POS_MOUSE; continue;
		/* Make last referrable widget insensitive */
		case op_INSENS:
			gtk_widget_set_sensitive(*origin_slot(r - 2), FALSE);
			continue;
		/* Install Change-event handler */
		case op_EVT_CHANGE:
		{
			void **slot = origin_slot(r - 2);
			int what = GET_OP(slot);
// !!! Support only what actually used on, and their brethren
			switch (what)
			{
			case op_SPIN: case op_XSPIN:
			case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
			case op_SPINa: case op_XSPINa: case op_TSPINa:
			case op_FSPIN:
				spin_connect(*slot,
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_TSPINSLIDE: case op_HTSPINSLIDE:
				mt_spinslide_connect(*slot,
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_CHECK: case op_TLCHECK: case op_CHECKb:
				gtk_signal_connect(GTK_OBJECT(*slot), "toggled",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_COLOR: case op_TCOLOR:
				gtk_signal_connect(GTK_OBJECT(*slot), "color_changed",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			}
		} // fallthrough
		/* Remember that event needs triggering here */
		case op_TRIGGER:
			*r++ = res;
			*r++ = pp - 1;
			continue;
		/* Set nondefault border size */
		case op_BOR_TABLE: case op_BOR_SPIN: case op_BOR_TLLABEL:
		case op_BOR_FRBOX: case op_BOR_OKBOX: case op_BOR_OKBTN:
			borders[op - op_BOR_0] = (int)v - DEF_BORDER;
			continue;
		default: continue;
		}
		/* Remember this */
		if (ref)
		{
			*r++ = widget;
			*r++ = pp - 1;
		}
		*(wp - 1) = widget; // pre-stack
		/* Border this */
		if (cw) gtk_container_set_border_width(GTK_CONTAINER(widget), cw);
		/* Frame this */
		if (pk & pkf_FRAME)
			widget = add_with_frame(NULL, _(pp[--lp]), widget);
		/* Pack this */
		switch (n = pk & pk_MASK)
		{
		case pk_PACK: tpad = 0;
		case pk_PACKp:
			gtk_box_pack_start(GTK_BOX(wp[0]), widget,
				FALSE, FALSE, tpad);
			break;
		case pk_XPACK: xpack(wp[0], widget); break;
		case pk_PACKEND: pack_end(wp[0], widget); break;
		case pk_TABLE: case pk_TABLEx: case pk_TABLEp:
			table_it(wp[0], widget, (int)pp[--lp], tpad, n);
			break;
		case pk_TABLE2: case pk_TABLE2x:
		{
			int y = next_table_level(wp[0], FALSE);
			add_to_table(_(pp[--lp]), wp[0], y, 0, tpad);
			gtk_table_attach(GTK_TABLE(wp[0]), widget, 1, 2,
				y, y + 1, GTK_EXPAND | GTK_FILL,
				n == pk_TABLE2x ? GTK_FILL : 0, 0, tpad);
			break;
		}
		}
		/* Stack this */
		if (pk & pkf_STACK) --wp;
	}
}

static void *do_query(char *data, void **wdata, int mode)
{
	void **pp, *v = NULL;
	int op;

	for (; (pp = wdata[1]); wdata += 2)
	{
		op = (int)*pp++;
		v = op & (~0 << 16) ? pp[0] : NULL;
		if (op & WB_FFLAG) v = data + (int)v;
		switch (op & WB_OPMASK)
		{
		case op_WTAGS:
			// For now, only this
			if (!(mode & 1) && v) update_window_spin(*wdata);
			break;
		case op_SPIN: case op_XSPIN:
		case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
		case op_SPINa: case op_XSPINa: case op_TSPINa:
			*(int *)v = mode & 1 ? gtk_spin_button_get_value_as_int(
				GTK_SPIN_BUTTON(*wdata)) : read_spin(*wdata);
			break;
		case op_FSPIN:
			*(int *)v = rint((mode & 1 ?
				GTK_SPIN_BUTTON(*wdata)->adjustment->value :
				read_float_spin(*wdata)) * 100);
			break;
		case op_TSPINSLIDE: case op_HTSPINSLIDE:
			*(int *)v = (mode & 1 ? mt_spinslide_read_value :
				mt_spinslide_get_value)(*wdata);
			break;
		case op_CHECK: case op_TLCHECK: case op_OKTOGGLE:
			*(int *)v = gtk_toggle_button_get_active(
				GTK_TOGGLE_BUTTON(*wdata));
			break;
		case op_CHECKb:
			inifile_set_gboolean(v, gtk_toggle_button_get_active(
				GTK_TOGGLE_BUTTON(*wdata)));
			break;
		case op_RPACK: case op_RPACKD: case op_FRPACK:
		case op_COLORLIST: case op_COLORLISTN:
			break; // self-reading
		case op_COLOR:
			*(int *)v = cpick_get_colour(*wdata, NULL);
			break;
		case op_TCOLOR:
			*(int *)v = cpick_get_colour(*wdata, (int *)v + 1);
			break;
		case op_OPT: case op_XOPT: case op_TLOPT:
			*(int *)v = wj_option_menu_get_history(*wdata);
			break;
		case op_PATH:
			gtkncpy(v, gtk_entry_get_text(GTK_ENTRY(*wdata)), PATHBUF);
			break;
		case op_PATHs:
		{
			char path[PATHBUF];
			gtkncpy(path, gtk_entry_get_text(GTK_ENTRY(*wdata)), PATHBUF);
			inifile_set(v, path);
			break;
		}
		default: v = NULL; break;
		}
		if (mode > 1) return (v);
	}
	return (NULL);
}

void run_query(void **wdata)
{
	do_query(GET_DDATA(wdata), GET_WINDOW(wdata), 0);
}

void cmd_sensitive(void **slot, int state)
{
	if (GET_OP(slot) < op_EVT_0) // any widget
		gtk_widget_set_sensitive(slot[0], state);
}

void cmd_showhide(void **slot, int state)
{
	if (GET_OP(slot) < op_EVT_0) // any widget
		widget_showhide(slot[0], state);
}

void cmd_set(void **slot, int v)
{
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_TSPINSLIDE: case op_HTSPINSLIDE:
		mt_spinslide_set_value(slot[0], v);
		break;
	case op_SPIN: case op_XSPIN:
	case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
	case op_SPINa: case op_XSPINa: case op_TSPINa:
		gtk_spin_button_set_value(slot[0], v);
		break;
	case op_FSPIN:
		gtk_spin_button_set_value(slot[0], v / 100.0);
		break;
	case op_PLAINBOOK:
		gtk_notebook_set_page(slot[0], v);
		break;
	}
}

void cmd_set2(void **slot, int v0, int v1)
{
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_COLOR: case op_TCOLOR:
		cpick_set_colour(slot[0], v0, v1);
		break;
	case op_COLORLIST: case op_COLORLISTN:
	{
		colorlist_set_color(slot[0], v0, v1);
		break;
	}
	}
}

void cmd_set3(void **slot, int *v)
{
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_TSPINSLIDE: case op_HTSPINSLIDE:
		mt_spinslide_set_range(slot[0], v[1], v[2]);
		mt_spinslide_set_value(slot[0], v[0]);
		break;
	case op_SPIN: case op_XSPIN:
	case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
	case op_SPINa: case op_XSPINa: case op_TSPINa:
		spin_set_range(slot[0], v[1], v[2]);
		gtk_spin_button_set_value(slot[0], v[0]);
		break;
	}
}

void cmd_set4(void **slot, int *v)
{
// !!! Support only what actually used on, and their brethren
	int op = GET_OP(slot);
	if ((op == op_COLOR) || (op == op_TCOLOR))
	{
		cpick_set_colour_previous(slot[0], v[2], v[3]);
		cpick_set_colour(slot[0], v[0], v[1]);
	}
}

/* Passively query one slot, show where the result went */
void *cmd_read(void **slot, void *ddata)
{
	return (do_query(ddata, origin_slot(slot), 3));
}

void cmd_repaint(void **slot)
{
	int op = GET_OP(slot);
	if ((op == op_COLORLIST) || (op == op_COLORLISTN))
	/* Stupid GTK+ does nothing for gtk_widget_queue_draw(allcol_list) */
		gtk_container_foreach(GTK_CONTAINER(slot[0]),
			(GtkCallback)gtk_widget_queue_draw, NULL);
	else gtk_widget_queue_draw(slot[0]);
}

void cmd_scroll(void **slot, int idx)
{
	int op = GET_OP(slot);
	if ((op == op_COLORLIST) || (op == op_COLORLISTN))
		colorlist_scroll_in(slot[0], idx);
}
