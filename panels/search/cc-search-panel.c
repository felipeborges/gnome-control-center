/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Red Hat, Inc
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
 * Author: Cosimo Cecchi <cosimoc@gnome.org>
 */

#include "cc-search-panel.h"
#include "cc-search-locations-dialog.h"
#include "cc-search-resources.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

CC_PANEL_REGISTER (CcSearchPanel, cc_search_panel)

#define WID(s) GTK_WIDGET (gtk_builder_get_object (self->priv->builder, s))

struct _CcSearchPanelPrivate
{
  GtkBuilder *builder;
  GtkWidget  *list_box;
  GtkWidget  *up_button;
  GtkWidget  *down_button;

  GCancellable *load_cancellable;
  GSettings  *search_settings;
  GHashTable *sort_order;

  CcSearchLocationsDialog  *locations_dialog;
};

#define SHELL_PROVIDER_GROUP "Shell Search Provider"

static gint
list_sort_func (gconstpointer a,
                gconstpointer b,
                gpointer user_data)
{
  CcSearchPanel *self = user_data;
  GAppInfo *app_a, *app_b;
  const gchar *id_a, *id_b;
  gint idx_a, idx_b;
  gpointer lookup;

  app_a = g_object_get_data (G_OBJECT (a), "app-info");
  app_b = g_object_get_data (G_OBJECT (b), "app-info");

  id_a = g_app_info_get_id (app_a);
  id_b = g_app_info_get_id (app_b);

  /* find the index of the application in the GSettings preferences */
  idx_a = -1;
  idx_b = -1;

  lookup = g_hash_table_lookup (self->priv->sort_order, id_a);
  if (lookup)
    idx_a = GPOINTER_TO_INT (lookup) - 1;

  lookup = g_hash_table_lookup (self->priv->sort_order, id_b);
  if (lookup)
    idx_b = GPOINTER_TO_INT (lookup) - 1;

  /* if neither app is found, use alphabetical order */
  if ((idx_a == -1) && (idx_b == -1))
    return g_utf8_collate (g_app_info_get_name (app_a), g_app_info_get_name (app_b));

  /* if app_a isn't found, it's sorted after app_b */
  if (idx_a == -1)
    return 1;

  /* if app_b isn't found, it's sorted after app_a */
  if (idx_b == -1)
    return -1;

  /* finally, if both apps are found, return their order in the list */
  return (idx_a - idx_b);
}

static void
search_panel_invalidate_button_state (CcSearchPanel *self)
{
  GList *children;
  gboolean is_first, is_last;
  GtkListBoxRow *row;

  row = gtk_list_box_get_selected_row (GTK_LIST_BOX (self->priv->list_box));
  children = gtk_container_get_children (GTK_CONTAINER (self->priv->list_box));

  if (!row || !children)
    return;

  is_first = (row == g_list_first (children)->data);
  is_last = (row == g_list_last (children)->data);

  gtk_widget_set_sensitive (self->priv->up_button, !is_first);
  gtk_widget_set_sensitive (self->priv->down_button, !is_last);

  g_list_free (children);
}

static void
search_panel_invalidate_sort_order (CcSearchPanel *self)
{
  gchar **sort_order;
  gint idx;

  g_hash_table_remove_all (self->priv->sort_order);
  sort_order = g_settings_get_strv (self->priv->search_settings, "sort-order");

  for (idx = 0; sort_order[idx] != NULL; idx++)
    g_hash_table_insert (self->priv->sort_order, g_strdup (sort_order[idx]), GINT_TO_POINTER (idx + 1));

  gtk_list_box_invalidate_sort (GTK_LIST_BOX (self->priv->list_box));
  g_strfreev (sort_order);

  search_panel_invalidate_button_state (self);
}

static gint
propagate_compare_func (gconstpointer a,
                        gconstpointer b,
                        gpointer user_data)
{
  CcSearchPanel *self = user_data;
  const gchar *key_a = a, *key_b = b;
  gint idx_a, idx_b;

  idx_a = GPOINTER_TO_INT (g_hash_table_lookup (self->priv->sort_order, key_a));
  idx_b = GPOINTER_TO_INT (g_hash_table_lookup (self->priv->sort_order, key_b));

  return (idx_a - idx_b);
}

