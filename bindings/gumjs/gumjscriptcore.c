/*
 * Copyright (C) 2015 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumjscriptcore.h"

#include "gumjscriptmacros.h"

#define GUM_SCRIPT_CORE_LOCK(core)   (g_mutex_lock (&(core)->mutex))
#define GUM_SCRIPT_CORE_UNLOCK(core) (g_mutex_unlock (&(core)->mutex))

struct _GumScriptScheduledCallback
{
  gint id;
  gboolean repeat;
  JSObjectRef func;
  GSource * source;
  GumScriptCore * core;
};

struct _GumScriptExceptionSink
{
  JSObjectRef callback;
  JSContextRef ctx;
};

struct _GumScriptMessageSink
{
  JSObjectRef callback;
  JSContextRef ctx;
};

GUMJS_DECLARE_FUNCTION (gumjs_set_timeout)
GUMJS_DECLARE_FUNCTION (gumjs_set_interval)
GUMJS_DECLARE_FUNCTION (gumjs_clear_timer)
GUMJS_DECLARE_FUNCTION (gumjs_gc)
GUMJS_DECLARE_FUNCTION (gumjs_send)
GUMJS_DECLARE_FUNCTION (gumjs_set_unhandled_exception_callback)
GUMJS_DECLARE_FUNCTION (gumjs_set_incoming_message_callback)
GUMJS_DECLARE_FUNCTION (gumjs_wait_for_event)

GUMJS_DECLARE_GETTER (gumjs_script_get_file_name)
GUMJS_DECLARE_GETTER (gumjs_script_get_source_map_data)

GUMJS_DECLARE_CONSTRUCTOR (gumjs_native_pointer_construct)
GUMJS_DECLARE_FINALIZER (gumjs_native_pointer_finalize)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_is_null)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_add)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_sub)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_and)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_or)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_xor)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_compare)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_int32)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_string)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_json)
GUMJS_DECLARE_FUNCTION (gumjs_native_pointer_to_match_pattern)

GUMJS_DECLARE_CONSTRUCTOR (gumjs_cpu_context_construct)
GUMJS_DECLARE_FINALIZER (gumjs_cpu_context_finalize)
static bool gumjs_cpu_context_set_register (GumScriptCpuContext * self,
    JSContextRef ctx, const GumScriptArgs * args, gsize * reg,
    JSValueRef * exception);

static JSValueRef gum_script_core_schedule_callback (GumScriptCore * self,
    const GumScriptArgs * args, gboolean repeat);
static void gum_script_core_add_scheduled_callback (GumScriptCore * self,
    GumScriptScheduledCallback * cb);
static void gum_script_core_remove_scheduled_callback (GumScriptCore * self,
    GumScriptScheduledCallback * cb);

static GumScriptScheduledCallback * gum_scheduled_callback_new (gint id,
    JSObjectRef func, gboolean repeat, GSource * source, GumScriptCore * core);
static void gum_scheduled_callback_free (GumScriptScheduledCallback * callback);
static gboolean gum_scheduled_callback_invoke (gpointer user_data);

static GumScriptExceptionSink * gum_script_exception_sink_new (JSContextRef ctx,
    JSObjectRef callback);
static void gum_script_exception_sink_free (GumScriptExceptionSink * sink);
static void gum_script_exception_sink_handle_exception (
    GumScriptExceptionSink * self, JSValueRef exception);

static GumScriptMessageSink * gum_script_message_sink_new (JSContextRef ctx,
    JSObjectRef callback);
static void gum_script_message_sink_free (GumScriptMessageSink * sink);
static void gum_script_message_sink_handle_message (GumScriptMessageSink * self,
    const gchar * message, JSValueRef * exception);

static const JSPropertyAttributes gumjs_attrs =
    kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete;

static const JSStaticValue gumjs_script_values[] =
{
  { "fileName", gumjs_script_get_file_name, NULL, gumjs_attrs },
  { "_sourceMapData", gumjs_script_get_source_map_data, NULL, gumjs_attrs },

  { NULL, NULL, NULL, 0 }
};

static const JSStaticFunction gumjs_native_pointer_functions[] =
{
  { "isNull", gumjs_native_pointer_is_null, gumjs_attrs },
  { "add", gumjs_native_pointer_add, gumjs_attrs },
  { "sub", gumjs_native_pointer_sub, gumjs_attrs },
  { "and", gumjs_native_pointer_and, gumjs_attrs },
  { "or", gumjs_native_pointer_or, gumjs_attrs },
  { "xor", gumjs_native_pointer_xor, gumjs_attrs },
  { "compare", gumjs_native_pointer_compare, gumjs_attrs },
  { "toInt32", gumjs_native_pointer_to_int32, gumjs_attrs },
  { "toString", gumjs_native_pointer_to_string, gumjs_attrs },
  { "toJSON", gumjs_native_pointer_to_json, gumjs_attrs },
  { "toMatchPattern", gumjs_native_pointer_to_match_pattern, gumjs_attrs },

  { NULL, NULL, 0 }
};

#define GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED(A, R) \
  GUMJS_DEFINE_GETTER (gumjs_cpu_context_get_##A) \
  { \
    GumScriptCpuContext * self = JSObjectGetPrivate (object); \
    \
    return _gumjs_native_pointer_new (ctx, \
        GSIZE_TO_POINTER (self->handle->R), args->core); \
  } \
  \
  GUMJS_DEFINE_SETTER (gumjs_cpu_context_set_##A) \
  { \
    GumScriptCpuContext * self = JSObjectGetPrivate (object); \
    \
    return gumjs_cpu_context_set_register (self, ctx, args, \
        (gsize *) &self->handle->R, exception); \
  }
#define GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR(R) \
  GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (R, R)

#define GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED(A, R) \
  { G_STRINGIFY (A), gumjs_cpu_context_get_##R, gumjs_cpu_context_set_##R, \
    gumjs_attrs }
#define GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR(R) \
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (R, R)

#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (eax)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (ecx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (edx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (ebx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (esp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (ebp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (esi)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (edi)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (eip)
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rax)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rcx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rdx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rbx)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rsp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rbp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rsi)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rdi)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r8)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r9)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r10)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r11)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r12)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r13)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r14)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (r15)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (rip)
#elif defined (HAVE_ARM)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (pc)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (sp)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r0, r[0])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r1, r[1])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r2, r[2])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r3, r[3])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r4, r[4])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r5, r[5])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r6, r[6])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (r7, r[7])

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (lr)
#elif defined (HAVE_ARM64)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (pc)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (sp)

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x0, x[0])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x1, x[1])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x2, x[2])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x3, x[3])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x4, x[4])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x5, x[5])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x6, x[6])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x7, x[7])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x8, x[8])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x9, x[9])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x10, x[10])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x11, x[11])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x12, x[12])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x13, x[13])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x14, x[14])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x15, x[15])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x16, x[16])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x17, x[17])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x18, x[18])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x19, x[19])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x20, x[20])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x21, x[21])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x22, x[22])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x23, x[23])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x24, x[24])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x25, x[25])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x26, x[26])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x27, x[27])
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR_ALIASED (x28, x[28])

GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (fp)
GUMJS_DEFINE_CPU_CONTEXT_ACCESSOR (lr)
#endif

static const JSStaticValue gumjs_cpu_context_values[] =
{
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (pc, eip),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (sp, esp),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (eax),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (ecx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (edx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (ebx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (esp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (ebp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (esi),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (edi),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (eip),
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (pc, rip),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR_ALIASED (sp, rsp),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rax),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rcx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rdx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rbx),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rsp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rbp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rsi),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rdi),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r8),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r9),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r10),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r11),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r12),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r13),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r14),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r15),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (rip),
#elif defined (HAVE_ARM)
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (pc),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (sp),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r0, r0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r1, r1),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r2, r2),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r3, r3),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r4, r4),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r5, r5),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r6, r6),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (r7, r7),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (lr),
#elif defined (HAVE_ARM64)
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (pc),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (sp),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x0),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x1),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x2),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x3),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x4),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x5),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x6),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x7),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x8),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x9),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x10),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x11),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x12),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x13),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x14),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x15),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x16),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x17),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x18),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x19),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x20),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x21),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x22),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x23),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x24),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x25),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x26),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x27),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (x28),

  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (fp),
  GUMJS_EXPORT_CPU_CONTEXT_ACCESSOR (lr),
#endif

  { NULL, NULL, NULL, 0 }
};

void
_gum_script_core_init (GumScriptCore * self,
                       GumScript * script,
                       GumScriptCoreMessageEmitter message_emitter,
                       GumScriptScheduler * scheduler,
                       JSContextRef ctx,
                       JSObjectRef scope)
{
  GumScriptFlavor flavor;
  JSClassDefinition def;
  JSObjectRef frida;
  JSClassRef klass;
  JSValueRef obj;

  g_object_get (script, "flavor", &flavor, NULL);

  self->script = script;
  self->message_emitter = message_emitter;
  self->scheduler = scheduler;
  self->exceptor = gum_exceptor_obtain ();
  self->ctx = ctx;

  g_mutex_init (&self->mutex);
  g_cond_init (&self->event_cond);

  JSObjectSetPrivate (scope, self);

  _gumjs_object_set (ctx, scope, "global", scope);

  frida = JSObjectMake (ctx, NULL, NULL);
  _gumjs_object_set_string (ctx, frida, "version", FRIDA_VERSION);
  _gumjs_object_set (ctx, scope, "Frida", frida);

  def = kJSClassDefinitionEmpty;
  def.className = "Script";
  def.staticValues = gumjs_script_values;
  klass = JSClassCreate (&def);
  obj = JSObjectMake (ctx, klass, self);
  JSClassRelease (klass);
  _gumjs_object_set (ctx, scope, "Script", obj);

  _gumjs_object_set_function (ctx, scope, "setTimeout", gumjs_set_timeout);
  _gumjs_object_set_function (ctx, scope, "clearTimeout", gumjs_clear_timer);
  _gumjs_object_set_function (ctx, scope, "setInterval", gumjs_set_interval);
  _gumjs_object_set_function (ctx, scope, "clearInterval", gumjs_clear_timer);
  _gumjs_object_set_function (ctx, scope, "gc", gumjs_gc);
  _gumjs_object_set_function (ctx, scope, "_send", gumjs_send);
  _gumjs_object_set_function (ctx, scope, "_setUnhandledExceptionCallback",
      gumjs_set_unhandled_exception_callback);
  _gumjs_object_set_function (ctx, scope, "_setIncomingMessageCallback",
      gumjs_set_incoming_message_callback);
  _gumjs_object_set_function (ctx, scope, "_waitForEvent",
      gumjs_wait_for_event);

  self->native_resources = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) _gumjs_native_resource_free);

  def = kJSClassDefinitionEmpty;
  def.className = "NativePointer";
  def.staticFunctions = gumjs_native_pointer_functions;
  def.finalize = gumjs_native_pointer_finalize;
  self->native_pointer = JSClassCreate (&def);
  _gumjs_object_set (ctx, scope, "NativePointer", JSObjectMakeConstructor (ctx,
      self->native_pointer, gumjs_native_pointer_construct));

  def = kJSClassDefinitionEmpty;
  def.className = "CpuContext";
  def.staticValues = gumjs_cpu_context_values;
  def.finalize = gumjs_cpu_context_finalize;
  self->cpu_context = JSClassCreate (&def);
  _gumjs_object_set (ctx, scope, "CpuContext", JSObjectMakeConstructor (ctx,
      self->cpu_context, gumjs_cpu_context_construct));

  self->array_buffer =
      (JSObjectRef) _gumjs_object_get (ctx, scope, "ArrayBuffer");
  JSValueProtect (ctx, self->array_buffer);
}

void
_gum_script_core_flush (GumScriptCore * self)
{
  GMainContext * context;

  GUM_SCRIPT_CORE_UNLOCK (self);
  gum_script_scheduler_flush_by_tag (self->scheduler, self);
  GUM_SCRIPT_CORE_LOCK (self);

  context = gum_script_scheduler_get_js_context (self->scheduler);
  while (g_main_context_pending (context))
    g_main_context_iteration (context, FALSE);
}

void
_gum_script_core_dispose (GumScriptCore * self)
{
  g_hash_table_unref (self->native_resources);
  self->native_resources = NULL;

  while (self->scheduled_callbacks != NULL)
  {
    g_source_destroy (((GumScriptScheduledCallback *) (
        self->scheduled_callbacks->data))->source);
    self->scheduled_callbacks = g_slist_delete_link (
        self->scheduled_callbacks, self->scheduled_callbacks);
  }

  gum_script_exception_sink_free (self->unhandled_exception_sink);
  self->unhandled_exception_sink = NULL;

  gum_script_message_sink_free (self->incoming_message_sink);
  self->incoming_message_sink = NULL;

  JSValueUnprotect (self->ctx, self->array_buffer);
  self->array_buffer = NULL;

  JSClassRelease (self->cpu_context);
  self->cpu_context = NULL;

  JSClassRelease (self->native_pointer);
  self->native_pointer = NULL;

  g_object_unref (self->exceptor);
  self->exceptor = NULL;
}

void
_gum_script_core_finalize (GumScriptCore * self)
{
  g_mutex_clear (&self->mutex);
  g_cond_clear (&self->event_cond);
}

void
_gum_script_core_emit_message (GumScriptCore * self,
                               const gchar * message,
                               GBytes * data)
{
  self->message_emitter (self->script, message, data);
}

void
_gum_script_core_post_message (GumScriptCore * self,
                               const gchar * message)
{
  if (self->incoming_message_sink != NULL)
  {
    GumScriptScope scope;

    _gum_script_scope_enter (&scope, self);

    gum_script_message_sink_handle_message (self->incoming_message_sink,
        message, &scope.exception);

    _gum_script_scope_leave (&scope);

    GUM_SCRIPT_CORE_LOCK (self);
    self->event_count++;
    g_cond_broadcast (&self->event_cond);
    GUM_SCRIPT_CORE_UNLOCK (self);
  }
}

void
_gum_script_core_push_job (GumScriptCore * self,
                           GumScriptJobFunc job_func,
                           gpointer data,
                           GDestroyNotify data_destroy)
{
  gum_script_scheduler_push_job_on_thread_pool (self->scheduler, job_func,
      data, data_destroy, self);
}

void
_gum_script_scope_enter (GumScriptScope * self,
                         GumScriptCore * core)
{
  self->core = core;
  self->exception = NULL;

  GUM_SCRIPT_CORE_LOCK (core);
}

void
_gum_script_scope_flush (GumScriptScope * self)
{
  GumScriptCore * core = self->core;

  if (self->exception != NULL && core->unhandled_exception_sink != NULL)
  {
    gum_script_exception_sink_handle_exception (core->unhandled_exception_sink,
        self->exception);
    self->exception = NULL;
  }
}

void
_gum_script_scope_leave (GumScriptScope * self)
{
  _gum_script_scope_flush (self);

  GUM_SCRIPT_CORE_UNLOCK (self->core);
}

void
_gum_script_yield_begin (GumScriptYield * self,
                         GumScriptCore * core)
{
  self->core = core;

  GUM_SCRIPT_CORE_UNLOCK (self->core);
}

void
_gum_script_yield_end (GumScriptYield * self)
{
  GUM_SCRIPT_CORE_LOCK (self->core);
}

GUMJS_DEFINE_GETTER (gumjs_script_get_file_name)
{
  return JSValueMakeNull (ctx);
}

GUMJS_DEFINE_GETTER (gumjs_script_get_source_map_data)
{
  return JSValueMakeNull (ctx);
}

GUMJS_DEFINE_FUNCTION (gumjs_set_timeout)
{
  return gum_script_core_schedule_callback (args->core, args, FALSE);
}

GUMJS_DEFINE_FUNCTION (gumjs_set_interval)
{
  return gum_script_core_schedule_callback (args->core, args, TRUE);
}

GUMJS_DEFINE_FUNCTION (gumjs_clear_timer)
{
  GumScriptCore * self = args->core;
  gint id;
  GumScriptScheduledCallback * callback = NULL;
  GSList * cur;

  if (!_gumjs_args_parse (args, "i", &id))
    return NULL;

  for (cur = self->scheduled_callbacks; cur != NULL; cur = cur->next)
  {
    GumScriptScheduledCallback * cb = cur->data;
    if (cb->id == id)
    {
      callback = cb;
      self->scheduled_callbacks =
          g_slist_delete_link (self->scheduled_callbacks, cur);
      break;
    }
  }

  if (callback != NULL)
    g_source_destroy (callback->source);

  return JSValueMakeBoolean (ctx, callback != NULL);
}

GUMJS_DEFINE_FUNCTION (gumjs_gc)
{
  JSGarbageCollect (ctx);

  return JSValueMakeUndefined (ctx);
}

GUMJS_DEFINE_FUNCTION (gumjs_send)
{
  gchar * message;
  GBytes * data = NULL;

  if (!_gumjs_args_parse (args, "s|B?", &message, &data))
    return NULL;

  _gum_script_core_emit_message (args->core, message, data);

  g_bytes_unref (data);
  g_free (message);

  return JSValueMakeUndefined (ctx);
}

GUMJS_DEFINE_FUNCTION (gumjs_set_unhandled_exception_callback)
{
  GumScriptCore * self = args->core;
  JSObjectRef callback;

  if (!_gumjs_args_parse (args, "C?", &callback))
    return NULL;

  gum_script_exception_sink_free (self->unhandled_exception_sink);
  self->unhandled_exception_sink = NULL;

  if (callback != NULL)
  {
    self->unhandled_exception_sink =
        gum_script_exception_sink_new (self->ctx, callback);
  }

  return JSValueMakeUndefined (ctx);
}

GUMJS_DEFINE_FUNCTION (gumjs_set_incoming_message_callback)
{
  GumScriptCore * self = args->core;
  JSObjectRef callback;

  if (!_gumjs_args_parse (args, "C?", &callback))
    return NULL;

  gum_script_message_sink_free (self->incoming_message_sink);
  self->incoming_message_sink = NULL;

  if (callback != NULL)
  {
    self->incoming_message_sink = gum_script_message_sink_new (self->ctx,
        callback);
  }

  return JSValueMakeUndefined (ctx);
}

GUMJS_DEFINE_FUNCTION (gumjs_wait_for_event)
{
  GumScriptCore * self = args->core;
  guint start_count;

  start_count = self->event_count;
  while (self->event_count == start_count)
    g_cond_wait (&self->event_cond, &self->mutex);

  return JSValueMakeUndefined (ctx);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_native_pointer_construct)
{
  gsize ptr = 0;

  if (!_gumjs_args_parse (args, "|p~", &ptr))
    return NULL;

  return _gumjs_native_pointer_new (ctx, GSIZE_TO_POINTER (ptr), args->core);
}

GUMJS_DEFINE_FINALIZER (gumjs_native_pointer_finalize)
{
  GumScriptNativePointer * self = JSObjectGetPrivate (object);

  g_slice_free1 (self->instance_size, self);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_is_null)
{
  return JSValueMakeBoolean (ctx,
      _gumjs_native_pointer_value (this_object) == NULL);
}

#define GUM_DEFINE_NATIVE_POINTER_OP_IMPL(name, op) \
  GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_##name) \
  { \
    gpointer rhs_ptr; \
    gsize lhs, rhs; \
    gpointer result; \
    \
    if (!_gumjs_args_parse (args, "p~", &rhs_ptr)) \
      return NULL; \
    \
    lhs = GPOINTER_TO_SIZE (_gumjs_native_pointer_value (this_object)); \
    rhs = GPOINTER_TO_SIZE (rhs_ptr); \
    \
    result = GSIZE_TO_POINTER (lhs op rhs); \
    \
    return _gumjs_native_pointer_new (ctx, result, args->core); \
  }

GUM_DEFINE_NATIVE_POINTER_OP_IMPL (add, +)
GUM_DEFINE_NATIVE_POINTER_OP_IMPL (sub, -)
GUM_DEFINE_NATIVE_POINTER_OP_IMPL (and, &)
GUM_DEFINE_NATIVE_POINTER_OP_IMPL (or,  |)
GUM_DEFINE_NATIVE_POINTER_OP_IMPL (xor, ^)

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_compare)
{
  gpointer rhs_ptr;
  gsize lhs, rhs;
  gint result;

  if (!_gumjs_args_parse (args, "p~", &rhs_ptr))
    return NULL;

  lhs = GPOINTER_TO_SIZE (_gumjs_native_pointer_value (this_object));
  rhs = GPOINTER_TO_SIZE (rhs_ptr);

  result = (lhs == rhs) ? 0 : ((lhs < rhs) ? -1 : 1);

  return JSValueMakeNumber (ctx, result);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_int32)
{
  gint32 result;

  result = (gint32)
      GPOINTER_TO_SIZE (_gumjs_native_pointer_value (this_object));

  return JSValueMakeNumber (ctx, result);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_string)
{
  gint radix = -1;
  gboolean radix_specified;
  gsize ptr;
  gchar str[32];

  if (!_gumjs_args_parse (args, "|u", &radix))
    return NULL;
  radix_specified = radix != -1;
  if (!radix_specified)
    radix = 16;
  else if (radix != 10 && radix != 16)
    goto unsupported_radix;

  ptr = GPOINTER_TO_SIZE (_gumjs_native_pointer_value (this_object));

  if (radix == 10)
  {
    sprintf (str, "%" G_GSIZE_MODIFIER "u", ptr);
  }
  else
  {
    if (radix_specified)
      sprintf (str, "%" G_GSIZE_MODIFIER "x", ptr);
    else
      sprintf (str, "0x%" G_GSIZE_MODIFIER "x", ptr);
  }

  return _gumjs_string_to_value (ctx, str);

unsupported_radix:
  {
    _gumjs_throw (ctx, exception, "unsupported radix");
    return NULL;
  }
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_json)
{
  gsize ptr;
  gchar str[32];

  ptr = GPOINTER_TO_SIZE (_gumjs_native_pointer_value (this_object));

  sprintf (str, "0x%" G_GSIZE_MODIFIER "x", ptr);

  return _gumjs_string_to_value (ctx, str);
}

GUMJS_DEFINE_FUNCTION (gumjs_native_pointer_to_match_pattern)
{
  gsize ptr;
  gchar str[24];
  gint src, dst;
  const gint num_bits = GLIB_SIZEOF_VOID_P * 8;
  const gchar nibble_to_char[] = {
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
      'a', 'b', 'c', 'd', 'e', 'f'
  };

  ptr = GPOINTER_TO_SIZE (_gumjs_native_pointer_value (this_object));

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  for (src = 0, dst = 0; src != num_bits; src += 8)
#else
  for (src = num_bits - 8, dst = 0; src >= 0; src -= 8)
#endif
  {
    if (dst != 0)
      str[dst++] = ' ';
    str[dst++] = nibble_to_char[(ptr >> (src + 4)) & 0xf];
    str[dst++] = nibble_to_char[(ptr >> (src + 0)) & 0xf];
  }
  str[dst] = '\0';

  return _gumjs_string_to_value (ctx, str);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_cpu_context_construct)
{
  _gumjs_throw (ctx, exception, "invalid argument");
  return NULL;
}

GUMJS_DEFINE_FINALIZER (gumjs_cpu_context_finalize)
{
  GumScriptCpuContext * self = JSObjectGetPrivate (object);

  g_slice_free (GumScriptCpuContext, self);
}

static bool
gumjs_cpu_context_set_register (GumScriptCpuContext * self,
                                JSContextRef ctx,
                                const GumScriptArgs * args,
                                gsize * reg,
                                JSValueRef * exception)
{
  gpointer value;

  if (self->access == GUM_CPU_CONTEXT_READONLY)
    goto invalid_operation;

  if (!_gumjs_args_parse (args, "p~", &value))
    return false;

  *reg = GPOINTER_TO_SIZE (value);
  return true;

invalid_operation:
  {
    _gumjs_throw (ctx, exception, "invalid operation");
    return false;
  }
}

static JSValueRef
gum_script_core_schedule_callback (GumScriptCore * self,
                                   const GumScriptArgs * args,
                                   gboolean repeat)
{
  JSObjectRef func;
  guint delay;
  gint id;
  GSource * source;
  GumScriptScheduledCallback * callback;

  if (!_gumjs_args_parse (args, "Cu", &func, &delay))
    return NULL;

  id = g_atomic_int_add (&self->last_callback_id, 1) + 1;
  if (delay == 0)
    source = g_idle_source_new ();
  else
    source = g_timeout_source_new (delay);

  callback = gum_scheduled_callback_new (id, func, repeat, source, self);
  g_source_set_callback (source, gum_scheduled_callback_invoke, callback,
      (GDestroyNotify) gum_scheduled_callback_free);
  gum_script_core_add_scheduled_callback (self, callback);

  g_source_attach (source,
      gum_script_scheduler_get_js_context (self->scheduler));

  return JSValueMakeNumber (args->ctx, id);
}

static void
gum_script_core_add_scheduled_callback (GumScriptCore * self,
                                        GumScriptScheduledCallback * cb)
{
  self->scheduled_callbacks =
      g_slist_prepend (self->scheduled_callbacks, cb);
}

static void
gum_script_core_remove_scheduled_callback (GumScriptCore * self,
                                           GumScriptScheduledCallback * cb)
{
  self->scheduled_callbacks =
      g_slist_remove (self->scheduled_callbacks, cb);
}

static GumScriptScheduledCallback *
gum_scheduled_callback_new (gint id,
                            JSObjectRef func,
                            gboolean repeat,
                            GSource * source,
                            GumScriptCore * core)
{
  GumScriptScheduledCallback * callback;

  callback = g_slice_new (GumScriptScheduledCallback);
  callback->id = id;
  JSValueProtect (core->ctx, func);
  callback->func = func;
  callback->repeat = repeat;
  callback->source = source;
  callback->core = core;

  return callback;
}

static void
gum_scheduled_callback_free (GumScriptScheduledCallback * callback)
{
  JSValueUnprotect (callback->core->ctx, callback->func);

  g_slice_free (GumScriptScheduledCallback, callback);
}

static gboolean
gum_scheduled_callback_invoke (gpointer user_data)
{
  GumScriptScheduledCallback * self = user_data;
  GumScriptCore * core = self->core;
  GumScriptScope scope;

  _gum_script_scope_enter (&scope, self->core);
  JSObjectCallAsFunction (core->ctx, self->func, NULL, 0, NULL,
      &scope.exception);
  _gum_script_scope_leave (&scope);

  if (!self->repeat)
    gum_script_core_remove_scheduled_callback (core, self);

  return self->repeat;
}

static GumScriptExceptionSink *
gum_script_exception_sink_new (JSContextRef ctx,
                               JSObjectRef callback)
{
  GumScriptExceptionSink * sink;

  sink = g_slice_new (GumScriptExceptionSink);
  JSValueProtect (ctx, callback);
  sink->callback = callback;
  sink->ctx = ctx;

  return sink;
}

static void
gum_script_exception_sink_free (GumScriptExceptionSink * sink)
{
  if (sink == NULL)
    return;

  JSValueUnprotect (sink->ctx, sink->callback);

  g_slice_free (GumScriptExceptionSink, sink);
}

static void
gum_script_exception_sink_handle_exception (GumScriptExceptionSink * self,
                                            JSValueRef exception)
{
  JSObjectCallAsFunction (self->ctx, self->callback, NULL, 1, &exception, NULL);
}

static GumScriptMessageSink *
gum_script_message_sink_new (JSContextRef ctx,
                             JSObjectRef callback)
{
  GumScriptMessageSink * sink;

  sink = g_slice_new (GumScriptMessageSink);
  JSValueProtect (ctx, callback);
  sink->callback = callback;
  sink->ctx = ctx;

  return sink;
}

static void
gum_script_message_sink_free (GumScriptMessageSink * sink)
{
  if (sink == NULL)
    return;

  JSValueUnprotect (sink->ctx, sink->callback);

  g_slice_free (GumScriptMessageSink, sink);
}

static void
gum_script_message_sink_handle_message (GumScriptMessageSink * self,
                                        const gchar * message,
                                        JSValueRef * exception)
{
  JSValueRef message_value;

  message_value = _gumjs_string_to_value (self->ctx, message);
  JSObjectCallAsFunction (self->ctx, self->callback, NULL, 1, &message_value,
      exception);
}
