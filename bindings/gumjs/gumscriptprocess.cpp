/*
 * Copyright (C) 2010-2014 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumscriptprocess.h"

#include "gumscriptscope.h"

#include <string.h>

#if defined (HAVE_I386)
# if GLIB_SIZEOF_VOID_P == 4
#  define GUM_SCRIPT_ARCH "ia32"
# else
#  define GUM_SCRIPT_ARCH "x64"
# endif
#elif defined (HAVE_ARM)
# define GUM_SCRIPT_ARCH "arm"
#elif defined (HAVE_ARM64)
# define GUM_SCRIPT_ARCH "arm64"
#endif

#if defined (HAVE_LINUX)
# define GUM_SCRIPT_PLATFORM "linux"
#elif defined (HAVE_DARWIN)
# define GUM_SCRIPT_PLATFORM "darwin"
#elif defined (G_OS_WIN32)
# define GUM_SCRIPT_PLATFORM "windows"
#elif defined (HAVE_QNX)
# define GUM_SCRIPT_PLATFORM "qnx"
#endif

using namespace v8;

typedef struct _GumScriptMatchContext GumScriptMatchContext;

struct _GumScriptExceptionHandler
{
  GumPersistent<Function>::type * callback;
  GumScriptCore * core;
};

struct _GumScriptMatchContext
{
  GumScriptProcess * self;
  Isolate * isolate;
  Local<Function> on_match;
  Local<Function> on_complete;
  Local<Object> receiver;
};

static void gum_script_process_on_is_debugger_attached (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_process_on_get_current_thread_id (
    const FunctionCallbackInfo<Value> & info);
static void gum_script_process_on_enumerate_threads (
    const FunctionCallbackInfo<Value> & info);
static gboolean gum_script_handle_thread_match (
    const GumThreadDetails * details, gpointer user_data);
static void gum_script_process_on_enumerate_modules (
    const FunctionCallbackInfo<Value> & info);
static gboolean gum_script_process_handle_module_match (
    const GumModuleDetails * details, gpointer user_data);
static void gum_script_process_on_enumerate_ranges (
    const FunctionCallbackInfo<Value> & info);
static gboolean gum_script_process_handle_range_match (
    const GumRangeDetails * details, gpointer user_data);
static void gum_script_process_on_enumerate_malloc_ranges (
    const FunctionCallbackInfo<Value> & info);
#ifdef HAVE_DARWIN
static gboolean gum_script_process_handle_malloc_range_match (
    const GumMallocRangeDetails * details, gpointer user_data);
#endif
static void gum_script_process_on_set_exception_handler (
    const FunctionCallbackInfo<Value> & info);

static GumScriptExceptionHandler * gum_script_exception_handler_new (
    Handle<Function> callback, GumScriptCore * core);
static void gum_script_exception_handler_free (
    GumScriptExceptionHandler * handler);
static gboolean gum_script_exception_handler_on_exception (
    GumExceptionDetails * details, gpointer user_data);

const gchar * gum_script_exception_type_to_string (GumExceptionType type);

void
_gum_script_process_init (GumScriptProcess * self,
                          GumScriptCore * core,
                          Handle<ObjectTemplate> scope)
{
  Isolate * isolate = core->isolate;

  self->core = core;

  Local<External> data (External::New (isolate, self));

  Handle<ObjectTemplate> process = ObjectTemplate::New (isolate);
  process->Set (String::NewFromUtf8 (isolate, "arch"),
      String::NewFromUtf8 (isolate, GUM_SCRIPT_ARCH), ReadOnly);
  process->Set (String::NewFromUtf8 (isolate, "platform"),
      String::NewFromUtf8 (isolate, GUM_SCRIPT_PLATFORM), ReadOnly);
  process->Set (String::NewFromUtf8 (isolate, "pageSize"),
      Number::New (isolate, gum_query_page_size ()), ReadOnly);
  process->Set (String::NewFromUtf8 (isolate, "pointerSize"),
      Number::New (isolate, GLIB_SIZEOF_VOID_P), ReadOnly);
  process->Set (String::NewFromUtf8 (isolate, "isDebuggerAttached"),
      FunctionTemplate::New (isolate,
      gum_script_process_on_is_debugger_attached));
  process->Set (String::NewFromUtf8 (isolate, "getCurrentThreadId"),
      FunctionTemplate::New (isolate,
      gum_script_process_on_get_current_thread_id));
  process->Set (String::NewFromUtf8 (isolate, "enumerateThreads"),
      FunctionTemplate::New (isolate, gum_script_process_on_enumerate_threads,
      data));
  process->Set (String::NewFromUtf8 (isolate, "enumerateModules"),
      FunctionTemplate::New (isolate, gum_script_process_on_enumerate_modules,
      data));
  process->Set (String::NewFromUtf8 (isolate, "_enumerateRanges"),
      FunctionTemplate::New (isolate, gum_script_process_on_enumerate_ranges,
      data));
  process->Set (String::NewFromUtf8 (isolate, "enumerateMallocRanges"),
      FunctionTemplate::New (isolate,
      gum_script_process_on_enumerate_malloc_ranges, data));
  process->Set (String::NewFromUtf8 (isolate, "setExceptionHandler"),
      FunctionTemplate::New (isolate,
      gum_script_process_on_set_exception_handler, data));
  scope->Set (String::NewFromUtf8 (isolate, "Process"), process);
}

void
_gum_script_process_realize (GumScriptProcess * self)
{
  (void) self;
}

void
_gum_script_process_dispose (GumScriptProcess * self)
{
  gum_script_exception_handler_free (self->exception_handler);
  self->exception_handler = NULL;
}

void
_gum_script_process_finalize (GumScriptProcess * self)
{
  (void) self;
}

/*
 * Prototype:
 * Process.isDebuggerAttached()
 *
 * Docs:
 * TBW
 *
 * Example:
 * TBW
 */