static void
search_panel_propagate_sort_order (CcSearchPanel *self)
{
  GList *keys, *l;
  GPtrArray *sort_order;

  sort_order = g_ptr_array_new ();
  keys = g_hash_table_get_keys (self->priv->sort_order);
  keys = g_list_sort_with_data (keys, propagate_compare_func, self);

  for (l = keys; l != NULL; l = l->next)
    g_ptr_array_add (sort_order, l->data);

  g_ptr_array_add (sort_order, NULL);
  g_settings_set_strv (self->priv->search_settings, "sort-order",
                       (const gchar **) sort_order->pdata);

  g_ptr_array_unref (sort_order);
  g_list_free (keys);
}

static void
search_panel_set_no_providers (CcSearchPanel *self)
{
  GtkWidget *w;

  /* center the list box in the scrolled window */
  gtk_widget_set_valign (self->priv->list_box, GTK_ALIGN_CENTER);

  w = gtk_label_new (_("No applications found"));
  gtk_widget_show (w);

  gtk_container_add (GTK_CONTAINER (self->priv->list_box), w);
}

static void
search_panel_move_selected (CcSearchPanel *self,
                            gboolean down)
{
  GtkListBoxRow *row, *other_row;
  GAppInfo *app_info, *other_app_info;
  const gchar *app_id, *other_app_id;
  const gchar *last_good_app, *target_app;
  gint idx, other_idx;
  gpointer idx_ptr;
  gboolean found;
  GList *children, *l, *other;

  row = gtk_list_box_get_selected_row (GTK_LIST_BOX (self->priv->list_box));
  app_info = g_object_get_data (G_OBJECT (row), "app-info");
  app_id = g_app_info_get_id (app_info);

  children = gtk_container_get_children (GTK_CONTAINER (self->priv->list_box));

  /* The assertions are valid only as long as we don't move the first
     or the last item. */

  l = g_list_find (children, row);
  g_assert (l != NULL);

  other = down ? g_list_next(l) : g_list_previous(l);
  g_assert (other != NULL);

  other_row = other->data;
  other_app_info = g_object_get_data (G_OBJECT (other_row), "app-info");
  other_app_id = g_app_info_get_id (other_app_info);

  g_assert (other_app_id != NULL);

  /* Check if we're moving one of the unsorted providers at the end of
     the list; in that case, the value we obtain from the sort order table
     is garbage.
     We need to find the last app with a valid sort order, and
     then set the sort order on all intermediate apps until we find the
     one we want to move, if moving up, or the neighbor, if moving down.
  */
  last_good_app = target_app = app_id;
  found = g_hash_table_lookup_extended (self->priv->sort_order, last_good_app, NULL, &idx_ptr);
  while (!found)
    {
      GAppInfo *tmp;
      const char *tmp_id;

      l = g_list_previous (l);
      if (l == NULL)
        {
          last_good_app = NULL;
          break;
        }

      tmp =  g_object_get_data (G_OBJECT (l->data), "app-info");
      tmp_id = g_app_info_get_id (tmp);

      last_good_app = tmp_id;
      found = g_hash_table_lookup_extended (self->priv->sort_order, tmp_id, NULL, &idx_ptr);
    }

  /* For simplicity's sake, set all sort orders to the previously visible state
     first, and only then do the modification requested.

     The loop actually sets the sort order on last_good_app even if we found a
     valid one already, but I preferred to keep the logic simple, at the expense
     of a small performance penalty.
  */
  if (found)
    {
      idx = GPOINTER_TO_INT (idx_ptr);
    }
  else
    {
      /* If not found, there is no configured app that has a sort order, so we start
         from the first position and walk the entire list.
         Sort orders are 1 based, so that 0 (NULL) is not a valid value.
      */
      idx = 1;
      l = children;
    }

  while (last_good_app != target_app)
    {
      GAppInfo *tmp;
      const char *tmp_id;

      tmp = g_object_get_data (G_OBJECT (l->data), "app-info");
      tmp_id = g_app_info_get_id (tmp);

      g_hash_table_replace (self->priv->sort_order, g_strdup (tmp_id), GINT_TO_POINTER (idx));

      l = g_list_next (l);
      idx++;
      last_good_app = tmp_id;
    }

  other_idx = GPOINTER_TO_INT (g_hash_table_lookup (self->priv->sort_order, app_id));
  idx = down ? (other_idx + 1) : (other_idx - 1);

  g_hash_table_replace (self->priv->sort_order, g_strdup (other_app_id), GINT_TO_POINTER (other_idx));
  g_hash_table_replace (self->priv->sort_order, g_strdup (app_id), GINT_TO_POINTER (idx));

  search_panel_propagate_sort_order (self);

  g_list_free (children);
}

