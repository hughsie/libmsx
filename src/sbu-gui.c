/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2017 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <locale.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <appstream-glib.h>

#include "generated-gdbus.h"

#include "egg-graph-widget.h"

#include "sbu-common.h"
#include "sbu-config.h"
#include "sbu-database.h"
#include "sbu-gui-resources.h"
#include "sbu-xml-modifier.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SbuManager, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SbuDevice, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SbuElement, g_object_unref)

typedef struct {
	GtkBuilder	*builder;
	GCancellable	*cancellable;
	SbuConfig	*config;
	SbuDatabase	*database;
	SbuManager	*manager;
	SbuDevice	*device;
	GtkSizeGroup	*details_sizegroup_title;
	GtkSizeGroup	*details_sizegroup_value;
	GtkWidget	*graph_widget;
	GPtrArray	*elements;
	guint		 refresh_id;
	gint64		 history_interval;
} SbuGui;

static void
sbu_gui_self_free (SbuGui *self)
{
	if (self->refresh_id != 0)
		g_source_remove (self->refresh_id);
	if (self->device != NULL)
		g_object_unref (self->device);
	if (self->manager != NULL)
		g_object_unref (self->manager);
	g_ptr_array_unref (self->elements);
	g_object_unref (self->builder);
	g_object_unref (self->database);
	g_object_unref (self->config);
	g_object_unref (self->cancellable);
	g_free (self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SbuGui, sbu_gui_self_free)

static int
sbu_gui_commandline_cb (GApplication *application,
			GApplicationCommandLine *cmdline,
			SbuGui *self)
{
	gboolean verbose = FALSE;
	gint argc;
	GtkWindow *window;
	g_auto(GStrv) argv;
	g_autoptr(GOptionContext) context = NULL;

	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
		  /* TRANSLATORS: show verbose debugging */
		  N_("Show extra debugging information"), NULL },
		{ NULL}
	};

	/* get arguments */
	argv = g_application_command_line_get_arguments (cmdline, &argc);

	context = g_option_context_new (NULL);
	/* TRANSLATORS: the program name */
	g_option_context_set_summary (context, _("PowerSBU GUI"));
	g_option_context_add_main_entries (context, options, NULL);
	if (!g_option_context_parse (context, &argc, &argv, NULL))
		return FALSE;

	if (verbose)
		g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

	/* make sure the window is raised */
	window = GTK_WINDOW (gtk_builder_get_object (self->builder, "dialog_main"));
	gtk_window_present (window);
	return TRUE;
}

static void
sbu_gui_add_details_item (SbuGui *self, const gchar *key, gint value)
{
	GtkWidget *widget_title;
	GtkWidget *widget_value;
	GtkWidget *b;
	GtkWidget *widget;
	const gchar *suffix = NULL;
	GString *str = g_string_new (NULL);

	/* format value */
	g_string_append_printf (str, "%.2f", (gdouble) value / 1000.f);

	/* remove unwanted precision */
	if (g_str_has_suffix (str->str, ".00"))
		g_string_truncate (str, str->len - 3);

	/* add suffix */
	if (g_str_has_suffix (key, "Voltage"))
		suffix = "V";
	else if (g_str_has_suffix (key, "Current"))
		suffix = "A";
	else if (g_str_has_suffix (key, "Frequency"))
		suffix = "Hz";
	else if (g_str_has_suffix (key, "ApparentPower"))
		suffix = "VA";
	else if (g_str_has_suffix (key, "Power"))
		suffix = "W";
	else if (g_str_has_suffix (key, "Percentage") ||
		 g_str_has_suffix (key, "Capacity"))
		suffix = "%";
	else if (g_str_has_suffix (key, "Temperature"))
		suffix = "°F";
	if (suffix != NULL)
		g_string_append (str, suffix);

	b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_visible (b, TRUE);

	widget_title = gtk_label_new (key);
	gtk_widget_set_visible (widget_title, TRUE);
	gtk_widget_set_hexpand (widget_title, TRUE);
	gtk_label_set_xalign (GTK_LABEL (widget_title), 1.0);
	gtk_size_group_add_widget (self->details_sizegroup_title, widget_title);
	gtk_container_add (GTK_CONTAINER (b), widget_title);

	widget_value = gtk_label_new (str->str);
	gtk_widget_set_visible (widget_value, TRUE);
	gtk_widget_set_hexpand (widget_value, TRUE);
	gtk_label_set_xalign (GTK_LABEL (widget_value), 0.0);
	gtk_size_group_add_widget (self->details_sizegroup_value, widget_value);
	gtk_container_add (GTK_CONTAINER (b), widget_value);

	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "listbox_details"));
	gtk_list_box_prepend (GTK_LIST_BOX (widget), b);
}

