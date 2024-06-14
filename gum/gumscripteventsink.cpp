/*
 * Copyright (C) 2012 Ole Andr� Vadla Ravn�s <ole.andre.ravnas@tillitech.com>
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

#include "gumscripteventsink.h"

#include "gumscriptscope.h"

using namespace v8;

static void gum_script_event_sink_iface_init (gpointer g_iface,
    gpointer iface_data);
static void gum_script_event_sink_finalize (GObject * obj);
static GumEventType gum_script_event_sink_query_mask (GumEventSink * sink);
static void gum_script_event_sink_process (GumEventSink * sink,
    const GumEvent * ev);
static gboolean gum_script_event_sink_drain (gpointer user_data);

G_DEFINE_TYPE_EXTENDED (GumScriptEventSink,
                        gum_script_event_sink,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (GUM_TYPE_EVENT_SINK,
                                               gum_script_event_sink_iface_init));

static void
gum_script_event_sink_class_init (GumScriptEventSinkClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gum_script_event_sink_finalize;
}

static void
gum_script_event_sink_iface_init (gpointer g_iface,
                                  gpointer iface_data)
{
  GumEventSinkIface * iface = (GumEventSinkIface *) g_iface;

  iface->query_mask = gum_script_event_sink_query_mask;
  iface->process = gum_script_event_sink_process;
}

static void
gum_script_event_sink_init (GumScriptEventSink * self)
{
  gum_spinlock_init (&self->lock);
  self->events = g_array_sized_new (FALSE, FALSE, sizeof (GumEvent), 16384);
}

static void
gum_script_event_sink_finalize (GObject * obj)
{
  GumScriptEventSink * self = GUM_SCRIPT_EVENT_SINK (obj);

  gum_script_event_sink_drain (self);

  gum_spinlock_free (&self->lock);
  g_array_free (self->events, TRUE);

  self->on_receive.Dispose ();
  g_source_destroy (self->source);

  G_OBJECT_CLASS (gum_script_event_sink_parent_class)->finalize (obj);
}

GumEventSink *
gum_script_event_sink_new (GumScript * script,
                           GMainContext * main_context,
                           Handle<Function> on_receive)
{
  GumScriptEventSink * sink;

  sink = GUM_SCRIPT_EVENT_SINK (
      g_object_new (GUM_TYPE_SCRIPT_EVENT_SINK, NULL));
  sink->script = script;
  sink->on_receive = Persistent<Function>::New (on_receive);
  sink->source = g_timeout_source_new (250);
  g_source_set_callback (sink->source, gum_script_event_sink_drain, sink, NULL);
  g_source_attach (sink->source, main_context);

  return GUM_EVENT_SINK (sink);
}

static GumEventType
gum_script_event_sink_query_mask (GumEventSink * sink)
{
  (void) sink;

  return GUM_CALL;
}

static void
gum_script_event_sink_process (GumEventSink * sink,
                               const GumEvent * ev)
{
  GumScriptEventSink * self = reinterpret_cast<GumScriptEventSink *> (sink);
  gum_spinlock_acquire (&self->lock);
  g_array_append_val (self->events, *ev);
  gum_spinlock_release (&self->lock);
}

static gboolean
gum_script_event_sink_drain (gpointer user_data)
{
  GumScriptEventSink * self = static_cast<GumScriptEventSink *> (user_data);
  GArray * raw_events = NULL;

  gum_spinlock_acquire (&self->lock);
  if (self->events->len > 0)
  {
    raw_events = self->events;
    self->events = g_array_sized_new (FALSE, FALSE, sizeof (GumEvent), 16384);
  }
  gum_spinlock_release (&self->lock);

  if (raw_events != NULL)
  {
    ScriptScope scope (self->script);

    Local<Array> events = Array::New (raw_events->len);
    for (guint i = 0; i != raw_events->len; i++)
    {
      GumCallEvent * raw_event = &g_array_index (raw_events, GumCallEvent, i);
      Local<Object> event (Object::New ());
      event->Set (String::New ("location"),
          Number::New (GPOINTER_TO_SIZE (raw_event->location)),
          ReadOnly);
      event->Set (String::New ("target"),
          Number::New (GPOINTER_TO_SIZE (raw_event->target)),
          ReadOnly);
      event->Set (String::New ("depth"),
          Int32::New (GPOINTER_TO_SIZE (raw_event->depth)),
          ReadOnly);
      events->Set (v8::Number::New (i), event);
    }

    Handle<Value> argv[] = { events };
    self->on_receive->Call (self->on_receive, 1, argv);
  }

  return TRUE;
}