static void
down_button_clicked (GtkWidget *widget,
                     CcSearchPanel *self)
{
  search_panel_move_selected (self, TRUE);
}

static void
up_button_clicked (GtkWidget *widget,
                   CcSearchPanel *self)
{
  search_panel_move_selected (self, FALSE);
}

static void
settings_button_clicked (GtkWidget *widget,
                         gpointer user_data)
{
  CcSearchPanel *self = user_data;

  if (self->priv->locations_dialog == NULL)
    {
      self->priv->locations_dialog = cc_search_locations_dialog_new (self);
      g_object_add_weak_pointer (G_OBJECT (self->priv->locations_dialog),
                                 (gpointer *) &self->priv->locations_dialog);
    }

  gtk_window_present (GTK_WINDOW (self->priv->locations_dialog));
}

static GVariant *
switch_settings_mapping_set_generic (const GValue *value,
                                     const GVariantType *expected_type,
                                     GtkWidget *row,
                                     gboolean default_enabled)
{
  CcSearchPanel *self = g_object_get_data (G_OBJECT (row), "self");
  GAppInfo *app_info = g_object_get_data (G_OBJECT (row), "app-info");
  gchar **apps;
  GPtrArray *new_apps;
  gint idx;
  gboolean remove, found;
  GVariant *variant;

  remove = !!g_value_get_boolean (value) == !!default_enabled;
  found = FALSE;
  new_apps = g_ptr_array_new_with_free_func (g_free);
  apps = g_settings_get_strv (self->priv->search_settings,
                              default_enabled ? "disabled" : "enabled");

  for (idx = 0; apps[idx] != NULL; idx++)
    {
      if (g_strcmp0 (apps[idx], g_app_info_get_id (app_info)) == 0)
        {
          found = TRUE;

          if (remove)
            continue;
        }

      g_ptr_array_add (new_apps, g_strdup (apps[idx]));
    }

  if (!found && !remove)
    g_ptr_array_add (new_apps, g_strdup (g_app_info_get_id (app_info)));

  g_ptr_array_add (new_apps, NULL);

  variant = g_variant_new_strv ((const gchar **) new_apps->pdata, -1);
  g_ptr_array_unref (new_apps);
  g_strfreev (apps);

  return variant;
}

static GVariant *
switch_settings_mapping_set_default_enabled (const GValue *value,
                                             const GVariantType *expected_type,
                                             gpointer user_data)
{
  return switch_settings_mapping_set_generic (value, expected_type,
                                              user_data, TRUE);
}

static GVariant *
switch_settings_mapping_set_default_disabled (const GValue *value,
                                              const GVariantType *expected_type,
                                              gpointer user_data)
{
  return switch_settings_mapping_set_generic (value, expected_type,
                                              user_data, FALSE);
}

static gboolean
switch_settings_mapping_get_generic (GValue *value,
                                     GVariant *variant,
                                     GtkWidget *row,
                                     gboolean default_enabled)
{
  GAppInfo *app_info = g_object_get_data (G_OBJECT (row), "app-info");
  const gchar **apps;
  gint idx;
  gboolean found;

  found = FALSE;
  apps = g_variant_get_strv (variant, NULL);

  for (idx = 0; apps[idx] != NULL; idx++)
    {
      if (g_strcmp0 (apps[idx], g_app_info_get_id (app_info)) == 0)
        {
          found = TRUE;
          break;
        }
    }

  g_value_set_boolean (value, !!default_enabled != !!found);

  return TRUE;
}

