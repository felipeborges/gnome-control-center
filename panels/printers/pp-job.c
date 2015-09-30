/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright 2015  Red Hat, Inc,
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
 * Author: Felipe Borges <feborges@redhat.com>
 */

#include "pp-job.h"

typedef struct
{
  GObject parent;

  gint   id;
  gchar *title;
  gint   state;
} PpJobPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PpJob, pp_job, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_ID,
  PROP_TITLE,
  PROP_STATE,
  LAST_PROPERTY
};

static GParamSpec *properties[LAST_PROPERTY];

static void
pp_job_init (PpJob *obj)
{
}

static void
job_get_property (GObject    *object,
                  guint       property_id,
                  GValue     *value,
                  GParamSpec *pspec)
{
  PpJobPrivate *priv;

  priv = pp_job_get_instance_private (PP_JOB (object));

  switch (property_id)
  {
    case PROP_ID:
      g_value_set_int (value, priv->id);
      break;
    case PROP_TITLE:
      g_value_set_string (value, priv->title);
      break;
    case PROP_STATE:
      g_value_set_int (value, priv->state);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
job_set_property (GObject      *object,
                  guint         property_id,
                  const GValue *value,
                  GParamSpec   *pspec)
{
  PpJobPrivate *priv;

  priv = pp_job_get_instance_private (PP_JOB (object));

  switch (property_id)
  {
    case PROP_ID:
      priv->id = g_value_get_int (value);
      break;
    case PROP_TITLE:
      g_free (priv->title);
      priv->title = g_value_dup_string (value);
      break;
    case PROP_STATE:
      priv->state = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
job_finalize (GObject *object)
{
  PpJobPrivate *priv;

  priv = pp_job_get_instance_private (PP_JOB (object));

  g_free (priv->title);

  G_OBJECT_CLASS (pp_job_parent_class)->finalize (object);
}

static void
pp_job_class_init (PpJobClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->get_property = job_get_property;
  object_class->set_property = job_set_property;
  object_class->finalize = job_finalize;

  properties[PROP_ID] = g_param_spec_int ("id", "id", "id",
                                          0, G_MAXINT, 0, G_PARAM_READWRITE);
  properties[PROP_TITLE] = g_param_spec_string ("title", "title", "title",
                                                NULL, G_PARAM_READWRITE);
  properties[PROP_STATE] = g_param_spec_int ("state", "state", "state",
                                             0, G_MAXINT, 0, G_PARAM_READWRITE);
  g_object_class_install_properties (object_class, LAST_PROPERTY, properties);
}