static void
sbu_gui_refresh_details (SbuGui *self)
{
	g_autoptr(GHashTable) results = NULL;
	g_autoptr(GList) keys = NULL;
	g_autoptr(GError) error = NULL;

	/* get all latest entries */
	results = sbu_database_get_latest (self->database, SBU_DEVICE_ID_DEFAULT, &error);
	if (results == NULL) {
		g_warning ("%s", error->message);
		return;
	}
	keys = g_hash_table_get_keys (results);
	for (GList *l = keys; l != NULL; l = l->next) {
		const gchar *key = l->data;
		SbuDatabaseItem *item = g_hash_table_lookup (results, key);
		sbu_gui_add_details_item (self, key, item->val);
	}
}

static GPtrArray *
mxs_gui_get_graph_data (SbuGui *self, const gchar *key, guint32 color, GError **error)
{
	gint64 now = g_get_real_time () / G_USEC_PER_SEC;
	g_autoptr(GPtrArray) data = NULL;
	g_autoptr(GPtrArray) results = NULL;

	results = sbu_database_query (self->database,
				      key,
				      SBU_DEVICE_ID_DEFAULT,
				      now - self->history_interval,
				      now,
				      error);
	if (results == NULL)
		return NULL;
	data = g_ptr_array_new_with_free_func ((GDestroyNotify) egg_graph_point_free);
	for (guint i = 0; i < results->len; i++) {
		SbuDatabaseItem *item = g_ptr_array_index (results, i);
		EggGraphPoint *point = egg_graph_point_new ();
		point->x = item->ts - now;
		point->y = (gdouble) item->val / 1000.f;
		point->color = color;
		g_ptr_array_add (data, point);
	}
	return g_steal_pointer (&data);
}

typedef struct {
	const gchar	*key;
	guint32		 color;
	const gchar	*text;
} PowerSBUGraphLine;

static void
sbu_gui_history_setup_lines (SbuGui *self, PowerSBUGraphLine *lines)
{
	for (guint i = 0; lines[i].key != NULL; i++) {
		g_autoptr(GError) error = NULL;
		g_autoptr(GPtrArray) data = NULL;

		data = mxs_gui_get_graph_data (self, lines[i].key, lines[i].color, &error);
		if (data == NULL) {
			g_warning ("%s", error->message);
			return;
		}
		egg_graph_widget_data_add (EGG_GRAPH_WIDGET (self->graph_widget),
					   EGG_GRAPH_WIDGET_PLOT_POINTS, data);
		egg_graph_widget_key_legend_add	(EGG_GRAPH_WIDGET (self->graph_widget),
						 lines[i].color, lines[i].text);
	}
}

static void
sbu_gui_history_setup_battery_voltage (SbuGui *self)
{
	PowerSBUGraphLine lines[] = {
		{ "BatteryVoltage",		0xff0000,	"BatteryVoltage" },
		{ NULL,				0x000000,	NULL },
	};
	egg_graph_widget_key_legend_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	egg_graph_widget_data_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	g_object_set (self->graph_widget,
		      "use-legend", FALSE,
		      "type-y", EGG_GRAPH_WIDGET_KIND_VOLTAGE,
		      "autorange-y", FALSE,
		      "start-y", (gdouble) 20.f,
		      "stop-y", (gdouble) 30.f,
		      NULL);
	sbu_gui_history_setup_lines (self, lines);
}