static void
gum_script_process_on_is_debugger_attached (
    const FunctionCallbackInfo<Value> & info)
{
  info.GetReturnValue ().Set (
      gum_process_is_debugger_attached () ? true : false);
}

/*
 * Prototype:
 * Process.getCurrentThreadId()
 *
 * Docs:
 * Returns the current thread ID as an unsigned 32-bit integer.
 *
 * Example:
 * -> Process.getCurrentThreadId()
 * 3075
 */
static void
gum_script_process_on_get_current_thread_id (
    const FunctionCallbackInfo<Value> & info)
{
  info.GetReturnValue ().Set (static_cast<uint32_t> (
      gum_process_get_current_thread_id ()));
}

/*
 * Prototype:
 * Process.enumerateThreads(callback)
 *
 * Docs:
 * TBW
 *
 * Example:
 * TBW
 */
static void
gum_script_process_on_enumerate_threads (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptMatchContext ctx;

  ctx.self = static_cast<GumScriptProcess *> (
      info.Data ().As<External> ()->Value ());
  ctx.isolate = info.GetIsolate ();

  Local<Value> callbacks_value = info[0];
  if (!callbacks_value->IsObject ())
  {
    ctx.isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        ctx.isolate, "Process.enumerateThreads: argument must be "
        "a callback object")));
    return;
  }

  Local<Object> callbacks = Local<Object>::Cast (callbacks_value);
  if (!_gum_script_callbacks_get (callbacks, "onMatch", &ctx.on_match,
      ctx.self->core))
  {
    return;
  }
  if (!_gum_script_callbacks_get (callbacks, "onComplete", &ctx.on_complete,
      ctx.self->core))
  {
    return;
  }

  ctx.receiver = info.This ();

  gum_process_enumerate_threads (gum_script_handle_thread_match, &ctx);

  ctx.on_complete->Call (ctx.receiver, 0, 0);
}

