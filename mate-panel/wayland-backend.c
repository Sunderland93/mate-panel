/*
 * wayland-backend.c: Support for running on Wayland compositors
 *
 * Copyright (C) 2019 William Wold
 * Copyright (C) 2019-2026 MATE Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Authors:
 *     William Wold <wm@wmww.sh>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>

#include <gtk/gtk.h>
#include <gtk-layer-shell.h>

#include "wayland-backend.h"

static void
wayland_panel_toplevel_apply_anchors (PanelToplevel*   toplevel,
				      PanelOrientation orientation,
				      gboolean         expand)
{
	GtkWindow* window;
	gboolean anchor[GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER];

	window = GTK_WINDOW (toplevel);

	for (int i = 0; i < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; i++)
		anchor[i] = expand;

	switch (orientation) {
	case PANEL_ORIENTATION_LEFT:
		anchor[GTK_LAYER_SHELL_EDGE_LEFT] = TRUE;
		anchor[GTK_LAYER_SHELL_EDGE_RIGHT] = FALSE;
		break;
	case PANEL_ORIENTATION_RIGHT:
		anchor[GTK_LAYER_SHELL_EDGE_RIGHT] = TRUE;
		anchor[GTK_LAYER_SHELL_EDGE_LEFT] = FALSE;
		break;
	case PANEL_ORIENTATION_TOP:
		anchor[GTK_LAYER_SHELL_EDGE_TOP] = TRUE;
		anchor[GTK_LAYER_SHELL_EDGE_BOTTOM] = FALSE;
		break;
	case PANEL_ORIENTATION_BOTTOM:
		anchor[GTK_LAYER_SHELL_EDGE_BOTTOM] = TRUE;
		anchor[GTK_LAYER_SHELL_EDGE_TOP] = FALSE;
		break;
	default:
		g_warning ("Invalid panel orientation %d", orientation);
	}

	for (int i = 0; i < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; i++)
		gtk_layer_set_anchor (window, i, anchor[i]);
}

/* size-allocate handler restoring the real anchors after the size change */
static void
wayland_panel_toplevel_set_anchor (GtkWidget      *widget,
				   GtkAllocation  *alloc,
				   gpointer        data)
{
	PanelToplevel* toplevel = data;

	gulong id = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (toplevel),
							 "wayland-set-anchor-id"));
	if (id != 0)
		g_signal_handler_disconnect (G_OBJECT (toplevel), id);
	g_object_set_data (G_OBJECT (toplevel), "wayland-set-anchor-id", NULL);

	wayland_panel_toplevel_apply_anchors (toplevel,
		panel_toplevel_get_orientation (toplevel),
		panel_toplevel_get_expand (toplevel));
}

/* idle handler switching to the two-anchor {top,left} corner state */
static void
wayland_panel_toplevel_set_anchor_default (gpointer data)
{
	PanelToplevel* toplevel = data;
	GtkWindow* window;
	GtkWidget *widget;

	if (g_object_get_data (G_OBJECT (toplevel), "wayland-set-anchor-id") == NULL) {
		window = GTK_WINDOW (toplevel);
		gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
		gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
		gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
		gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_RIGHT, FALSE);

		widget = GTK_WIDGET (toplevel);
		gulong id = g_signal_connect (widget, "size-allocate",
					      G_CALLBACK (wayland_panel_toplevel_set_anchor),
					      toplevel);
		g_object_set_data (G_OBJECT (toplevel), "wayland-set-anchor-id",
				   GSIZE_TO_POINTER (id));
	}

	g_object_set_data (G_OBJECT (toplevel), "wayland-set-anchor-default-id", NULL);
}

/* schedule the corner state switch; must be called before a size change */
static void
wayland_panel_toplevel_queue_anchor_default (PanelToplevel* toplevel)
{
	guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (toplevel),
							"wayland-set-anchor-default-id"));

	if (id != 0)
		g_source_remove (id);

	id = g_idle_add ((GSourceFunc) wayland_panel_toplevel_set_anchor_default, toplevel);
	g_object_set_data (G_OBJECT (toplevel), "wayland-set-anchor-default-id",
			   GUINT_TO_POINTER (id));
}

