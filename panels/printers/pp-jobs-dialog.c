/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright 2012  Red Hat, Inc,
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Marek Kasik <mkasik@redhat.com>
 */

#include "config.h"

#include <unistd.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdesktop-enums.h>

#include <cups/cups.h>

#include "shell/list-box-helper.h"
#include "pp-jobs-dialog.h"
#include "pp-utils.h"
#include "pp-job.h"

#define EMPTY_TEXT "\xe2\x80\x94"

#define CLOCK_SCHEMA "org.gnome.desktop.interface"
#define CLOCK_FORMAT_KEY "clock-format"

static void pp_jobs_dialog_hide (PpJobsDialog *dialog);

struct _PpJobsDialog {
  GtkBuilder *builder;
  GtkWidget  *parent;

  GtkWidget  *dialog;
  GtkListBox *listbox;

  UserResponseCallback user_callback;
  gpointer             user_data;

  gchar *printer_name;

  cups_job_t *jobs;
  gint num_jobs;

  gint ref_count;
};

static void
job_process_cb_cb (gpointer user_data)
{
}

static void
job_stop_cb (GtkButton *button,
             gint       job_id)
{
  job_cancel_purge_async (job_id,
                          FALSE,
                          NULL,
                          job_process_cb_cb,
                          NULL);
}

static void
job_pause_cb (GtkButton *button,
              gpointer   user_data)
{
  PpJob *job = (PpJob *)user_data;
  gint job_id, job_state;

  g_object_get (job, "id", &job_id, NULL);
  g_object_get (job, "state", &job_state, NULL);

  job_set_hold_until_async (job_id,
                            job_state == IPP_JOB_HELD ? "no-hold" : "indefinite",
                            NULL,
                            job_process_cb_cb,
                            NULL);

  gtk_button_set_image (button,
                        gtk_image_new_from_icon_name (job_state == IPP_JOB_HELD ?
                                                      "media-playback-pause-symbolic" : "media-playback-start-symbolic",
                                                      GTK_ICON_SIZE_SMALL_TOOLBAR));
}