static void
sbu_gui_history_setup_mains_voltage (SbuGui *self)
{
	PowerSBUGraphLine lines[] = {
		{ "GridVoltage",		0xff0000,	"GridVoltage" },
		{ "AcOutputVoltage",		0x00ff00,	"AcOutputVoltage" },
		{ NULL,				0x000000,	NULL },
	};
	egg_graph_widget_key_legend_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	egg_graph_widget_data_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	g_object_set (self->graph_widget,
		      "use-legend", TRUE,
		      "type-y", EGG_GRAPH_WIDGET_KIND_VOLTAGE,
		      "autorange-y", FALSE,
		      "start-y", (gdouble) 220.f,
		      "stop-y", (gdouble) 270.f,
		      NULL);
	sbu_gui_history_setup_lines (self, lines);
}

static void
sbu_gui_history_setup_power_usage (SbuGui *self)
{
	PowerSBUGraphLine lines[] = {
		{ "AcOutputPower",		0xff0000,	"AcOutputPower" },
		{ NULL,				0x000000,	NULL },
	};
	egg_graph_widget_key_legend_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	egg_graph_widget_data_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	g_object_set (self->graph_widget,
		      "use-legend", TRUE,
		      "type-y", EGG_GRAPH_WIDGET_KIND_POWER,
		      "autorange-y", TRUE,
		      NULL);
	sbu_gui_history_setup_lines (self, lines);
}

static void
sbu_gui_history_setup_current (SbuGui *self)
{
	PowerSBUGraphLine lines[] = {
		{ "PvInputCurrentForBattery",	0xff0000,	"PvInputCurrentForBattery" },
		{ "BatteryCurrent",		0x00ff00,	"BatteryCurrent" },
		{ "BatteryDischargeCurrent",	0x0000ff,	"BatteryDischargeCurrent" },
		{ NULL,				0x000000,	NULL },
	};
	egg_graph_widget_key_legend_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	egg_graph_widget_data_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	g_object_set (self->graph_widget,
		      "use-legend", TRUE,
		      "type-y", EGG_GRAPH_WIDGET_KIND_CURRENT,
		      "autorange-y", FALSE,
		      "start-y", (gdouble) 0.f,
		      "stop-y", (gdouble) 15.f,
		      NULL);
	sbu_gui_history_setup_lines (self, lines);
}

static void
sbu_gui_history_setup_panel_voltage (SbuGui *self)
{
	PowerSBUGraphLine lines[] = {
		{ "PvInputVoltage",		0xff0000,	"PvInputVoltage" },
		{ "BatteryVoltageFromScc",	0x00ff00,	"BatteryVoltageFromScc" },
		{ NULL,				0x000000,	NULL },
	};
	egg_graph_widget_key_legend_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	egg_graph_widget_data_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	g_object_set (self->graph_widget,
		      "use-legend", TRUE,
		      "type-y", EGG_GRAPH_WIDGET_KIND_VOLTAGE,
		      "autorange-y", FALSE,
		      "start-y", (gdouble) 0.f,
		      "stop-y", (gdouble) 40.f,
		      NULL);
	sbu_gui_history_setup_lines (self, lines);
}

static void
sbu_gui_history_setup_status (SbuGui *self)
{
	PowerSBUGraphLine lines[] = {
		{ "ChargingOn",			0xff0000,	"ChargingOn" },
		{ "ChargingOnSolar",		0x00ff00,	"ChargingOnSolar" },
		{ "ChargingOnAC",		0x0000ff,	"ChargingOnAC" },
		{ NULL,				0x000000,	NULL },
	};
	egg_graph_widget_key_legend_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	egg_graph_widget_data_clear (EGG_GRAPH_WIDGET (self->graph_widget));
	g_object_set (self->graph_widget,
		      "use-legend", TRUE,
		      "type-y", EGG_GRAPH_WIDGET_KIND_FACTOR,
		      "autorange-y", FALSE,
		      "start-y", (gdouble) 0.f,
		      "stop-y", (gdouble) 1.f,
		      NULL);
	sbu_gui_history_setup_lines (self, lines);
}

