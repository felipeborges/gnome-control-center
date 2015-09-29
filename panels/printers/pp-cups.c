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

#include "pp-cups.h"

G_DEFINE_TYPE (PpCups, pp_cups, G_TYPE_OBJECT);

static void
pp_cups_finalize (GObject *object)
{
  G_OBJECT_CLASS (pp_cups_parent_class)->finalize (object);
}

static void
pp_cups_class_init (PpCupsClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = pp_cups_finalize;
}

static void
pp_cups_init (PpCups *cups)
{
}

PpCups *
pp_cups_new ()
{
  return g_object_new (PP_TYPE_CUPS, NULL);
}

static void
_pp_cups_get_dests_thread (GTask              *task,
                           gpointer            source_object,
                           gpointer            task_data,
                           GCancellable       *cancellable)
{
  PpCupsDests *dests;

  dests = g_slice_new (PpCupsDests);
  dests->num_of_dests = cupsGetDests (&dests->dests);

  g_task_return_pointer (task, dests, g_object_unref);
}

static void
pp_cups_get_dests_free (PpCupsDests *data)
{
  if (data)
    {
      if (data->dests)
        {
          cupsFreeDests (data->num_of_dests, data->dests);
          g_free (data->dests);
        }

      g_free (data);
    }
}

void
pp_cups_get_dests_async (PpCups              *cups,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GTask *task;

  task = g_task_new (cups, cancellable, callback, user_data);
  g_task_set_task_data (task, NULL, (GDestroyNotify) pp_cups_get_dests_free);
  g_task_run_in_thread (task, _pp_cups_get_dests_thread);

  g_object_unref (task);
}

PpCupsDests *
pp_cups_get_dests_finish (PpCups        *cups,
                          GAsyncResult  *result,
                          GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, cups), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

void
pp_cups_connection_test_async (PpCups              *cups,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  GSocketClient *client;
  gchar         *address;
  int            port = ippPort ();

  address = g_strdup_printf ("%s:%d", cupsServer (), port);

  if (is_address_local (address))
    address = g_strdup_printf ("localhost:%d", port);

  client = g_socket_client_new ();
  g_socket_client_connect_to_host_async (client,
                                         address,
                                         port,
                                         NULL,
                                         callback,
                                         user_data);

  g_object_unref (client);
  g_free (address);
}

gboolean
pp_cups_connection_test_finish (GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  GSocketConnection *connection;
  GError            *error = NULL;

  connection = g_socket_client_connect_to_host_finish (G_SOCKET_CLIENT (source_object),
                                                       result, &error);

  if (connection != NULL)
    {
      g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
      g_object_unref (connection);

      return TRUE;
    }

  return FALSE;
}

/* Returns id of renewed subscription or new id */
gint
pp_cups_renew_subscription (gint id,
                            const char * const *events,
                            gint num_events,
                            gint lease_duration)
{
  ipp_attribute_t              *attr = NULL;
  ipp_t                        *request;
  ipp_t                        *response = NULL;
  gint                          result = -1;

  if (id >= 0)
    {
      request = ippNewRequest (IPP_RENEW_SUBSCRIPTION);
      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                    "printer-uri", NULL, "/");
      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                    "requesting-user-name", NULL, cupsUser ());
      ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
                     "notify-subscription-id", id);
      ippAddInteger (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER,
                     "notify-lease-duration", lease_duration);
      response = cupsDoRequest (CUPS_HTTP_DEFAULT, request, "/");
      if (response != NULL &&
          ippGetStatusCode (response) <= IPP_OK_CONFLICT)
        {
          if ((attr = ippFindAttribute (response, "notify-lease-duration",
                                        IPP_TAG_INTEGER)) == NULL)
            g_debug ("No notify-lease-duration in response!\n");
          else
            if (ippGetInteger (attr, 0) == lease_duration)
              result = id;
        }
    }

  if (result < 0)
    {
      request = ippNewRequest (IPP_CREATE_PRINTER_SUBSCRIPTION);
      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                    "printer-uri", NULL, "/");
      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                    "requesting-user-name", NULL, cupsUser ());
      ippAddStrings (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD,
                     "notify-events", num_events, NULL, events);
      ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD,
                    "notify-pull-method", NULL, "ippget");
      ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_URI,
                    "notify-recipient-uri", NULL, "dbus://");
      ippAddInteger (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER,
                     "notify-lease-duration", lease_duration);
      response = cupsDoRequest (CUPS_HTTP_DEFAULT, request, "/");

      if (response != NULL &&
          ippGetStatusCode (response) <= IPP_OK_CONFLICT)
        {
          if ((attr = ippFindAttribute (response, "notify-subscription-id",
               IPP_TAG_INTEGER)) == NULL)
            g_debug ("No notify-subscription-id in response!\n");
          else
            result = ippGetInteger (attr, 0);
       }

      if (response)
        ippDelete (response);
    }

  return result;
}

/* Cancels subscription of given id */
static void
pp_cups_cancel_subscription_cb (GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  gboolean success;
  gint id = GPOINTER_TO_INT (user_data);

  success = pp_cups_connection_test_finish (source_object, result, NULL);
  ipp_t  *request;

  if (id >= 0 && success)
    {
      request = ippNewRequest (IPP_CANCEL_SUBSCRIPTION);
      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                    "printer-uri", NULL, "/");
      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                    "requesting-user-name", NULL, cupsUser ());
      ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
                     "notify-subscription-id", id);
      ippDelete (cupsDoRequest (CUPS_HTTP_DEFAULT, request, "/"));
    }
}

void
pp_cups_cancel_subscription (gint id)
{
  PpCups *cups;

  cups = pp_cups_new ();
  pp_cups_connection_test_async (cups, pp_cups_cancel_subscription_cb, GINT_TO_POINTER (id));
}