static gboolean
gum_script_handle_thread_match (const GumThreadDetails * details,
                                gpointer user_data)
{
  GumScriptMatchContext * ctx =
      static_cast<GumScriptMatchContext *> (user_data);
  GumScriptCore * core = ctx->self->core;
  Isolate * isolate = ctx->isolate;

  if (gum_script_is_ignoring (details->id))
    return TRUE;

  Local<Object> thread (Object::New (isolate));
  _gum_script_set (thread, "id", Number::New (isolate, details->id), core);
  _gum_script_set (thread, "state", String::NewFromOneByte (isolate,
      (const uint8_t *) _gum_script_thread_state_to_string (details->state)),
      core);
  Local<Object> cpu_context =
      _gum_script_cpu_context_new (&details->cpu_context, ctx->self->core);
  _gum_script_set (thread, "context", cpu_context, core);

  Handle<Value> argv[] = { thread };
  Local<Value> result = ctx->on_match->Call (ctx->receiver, 1, argv);

  gboolean proceed = TRUE;
  if (!result.IsEmpty () && result->IsString ())
  {
    String::Utf8Value str (result);
    proceed = (strcmp (*str, "stop") != 0);
  }

  _gum_script_cpu_context_free_later (
      new GumPersistent<Object>::type (isolate, cpu_context),
      core);

  return proceed;
}

/*
 * Prototype:
 * Process.enumerateModules(callback)
 *
 * Docs:
 * TBW
 *
 * Example:
 * TBW
 */
static void
gum_script_process_on_enumerate_modules (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptMatchContext ctx;

  ctx.self = static_cast<GumScriptProcess *> (
      info.Data ().As<External> ()->Value ());
  ctx.isolate = info.GetIsolate ();

  Local<Value> callbacks_value = info[0];
  if (!callbacks_value->IsObject ())
  {
    ctx.isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        ctx.isolate, "Process.enumerateModules: argument must be "
        "a callback object")));
    return;
  }

  Local<Object> callbacks = Local<Object>::Cast (callbacks_value);
  if (!_gum_script_callbacks_get (callbacks, "onMatch", &ctx.on_match,
      ctx.self->core))
  {
    return;
  }
  if (!_gum_script_callbacks_get (callbacks, "onComplete", &ctx.on_complete,
      ctx.self->core))
  {
    return;
  }

  ctx.receiver = info.This ();

  gum_process_enumerate_modules (gum_script_process_handle_module_match, &ctx);

  ctx.on_complete->Call (ctx.receiver, 0, 0);
}

static gboolean
gum_script_process_handle_module_match (const GumModuleDetails * details,
                                        gpointer user_data)
{
  GumScriptMatchContext * ctx =
      static_cast<GumScriptMatchContext *> (user_data);
  GumScriptCore * core = ctx->self->core;
  Isolate * isolate = ctx->isolate;

  Local<Object> module (Object::New (isolate));
  _gum_script_set_ascii (module, "name", details->name, core);
  _gum_script_set_pointer (module, "base", details->range->base_address, core);
  _gum_script_set_uint (module, "size", details->range->size, core);
  _gum_script_set_utf8 (module, "path", details->path, core);

  Handle<Value> argv[] = {
    module
  };
  Local<Value> result = ctx->on_match->Call (ctx->receiver, 1, argv);

  gboolean proceed = TRUE;
  if (!result.IsEmpty () && result->IsString ())
  {
    String::Utf8Value str (result);
    proceed = (strcmp (*str, "stop") != 0);
  }

  return proceed;
}

/*
 * Prototype:
 * Process._enumerateRanges(prot, callback)
 *
 * Docs:
 * TBW
 *
 * Example:
 * TBW
 */
static void
gum_script_process_on_enumerate_ranges (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptMatchContext ctx;

  ctx.self = static_cast<GumScriptProcess *> (
      info.Data ().As<External> ()->Value ());
  ctx.isolate = info.GetIsolate ();

  GumPageProtection prot;
  if (!_gum_script_page_protection_get (info[0], &prot, ctx.self->core))
    return;

  Local<Value> callbacks_value = info[1];
  if (!callbacks_value->IsObject ())
  {
    ctx.isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        ctx.isolate, "Process.enumerateRanges: second argument must be "
        "a callback object")));
    return;
  }

  Local<Object> callbacks = Local<Object>::Cast (callbacks_value);
  if (!_gum_script_callbacks_get (callbacks, "onMatch", &ctx.on_match,
      ctx.self->core))
  {
    return;
  }
  if (!_gum_script_callbacks_get (callbacks, "onComplete", &ctx.on_complete,
      ctx.self->core))
  {
    return;
  }

  ctx.receiver = info.This ();

  gum_process_enumerate_ranges (prot, gum_script_process_handle_range_match,
      &ctx);

  ctx.on_complete->Call (ctx.receiver, 0, 0);
}