static gboolean
switch_settings_mapping_get_default_enabled (GValue *value,
                                             GVariant *variant,
                                             gpointer user_data)
{
  return switch_settings_mapping_get_generic (value, variant,
                                              user_data, TRUE);
}

static gboolean
switch_settings_mapping_get_default_disabled (GValue *value,
                                              GVariant *variant,
                                              gpointer user_data)
{
  return switch_settings_mapping_get_generic (value, variant,
                                              user_data, FALSE);
}

static void
search_panel_add_one_app_info (CcSearchPanel *self,
                               GAppInfo *app_info,
                               gboolean default_enabled)
{
  GtkWidget *row, *box, *w;
  GIcon *icon;
  gint width, height;

  /* gnome-control-center is special cased in the shell,
     and is not configurable */
  if (g_strcmp0 (g_app_info_get_id (app_info),
                 "gnome-control-center.desktop") == 0)
    return;

  /* reset valignment of the list box */
  gtk_widget_set_valign (self->priv->list_box, GTK_ALIGN_FILL);

  row = gtk_list_box_row_new ();
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_container_add (GTK_CONTAINER (row), box);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_container_set_border_width (GTK_CONTAINER (box), 6);
  g_object_set_data_full (G_OBJECT (row), "app-info",
                          g_object_ref (app_info), g_object_unref);
  g_object_set_data (G_OBJECT (row), "self", self);
  gtk_container_add (GTK_CONTAINER (self->priv->list_box), row);

  icon = g_app_info_get_icon (app_info);
  if (icon == NULL)
    icon = g_themed_icon_new ("application-x-executable");
  else
    g_object_ref (icon);

  w = gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_DIALOG);
  gtk_icon_size_lookup (GTK_ICON_SIZE_DIALOG, &width, &height);
  gtk_image_set_pixel_size (GTK_IMAGE (w), MAX (width, height));
  gtk_container_add (GTK_CONTAINER (box), w);
  g_object_unref (icon);

  w = gtk_label_new (g_app_info_get_name (app_info));
  gtk_container_add (GTK_CONTAINER (box), w);

  w = gtk_switch_new ();
  gtk_widget_set_valign (w, GTK_ALIGN_CENTER);
  gtk_box_pack_end (GTK_BOX (box), w, FALSE, FALSE, 0);

  if (default_enabled)
    {
      g_settings_bind_with_mapping (self->priv->search_settings, "disabled",
                                    w, "active",
                                    G_SETTINGS_BIND_DEFAULT,
                                    switch_settings_mapping_get_default_enabled,
                                    switch_settings_mapping_set_default_enabled,
                                    row, NULL);
    }
  else
    {
      g_settings_bind_with_mapping (self->priv->search_settings, "enabled",
                                    w, "active",
                                    G_SETTINGS_BIND_DEFAULT,
                                    switch_settings_mapping_get_default_disabled,
                                    switch_settings_mapping_set_default_disabled,
                                    row, NULL);
    }

  gtk_widget_show_all (row);
}

static void
search_panel_add_one_provider (CcSearchPanel *self,
                               GFile *provider)
{
  gchar *path, *desktop_id;
  GKeyFile *keyfile;
  GAppInfo *app_info;
  GError *error = NULL;
  gboolean default_disabled;

  path = g_file_get_path (provider);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, &error);

  if (error != NULL)
    {
      g_warning ("Error loading %s: %s - search provider will be ignored",
                 path, error->message);
      goto out;
    }

  if (!g_key_file_has_group (keyfile, SHELL_PROVIDER_GROUP))
    goto out;

  desktop_id = g_key_file_get_string (keyfile, SHELL_PROVIDER_GROUP,
                                      "DesktopId", &error);

  if (error != NULL)
    {
      g_warning ("Unable to read desktop ID from %s: %s - search provider will be ignored",
                 path, error->message);
      goto out;
    }

  app_info = G_APP_INFO (g_desktop_app_info_new (desktop_id));
  g_free (desktop_id);

  if (app_info == NULL)
    goto out;

  default_disabled = g_key_file_get_boolean (keyfile, SHELL_PROVIDER_GROUP,
                                             "DefaultDisabled", NULL);
  search_panel_add_one_app_info (self, app_info, !default_disabled);
  g_object_unref (app_info);

 out:
  g_free (path);
  g_clear_error (&error);
  g_key_file_unref (keyfile);
}