static void
sbu_gui_history_refresh_graph (SbuGui *self)
{
	GtkListBox *list_box;
	GtkListBoxRow *row;

	list_box = GTK_LIST_BOX (gtk_builder_get_object (self->builder, "listbox_history"));
	row = gtk_list_box_get_selected_row (list_box);
	if (row == NULL)
		return;
	switch (gtk_list_box_row_get_index (row)) {
	case 0:
		sbu_gui_history_setup_battery_voltage (self);
		break;
	case 1:
		sbu_gui_history_setup_mains_voltage (self);
		break;
	case 2:
		sbu_gui_history_setup_power_usage (self);
		break;
	case 3:
		sbu_gui_history_setup_current (self);
		break;
	case 4:
		sbu_gui_history_setup_panel_voltage (self);
		break;
	case 5:
		sbu_gui_history_setup_status (self);
		break;
	default:
		break;
	}
}

static void
sbu_gui_history_row_selected_cb (GtkListBox *box, GtkListBoxRow *row, SbuGui *self)
{
	sbu_gui_history_refresh_graph (self);
}

static void
sbu_gui_add_history_item (SbuGui *self, const gchar *title)
{
	GtkWidget *widget_title;
	GtkWidget *widget;

	widget_title = gtk_label_new (title);
	gtk_widget_set_visible (widget_title, TRUE);
	gtk_widget_set_margin_start (widget_title, 12);
	gtk_widget_set_margin_end (widget_title, 12);
	gtk_widget_set_margin_top (widget_title, 6);
	gtk_widget_set_margin_bottom (widget_title, 6);
	gtk_label_set_xalign (GTK_LABEL (widget_title), 0.0);

	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "listbox_history"));
	gtk_list_box_prepend (GTK_LIST_BOX (widget), widget_title);
}

static void
sbu_gui_refresh_history (SbuGui *self)
{
	sbu_gui_add_history_item (self, "Status");
	sbu_gui_add_history_item (self, "Panel Voltage");
	sbu_gui_add_history_item (self, "Current Flow");
	sbu_gui_add_history_item (self, "Power Usage");
	sbu_gui_add_history_item (self, "Utility Voltage");
	sbu_gui_add_history_item (self, "Battery Voltage");
}

static SbuElement *
sbu_gui_get_element_by_type (SbuGui *self, SbuElementKind kind)
{
	for (guint i = 0; i < self->elements->len; i++) {
		SbuElement *element = g_ptr_array_index (self->elements, i);
		SbuElementKind kind_tmp;
		g_object_get (element, "kind", &kind_tmp, NULL);
		if (kind == kind_tmp)
			return element;
	}
	return NULL;
}

static gchar *
sbu_gui_format_number (gdouble val, const gchar *suffix)
{
	GString *str = g_string_new (NULL);
	gboolean kilo = FALSE;
	const guint numdigits = 4;

	/* big number */
	if (val > 1000) {
		kilo = TRUE;
		g_string_printf (str, "%.2fk", val / 1000);
	} else {
		g_string_printf (str, "%.2f", val);
	}
	if (g_str_has_suffix (str->str, ".00"))
		g_string_truncate (str, str->len - 3);
	if (str->len > numdigits - kilo)
		g_string_truncate (str, numdigits - kilo);
	if (g_str_has_suffix (str->str, "."))
		g_string_truncate (str, str->len - 1);
	if (kilo)
		g_string_append (str, "k");
	g_string_append (str, suffix);
	return g_string_free (str, FALSE);
}

static gchar *
sbu_gui_format_voltage (SbuElement *element)
{
	if (sbu_element_get_voltage_max (element) > 0) {
		g_autofree gchar *tmp1 = sbu_gui_format_number (sbu_element_get_voltage (element), "V");
		g_autofree gchar *tmp2 = sbu_gui_format_number (sbu_element_get_voltage_max (element), "V");
		return g_strdup_printf ("%s/%s", tmp1, tmp2);
	}
	return sbu_gui_format_number (sbu_element_get_voltage (element), "V");
}