static gboolean
gum_script_process_handle_range_match (const GumRangeDetails * details,
                                       gpointer user_data)
{
  GumScriptMatchContext * ctx =
      static_cast<GumScriptMatchContext *> (user_data);
  GumScriptCore * core = ctx->self->core;
  Isolate * isolate = ctx->isolate;

  char prot_str[4] = "---";
  if ((details->prot & GUM_PAGE_READ) != 0)
    prot_str[0] = 'r';
  if ((details->prot & GUM_PAGE_WRITE) != 0)
    prot_str[1] = 'w';
  if ((details->prot & GUM_PAGE_EXECUTE) != 0)
    prot_str[2] = 'x';

  Local<Object> range (Object::New (isolate));
  _gum_script_set_pointer (range, "base", details->range->base_address, core);
  _gum_script_set_uint (range, "size", details->range->size, core);
  _gum_script_set_ascii (range, "protection", prot_str, core);

  const GumFileMapping * f = details->file;
  if (f != NULL)
  {
    Local<Object> file (Object::New (isolate));
    _gum_script_set_utf8 (range, "path", f->path, core);
    _gum_script_set_uint (range, "offset", f->offset, core);
    _gum_script_set (range, "file", file, core);
  }

  Handle<Value> argv[] = {
    range
  };
  Local<Value> result = ctx->on_match->Call (ctx->receiver, 1, argv);

  gboolean proceed = TRUE;
  if (!result.IsEmpty () && result->IsString ())
  {
    String::Utf8Value str (result);
    proceed = (strcmp (*str, "stop") != 0);
  }

  return proceed;
}

/*
 * Prototype:
 * Process.enumerateMallocRanges(callback)
 *
 * Docs:
 * TBW
 *
 * Example:
 * TBW
 */
static void
gum_script_process_on_enumerate_malloc_ranges (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptMatchContext ctx;

  ctx.isolate = info.GetIsolate ();

#ifdef HAVE_DARWIN
  ctx.self = static_cast<GumScriptProcess *> (
      info.Data ().As<External> ()->Value ());

  Local<Value> callbacks_value = info[0];
  if (!callbacks_value->IsObject ())
  {
    ctx.isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        ctx.isolate, "Process.enumerateMallocRanges: first argument must be "
        "a callback object")));
    return;
  }

  Local<Object> callbacks = Local<Object>::Cast (callbacks_value);
  if (!_gum_script_callbacks_get (callbacks, "onMatch", &ctx.on_match,
      ctx.self->core))
  {
    return;
  }
  if (!_gum_script_callbacks_get (callbacks, "onComplete", &ctx.on_complete,
      ctx.self->core))
  {
    return;
  }

  ctx.receiver = info.This ();

  gum_process_enumerate_malloc_ranges (gum_script_process_handle_malloc_range_match,
      &ctx);

  ctx.on_complete->Call (ctx.receiver, 0, 0);
#else
  ctx.isolate->ThrowException (Exception::Error (String::NewFromUtf8 (
      ctx.isolate, "Process.enumerateMallocRanges: not implemented yet for "
      GUM_SCRIPT_PLATFORM)));
#endif
}

#ifdef HAVE_DARWIN

static gboolean
gum_script_process_handle_malloc_range_match (const GumMallocRangeDetails * details,
                                              gpointer user_data)
{
  GumScriptMatchContext * ctx =
      static_cast<GumScriptMatchContext *> (user_data);
  GumScriptCore * core = ctx->self->core;
  Isolate * isolate = ctx->isolate;

  Local<Object> range (Object::New (isolate));
  _gum_script_set_pointer (range, "base", details->range->base_address, core);
  _gum_script_set_uint (range, "size", details->range->size, core);

  Handle<Value> argv[] = {
    range
  };
  Local<Value> result = ctx->on_match->Call (ctx->receiver, 1, argv);

  gboolean proceed = TRUE;
  if (!result.IsEmpty () && result->IsString ())
  {
    String::Utf8Value str (result);
    proceed = (strcmp (*str, "stop") != 0);
  }

  return proceed;
}

#endif

/*
 * Prototype:
 * Process.setExceptionHandler(callback)
 *
 * Docs:
 * TBW
 *
 * Example:
 * TBW
 */