static gboolean
wayland_panel_toplevel_placement_changed (PanelToplevel*   toplevel,
					  PanelOrientation orientation,
					  gboolean         expand)
{
	gpointer prev = g_object_get_data (G_OBJECT (toplevel),
					   "wayland-panel-placement");
	PanelOrientation prev_orientation =
		prev != NULL ? (PanelOrientation) GPOINTER_TO_INT (prev) : orientation;
	gboolean prev_expand = g_object_get_data (G_OBJECT (toplevel),
						  "wayland-panel-expand") != NULL;
	gboolean changed = prev != NULL &&
		(prev_orientation != orientation || prev_expand != expand);

	g_object_set_data (G_OBJECT (toplevel), "wayland-panel-placement",
			   GINT_TO_POINTER (orientation));
	g_object_set_data (G_OBJECT (toplevel), "wayland-panel-expand",
			   expand ? GINT_TO_POINTER (1) : NULL);

	return changed;
}

void
wayland_panel_toplevel_update_placement (PanelToplevel* toplevel)
{
	gboolean expand;
	PanelOrientation orientation;

	orientation = panel_toplevel_get_orientation (toplevel);
	expand = panel_toplevel_get_expand (toplevel);

	if (wayland_panel_toplevel_placement_changed (toplevel, orientation, expand)) {
		/* the panel size is going to change: let it resize in the
		 * two-anchor corner state first */
		g_object_set_data (G_OBJECT (toplevel), "wayland-placement-orient",
				   GINT_TO_POINTER (orientation));
		if (expand)
			g_object_set_data (G_OBJECT (toplevel), "wayland-placement-expand",
					   GINT_TO_POINTER (1));
		else
			g_object_set_data (G_OBJECT (toplevel), "wayland-placement-expand", NULL);

		wayland_panel_toplevel_queue_anchor_default (toplevel);
		return;
	}

	wayland_panel_toplevel_apply_anchors (toplevel, orientation, expand);
}

void
wayland_panel_toplevel_init (PanelToplevel* toplevel)
{
	GtkWindow* window;

	window = GTK_WINDOW (toplevel);
	gtk_layer_init_for_window (window);
	gtk_layer_set_layer (window, GTK_LAYER_SHELL_LAYER_TOP);
	gtk_layer_set_namespace (window, "panel");
	wayland_panel_toplevel_update_placement (toplevel);
}

void
wayland_panel_toplevel_move_resize (PanelToplevel           *toplevel,
                                    const GdkRectangle      *geometry,
                                    const GdkRectangle      *monitor_geom)
{
	GtkWindow* window;
	gint margin[GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER];

	window = GTK_WINDOW (toplevel);

	margin[GTK_LAYER_SHELL_EDGE_LEFT] = geometry->x - monitor_geom->x;
	margin[GTK_LAYER_SHELL_EDGE_TOP]  = geometry->y - monitor_geom->y;
	margin[GTK_LAYER_SHELL_EDGE_RIGHT] =
		monitor_geom->x + monitor_geom->width - (geometry->x + geometry->width);
	margin[GTK_LAYER_SHELL_EDGE_BOTTOM] =
		monitor_geom->y + monitor_geom->height - (geometry->y + geometry->height);

	/*
	 * gtk-layer-shell driven layer surfaces never resize the underlying
	 * GDK window themselves; if the widget was reallocated to a different
	 * size (e.g. after an orientation change) the compositor keeps showing
	 * the stale surface footprint. Force the GDK window to the geometry
	 * size and invalidate it, so a correctly sized buffer is committed.
	 */
	{
		GdkWindow *gdk = gtk_widget_get_window (GTK_WIDGET (window));
		if (gdk && (gdk_window_get_width (gdk) != geometry->width ||
			    gdk_window_get_height (gdk) != geometry->height)) {
			gdk_window_resize (gdk, geometry->width, geometry->height);
			gdk_window_invalidate_rect (gdk, NULL, FALSE);
		}
	}

	for (int i = 0; i < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; i++)
		gtk_layer_set_margin (window, i, margin[i]);
}

void
wayland_panel_toplevel_update_exclusive_zone (PanelToplevel* toplevel,
                                              gint           exclusive_zone)
{
	gtk_layer_set_exclusive_zone (GTK_WINDOW (toplevel), exclusive_zone);
}
