/*
 * Copyright (C) 2012 Ole Andr� Vadla Ravn�s <ole.andre.ravnas@tillitech.com>
 * Copyright (C) 2012 Karl Trygve Kalleberg <karltk@boblycat.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gumstalker.h"

struct _GumStalkerPrivate
{
  gboolean dummy;
};

#define GUM_STALKER_GET_PRIVATE(o) ((o)->priv)

static void gum_stalker_finalize (GObject * object);

G_DEFINE_TYPE (GumStalker, gum_stalker, G_TYPE_OBJECT);

static void
gum_stalker_class_init (GumStalkerClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GumStalkerPrivate));

  object_class->finalize = gum_stalker_finalize;
}

static void
gum_stalker_init (GumStalker * self)
{
  GumStalkerPrivate * priv;

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GUM_TYPE_STALKER, GumStalkerPrivate);
  priv = GUM_STALKER_GET_PRIVATE (self);
}

static void
gum_stalker_finalize (GObject * object)
{
  GumStalker * self = GUM_STALKER (object);

  (void) self;

  G_OBJECT_CLASS (gum_stalker_parent_class)->finalize (object);
}

GumStalker *
gum_stalker_new (void)
{
  return GUM_STALKER (g_object_new (GUM_TYPE_STALKER, NULL));
}

void
gum_stalker_follow_me (GumStalker * self,
                       GumEventSink * sink)
{
}

void
gum_stalker_unfollow_me (GumStalker * self)
{
}

gboolean
gum_stalker_is_following_me (GumStalker * self)
{
  return FALSE;
}

GumProbeId
gum_stalker_add_call_probe (GumStalker * self,
                            gpointer target_address,
                            GumCallProbeCallback callback,
                            gpointer data)
{
  return 0;
}

void
gum_stalker_remove_call_probe (GumStalker * self,
                               GumProbeId id)
{
}

