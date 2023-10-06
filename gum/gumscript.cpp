/*
 * Copyright (C) 2010-2011 Ole Andr� Vadla Ravn�s <ole.andre.ravnas@tillitech.com>
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

#include "gumscript.h"

#include "guminterceptor.h"

#include <gio/gio.h>
#include <string.h>
#include <v8.h>

using namespace v8;

typedef struct _GumScriptAttachEntry GumScriptAttachEntry;

struct _GumScriptPrivate
{
  GumInterceptor * interceptor;

  Persistent<Context> context;
  Persistent<Script> raw_script;

  GumScriptMessageHandler message_handler_func;
  gpointer message_handler_data;
  GDestroyNotify message_handler_notify;

  GQueue * attach_entries;
  GumInvocationContext * current_invocation_context;
};

struct _GumScriptAttachEntry
{
  Persistent<Function> on_enter;
  Persistent<Function> on_leave;
};

static void gum_script_listener_iface_init (gpointer g_iface,
    gpointer iface_data);

static void gum_script_dispose (GObject * object);
static void gum_script_finalize (GObject * object);

static Handle<Value> gum_script_on_get_nth_argument (uint32_t index,
    const AccessorInfo & info);
static Handle<Value> gum_script_on_send (const Arguments & args);
static Handle<Value> gum_script_on_interceptor_attach (const Arguments & args);
static Handle<Value> gum_script_on_memory_read_utf8_string (
    const Arguments & args);
static Handle<Value> gum_script_on_memory_read_utf16_string (
    const Arguments & args);

static void gum_script_on_enter (GumInvocationListener * listener,
    GumInvocationContext * context);
static void gum_script_on_leave (GumInvocationListener * listener,
    GumInvocationContext * context);

G_DEFINE_TYPE_EXTENDED (GumScript,
                        gum_script,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (GUM_TYPE_INVOCATION_LISTENER,
                            gum_script_listener_iface_init));

static void
gum_script_class_init (GumScriptClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GumScriptPrivate));

  object_class->dispose = gum_script_dispose;
  object_class->finalize = gum_script_finalize;
}

static void
gum_script_listener_iface_init (gpointer g_iface,
                                gpointer iface_data)
{
  GumInvocationListenerIface * iface = (GumInvocationListenerIface *) g_iface;

  (void) iface_data;

  iface->on_enter = gum_script_on_enter;
  iface->on_leave = gum_script_on_leave;
}

static void
gum_script_init (GumScript * self)
{
  GumScriptPrivate * priv;

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GUM_TYPE_SCRIPT, GumScriptPrivate);

  priv->interceptor = gum_interceptor_obtain ();

  priv->attach_entries = g_queue_new ();
}

static void
gum_script_dispose (GObject * object)
{
  GumScript * self = GUM_SCRIPT (object);
  GumScriptPrivate * priv = self->priv;

  if (priv->interceptor != NULL)
  {
    gum_script_unload (self);

    g_object_unref (priv->interceptor);
    priv->interceptor = NULL;

    while (!g_queue_is_empty (priv->attach_entries))
    {
      GumScriptAttachEntry * entry = static_cast<GumScriptAttachEntry *> (
          g_queue_pop_tail (priv->attach_entries));
      entry->on_enter.Clear ();
      entry->on_leave.Clear ();
      g_slice_free (GumScriptAttachEntry, entry);
    }

    priv->raw_script.Dispose ();
    priv->context.Dispose ();
  }

  G_OBJECT_CLASS (gum_script_parent_class)->dispose (object);
}

static void
gum_script_finalize (GObject * object)
{
  GumScript * self = GUM_SCRIPT (object);
  GumScriptPrivate * priv = self->priv;

  if (priv->message_handler_notify != NULL)
    priv->message_handler_notify (priv->message_handler_data);

  g_queue_free (priv->attach_entries);

  G_OBJECT_CLASS (gum_script_parent_class)->finalize (object);
}

GumScript *
gum_script_from_string (const gchar * source,
                        GError ** error)
{
  GumScript * script = GUM_SCRIPT (g_object_new (GUM_TYPE_SCRIPT, NULL));

  Locker l;
  HandleScope handle_scope;

  Handle<ObjectTemplate> global_templ = ObjectTemplate::New ();

  Handle<ObjectTemplate> arg_templ = ObjectTemplate::New ();
  arg_templ->SetIndexedPropertyHandler (gum_script_on_get_nth_argument,
      0, 0, 0, 0, External::Wrap (script));
  global_templ->Set (String::New ("arg"), arg_templ);

  global_templ->Set (String::New ("_send"),
      FunctionTemplate::New (gum_script_on_send, External::Wrap (script)));

  Handle<ObjectTemplate> interceptor_templ = ObjectTemplate::New ();
  interceptor_templ->Set (String::New ("attach"), FunctionTemplate::New (
      gum_script_on_interceptor_attach, External::Wrap (script)));
  global_templ->Set (String::New ("Interceptor"), interceptor_templ);

  Handle<ObjectTemplate> memory_templ = ObjectTemplate::New ();
  memory_templ->Set (String::New ("readUtf8String"),
      FunctionTemplate::New (gum_script_on_memory_read_utf8_string));
  memory_templ->Set (String::New ("readUtf16String"),
      FunctionTemplate::New (gum_script_on_memory_read_utf16_string));
  global_templ->Set (String::New ("Memory"), memory_templ);

  Persistent<Context> context = Context::New (NULL, global_templ);
  Context::Scope context_scope (context);

  gchar * combined_source = g_strconcat (source,
      "\n"
      "\n"
      "function send(payload) {\n"
      "  var message = {\n"
      "    'type': 'send',\n"
      "    'payload': payload\n"
      "  };\n"
      "  _send(JSON.stringify(message));\n"
      "}\n",
      NULL);
  Handle<String> source_value = String::New (combined_source);
  g_free (combined_source);
  TryCatch trycatch;
  Handle<Script> raw_script = Script::Compile (source_value);
  if (raw_script.IsEmpty())
  {
    context.Dispose ();
    g_object_unref (script);

    Handle<Message> message = trycatch.Message ();
    Handle<Value> exception = trycatch.Exception ();
    String::AsciiValue exception_str (exception);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Script(line %d): %s",
        message->GetLineNumber (), *exception_str);

    return NULL;
  }

  script->priv->context = context;
  script->priv->raw_script = Persistent<Script>::New (raw_script);

  return script;
}

void
gum_script_set_message_handler (GumScript * self,
                                GumScriptMessageHandler func,
                                gpointer data,
                                GDestroyNotify notify)
{
  self->priv->message_handler_func = func;
  self->priv->message_handler_data = data;
  self->priv->message_handler_notify = notify;
}

void
gum_script_load (GumScript * self)
{
  GumScriptPrivate * priv = self->priv;

  Locker l;
  HandleScope handle_scope;
  Context::Scope context_scope (priv->context);

  TryCatch trycatch;
  priv->raw_script->Run ();

  if (trycatch.HasCaught () && priv->message_handler_func != NULL)
  {
    Handle<Message> message = trycatch.Message ();
    Handle<Value> exception = trycatch.Exception ();
    String::AsciiValue exception_str (exception);
    gchar * error = g_strdup_printf (
        "{\"type\":\"error\",\"lineNumber\":%d,\"description\":\"%s\"}",
        message->GetLineNumber (), *exception_str);
    priv->message_handler_func (self, error, priv->message_handler_data);
    g_free (error);
  }
}

void
gum_script_unload (GumScript * self)
{
  gum_interceptor_detach_listener (self->priv->interceptor,
      GUM_INVOCATION_LISTENER (self));
}

static Handle<Value>
gum_script_on_get_nth_argument (uint32_t index,
                                const AccessorInfo & info)
{
  GumScript * self = GUM_SCRIPT_CAST (External::Unwrap (info.Data ()));

  gpointer raw_value = gum_invocation_context_get_nth_argument (
      self->priv->current_invocation_context, index);

  return Number::New (GPOINTER_TO_SIZE (raw_value));
}

static Handle<Value>
gum_script_on_send (const Arguments & args)
{
  GumScript * self = GUM_SCRIPT_CAST (External::Unwrap (args.Data ()));
  GumScriptPrivate * priv = self->priv;

  if (priv->message_handler_func != NULL)
  {
    String::Utf8Value message (args[0]);
    priv->message_handler_func (self, *message, priv->message_handler_data);
  }

  return Undefined ();
}

static Handle<Value>
gum_script_on_interceptor_attach (const Arguments & args)
{
  GumScript * self = GUM_SCRIPT_CAST (External::Unwrap (args.Data ()));
  GumScriptPrivate * priv = self->priv;

  Local<Value> target_spec = args[0];
  if (!target_spec->IsNumber ())
  {
    ThrowException (Exception::TypeError (String::New ("Interceptor.attach: "
        "first argument must be a memory address")));
    return Undefined ();
  }

  Local<Value> callbacks_value = args[1];
  if (!callbacks_value->IsObject ())
  {
    ThrowException (Exception::TypeError (String::New ("Interceptor.attach: "
        "second argument must be a callback object")));
    return Undefined ();
  }

  Local<Function> on_enter, on_leave;

  Local<Object> callbacks = Local<Object>::Cast (callbacks_value);
  Local<Value> on_enter_value = callbacks->Get (String::New ("onEnter"));
  if (!on_enter_value.IsEmpty ())
  {
    if (!on_enter_value->IsFunction ())
    {
      ThrowException (Exception::TypeError (String::New ("Interceptor.attach: "
          "onEnter must be a function")));
      return Undefined ();
    }
    on_enter = Local<Function>::Cast (on_enter_value);
  }

  GumScriptAttachEntry * entry = g_slice_new (GumScriptAttachEntry);
  entry->on_enter = Persistent<Function>::New (on_enter);
  entry->on_leave = Persistent<Function>::New (on_leave);

  gpointer function_address = GSIZE_TO_POINTER (target_spec->IntegerValue ());
  GumAttachReturn attach_ret = gum_interceptor_attach_listener (
      priv->interceptor, function_address, GUM_INVOCATION_LISTENER (self),
      entry);

  g_queue_push_tail (priv->attach_entries, entry);

  return (attach_ret == GUM_ATTACH_OK) ? True () : False ();
}

static Handle<Value>
gum_script_on_memory_read_utf8_string (const Arguments & args)
{
  const char * data = static_cast<const char *> (
      GSIZE_TO_POINTER (args[0]->IntegerValue ()));
  return String::New (data, static_cast<int> (strlen (data)));
}

static Handle<Value>
gum_script_on_memory_read_utf16_string (const Arguments & args)
{
  const uint16_t * data = static_cast<const uint16_t *> (
      GSIZE_TO_POINTER (args[0]->IntegerValue ()));
  return String::New (data);
}

static void
gum_script_on_enter (GumInvocationListener * listener,
                     GumInvocationContext * context)
{
  (void) listener;

  GumScriptAttachEntry * entry = static_cast<GumScriptAttachEntry *> (
      gum_invocation_context_get_listener_function_data (context));
  if (!entry->on_enter.IsEmpty ())
  {
    g_assert (FALSE);
  }
}

static void
gum_script_on_leave (GumInvocationListener * listener,
                     GumInvocationContext * context)
{
  (void) listener;
  (void) context;
}