static gchar *
sbu_gui_format_watts (SbuElement *element)
{
	if (sbu_element_get_power_max (element) > 0) {
		g_autofree gchar *tmp1 = sbu_gui_format_number (sbu_element_get_power (element), "W");
		g_autofree gchar *tmp2 = sbu_gui_format_number (sbu_element_get_power_max (element), "W");
		return g_strdup_printf ("%s/%s", tmp1, tmp2);
	}
	return sbu_gui_format_number (sbu_element_get_power (element), "W");
}

#define SBU_SVG_ID_TEXT_SERIAL_NUMBER		"tspan8822"
#define SBU_SVG_ID_TEXT_FIRMWARE_VERSION	"tspan8818"
#define SBU_SVG_ID_TEXT_DEVICE_MODEL		"tspan8814"
#define SBU_SVG_ID_TEXT_SOLAR2UTILITY		"tspan8408"
#define SBU_SVG_ID_TEXT_BATVOLT			"tspan8424"
#define SBU_SVG_ID_TEXT_BAT2LOAD		"tspan8438"
#define SBU_SVG_ID_TEXT_LOAD			"tspan8442"
#define SBU_SVG_ID_TEXT_SOLAR			"tspan8410"
#define SBU_SVG_ID_TEXT_UTILITY			"tspan8725"
#define SBU_SVG_ID_PATH_SOLAR2UTILITY		"path5032"

#define SBU_SVG_OFFSCREEN			"-999"

static void
sbu_gui_refresh_overview (SbuGui *self)
{
	GtkWidget *widget;
	g_autofree gchar *data = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GdkPixbuf) pixbuf = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GInputStream) stream = NULL;
	g_autoptr(GString) svg_data = NULL;
	SbuElement *element_battery;
	SbuElement *element_load;
	SbuElement *element_solar;
	SbuElement *element_utility;
	g_autoptr(SbuXmlModifier) xml_mod = sbu_xml_modifier_new ();

	/* load GResource */
	bytes = g_resource_lookup_data (sbu_get_resource (),
					"/com/hughski/PowerSBU/sbu-overview.svg",
					G_RESOURCE_LOOKUP_FLAGS_NONE,
					&error);
	if (bytes == NULL) {
		g_warning ("failed to load image: %s", error->message);
		return;
	}

	/* load XML */
	sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_SERIAL_NUMBER,
					sbu_device_get_serial_number (self->device));
	sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_FIRMWARE_VERSION,
					sbu_device_get_firmware_version (self->device));
	sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_DEVICE_MODEL,
					sbu_device_get_description (self->device));

	/* not supported yet */
	sbu_xml_modifier_replace_attr (xml_mod, SBU_SVG_ID_TEXT_SOLAR2UTILITY,
				       "x", SBU_SVG_OFFSCREEN);
	sbu_xml_modifier_replace_attr (xml_mod, SBU_SVG_ID_PATH_SOLAR2UTILITY,
				       "style", "");

	/* replace string data */
	element_battery = sbu_gui_get_element_by_type (self, SBU_ELEMENT_KIND_BATTERY);
	if (element_battery != NULL) {
		g_autofree gchar *tmp = sbu_gui_format_voltage (element_battery);
		g_autofree gchar *tmp2 = sbu_gui_format_watts (element_battery);
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_BATVOLT, tmp);
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_BAT2LOAD, tmp2);
	} else {
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_BATVOLT, "?");
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_BAT2LOAD, "?");
	}

	element_load = sbu_gui_get_element_by_type (self, SBU_ELEMENT_KIND_LOAD);
	if (element_load != NULL) {
		g_autofree gchar *tmp = sbu_gui_format_watts (element_load);
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_LOAD, tmp);
	} else {
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_LOAD, "?");
	}

	element_solar = sbu_gui_get_element_by_type (self, SBU_ELEMENT_KIND_SOLAR);
	if (element_solar != NULL) {
		g_autofree gchar *tmp = sbu_gui_format_watts (element_solar);
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_SOLAR, tmp);
	} else {
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_SOLAR, "?");
	}

	element_utility = sbu_gui_get_element_by_type (self, SBU_ELEMENT_KIND_UTILITY);
	if (element_utility != NULL) {
		g_autofree gchar *tmp = sbu_gui_format_watts (element_utility);
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_UTILITY, tmp);
	} else {
		sbu_xml_modifier_replace_cdata (xml_mod, SBU_SVG_ID_TEXT_UTILITY, "?");
	}

	/* process replacements */
	svg_data = sbu_xml_modifier_process (xml_mod,
					     g_bytes_get_data (bytes, NULL),
					     g_bytes_get_size (bytes),
					     &error);
	if (svg_data == NULL) {
		g_warning ("failed to modify the SVG image: %s", error->message);
		return;
	}

	/* load as image */
	stream = g_memory_input_stream_new_from_data (svg_data->str,
						      (gssize) svg_data->len,
						      NULL);
	pixbuf = gdk_pixbuf_new_from_stream_at_scale (stream, -1, 600, TRUE, NULL, &error);
	if (pixbuf == NULL) {
		g_warning ("failed to load image: %s", error->message);
		return;
	}
	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "image_overview"));
	gtk_image_set_from_pixbuf (GTK_IMAGE (widget), pixbuf);
}