static void
search_providers_discover_ready (GObject *source,
                                 GAsyncResult *result,
                                 gpointer user_data)
{
  GList *providers, *l;
  GFile *provider;
  CcSearchPanel *self = CC_SEARCH_PANEL (source);
  GError *error = NULL;

  providers = g_task_propagate_pointer (G_TASK (result), &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_error_free (error);
      return;
    }

  g_clear_object (&self->priv->load_cancellable);

  if (providers == NULL)
    {
      search_panel_set_no_providers (self);
      return;
    }

  for (l = providers; l != NULL; l = l->next)
    {
      provider = l->data;
      search_panel_add_one_provider (self, provider);
      g_object_unref (provider);
    }

  /* propagate a write to GSettings, to make sure we always have
   * all the providers in the list.
   */
  search_panel_propagate_sort_order (self);
  g_list_free (providers);
}

static GList *
search_providers_discover_one_directory (const gchar *system_dir,
                                         GCancellable *cancellable)
{
  GList *providers = NULL;
  gchar *providers_path;
  GFile *providers_location, *provider;
  GFileInfo *info;
  GFileEnumerator *enumerator;
  GError *error = NULL;

  providers_path = g_build_filename (system_dir, "gnome-shell", "search-providers", NULL);
  providers_location = g_file_new_for_path (providers_path);

  enumerator = g_file_enumerate_children (providers_location,
                                          "standard::type,standard::name,standard::content-type",
                                          G_FILE_QUERY_INFO_NONE,
                                          cancellable, &error);

  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Error opening %s: %s - search provider configuration won't be possible",
                   providers_path, error->message);
      g_clear_error (&error);

      goto out;
    }

  while ((info = g_file_enumerator_next_file (enumerator, cancellable, &error)) != NULL)
    {
      provider = g_file_get_child (providers_location, g_file_info_get_name (info));
      providers = g_list_prepend (providers, provider);
      g_object_unref (info);
    }

  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Error reading from %s: %s - search providers might be missing from the panel",
                   providers_path, error->message);
      g_clear_error (&error);
    }

 out:
  g_clear_object (&enumerator);
  g_clear_object (&providers_location);
  g_free (providers_path);

  return providers;
}

static void
search_providers_discover_thread (GTask *task,
                                  gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable)
{
  GList *providers = NULL;
  const gchar * const *system_data_dirs;
  int idx;

  system_data_dirs = g_get_system_data_dirs ();
  for (idx = 0; system_data_dirs[idx] != NULL; idx++)
    {
      providers = g_list_concat (search_providers_discover_one_directory (system_data_dirs[idx], cancellable),
                                 providers);

      if (g_task_return_error_if_cancelled (task))
        {
          g_list_free_full (providers, g_object_unref);
          return;
        }
    }

  g_task_return_pointer (task, providers, NULL);
}

static void
populate_search_providers (CcSearchPanel *self)
{
  GTask *task;

  self->priv->load_cancellable = g_cancellable_new ();
  task = g_task_new (self, self->priv->load_cancellable,
                     search_providers_discover_ready, self);
  g_task_run_in_thread (task, search_providers_discover_thread);
  g_object_unref (task);
}

static void
cc_search_panel_dispose (GObject *object)
{
  CcSearchPanelPrivate *priv = CC_SEARCH_PANEL (object)->priv;

  if (priv->load_cancellable != NULL)
    g_cancellable_cancel (priv->load_cancellable);
  g_clear_object (&priv->load_cancellable);

  G_OBJECT_CLASS (cc_search_panel_parent_class)->dispose (object);
}