static GtkWidget *
create_listbox_row (gpointer item,
                    gpointer user_data)
{
  PpJob *job = (PpJob *)item;
  GtkWidget *box, *widget;
  gchar *time_string, *state_string;
  gint *job_id, job_state = NULL;

  g_object_get (job, "id", &job_id, NULL);
  g_object_get (job, "state", &job_state, NULL);
  g_object_get (job, "time_string", &time_string, NULL);

  switch (job_state)
  {
    case IPP_JOB_PENDING:
      /* Translators: Job's state (job is waiting to be printed) */
      state_string = g_strdup (C_("print job", "Pending"));
      break;
    case IPP_JOB_HELD:
      /* Translators: Job's state (job is held for printing) */
      state_string = g_strdup (C_("print job", "Held"));
      break;
    case IPP_JOB_PROCESSING:
      /* Translators: Job's state (job is currently printing) */
      state_string = g_strdup (C_("print job", "Processing"));
      break;
    case IPP_JOB_STOPPED:
      /* Translators: Job's state (job has been stopped) */
      state_string = g_strdup (C_("print job", "Stopped"));
      break;
    case IPP_JOB_CANCELED:
      /* Translators: Job's state (job has been canceled) */
      state_string = g_strdup (C_("print job", "Canceled"));
      break;
    case IPP_JOB_ABORTED:
      /* Translators: Job's state (job has aborted due to error) */
      state_string = g_strdup (C_("print job", "Aborted"));
      break;
    case IPP_JOB_COMPLETED:
      /* Translators: Job's state (job has completed successfully) */
      state_string = g_strdup (C_("print job", "Completed"));
      break;
  }

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  g_object_set (box, "margin", 6, NULL);
  gtk_widget_set_tooltip_text (box, time_string);
  gtk_container_set_border_width (GTK_CONTAINER (box), 2);

  widget = gtk_label_new ("");
  g_object_bind_property (job, "title", widget, "label", G_BINDING_SYNC_CREATE);
  gtk_widget_set_halign (widget, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (box), widget, TRUE, TRUE, 10);

  widget = gtk_label_new (state_string);
  gtk_widget_set_halign (widget, GTK_ALIGN_END);
  gtk_widget_set_margin_end (widget, 64);
  gtk_box_pack_start (GTK_BOX (box), widget, FALSE, FALSE, 10);

  widget = gtk_button_new_from_icon_name (job_state == IPP_JOB_HELD ? "media-playback-start-symbolic" : "media-playback-pause-symbolic",
                                          GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (widget, "clicked", G_CALLBACK (job_pause_cb), item);
  gtk_box_pack_start (GTK_BOX (box), widget, FALSE, FALSE, 4);

  widget = gtk_button_new_from_icon_name ("media-playback-stop-symbolic",
                                          GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (widget, "clicked", G_CALLBACK (job_stop_cb), job_id);
  gtk_box_pack_start (GTK_BOX (box), widget, FALSE, FALSE, 4);

  gtk_widget_show_all (box);

  return box;
}

static void
update_jobs_list_cb (cups_job_t *jobs,
                     gint        num_of_jobs,
                     gpointer    user_data)
{
  PpJobsDialog     *dialog = (PpJobsDialog *) user_data;
  GListStore       *store;
  GSettings        *settings;
  gint              i;

  store = g_list_store_new (pp_job_get_type ());

  if (dialog->num_jobs > 0)
    cupsFreeJobs (dialog->num_jobs, dialog->jobs);

  dialog->num_jobs = num_of_jobs;
  dialog->jobs = jobs;

  for (i = 0; i < dialog->num_jobs; i++)
    {
      GDesktopClockFormat  value;
      GDateTime           *time;
      struct tm           *ts;
      gchar               *time_string;
      gchar               *state = NULL;
      PpJob               *job;

      ts = localtime (&(dialog->jobs[i].creation_time));
      time = g_date_time_new_local (ts->tm_year + 1900,
                                    ts->tm_mon + 1,
                                    ts->tm_mday,
                                    ts->tm_hour,
                                    ts->tm_min,
                                    ts->tm_sec);

      settings = g_settings_new (CLOCK_SCHEMA);
      value = g_settings_get_enum (settings, CLOCK_FORMAT_KEY);

      if (value == G_DESKTOP_CLOCK_FORMAT_24H)
        time_string = g_date_time_format (time, "%k:%M");
      else
        time_string = g_date_time_format (time, "%l:%M %p");

      g_date_time_unref (time);

      job = g_object_new (pp_job_get_type (),
                          "id", dialog->jobs[i].id,
                          "title", dialog->jobs[i].title,
                          "state", dialog->jobs[i].state,
                          "time_string", time_string,
                          NULL);
      g_list_store_append (store, job);

      g_object_unref (job);
      g_free (time_string);
      g_free (state);
    }

  gtk_list_box_bind_model (dialog->listbox, G_LIST_MODEL (store),
                           create_listbox_row, NULL, NULL);

  gtk_container_add (GTK_CONTAINER (dialog->listbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  dialog->ref_count--;
}

static void
update_jobs_list (PpJobsDialog *dialog)
{
  if (dialog->printer_name)
    {
      dialog->ref_count++;
      cups_get_jobs_async (dialog->printer_name,
                           TRUE,
                           CUPS_WHICHJOBS_ACTIVE,
                           update_jobs_list_cb,
                           dialog);
    }
}

static void
jobs_dialog_response_cb (GtkDialog *dialog,
                         gint       response_id,
                         gpointer   user_data)
{
  PpJobsDialog *jobs_dialog = (PpJobsDialog*) user_data;

  pp_jobs_dialog_hide (jobs_dialog);

  jobs_dialog->user_callback (GTK_DIALOG (jobs_dialog->dialog),
                              response_id,
                              jobs_dialog->user_data);
}

static void
on_clear_all_button_clicked (GtkButton *button,
                             gpointer   user_data)
{
  PpJobsDialog *dialog = (PpJobsDialog *) user_data;

  job_cancel_purge_all_async (dialog->jobs,
                              dialog->num_jobs,
                              FALSE,
                              NULL,
                              job_process_cb_cb,
                              NULL);
}

PpJobsDialog *
pp_jobs_dialog_new (GtkWindow            *parent,
                    UserResponseCallback  user_callback,
                    gpointer              user_data,
                    gchar                *printer_name)
{
  PpJobsDialog    *dialog;
  GtkButton       *clear_all_button;
  GError          *error = NULL;
  gchar           *objects[] = { "jobs-dialog", NULL };
  guint            builder_result;
  gchar           *title;

  dialog = g_new0 (PpJobsDialog, 1);

  dialog->builder = gtk_builder_new ();
  dialog->parent = GTK_WIDGET (parent);

  builder_result = gtk_builder_add_objects_from_resource (dialog->builder,
                                                          "/org/gnome/control-center/printers/jobs-dialog.ui",
                                                          objects, &error);

  if (builder_result == 0)
    {
      g_warning ("Could not load ui: %s", error->message);
      g_error_free (error);
      return NULL;
    }

  dialog->dialog = (GtkWidget *) gtk_builder_get_object (dialog->builder, "jobs-dialog");
  dialog->user_callback = user_callback;
  dialog->user_data = user_data;
  dialog->printer_name = g_strdup (printer_name);
  dialog->ref_count = 0;

  /* connect signals */
  g_signal_connect (dialog->dialog, "delete-event", G_CALLBACK (gtk_widget_hide_on_delete), NULL);
  g_signal_connect (dialog->dialog, "response", G_CALLBACK (jobs_dialog_response_cb), dialog);

  clear_all_button = (GtkButton *) gtk_builder_get_object (dialog->builder, "jobs-clear-all-button");
  g_signal_connect (clear_all_button, "clicked", G_CALLBACK (on_clear_all_button_clicked), dialog);

  title = g_strdup_printf (_("%s - Active Jobs"), printer_name);
  gtk_window_set_title (GTK_WINDOW (dialog->dialog), title);
  g_free (title);

  dialog->listbox = (GtkListBox*)
        gtk_builder_get_object (dialog->builder, "jobs-listbox");
  gtk_list_box_set_header_func (dialog->listbox,
                                cc_list_box_update_header_func, NULL, NULL);

  update_jobs_list (dialog);

  gtk_window_set_transient_for (GTK_WINDOW (dialog->dialog), GTK_WINDOW (parent));
  gtk_window_present (GTK_WINDOW (dialog->dialog));
  gtk_widget_show_all (GTK_WIDGET (dialog->dialog));

  return dialog;
}

void
pp_jobs_dialog_update (PpJobsDialog *dialog)
{
  update_jobs_list (dialog);
}

static gboolean
pp_jobs_dialog_free_idle (gpointer user_data)
{
  PpJobsDialog *dialog = (PpJobsDialog*) user_data;

  if (dialog->ref_count == 0)
    {
      gtk_widget_destroy (GTK_WIDGET (dialog->dialog));
      dialog->dialog = NULL;

      g_object_unref (dialog->builder);
      dialog->builder = NULL;

      if (dialog->num_jobs > 0)
        cupsFreeJobs (dialog->num_jobs, dialog->jobs);

      g_free (dialog->printer_name);

      g_free (dialog);

      return FALSE;
    }
  else
    {
      return TRUE;
    }
}

void
pp_jobs_dialog_free (PpJobsDialog *dialog)
{
  g_idle_add (pp_jobs_dialog_free_idle, dialog);
}

static void
pp_jobs_dialog_hide (PpJobsDialog *dialog)
{
  gtk_widget_hide (GTK_WIDGET (dialog->dialog));
}