static gboolean
sbu_gui_element_notify_delay_cb (gpointer user_data)
{
	SbuGui *self = (SbuGui *) user_data;
	sbu_gui_refresh_overview (self);
	self->refresh_id = 0;
	return FALSE;
}

static void
sbu_gui_element_notify_cb (GObject *gobject, GParamSpec *pspec, gpointer user_data)
{
	SbuGui *self = (SbuGui *) user_data;
	if (self->refresh_id != 0)
		g_source_remove (self->refresh_id);
	self->refresh_id = g_timeout_add (100, sbu_gui_element_notify_delay_cb, self);
}

static gboolean
sbu_gui_update_default_device (SbuGui *self, GError **error)
{
	g_auto(GStrv) devices = NULL;
	g_auto(GStrv) elements = NULL;

	/* get default device */
	if (!sbu_manager_call_get_devices_sync (self->manager,
						&devices,
						self->cancellable,
						error))
		return FALSE;
	if (g_strv_length (devices) == 0) {
		g_set_error_literal (error, 1, 0, "no devices!");
		return FALSE;
	}

	/* just use the first device we find */
	g_debug ("using device: %s", devices[0]);
	self->device = sbu_device_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
							  G_DBUS_PROXY_FLAGS_NONE,
							  SBU_DBUS_NAME,
							  devices[0],
							  self->cancellable,
							  error);
	if (self->device == NULL)
		return FALSE;

	/* show each element */
	if (!sbu_device_call_get_elements_sync (self->device,
						&elements,
						self->cancellable,
						error))
		return FALSE;
	for (guint i = 0; elements[i] != NULL; i++) {
		g_autoptr(SbuElement) element = NULL;
		g_debug ("using element: %s", elements[i]);
		element = sbu_element_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
							      G_DBUS_PROXY_FLAGS_NONE,
							      SBU_DBUS_NAME,
							      elements[i],
							      self->cancellable,
							      error);
		if (element == NULL)
			return FALSE;
		g_signal_connect (element, "notify",
				  G_CALLBACK (sbu_gui_element_notify_cb),
				  self);
		g_ptr_array_add (self->elements, g_object_ref (element));
	}

	/* initial load */
	sbu_gui_refresh_overview (self);
	return TRUE;
}

static void
sbu_gui_history_interval_changed_cb (GtkComboBox *combo_box, SbuGui *self)
{
	switch (gtk_combo_box_get_active (combo_box)) {
	case 0:
		self->history_interval = 60 * 60;
		break;
	case 1:
		self->history_interval = 24 * 60 * 60;
		break;
	case 2:
		self->history_interval = 7 * 24 * 60 * 60;
		break;
	case 3:
		self->history_interval = 30 * 24 * 60 * 60;
		break;
	default:
		g_assert_not_reached ();
	}
	sbu_gui_history_refresh_graph (self);
}

