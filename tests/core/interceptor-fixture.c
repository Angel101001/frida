/*
 * Copyright (C) 2008-2010 Ole Andr� Vadla Ravn�s <ole.andre.ravnas@tandberg.com>
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

#include "guminterceptor.h"

#include "interceptor-callbacklistener.c"
#include "lowlevel-helpers.h"
#include "testutil.h"

#include <stdlib.h>
#include <string.h>

#define INTERCEPTOR_TESTCASE(NAME) \
    void test_interceptor_ ## NAME ( \
        TestInterceptorFixture * fixture, gconstpointer data)
#define INTERCEPTOR_TESTENTRY(NAME) \
    TEST_ENTRY_WITH_FIXTURE (Interceptor, test_interceptor, NAME, \
        TestInterceptorFixture)

typedef struct _TestInterceptorFixture   TestInterceptorFixture;
typedef struct _ListenerContext      ListenerContext;
typedef struct _ListenerContextClass ListenerContextClass;

typedef gpointer (* InterceptorTestFunc) (gpointer data);

struct _ListenerContext
{
  GObject parent;

  TestInterceptorFixture * harness;
  gchar enter_char;
  gchar leave_char;
  guint last_thread_id;
  gsize last_seen_argument;
  gpointer last_return_value;
  GumCpuContext last_on_enter_cpu_context;
};

struct _ListenerContextClass
{
  GObjectClass parent_class;
};

struct _TestInterceptorFixture
{
  GumInterceptor * interceptor;
  GString * result;
  ListenerContext * listener_context[2];
};

static void listener_context_iface_init (gpointer g_iface,
    gpointer iface_data);

G_DEFINE_TYPE_EXTENDED (ListenerContext,
                        listener_context,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (GUM_TYPE_INVOCATION_LISTENER,
                            listener_context_iface_init));

static void
test_interceptor_fixture_setup (TestInterceptorFixture * fixture,
                                gconstpointer data)
{
  fixture->interceptor = gum_interceptor_obtain ();
  fixture->result = g_string_new ("");
  memset (&fixture->listener_context, 0, sizeof (fixture->listener_context));
}

static void
test_interceptor_fixture_teardown (TestInterceptorFixture * fixture,
                                   gconstpointer data)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (fixture->listener_context); i++)
  {
    ListenerContext * ctx = fixture->listener_context[i];

    if (ctx != NULL)
    {
      gum_interceptor_detach_listener (fixture->interceptor,
          GUM_INVOCATION_LISTENER (ctx));
      g_object_unref (ctx);
    }
  }

  g_string_free (fixture->result, TRUE);
  g_object_unref (fixture->interceptor);
}

GumAttachReturn
interceptor_fixture_try_attaching_listener (TestInterceptorFixture * h,
                                            guint listener_index,
                                            gpointer test_func,
                                            gchar enter_char,
                                            gchar leave_char)
{
  GumAttachReturn result;
  ListenerContext * ctx;

  ctx = (ListenerContext *) g_object_new (listener_context_get_type (), NULL);
  ctx->harness = h;
  ctx->enter_char = enter_char;
  ctx->leave_char = leave_char;

  result = gum_interceptor_attach_listener (h->interceptor, test_func,
      GUM_INVOCATION_LISTENER (ctx), NULL);
  if (result == GUM_ATTACH_OK)
  {
    h->listener_context[listener_index] = ctx;
  }
  else
  {
    g_object_unref (ctx);
  }

  return result;
}

void
interceptor_fixture_attach_listener (TestInterceptorFixture * h,
                                     guint listener_index,
                                     gpointer test_func,
                                     gchar enter_char,
                                     gchar leave_char)
{
  g_assert_cmpint (interceptor_fixture_try_attaching_listener (h,
      listener_index, test_func, enter_char, leave_char), ==,
      GUM_ATTACH_OK);
}

void
interceptor_fixture_detach_listener (TestInterceptorFixture * h,
                                     guint listener_index)
{
  gum_interceptor_detach_listener (h->interceptor,
    GUM_INVOCATION_LISTENER (h->listener_context[listener_index]));
}

static void
listener_context_on_enter (GumInvocationListener * listener,
                           GumInvocationContext * context)
{
  ListenerContext * self = (ListenerContext *) listener;

  g_string_append_c (self->harness->result, self->enter_char);

  self->last_seen_argument = (gsize)
      gum_invocation_context_get_nth_argument (context, 0);
  self->last_on_enter_cpu_context = *context->cpu_context;
}

static void
listener_context_on_leave (GumInvocationListener * listener,
                           GumInvocationContext * context)
{
  ListenerContext * self = (ListenerContext *) listener;

  g_string_append_c (self->harness->result, self->leave_char);

  self->last_return_value = gum_invocation_context_get_return_value (context);
}

static gpointer
listener_context_provide_thread_data (GumInvocationListener * listener,
                                      gpointer function_instance_data,
                                      guint thread_id)
{
  ListenerContext * self = (ListenerContext *) listener;
  self->last_thread_id = thread_id;
  return NULL;
}

static void
listener_context_class_init (ListenerContextClass * klass)
{
}

static void
listener_context_iface_init (gpointer g_iface,
                             gpointer iface_data)
{
  GumInvocationListenerIface * iface = (GumInvocationListenerIface *) g_iface;

  iface->on_enter = listener_context_on_enter;
  iface->on_leave = listener_context_on_leave;
  iface->provide_thread_data = listener_context_provide_thread_data;
}

static void
listener_context_init (ListenerContext * self)
{
}