static void
cc_search_panel_finalize (GObject *object)
{
  CcSearchPanelPrivate *priv = CC_SEARCH_PANEL (object)->priv;

  g_clear_object (&priv->builder);
  g_clear_object (&priv->search_settings);
  g_hash_table_destroy (priv->sort_order);

  if (priv->locations_dialog)
    gtk_widget_destroy (GTK_WIDGET (priv->locations_dialog));

  G_OBJECT_CLASS (cc_search_panel_parent_class)->finalize (object);
}

static void
cc_search_panel_constructed (GObject *object)
{
  CcSearchPanel *self = CC_SEARCH_PANEL (object);
  GtkWidget *box, *widget, *search_box;

  G_OBJECT_CLASS (cc_search_panel_parent_class)->constructed (object);

  /* add the disable all switch */
  search_box = WID ("search_vbox");
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

  widget = gtk_switch_new ();
  gtk_widget_set_valign (widget, GTK_ALIGN_CENTER);
  gtk_box_pack_start (GTK_BOX (box), widget, FALSE, FALSE, 4);

  g_settings_bind (self->priv->search_settings, "disable-external",
                   widget, "active",
                   G_SETTINGS_BIND_DEFAULT |
                   G_SETTINGS_BIND_INVERT_BOOLEAN);

  g_object_bind_property (widget, "active",
                          search_box, "sensitive",
                          G_BINDING_DEFAULT |
                          G_BINDING_SYNC_CREATE);

  gtk_widget_show_all (box);
  cc_shell_embed_widget_in_header (cc_panel_get_shell (CC_PANEL (self)), box);
}

static void
cc_search_panel_init (CcSearchPanel *self)
{
  GError    *error;
  GtkWidget *widget;
  GtkWidget *scrolled_window;
  guint res;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, CC_TYPE_SEARCH_PANEL, CcSearchPanelPrivate);
  g_resources_register (cc_search_get_resource ());

  self->priv->builder = gtk_builder_new ();

  error = NULL;
  res = gtk_builder_add_from_resource (self->priv->builder,
                                       "/org/gnome/control-center/search/search.ui",
                                       &error);

  if (res == 0)
    {
      g_warning ("Could not load interface file: %s",
                 (error != NULL) ? error->message : "unknown error");
      g_clear_error (&error);
      return;
    }

  scrolled_window = WID ("scrolled_window");
  widget = GTK_WIDGET (gtk_list_box_new ());
  gtk_list_box_set_sort_func (GTK_LIST_BOX (widget),
                              (GtkListBoxSortFunc)list_sort_func, self, NULL);
  gtk_container_add (GTK_CONTAINER (scrolled_window), widget);
  self->priv->list_box = widget;
  gtk_widget_show (widget);

  g_signal_connect_swapped (widget, "row-selected",
                            G_CALLBACK (search_panel_invalidate_button_state), self);

  self->priv->up_button = WID ("up_button");
  g_signal_connect (self->priv->up_button, "clicked",
                    G_CALLBACK (up_button_clicked), self);
  gtk_widget_set_sensitive (self->priv->up_button, FALSE);

  self->priv->down_button = WID ("down_button");
  g_signal_connect (self->priv->down_button, "clicked",
                    G_CALLBACK (down_button_clicked), self);
  gtk_widget_set_sensitive (self->priv->down_button, FALSE);

  widget = WID ("settings_button");
  g_signal_connect (widget, "clicked",
                    G_CALLBACK (settings_button_clicked), self);
  gtk_widget_set_sensitive (widget, cc_search_locations_dialog_is_available ());

  self->priv->search_settings = g_settings_new ("org.gnome.desktop.search-providers");
  self->priv->sort_order = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);
  g_signal_connect_swapped (self->priv->search_settings, "changed::sort-order",
                            G_CALLBACK (search_panel_invalidate_sort_order), self);
  search_panel_invalidate_sort_order (self);

  populate_search_providers (self);

  widget = WID ("search_vbox");
  gtk_container_add (GTK_CONTAINER (self), widget);
}

static void
cc_search_panel_class_init (CcSearchPanelClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);

  oclass->constructed = cc_search_panel_constructed;
  oclass->dispose = cc_search_panel_dispose;
  oclass->finalize = cc_search_panel_finalize;

  g_type_class_add_private (klass, sizeof (CcSearchPanelPrivate));
}