static void
sbu_gui_startup_cb (GApplication *application, SbuGui *self)
{
	GtkWidget *widget;
	GtkWindow *window;
	guint retval;
	g_autofree gchar *location = NULL;
	g_autoptr(GError) error = NULL;

	/* get UI */
	retval = gtk_builder_add_from_resource (self->builder,
						"/com/hughski/PowerSBU/sbu-gui.ui",
						&error);
	if (retval == 0) {
		g_warning ("failed to load ui: %s", error->message);
		return;
	}

	window = GTK_WINDOW (gtk_builder_get_object (self->builder, "dialog_main"));
	gtk_window_set_application (window, GTK_APPLICATION (application));
	gtk_window_set_default_size (window, 1200, 500);
	gtk_window_set_application (window, GTK_APPLICATION (application));
	gtk_window_set_default_icon_name ("battery-good-charging");

	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "dialog_main"));
	gtk_widget_show (widget);

	/* load database */
	location = sbu_config_get_string (self->config, "DatabaseLocation", &error);
	if (location == NULL) {
		g_warning ("failed to load config: %s", error->message);
		return;
	}
	sbu_database_set_location (self->database, location);
	if (!sbu_database_open (self->database, &error)) {
		g_warning ("failed to load database: %s", error->message);
		return;
	}

	/* find the device to use */
	self->manager = sbu_manager_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
							    G_DBUS_PROXY_FLAGS_NONE,
							    SBU_DBUS_NAME,
							    SBU_DBUS_PATH_MANAGER,
							    self->cancellable,
							    &error);
	if (self->manager == NULL) {
		g_warning ("failed to connect to daemon: %s", error->message);
		return;
	}
	g_debug ("daemon version: %s", sbu_manager_get_version (self->manager));
	if (!sbu_gui_update_default_device (self, &error)) {
		g_warning ("failed to get device: %s", error->message);
		return;
	}

	/* set up overview page */
	sbu_gui_refresh_overview (self);

	/* set up history page */
	self->graph_widget = egg_graph_widget_new ();
	gtk_widget_set_visible (self->graph_widget, TRUE);
	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "box_history"));
	gtk_box_pack_start (GTK_BOX (widget), self->graph_widget, TRUE, TRUE, 0);
	gtk_widget_set_size_request (self->graph_widget, 500, 250);
	g_object_set (self->graph_widget,
		      "type-x", EGG_GRAPH_WIDGET_KIND_TIME,
		      "autorange-x", TRUE,
		      NULL);
	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "listbox_history"));
	g_signal_connect (widget, "row-selected",
			  G_CALLBACK (sbu_gui_history_row_selected_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "combobox_timespan"));
	g_signal_connect (widget, "changed",
			  G_CALLBACK (sbu_gui_history_interval_changed_cb), self);
	gtk_combo_box_set_active (GTK_COMBO_BOX (widget), 1);

	/* populate pages */
	sbu_gui_refresh_details (self);
	sbu_gui_refresh_history (self);
}

static SbuGui *
sbu_gui_self_new (void)
{
	SbuGui *self = g_new0 (SbuGui, 1);
	self->cancellable = g_cancellable_new ();
	self->config = sbu_config_new ();
	self->database = sbu_database_new ();
	self->builder = gtk_builder_new ();
	self->elements = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	self->details_sizegroup_title = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
	self->details_sizegroup_value = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
	return self;
}

int
main (int argc, char *argv[])
{
	g_autoptr(GtkApplication) application = NULL;
	g_autoptr(SbuGui) self = sbu_gui_self_new ();

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* are we already activated? */
	application = gtk_application_new ("com.hughski.PowerSBU",
					   G_APPLICATION_HANDLES_COMMAND_LINE);
	g_signal_connect (application, "startup",
			  G_CALLBACK (sbu_gui_startup_cb), self);
	g_signal_connect (application, "command-line",
			  G_CALLBACK (sbu_gui_commandline_cb), self);

	/* run */
	return g_application_run (G_APPLICATION (application), argc, argv);
}