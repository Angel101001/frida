/*
 * Copyright (C) 2010-2014 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumscriptcore.h"

#include "gumscriptscope.h"

#include <ffi.h>
#include <string.h>

#define GUM_SCRIPT_CORE_LOCK()   (g_mutex_lock (&self->mutex))
#define GUM_SCRIPT_CORE_UNLOCK() (g_mutex_unlock (&self->mutex))

using namespace v8;

typedef struct _GumWeakRef GumWeakRef;
typedef struct _GumScriptJob GumScriptJob;
typedef struct _GumFFIFunction GumFFIFunction;
typedef struct _GumFFICallback GumFFICallback;
typedef union _GumFFIValue GumFFIValue;
typedef struct _GumFFITypeMapping GumFFITypeMapping;
typedef struct _GumFFIABIMapping GumFFIABIMapping;

struct _GumWeakRef
{
  gint id;
  GumPersistent<Value>::type * target;
  GumPersistent<Function>::type * callback;
  GumScriptCore * core;
};

struct _GumScriptJob
{
  GumScriptCoreJobFunc func;
  gpointer user_data;
  GDestroyNotify notify;
};

struct _GumScheduledCallback
{
  gint id;
  gboolean repeat;
  GumPersistent<Function>::type * func;
  GumPersistent<Value>::type * receiver;
  GSource * source;
  GumScriptCore * core;
};

struct _GumMessageSink
{
  GumPersistent<Function>::type * callback;
  GumPersistent<Value>::type * receiver;
  Isolate * isolate;
};

struct _GumFFIFunction
{
  gpointer fn;
  ffi_cif cif;
  ffi_type ** atypes;
  GumPersistent<Object>::type * weak_instance;
};

struct _GumFFICallback
{
  GumScriptCore * core;
  GumPersistent<Function>::type * func;
  GumPersistent<Value>::type * receiver;
  ffi_closure * closure;
  ffi_cif cif;
  ffi_type ** atypes;
  GumPersistent<Object>::type * weak_instance;
};

union _GumFFIValue
{
  gpointer v_pointer;
  gint v_sint;
  guint v_uint;
  glong v_slong;
  gulong v_ulong;
  gchar v_schar;
  guchar v_uchar;
  gfloat v_float;
  gdouble v_double;
  gint8 v_sint8;
  guint8 v_uint8;
  gint16 v_sint16;
  guint16 v_uint16;
  gint32 v_sint32;
  guint32 v_uint32;
  gint64 v_sint64;
  guint64 v_uint64;
};

struct _GumFFITypeMapping
{
  const gchar * name;
  ffi_type * type;
};

struct _GumFFIABIMapping
{
  const gchar * name;
  ffi_abi abi;
};

static void gum_script_core_on_weak_ref_bind (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_weak_ref_unbind (
    const FunctionCallbackInfo<Value> & info);
static GumWeakRef * gum_weak_ref_new (gint id, Handle<Value> target,
    Handle<Function> callback, GumScriptCore * core);
static void gum_weak_ref_free (GumWeakRef * ref);
static void gum_weak_ref_on_weak_notify (const WeakCallbackData<Value,
    GumWeakRef> & data);
static void gum_script_core_on_console_log (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_perform (GumScriptJob * job, GumScriptCore * self);
static void gum_script_core_on_set_timeout (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_set_interval (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_clear_timeout (
    const FunctionCallbackInfo<Value> & info);
static GumScheduledCallback * gum_scheduled_callback_new (gint id,
    gboolean repeat, GSource * source, GumScriptCore * core);
static void gum_scheduled_callback_free (GumScheduledCallback * callback);
static gboolean gum_scheduled_callback_invoke (gpointer user_data);
static void gum_script_core_on_send (const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_set_incoming_message_callback (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_wait_for_event (
    const FunctionCallbackInfo<Value> & info);

static void gum_script_core_on_new_native_pointer (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_native_pointer_is_null (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_native_pointer_add (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_native_pointer_sub (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_native_pointer_to_int32 (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_native_pointer_to_string (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_native_pointer_to_json (
    const FunctionCallbackInfo<Value> & info);

static void gum_script_core_on_new_native_function (
    const FunctionCallbackInfo<Value> & info);
static void gum_ffi_function_on_weak_notify (
    const WeakCallbackData<Object, GumFFIFunction> & data);
static void gum_script_core_on_invoke_native_function (
    const FunctionCallbackInfo<Value> & info);
static void gum_ffi_function_free (GumFFIFunction * func);

static void gum_script_core_on_new_native_callback (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_core_on_free_native_callback (
    const WeakCallbackData<Object, GumFFICallback> & data);
static void gum_script_core_on_invoke_native_callback (ffi_cif * cif,
    void * return_value, void ** args, void * user_data);
static void gum_ffi_callback_free (GumFFICallback * callback);

static GumMessageSink * gum_message_sink_new (Handle<Function> callback,
    Handle<Value> receiver, Isolate * isolate);
static void gum_message_sink_free (GumMessageSink * sink);
static void gum_message_sink_handle_message (GumMessageSink * self,
    const gchar * message);

static gboolean gum_script_ffi_type_get (GumScriptCore * core,
    Handle<Value> name, ffi_type ** type);
static gboolean gum_script_ffi_abi_get (GumScriptCore * core,
    Handle<Value> name, ffi_abi * abi);
static gboolean gum_script_value_to_ffi_type (GumScriptCore * core,
    const Handle<Value> svalue, GumFFIValue * value, const ffi_type * type);
static gboolean gum_script_value_from_ffi_type (GumScriptCore * core,
    Handle<Value> * svalue, const GumFFIValue * value, const ffi_type * type);

static void gum_byte_array_on_weak_notify (
    const WeakCallbackData<Object, GumByteArray> & data);
static void gum_heap_block_on_weak_notify (
    const WeakCallbackData<Object, GumHeapBlock> & data);

void
_gum_script_core_init (GumScriptCore * self,
                       GumScript * script,
                       GMainContext * main_context,
                       v8::Isolate * isolate,
                       Handle<ObjectTemplate> scope)
{
  self->script = script;
  self->main_context = main_context;
  self->isolate = isolate;

  g_mutex_init (&self->mutex);
  g_cond_init (&self->event_cond);

  self->weak_refs = g_hash_table_new_full (NULL, NULL, NULL,
      reinterpret_cast<GDestroyNotify> (gum_weak_ref_free));

  Local<External> data (External::New (isolate, self));

  Handle<ObjectTemplate> weak = ObjectTemplate::New ();
  weak->Set (String::NewFromUtf8 (isolate, "bind"),
      FunctionTemplate::New (isolate, gum_script_core_on_weak_ref_bind, data));
  weak->Set (String::NewFromUtf8 (isolate, "unbind"),
      FunctionTemplate::New (isolate, gum_script_core_on_weak_ref_unbind,
          data));
  scope->Set (String::NewFromUtf8 (isolate, "WeakRef"), weak);

  Handle<ObjectTemplate> console = ObjectTemplate::New ();
  console->Set (String::NewFromUtf8 (isolate, "log"),
      FunctionTemplate::New (isolate, gum_script_core_on_console_log, data));
  scope->Set (String::NewFromUtf8 (isolate, "console"), console);

  scope->Set (String::NewFromUtf8 (isolate, "setTimeout"),
      FunctionTemplate::New (isolate, gum_script_core_on_set_timeout, data));
  scope->Set (String::NewFromUtf8 (isolate, "setInterval"),
      FunctionTemplate::New (isolate, gum_script_core_on_set_interval, data));
  scope->Set (String::NewFromUtf8 (isolate, "clearTimeout"),
      FunctionTemplate::New (isolate, gum_script_core_on_clear_timeout, data));
  scope->Set (String::NewFromUtf8 (isolate, "clearInterval"),
      FunctionTemplate::New (isolate, gum_script_core_on_clear_timeout, data));
  scope->Set (String::NewFromUtf8 (isolate, "_send"),
      FunctionTemplate::New (isolate, gum_script_core_on_send, data));
  scope->Set (String::NewFromUtf8 (isolate, "_setIncomingMessageCallback"),
      FunctionTemplate::New (isolate,
          gum_script_core_on_set_incoming_message_callback, data));
  scope->Set (String::NewFromUtf8 (isolate, "_waitForEvent"),
      FunctionTemplate::New (isolate,
          gum_script_core_on_wait_for_event, data));

  Local<FunctionTemplate> native_pointer = FunctionTemplate::New (isolate,
      gum_script_core_on_new_native_pointer, data);
  native_pointer->SetClassName (
      String::NewFromUtf8 (isolate, "NativePointer"));
  Local<ObjectTemplate> native_pointer_proto =
      native_pointer->PrototypeTemplate ();
  native_pointer_proto->Set (String::NewFromUtf8 (isolate, "isNull"),
      FunctionTemplate::New (isolate,
          gum_script_core_on_native_pointer_is_null));
  native_pointer_proto->Set (String::NewFromUtf8 (isolate, "add"),
      FunctionTemplate::New (isolate,
          gum_script_core_on_native_pointer_add, data));
  native_pointer_proto->Set (String::NewFromUtf8 (isolate, "sub"),
      FunctionTemplate::New (isolate,
          gum_script_core_on_native_pointer_sub, data));
  native_pointer_proto->Set (String::NewFromUtf8 (isolate, "toInt32"),
      FunctionTemplate::New (isolate,
          gum_script_core_on_native_pointer_to_int32, data));
  native_pointer_proto->Set (String::NewFromUtf8 (isolate, "toString"),
      FunctionTemplate::New (isolate,
          gum_script_core_on_native_pointer_to_string, data));
  native_pointer_proto->Set (String::NewFromUtf8 (isolate, "toJSON"),
      FunctionTemplate::New (isolate,
          gum_script_core_on_native_pointer_to_json, data));
  native_pointer->InstanceTemplate ()->SetInternalFieldCount (1);
  scope->Set (String::NewFromUtf8 (isolate, "NativePointer"), native_pointer);
  self->native_pointer =
      new GumPersistent<FunctionTemplate>::type (isolate, native_pointer);

  Local<FunctionTemplate> native_function = FunctionTemplate::New (isolate,
      gum_script_core_on_new_native_function, data);
  native_function->SetClassName (
      String::NewFromUtf8 (isolate, "NativeFunction"));
  native_function->Inherit (native_pointer);
  Local<ObjectTemplate> native_function_object =
      native_function->InstanceTemplate ();
  native_function_object->SetCallAsFunctionHandler (
      gum_script_core_on_invoke_native_function, data);
  native_function_object->SetInternalFieldCount (2);
  scope->Set (String::NewFromUtf8 (isolate, "NativeFunction"),
      native_function);

  Local<FunctionTemplate> native_callback = FunctionTemplate::New (isolate,
      gum_script_core_on_new_native_callback, data);
  native_callback->SetClassName (
      String::NewFromUtf8 (isolate, "NativeCallback"));
  native_callback->Inherit (native_pointer);
  native_callback->InstanceTemplate ()->SetInternalFieldCount (1);
  scope->Set (String::NewFromUtf8 (isolate, "NativeCallback"),
      native_callback);
}

void
_gum_script_core_realize (GumScriptCore * self)
{
  Local<FunctionTemplate> native_pointer (
      Local<FunctionTemplate>::New (self->isolate, *self->native_pointer));
  self->native_pointer_value = new GumPersistent<Object>::type (self->isolate,
      native_pointer->InstanceTemplate ()->NewInstance ());

  self->thread_pool = g_thread_pool_new (
      reinterpret_cast<GFunc> (gum_script_core_perform), self, 1, FALSE, NULL);
}

void
_gum_script_core_flush (GumScriptCore * self)
{
  g_thread_pool_free (self->thread_pool, FALSE, TRUE);
  self->thread_pool = NULL;

  g_hash_table_remove_all (self->weak_refs);
}

void
_gum_script_core_dispose (GumScriptCore * self)
{
  while (self->scheduled_callbacks != NULL)
  {
    g_source_destroy (static_cast<GumScheduledCallback *> (
        self->scheduled_callbacks->data)->source);
    self->scheduled_callbacks = g_slist_delete_link (
        self->scheduled_callbacks, self->scheduled_callbacks);
  }

  gum_message_sink_free (self->incoming_message_sink);
  self->incoming_message_sink = NULL;

  if (self->message_handler_notify != NULL)
    self->message_handler_notify (self->message_handler_data);

  delete self->native_pointer_value;
  self->native_pointer_value = NULL;
}

void
_gum_script_core_finalize (GumScriptCore * self)
{
  g_hash_table_unref (self->weak_refs);
  self->weak_refs = NULL;

  delete self->native_pointer;
  self->native_pointer = NULL;

  g_mutex_clear (&self->mutex);
  g_cond_clear (&self->event_cond);
}

void
_gum_script_core_set_message_handler (GumScriptCore * self,
                                      GumScriptMessageHandler func,
                                      gpointer data,
                                      GDestroyNotify notify)
{
  self->message_handler_func = func;
  self->message_handler_data = data;
  self->message_handler_notify = notify;
}

void
_gum_script_core_emit_message (GumScriptCore * self,
                               const gchar * message,
                               const guint8 * data,
                               gint data_length)
{
  if (self->message_handler_func != NULL)
  {
    self->message_handler_func (self->script, message, data, data_length,
        self->message_handler_data);
  }
}

void
_gum_script_core_post_message (GumScriptCore * self,
                               const gchar * message)
{
  if (self->incoming_message_sink != NULL)
  {
    {
      ScriptScope scope (self->script);
      gum_message_sink_handle_message (self->incoming_message_sink, message);
      self->event_count++;
    }

    GUM_SCRIPT_CORE_LOCK ();
    g_cond_broadcast (&self->event_cond);
    GUM_SCRIPT_CORE_UNLOCK ();
  }
}

static void
gum_script_core_on_weak_ref_bind (const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = info.GetIsolate ();
  GumWeakRef * ref;

  Local<Value> target = info[0];
  if (target->IsUndefined () || target->IsNull ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "first argument must be a value with a regular lifespan")));
    return;
  }

  Local<Value> callback_val = info[1];
  if (!callback_val->IsFunction ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "second argument must be a function")));
    return;
  }

  gint id = g_atomic_int_add (&self->last_weak_ref_id, 1) + 1;

  ref = gum_weak_ref_new (id, target, callback_val.As <Function> (), self);
  g_hash_table_insert (self->weak_refs, GINT_TO_POINTER (id), ref);

  info.GetReturnValue ().Set (id);
}

static void
gum_script_core_on_weak_ref_unbind (const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = info.GetIsolate ();

  Local<Value> id_val = info[0];
  if (!id_val->IsNumber ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "argument must be a weak ref id")));
    return;
  }
  gint id = id_val->ToInt32 ()->Value ();

  bool removed =
      !!g_hash_table_remove (self->weak_refs, GINT_TO_POINTER (id));

  info.GetReturnValue ().Set (removed);
}

static GumWeakRef *
gum_weak_ref_new (gint id,
                  Handle<Value> target,
                  Handle<Function> callback,
                  GumScriptCore * core)
{
  GumWeakRef * ref;
  Isolate * isolate = core->isolate;

  ref = g_slice_new (GumWeakRef);
  ref->id = id;
  ref->target = new GumPersistent<Value>::type (isolate, target);
  ref->target->SetWeak (ref, gum_weak_ref_on_weak_notify);
  ref->target->MarkIndependent ();
  ref->callback = new GumPersistent<Function>::type (isolate, callback);
  ref->core = core;

  return ref;
}

static void
gum_weak_ref_free (GumWeakRef * ref)
{
  {
    ScriptScope scope (ref->core->script);
    Isolate * isolate = ref->core->isolate;
    Local<Function> callback (Local<Function>::New (isolate, *ref->callback));
    callback->Call (Null (isolate), 0, NULL);
  }

  delete ref->target;
  delete ref->callback;

  g_slice_free (GumWeakRef, ref);
}

static void
gum_weak_ref_on_weak_notify (const WeakCallbackData<Value,
    GumWeakRef> & data)
{
  GumWeakRef * self = data.GetParameter ();

  g_hash_table_remove (self->core->weak_refs, GINT_TO_POINTER (self->id));
}

static void
gum_script_core_on_console_log (const FunctionCallbackInfo<Value> & info)
{
  String::Utf8Value message (info[0]);
  g_print ("%s\n", *message);
}

void
_gum_script_core_push_job (GumScriptCore * self,
                           GumScriptCoreJobFunc job_func,
                           gpointer user_data,
                           GDestroyNotify notify)
{
  GumScriptJob * job;

  job = g_slice_new (GumScriptJob);
  job->func = job_func;
  job->user_data = user_data;
  job->notify = notify;
  g_thread_pool_push (self->thread_pool, job, NULL);
}

static void
gum_script_core_perform (GumScriptJob * job,
                         GumScriptCore * self)
{
  (void) self;

  job->func (job->user_data);
  job->notify (job->user_data);
  g_slice_free (GumScriptJob, job);
}

static void
gum_script_core_add_scheduled_callback (GumScriptCore * self,
                                        GumScheduledCallback * callback)
{
  GUM_SCRIPT_CORE_LOCK ();
  self->scheduled_callbacks =
      g_slist_prepend (self->scheduled_callbacks, callback);
  GUM_SCRIPT_CORE_UNLOCK ();
}

static void
gum_script_core_remove_scheduled_callback (GumScriptCore * self,
                                           GumScheduledCallback * callback)
{
  GUM_SCRIPT_CORE_LOCK ();
  self->scheduled_callbacks =
      g_slist_remove (self->scheduled_callbacks, callback);
  GUM_SCRIPT_CORE_UNLOCK ();
}

static void
gum_script_core_on_schedule_callback (const FunctionCallbackInfo<Value> & info,
                                      gboolean repeat)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;

  Local<Value> func_val = info[0];
  if (!func_val->IsFunction ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "first argument must be a function")));
    return;
  }

  Local<Value> delay_val = info[1];
  if (!delay_val->IsNumber ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "second argument must be a number specifying delay")));
    return;
  }
  int32_t delay = delay_val->ToInt32 ()->Value ();
  if (delay < 0)
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "second argument must be a positive integer")));
    return;
  }

  gint id = g_atomic_int_add (&self->last_callback_id, 1) + 1;
  GSource * source;
  if (delay == 0)
    source = g_idle_source_new ();
  else
    source = g_timeout_source_new (delay);
  GumScheduledCallback * callback =
      gum_scheduled_callback_new (id, repeat, source, self);
  callback->func = new GumPersistent<Function>::type (isolate,
      func_val.As <Function> ());
  callback->receiver = new GumPersistent<Value>::type (isolate, info.This ());
  g_source_set_callback (source, gum_scheduled_callback_invoke, callback,
      reinterpret_cast<GDestroyNotify> (gum_scheduled_callback_free));
  gum_script_core_add_scheduled_callback (self, callback);

  g_source_attach (source, self->main_context);

  info.GetReturnValue ().Set (id);
}

static void
gum_script_core_on_set_timeout (const FunctionCallbackInfo<Value> & info)
{
  gum_script_core_on_schedule_callback (info, FALSE);
}

static void
gum_script_core_on_set_interval (const FunctionCallbackInfo<Value> & info)
{
  gum_script_core_on_schedule_callback (info, TRUE);
}

static void
gum_script_core_on_clear_timeout (const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;
  GSList * cur;

  Local<Value> id_val = info[0];
  if (!id_val->IsNumber ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "argument must be a timeout id")));
    return;
  }
  gint id = id_val->ToInt32 ()->Value ();

  GumScheduledCallback * callback = NULL;
  GUM_SCRIPT_CORE_LOCK ();
  for (cur = self->scheduled_callbacks; cur != NULL; cur = cur->next)
  {
    GumScheduledCallback * cb =
        static_cast<GumScheduledCallback *> (cur->data);
    if (cb->id == id)
    {
      callback = cb;
      self->scheduled_callbacks =
          g_slist_delete_link (self->scheduled_callbacks, cur);
      break;
    }
  }
  GUM_SCRIPT_CORE_UNLOCK ();

  if (callback != NULL)
    g_source_destroy (callback->source);

  info.GetReturnValue ().Set (callback != NULL);
}

static GumScheduledCallback *
gum_scheduled_callback_new (gint id,
                            gboolean repeat,
                            GSource * source,
                            GumScriptCore * core)
{
  GumScheduledCallback * callback;

  callback = g_slice_new (GumScheduledCallback);
  callback->id = id;
  callback->repeat = repeat;
  callback->source = source;
  callback->core = core;

  return callback;
}

static void
gum_scheduled_callback_free (GumScheduledCallback * callback)
{
  ScriptScope (callback->core->script);
  delete callback->func;
  delete callback->receiver;

  g_slice_free (GumScheduledCallback, callback);
}

static gboolean
gum_scheduled_callback_invoke (gpointer user_data)
{
  GumScheduledCallback * self =
      static_cast<GumScheduledCallback *> (user_data);
  Isolate * isolate = self->core->isolate;

  ScriptScope scope (self->core->script);
  Local<Function> func (Local<Function>::New (isolate, *self->func));
  Local<Value> receiver (Local<Value>::New (isolate, *self->receiver));
  func->Call (receiver, 0, NULL);

  if (!self->repeat)
    gum_script_core_remove_scheduled_callback (self->core, self);

  return self->repeat;
}

static void
gum_script_core_on_send (const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;

  String::Utf8Value message (info[0]);

  const guint8 * data = NULL;
  gint data_length = 0;
  if (!info[1]->IsNull ())
  {
    Local<Object> array = info[1]->ToObject ();
    if (array->HasIndexedPropertiesInExternalArrayData () &&
        array->GetIndexedPropertiesExternalArrayDataType ()
        == kExternalUint8Array)
    {
      data = static_cast<guint8 *> (
          array->GetIndexedPropertiesExternalArrayData ());
      data_length = array->GetIndexedPropertiesExternalArrayDataLength ();
    }
    else
    {
      isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
          isolate, "unsupported data value")));
      return;
    }
  }

  _gum_script_core_emit_message (self, *message, data, data_length);
}

static void
gum_script_core_on_set_incoming_message_callback (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;

  if (info.Length () > 1)
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "invalid argument count")));
    return;
  }

  gum_message_sink_free (self->incoming_message_sink);
  self->incoming_message_sink = NULL;

  if (info.Length () == 1)
  {
    self->incoming_message_sink =
        gum_message_sink_new (info[0].As<Function> (), info.This (), isolate);
  }
}

static void
gum_script_core_on_wait_for_event (const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  guint start_count;

  start_count = self->event_count;
  while (self->event_count == start_count)
  {
    self->isolate->Exit ();

    {
      Unlocker ul (self->isolate);

      GUM_SCRIPT_CORE_LOCK ();
      g_cond_wait (&self->event_cond, &self->mutex);
      GUM_SCRIPT_CORE_UNLOCK ();
    }

    self->isolate->Enter ();
  }
}

static void
gum_script_core_on_new_native_pointer (
    const FunctionCallbackInfo<Value> & info)
{
  guint64 ptr;

  if (info.Length () == 0)
  {
    ptr = 0;
  }
  else
  {
    GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
    Isolate * isolate = self->isolate;

    String::Utf8Value ptr_as_utf8 (info[0]);
    const gchar * ptr_as_string = *ptr_as_utf8;
    gchar * endptr;
    if (g_str_has_prefix (ptr_as_string, "0x"))
    {
      ptr = g_ascii_strtoull (ptr_as_string + 2, &endptr, 16);
      if (endptr == ptr_as_string + 2)
      {
        isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
            isolate, "NativePointer: argument is not a valid "
            "hexadecimal string")));
        return;
      }
    }
    else
    {
      ptr = g_ascii_strtoull (ptr_as_string, &endptr, 10);
      if (endptr == ptr_as_string)
      {
        isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
            isolate, "NativePointer: argument is not a valid "
            "decimal string")));
        return;
      }
    }
  }

  info.Holder ()->SetInternalField (0,
      External::New (info.GetIsolate (), GSIZE_TO_POINTER (ptr)));
}

static void
gum_script_core_on_native_pointer_is_null (
    const FunctionCallbackInfo<Value> & info)
{
  info.GetReturnValue ().Set (GUM_NATIVE_POINTER_VALUE (info.Holder ()) == 0);
}

static void
gum_script_core_on_native_pointer_add (const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());

  guint64 lhs = reinterpret_cast<guint64> (
      GUM_NATIVE_POINTER_VALUE (info.Holder ()));

  gpointer result;
  Local<FunctionTemplate> native_pointer (
    Local<FunctionTemplate>::New (self->isolate, *self->native_pointer));
  if (native_pointer->HasInstance (info[0]))
  {
    guint64 rhs = reinterpret_cast<guint64> (
        GUM_NATIVE_POINTER_VALUE (info[0].As<Object> ()));
    result = GSIZE_TO_POINTER (lhs + rhs);
  }
  else
  {
    result = GSIZE_TO_POINTER (lhs + info[0]->ToInteger ()->Value ());
  }

  info.GetReturnValue ().Set (_gum_script_pointer_new (result, self));
}

static void
gum_script_core_on_native_pointer_sub (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());

  guint64 lhs = reinterpret_cast<guint64> (
      GUM_NATIVE_POINTER_VALUE (info.Holder ()));

  gpointer result;
  Local<FunctionTemplate> native_pointer (
      Local<FunctionTemplate>::New (self->isolate, *self->native_pointer));
  if (native_pointer->HasInstance (info[0]))
  {
    guint64 rhs = reinterpret_cast<guint64> (
        GUM_NATIVE_POINTER_VALUE (info[0].As<Object> ()));
    result = GSIZE_TO_POINTER (lhs - rhs);
  }
  else
  {
    result = GSIZE_TO_POINTER (lhs - info[0]->ToInteger ()->Value ());
  }

  info.GetReturnValue ().Set (_gum_script_pointer_new (result, self));
}

static void
gum_script_core_on_native_pointer_to_int32 (
    const FunctionCallbackInfo<Value> & info)
{
  info.GetReturnValue ().Set (static_cast<int32_t> (GPOINTER_TO_SIZE (
      GUM_NATIVE_POINTER_VALUE (info.Holder ()))));
}

static void
gum_script_core_on_native_pointer_to_string (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;

  gsize ptr = GPOINTER_TO_SIZE (GUM_NATIVE_POINTER_VALUE (info.Holder ()));
  gint radix = 16;
  bool radix_specified = info.Length () > 0;
  if (radix_specified)
    radix = info[0]->Int32Value ();
  if (radix != 10 && radix != 16)
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "unsupported radix")));
    return;
  }

  gchar buf[32];
  if (radix == 10)
  {
    sprintf (buf, "%" G_GSIZE_MODIFIER "u", ptr);
  }
  else
  {
    if (radix_specified)
      sprintf (buf, "%" G_GSIZE_MODIFIER "x", ptr);
    else
      sprintf (buf, "0x%" G_GSIZE_MODIFIER "x", ptr);
  }

  info.GetReturnValue ().Set (String::NewFromUtf8 (isolate, buf));
}

static void
gum_script_core_on_native_pointer_to_json (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;

  gsize ptr = GPOINTER_TO_SIZE (GUM_NATIVE_POINTER_VALUE (info.Holder ()));

  gchar buf[32];
  sprintf (buf, "0x%" G_GSIZE_MODIFIER "x", ptr);

  info.GetReturnValue ().Set (String::NewFromUtf8 (isolate, buf));
}

static void
gum_script_core_on_new_native_function (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;
  GumFFIFunction * func;
  Local<Value> rtype_value;
  ffi_type * rtype;
  Local<Value> atypes_value;
  Local<Array> atypes_array;
  uint32_t nargs_fixed, nargs_total, i;
  gboolean is_variadic;
  ffi_abi abi;
  Local<Object> instance;

  func = g_slice_new0 (GumFFIFunction);

  if (!_gum_script_pointer_get (info[0], &func->fn, self))
    goto error;

  rtype_value = info[1];
  if (!rtype_value->IsString ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeFunction: second argument must be a string specifying "
        "return type")));
    goto error;
  }
  if (!gum_script_ffi_type_get (self, rtype_value, &rtype))
    goto error;

  atypes_value = info[2];
  if (!atypes_value->IsArray ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeFunction: third argument must be an array specifying "
        "argument types")));
    goto error;
  }
  atypes_array = atypes_value.As<Array> ();
  nargs_fixed = nargs_total = atypes_array->Length ();
  is_variadic = FALSE;
  func->atypes = g_new (ffi_type *, nargs_total);
  for (i = 0; i != nargs_total; i++)
  {
    Handle<Value> type (atypes_array->Get (i));
    String::Utf8Value type_utf (type);
    if (strcmp (*type_utf, "...") == 0)
    {
      if (is_variadic)
      {
        isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
            isolate, "NativeFunction: only one variadic marker may be "
            "specified")));
        goto error;
      }

      nargs_fixed = i;
      is_variadic = TRUE;
    }
    else if (!gum_script_ffi_type_get (self, type,
        &func->atypes[is_variadic ? i - 1 : i]))
    {
      goto error;
    }
  }
  if (is_variadic)
    nargs_total--;

  abi = FFI_DEFAULT_ABI;
  if (info.Length () > 3)
  {
    if (!gum_script_ffi_abi_get (self, info[3], &abi))
      goto error;
  }

  if (is_variadic)
  {
    if (ffi_prep_cif_var (&func->cif, abi, nargs_fixed, nargs_total, rtype,
        func->atypes) != FFI_OK)
    {
      isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
          isolate, "NativeFunction: failed to compile function call "
          "interface")));
      goto error;
    }
  }
  else
  {
    if (ffi_prep_cif (&func->cif, abi, nargs_total, rtype,
        func->atypes) != FFI_OK)
    {
      isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
          isolate, "NativeFunction: failed to compile function call "
          "interface")));
      goto error;
    }
  }

  instance = info.Holder ();
  instance->SetInternalField (0, External::New (isolate, func->fn));
  instance->SetAlignedPointerInInternalField (1, func);

  func->weak_instance = new GumPersistent<Object>::type (isolate, instance);
  func->weak_instance->SetWeak (func, gum_ffi_function_on_weak_notify);
  func->weak_instance->MarkIndependent ();

  return;

error:
  gum_ffi_function_free (func);

  return;
}

static void
gum_ffi_function_on_weak_notify (
    const WeakCallbackData<Object, GumFFIFunction> & data)
{
  HandleScope handle_scope (data.GetIsolate ());
  gum_ffi_function_free (data.GetParameter ());
}

static void
gum_script_core_on_invoke_native_function (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;
  Local<Object> instance = info.Holder ();
  GumFFIFunction * func = static_cast<GumFFIFunction *> (
      instance->GetAlignedPointerFromInternalField (1));

  if (info.Length () != static_cast<gint> (func->cif.nargs))
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeFunction: bad argument count")));
    return;
  }

  GumFFIValue rvalue;
  void ** avalue = static_cast<void **> (
      g_alloca (func->cif.nargs * sizeof (void *)));
  GumFFIValue * ffi_args = static_cast<GumFFIValue *> (
      g_alloca (func->cif.nargs * sizeof (GumFFIValue)));
  for (uint32_t i = 0; i != func->cif.nargs; i++)
  {
    if (!gum_script_value_to_ffi_type (self, info[i], &ffi_args[i],
        func->cif.arg_types[i]))
    {
      return;
    }
    avalue[i] = &ffi_args[i];
  }

  ffi_call (&func->cif, FFI_FN (func->fn), &rvalue, avalue);

  Local<Value> result;
  if (!gum_script_value_from_ffi_type (self, &result, &rvalue, func->cif.rtype))
    return;

  info.GetReturnValue ().Set (result);
}

static void
gum_ffi_function_free (GumFFIFunction * func)
{
  delete func->weak_instance;
  g_free (func->atypes);
  g_slice_free (GumFFIFunction, func);
}

static void
gum_script_core_on_new_native_callback (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptCore * self = static_cast<GumScriptCore *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = self->isolate;
  GumFFICallback * callback;
  Local<Value> func_value;
  Local<Value> rtype_value;
  ffi_type * rtype;
  Local<Value> atypes_value;
  Local<Array> atypes_array;
  uint32_t nargs, i;
  ffi_abi abi;
  gpointer func = NULL;
  Local<Object> instance;

  callback = g_slice_new0 (GumFFICallback);
  callback->core = self;

  func_value = info[0];
  if (!func_value->IsFunction ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeCallback: first argument must be a function implementing "
        "the callback")));
    goto error;
  }
  callback->func = new GumPersistent<Function>::type (isolate,
      func_value.As<Function> ());
  callback->receiver = new GumPersistent<Value>::type (isolate, info.This ());

  rtype_value = info[1];
  if (!rtype_value->IsString ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeCallback: second argument must be a string specifying "
        "return type")));
    goto error;
  }
  if (!gum_script_ffi_type_get (self, rtype_value, &rtype))
    goto error;

  atypes_value = info[2];
  if (!atypes_value->IsArray ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeCallback: third argument must be an array specifying "
        "argument types")));
    goto error;
  }
  atypes_array = atypes_value.As<Array> ();
  nargs = atypes_array->Length ();
  callback->atypes = g_new (ffi_type *, nargs);
  for (i = 0; i != nargs; i++)
  {
    if (!gum_script_ffi_type_get (self, atypes_array->Get (i),
        &callback->atypes[i]))
    {
      goto error;
    }
  }

  abi = FFI_DEFAULT_ABI;
  if (info.Length () > 3)
  {
    if (!gum_script_ffi_abi_get (self, info[3], &abi))
      goto error;
  }

  callback->closure = static_cast<ffi_closure *> (
      ffi_closure_alloc (sizeof (ffi_closure), &func));
  if (callback->closure == NULL)
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeCallback: failed to allocate closure")));
    goto error;
  }

  if (ffi_prep_cif (&callback->cif, abi, nargs, rtype,
        callback->atypes) != FFI_OK)
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeCallback: failed to compile function call interface")));
    goto error;
  }

  if (ffi_prep_closure_loc (callback->closure, &callback->cif,
        gum_script_core_on_invoke_native_callback, callback, func) != FFI_OK)
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "NativeCallback: failed to prepare closure")));
    goto error;
  }

  instance = info.Holder ();
  instance->SetInternalField (0, External::New (isolate, func));

  callback->weak_instance = new GumPersistent<Object>::type (isolate, instance);
  callback->weak_instance->SetWeak (callback,
      gum_script_core_on_free_native_callback);
  callback->weak_instance->MarkIndependent ();

  return;

error:
  gum_ffi_callback_free (callback);
  return;
}

static void
gum_script_core_on_free_native_callback (
    const WeakCallbackData<Object, GumFFICallback> & data)
{
  HandleScope handle_scope (data.GetIsolate ());
  gum_ffi_callback_free (data.GetParameter ());
}

static void
gum_script_core_on_invoke_native_callback (ffi_cif * cif,
                                           void * return_value,
                                           void ** args,
                                           void * user_data)
{
  GumFFICallback * self = static_cast<GumFFICallback *> (user_data);
  ScriptScope scope (self->core->script);
  Isolate * isolate = self->core->isolate;

  Local<Value> * argv = static_cast<Local<Value> *> (
      g_alloca (cif->nargs * sizeof (Local<Value>)));
  for (guint i = 0; i != cif->nargs; i++)
  {
    if (!gum_script_value_from_ffi_type (self->core, &argv[i],
          static_cast<GumFFIValue *> (args[i]), cif->arg_types[i]))
    {
      return;
    }
  }

  Local<Function> func (Local<Function>::New (isolate, *self->func));
  Local<Value> receiver (Local<Value>::New (isolate, *self->receiver));
  Local<Value> result = func->Call (receiver, cif->nargs, argv);
  if (cif->rtype != &ffi_type_void)
  {
    gum_script_value_to_ffi_type (self->core, result,
        static_cast<GumFFIValue *> (return_value), cif->rtype);
  }
}

static void
gum_ffi_callback_free (GumFFICallback * callback)
{
  delete callback->weak_instance;

  delete callback->func;
  delete callback->receiver;

  ffi_closure_free (callback->closure);
  g_free (callback->atypes);

  g_slice_free (GumFFICallback, callback);
}

static GumMessageSink *
gum_message_sink_new (Handle<Function> callback,
                      Handle<Value> receiver,
                      Isolate * isolate)
{
  GumMessageSink * sink;

  sink = g_slice_new (GumMessageSink);
  sink->callback = new GumPersistent<Function>::type (isolate, callback);
  sink->receiver = new GumPersistent<Value>::type (isolate, receiver);
  sink->isolate = isolate;

  return sink;
}

static void
gum_message_sink_free (GumMessageSink * sink)
{
  if (sink == NULL)
    return;

  delete sink->callback;
  delete sink->receiver;

  g_slice_free (GumMessageSink, sink);
}

static void
gum_message_sink_handle_message (GumMessageSink * self,
                                 const gchar * message)
{
  Isolate * isolate = self->isolate;
  Handle<Value> argv[] = { String::NewFromUtf8 (isolate, message) };

  Local<Function> callback (Local<Function>::New (isolate, *self->callback));
  Local<Value> receiver (Local<Value>::New (isolate, *self->receiver));
  callback->Call (receiver, 1, argv);
}

static const GumFFITypeMapping gum_ffi_type_mappings[] =
{
  { "void", &ffi_type_void },
  { "pointer", &ffi_type_pointer },
  { "int", &ffi_type_sint },
  { "uint", &ffi_type_uint },
  { "long", &ffi_type_slong },
  { "ulong", &ffi_type_ulong },
  { "char", &ffi_type_schar },
  { "uchar", &ffi_type_uchar },
  { "float", &ffi_type_float },
  { "double", &ffi_type_double },
  { "int8", &ffi_type_sint8 },
  { "uint8", &ffi_type_uint8 },
  { "int16", &ffi_type_sint16 },
  { "uint16", &ffi_type_uint16 },
  { "int32", &ffi_type_sint32 },
  { "uint32", &ffi_type_uint32 },
  { "int64", &ffi_type_sint64 },
  { "uint64", &ffi_type_uint64 }
};

static const GumFFIABIMapping gum_ffi_abi_mappings[] =
{
  { "default", FFI_DEFAULT_ABI },
#if defined (X86_WIN32)
  { "sysv", FFI_SYSV },
  { "stdcall", FFI_STDCALL },
  { "thiscall", FFI_THISCALL },
  { "fastcall", FFI_FASTCALL },
  { "mscdecl", FFI_MS_CDECL }
#elif defined (X86_WIN64)
  { "win64", FFI_WIN64 }
#elif defined (X86_ANY)
  { "sysv", FFI_SYSV },
  { "unix64", FFI_UNIX64 }
#elif defined (ARM)
  { "sysv", FFI_SYSV },
  { "vfp", FFI_VFP }
#endif
};

static gboolean
gum_script_ffi_type_get (GumScriptCore * core,
                         Handle<Value> name,
                         ffi_type ** type)
{
  String::Utf8Value str_value (name);
  const gchar * str = *str_value;
  for (guint i = 0; i != G_N_ELEMENTS (gum_ffi_type_mappings); i++)
  {
    const GumFFITypeMapping * m = &gum_ffi_type_mappings[i];
    if (strcmp (str, m->name) == 0)
    {
      *type = m->type;
      return TRUE;
    }
  }

  core->isolate->ThrowException (Exception::TypeError (
      String::NewFromUtf8 (core->isolate, "invalid type specified")));
  return FALSE;
}

static gboolean
gum_script_ffi_abi_get (GumScriptCore * core,
                        Handle<Value> name,
                        ffi_abi * abi)
{
  String::Utf8Value str_value (name);
  const gchar * str = *str_value;
  for (guint i = 0; i != G_N_ELEMENTS (gum_ffi_abi_mappings); i++)
  {
    const GumFFIABIMapping * m = &gum_ffi_abi_mappings[i];
    if (strcmp (str, m->name) == 0)
    {
      *abi = m->abi;
      return TRUE;
    }
  }

  core->isolate->ThrowException (Exception::TypeError (
      String::NewFromUtf8 (core->isolate, "invalid abi specified")));
  return FALSE;
}

static gboolean
gum_script_value_to_ffi_type (GumScriptCore * core,
                              const Handle<Value> svalue,
                              GumFFIValue * value,
                              const ffi_type * type)
{
  if (type == &ffi_type_void)
  {
    value->v_pointer = NULL;
  }
  else if (type == &ffi_type_pointer)
  {
    if (!_gum_script_pointer_get (svalue, &value->v_pointer, core))
      return FALSE;
  }
  else if (type == &ffi_type_sint)
  {
    value->v_sint = svalue->IntegerValue ();
  }
  else if (type == &ffi_type_uint)
  {
    value->v_uint = static_cast<guint> (svalue->IntegerValue ());
  }
  else if (type == &ffi_type_slong)
  {
    value->v_slong = svalue->IntegerValue ();
  }
  else if (type == &ffi_type_ulong)
  {
    value->v_ulong = static_cast<gulong> (svalue->IntegerValue ());
  }
  else if (type == &ffi_type_schar)
  {
    value->v_schar = static_cast<gchar> (svalue->Int32Value ());
  }
  else if (type == &ffi_type_uchar)
  {
    value->v_uchar = static_cast<guchar> (svalue->Uint32Value ());
  }
  else if (type == &ffi_type_float)
  {
    value->v_float = svalue->NumberValue ();
  }
  else if (type == &ffi_type_double)
  {
    value->v_double = svalue->NumberValue ();
  }
  else if (type == &ffi_type_sint8)
  {
    value->v_sint8 = static_cast<gint8> (svalue->Int32Value ());
  }
  else if (type == &ffi_type_uint8)
  {
    value->v_uint8 = static_cast<guint8> (svalue->Uint32Value ());
  }
  else if (type == &ffi_type_sint16)
  {
    value->v_sint16 = static_cast<gint16> (svalue->Int32Value ());
  }
  else if (type == &ffi_type_uint16)
  {
    value->v_uint16 = static_cast<guint16> (svalue->Uint32Value ());
  }
  else if (type == &ffi_type_sint32)
  {
    value->v_sint32 = static_cast<gint32> (svalue->Int32Value ());
  }
  else if (type == &ffi_type_uint32)
  {
    value->v_uint32 = static_cast<guint32> (svalue->Uint32Value ());
  }
  else if (type == &ffi_type_sint64)
  {
    value->v_sint64 = static_cast<gint64> (svalue->IntegerValue ());
  }
  else if (type == &ffi_type_uint64)
  {
    value->v_uint64 = static_cast<guint64> (svalue->IntegerValue ());
  }
  else
  {
    core->isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        core->isolate, "value_to_ffi_type: unsupported type")));
    return FALSE;
  }

  return TRUE;
}

static gboolean
gum_script_value_from_ffi_type (GumScriptCore * core,
                                Handle<Value> * svalue,
                                const GumFFIValue * value,
                                const ffi_type * type)
{
  Isolate * isolate = core->isolate;

  if (type == &ffi_type_void)
  {
    *svalue = Undefined (isolate);
  }
  else if (type == &ffi_type_pointer)
  {
    *svalue = _gum_script_pointer_new (value->v_pointer, core);
  }
  else if (type == &ffi_type_sint)
  {
    *svalue = Number::New (isolate, value->v_sint);
  }
  else if (type == &ffi_type_uint)
  {
    *svalue = Number::New (isolate, value->v_uint);
  }
  else if (type == &ffi_type_slong)
  {
    *svalue = Number::New (isolate, value->v_slong);
  }
  else if (type == &ffi_type_ulong)
  {
    *svalue = Number::New (isolate, value->v_ulong);
  }
  else if (type == &ffi_type_schar)
  {
    *svalue = Integer::New (isolate, value->v_schar);
  }
  else if (type == &ffi_type_uchar)
  {
    *svalue = Integer::NewFromUnsigned (isolate, value->v_uchar);
  }
  else if (type == &ffi_type_float)
  {
    *svalue = Number::New (isolate, value->v_float);
  }
  else if (type == &ffi_type_double)
  {
    *svalue = Number::New (isolate, value->v_double);
  }
  else if (type == &ffi_type_sint8)
  {
    *svalue = Integer::New (isolate, value->v_sint8);
  }
  else if (type == &ffi_type_uint8)
  {
    *svalue = Integer::NewFromUnsigned (isolate, value->v_uint8);
  }
  else if (type == &ffi_type_sint16)
  {
    *svalue = Integer::New (isolate, value->v_sint16);
  }
  else if (type == &ffi_type_uint16)
  {
    *svalue = Integer::NewFromUnsigned (isolate, value->v_uint16);
  }
  else if (type == &ffi_type_sint32)
  {
    *svalue = Integer::New (isolate, value->v_sint32);
  }
  else if (type == &ffi_type_uint32)
  {
    *svalue = Integer::NewFromUnsigned (isolate, value->v_uint32);
  }
  else if (type == &ffi_type_sint64)
  {
    *svalue = Number::New (isolate, value->v_sint64);
  }
  else if (type == &ffi_type_uint64)
  {
    *svalue = Number::New (isolate, value->v_uint64);
  }
  else
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "value_from_ffi_type: unsupported type")));
    return FALSE;
  }

  return TRUE;
}

GumByteArray *
_gum_byte_array_new (gpointer data,
                     gsize size,
                     GumScriptCore * core)
{
  Isolate * isolate = core->isolate;
  GumByteArray * buffer;

  Local<Object> arr (Object::New (isolate));
  arr->Set (String::NewFromUtf8 (isolate, "length"), Int32::New (isolate, size),
      ReadOnly);
  if (size > 0)
  {
    arr->SetIndexedPropertiesToExternalArrayData (data,
        kExternalUnsignedByteArray, size);
  }
  buffer = g_slice_new (GumByteArray);
  buffer->instance = new GumPersistent<Object>::type (core->isolate, arr);
  buffer->instance->MarkIndependent ();
  buffer->instance->SetWeak (buffer, gum_byte_array_on_weak_notify);
  buffer->data = data;
  buffer->size = size;
  buffer->isolate = core->isolate;

  if (buffer->size > 0)
  {
    core->isolate->AdjustAmountOfExternalAllocatedMemory (size);
  }

  return buffer;
}

void
_gum_byte_array_free (GumByteArray * buffer)
{
  if (buffer->size > 0)
  {
    buffer->isolate->AdjustAmountOfExternalAllocatedMemory (
        -static_cast<gssize> (buffer->size));
  }

  delete buffer->instance;
  g_free (buffer->data);
  g_slice_free (GumByteArray, buffer);
}

static void
gum_byte_array_on_weak_notify (
    const WeakCallbackData<Object, GumByteArray> & data)
{
  HandleScope handle_scope (data.GetIsolate ());
  _gum_byte_array_free (data.GetParameter ());
}

GumHeapBlock *
_gum_heap_block_new (gpointer data,
                     gsize size,
                     GumScriptCore * core)
{
  GumHeapBlock * block;

  block = g_slice_new (GumHeapBlock);
  block->instance = new GumPersistent<Object>::type (core->isolate,
      _gum_script_pointer_new (data, core));
  block->instance->MarkIndependent ();
  block->instance->SetWeak (block, gum_heap_block_on_weak_notify);
  block->data = data;
  block->size = size;
  block->isolate = core->isolate;

  core->isolate->AdjustAmountOfExternalAllocatedMemory (size);

  return block;
}

void
_gum_heap_block_free (GumHeapBlock * block)
{
  block->isolate->AdjustAmountOfExternalAllocatedMemory (
      -static_cast<gssize> (block->size));

  delete block->instance;
  g_free (block->data);
  g_slice_free (GumHeapBlock, block);
}

static void
gum_heap_block_on_weak_notify (
    const WeakCallbackData<Object, GumHeapBlock> & data)
{
  HandleScope handle_scope (data.GetIsolate ());
  _gum_heap_block_free (data.GetParameter ());
}

Local<Object>
_gum_script_pointer_new (gpointer address,
                         GumScriptCore * core)
{
  Local<Object> native_pointer_value (Local<Object>::New (core->isolate,
      *core->native_pointer_value));
  Local<Object> native_pointer_object (native_pointer_value->Clone ());
  native_pointer_object->SetInternalField (0,
      External::New (core->isolate, address));
  return native_pointer_object;
}

gboolean
_gum_script_pointer_get (Handle<Value> value,
                         gpointer * target,
                         GumScriptCore * core)
{
  Isolate * isolate = core->isolate;

  Local<FunctionTemplate> native_pointer (Local<FunctionTemplate>::New (
      isolate, *core->native_pointer));
  if (!native_pointer->HasInstance (value))
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "expected NativePointer object")));
    return FALSE;
  }
  *target = GUM_NATIVE_POINTER_VALUE (value.As<Object> ());

  return TRUE;
}

gboolean
_gum_script_callbacks_get (Handle<Object> callbacks,
                           const gchar * name,
                           Handle<Function> * callback_function,
                           GumScriptCore * core)
{
  if (!_gum_script_callbacks_get_opt (callbacks, name, callback_function, core))
    return FALSE;

  if ((*callback_function).IsEmpty ())
  {
    gchar * message = g_strdup_printf ("%s callback is required", name);
    core->isolate->ThrowException (Exception::TypeError (
        String::NewFromUtf8 (core->isolate, message)));
    g_free (message);

    return FALSE;
  }

  return TRUE;
}

gboolean
_gum_script_callbacks_get_opt (Handle<Object> callbacks,
                               const gchar * name,
                               Handle<Function> * callback_function,
                               GumScriptCore * core)
{
  Isolate * isolate = core->isolate;

  Local<Value> val = callbacks->Get (String::NewFromUtf8 (isolate, name));
  if (!val->IsUndefined ())
  {
    if (!val->IsFunction ())
    {
      gchar * message = g_strdup_printf ("%s must be a function", name);
      isolate->ThrowException (Exception::TypeError (
          String::NewFromUtf8 (isolate, message)));
      g_free (message);

      return FALSE;
    }

    *callback_function = Local<Function>::Cast (val);
  }

  return TRUE;
}

Handle<Object>
_gum_script_cpu_context_to_object (const GumCpuContext * ctx,
                                   GumScriptCore * core)
{
  Isolate * isolate = core->isolate;
  Local<Object> result (Object::New (isolate));
  gsize pc, sp;

#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  pc = ctx->eip;
  sp = ctx->esp;
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  pc = ctx->rip;
  sp = ctx->rsp;
#elif defined (HAVE_ARM) || defined (HAVE_ARM64)
  pc = ctx->pc;
  sp = ctx->sp;
#endif

  result->Set (String::NewFromUtf8 (isolate, "pc"),
      _gum_script_pointer_new (GSIZE_TO_POINTER (pc), core), ReadOnly);
  result->Set (String::NewFromUtf8 (isolate, "sp"),
      _gum_script_pointer_new (GSIZE_TO_POINTER (sp), core), ReadOnly);

  return result;
}

gboolean
_gum_script_page_protection_get (Handle<Value> prot_val,
                                 GumPageProtection * prot,
                                 GumScriptCore * core)
{
  Isolate * isolate = core->isolate;

  if (!prot_val->IsString ())
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (isolate,
        "argument must be a string specifying memory protection")));
    return FALSE;
  }
  String::Utf8Value prot_str (prot_val);

  *prot = GUM_PAGE_NO_ACCESS;
  for (const gchar * ch = *prot_str; *ch != '\0'; ch++)
  {
    switch (*ch)
    {
      case 'r':
        *prot |= GUM_PAGE_READ;
        break;
      case 'w':
        *prot |= GUM_PAGE_WRITE;
        break;
      case 'x':
        *prot |= GUM_PAGE_EXECUTE;
        break;
      case '-':
        break;
      default:
        isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
            isolate, "invalid character in memory protection "
            "specifier string")));
        return FALSE;
    }
  }

  return TRUE;
}