static void
gum_script_process_on_set_exception_handler (
    const FunctionCallbackInfo<Value> & info)
{
  GumScriptProcess * self = static_cast<GumScriptProcess *> (
      info.Data ().As<External> ()->Value ());
  Isolate * isolate = info.GetIsolate ();

  bool argument_valid = false;
  Local<Function> callback;
  if (info.Length () >= 1)
  {
    Local<Value> argument = info[0];
    if (argument->IsFunction ())
    {
      argument_valid = true;
      callback = argument.As<Function> ();
    }
    else if (argument->IsNull ())
    {
      argument_valid = true;
    }
  }
  if (!argument_valid)
  {
    isolate->ThrowException (Exception::TypeError (String::NewFromUtf8 (
        isolate, "invalid argument")));
    return;
  }

  gum_script_exception_handler_free (self->exception_handler);
  self->exception_handler = NULL;

  if (!callback.IsEmpty ())
  {
    self->exception_handler = gum_script_exception_handler_new (callback,
        self->core);
  }
}

static GumScriptExceptionHandler *
gum_script_exception_handler_new (Handle<Function> callback,
                                  GumScriptCore * core)
{
  GumScriptExceptionHandler * handler;

  handler = g_slice_new (GumScriptExceptionHandler);
  handler->callback =
      new GumPersistent<Function>::type (core->isolate, callback);
  handler->core = core;

  gum_exceptor_add (core->exceptor, gum_script_exception_handler_on_exception,
      handler);

  return handler;
}

static void
gum_script_exception_handler_free (GumScriptExceptionHandler * handler)
{
  if (handler == NULL)
    return;

  gum_exceptor_remove (handler->core->exceptor,
      gum_script_exception_handler_on_exception, handler);

  delete handler->callback;

  g_slice_free (GumScriptExceptionHandler, handler);
}

static gboolean
gum_script_exception_handler_on_exception (GumExceptionDetails * details,
                                           gpointer user_data)
{
  GumScriptExceptionHandler * handler = (GumScriptExceptionHandler *) user_data;
  GumScriptCore * core = handler->core;

  ScriptScope scope (core->script);
  Isolate * isolate = core->isolate;

  Local<Function> callback (Local<Function>::New (isolate, *handler->callback));

  Local<Object> ex (Object::New (isolate));
  _gum_script_set_ascii (ex, "type",
      gum_script_exception_type_to_string (details->type), core);
  _gum_script_set_pointer (ex, "address", details->address, core);
  if (details->type == GUM_EXCEPTION_ACCESS_VIOLATION)
  {
    const GumExceptionMemoryAccessDetails * mad = &details->memory_access;
    Local<Object> ma (Object::New (isolate));
    _gum_script_set_ascii (ma, "operation",
        _gum_script_memory_operation_to_string (mad->operation), core);
    _gum_script_set_pointer (ma, "address", mad->address, core);
    _gum_script_set (ex, "memoryAccess", ma, core);
  }
  Local<Object> cpu_context =
      _gum_script_cpu_context_new (&details->cpu_context, core);
  _gum_script_set (ex, "context", cpu_context, core);

  Handle<Value> argv[] = { ex };
  Local<Value> result = callback->Call (Null (isolate), 1, argv);

  _gum_script_cpu_context_free_later (
      new GumPersistent<Object>::type (isolate, cpu_context),
      core);

  if (!result.IsEmpty () && result->IsBoolean ())
  {
    bool handled = result.As<Boolean> ()->Value ();
    return handled ? TRUE : FALSE;
  }

  return FALSE;
}

const gchar *
gum_script_exception_type_to_string (GumExceptionType type)
{
  switch (type)
  {
    case GUM_EXCEPTION_ACCESS_VIOLATION: return "access-violation";
    case GUM_EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal-instruction";
    case GUM_EXCEPTION_STACK_OVERFLOW: return "stack-overflow";
    case GUM_EXCEPTION_ARITHMETIC: return "arithmetic";
    case GUM_EXCEPTION_BREAKPOINT: return "breakpoint";
    case GUM_EXCEPTION_SINGLE_STEP: return "single-step";
    default:
      break;
  }

  g_assert_not_reached ();
}
