/*
 * Copyright (C) 2016 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumv8stream.h"

#include "gumv8macros.h"
#include "gumv8scope.h"

using namespace v8;

#ifdef G_OS_WIN32
# include <gio/gwin32inputstream.h>
# include <gio/gwin32outputstream.h>

# define GUM_NATIVE_INPUT_STREAM "Win32InputStream"
# define GUM_NATIVE_OUTPUT_STREAM "Win32OutputStream"
# define GUM_NATIVE_KIND "Windows file handle"
# define GUM_NATIVE_FORMAT "p"
typedef gpointer GumStreamHandle;
#else
# include <gio/gunixinputstream.h>
# include <gio/gunixoutputstream.h>

# define GUM_NATIVE_INPUT_STREAM "UnixInputStream"
# define GUM_NATIVE_OUTPUT_STREAM "UnixOutputStream"
# define GUM_NATIVE_KIND "file descriptor"
# define GUM_NATIVE_FORMAT "i"
typedef gint GumStreamHandle;
#endif

#define GUMJS_MODULE_NAME Stream

struct GumV8CloseIOStreamOperation
    : public GumV8ObjectOperation<GIOStream, GumV8Stream>
{
};

struct GumV8CloseInputOperation
    : public GumV8ObjectOperation<GInputStream, GumV8Stream>
{
};

enum GumV8ReadStrategy
{
  GUM_V8_READ_SOME,
  GUM_V8_READ_ALL
};

struct GumV8ReadOperation
    : public GumV8ObjectOperation<GInputStream, GumV8Stream>
{
  GumV8ReadStrategy strategy;
  gpointer buffer;
  gsize buffer_size;
};

struct GumV8CloseOutputOperation
    : public GumV8ObjectOperation<GOutputStream, GumV8Stream>
{
};

enum GumV8WriteStrategy
{
  GUM_V8_WRITE_SOME,
  GUM_V8_WRITE_ALL
};

struct GumV8WriteOperation
    : public GumV8ObjectOperation<GOutputStream, GumV8Stream>
{
  GumV8WriteStrategy strategy;
  GBytes * bytes;
};

GUMJS_DECLARE_CONSTRUCTOR (gumjs_io_stream_construct)
GUMJS_DECLARE_FUNCTION (gumjs_io_stream_close)
static void gum_v8_close_io_stream_operation_start (
    GumV8CloseIOStreamOperation * self);
static void gum_v8_close_io_stream_operation_finish (GIOStream * stream,
    GAsyncResult * result, GumV8CloseIOStreamOperation * self);

GUMJS_DECLARE_CONSTRUCTOR (gumjs_input_stream_construct)
GUMJS_DECLARE_FUNCTION (gumjs_input_stream_close)
static void gum_v8_close_input_operation_start (
    GumV8CloseInputOperation * self);
static void gum_v8_close_input_operation_finish (GInputStream * stream,
    GAsyncResult * result, GumV8CloseInputOperation * self);
GUMJS_DECLARE_FUNCTION (gumjs_input_stream_read)
GUMJS_DECLARE_FUNCTION (gumjs_input_stream_read_all)
static void gumjs_input_stream_read_with_strategy (GumV8InputStream * self,
    const GumV8Args * args, GumV8ReadStrategy strategy);
static void gum_v8_read_operation_dispose (GumV8ReadOperation * self);
static void gum_v8_read_operation_start (GumV8ReadOperation * self);
static void gum_v8_read_operation_finish (GInputStream * stream,
    GAsyncResult * result, GumV8ReadOperation * self);

GUMJS_DECLARE_CONSTRUCTOR (gumjs_output_stream_construct)
GUMJS_DECLARE_FUNCTION (gumjs_output_stream_close)
static void gum_v8_close_output_operation_start (
    GumV8CloseOutputOperation * self);
static void gum_v8_close_output_operation_finish (GOutputStream * stream,
    GAsyncResult * result, GumV8CloseOutputOperation * self);
GUMJS_DECLARE_FUNCTION (gumjs_output_stream_write)
GUMJS_DECLARE_FUNCTION (gumjs_output_stream_write_all)
static void gumjs_output_stream_write_with_strategy (GumV8OutputStream * self,
    const GumV8Args * args, GumV8WriteStrategy strategy);
static void gum_v8_write_operation_dispose (GumV8WriteOperation * self);
static void gum_v8_write_operation_start (GumV8WriteOperation * self);
static void gum_v8_write_operation_finish (GOutputStream * stream,
    GAsyncResult * result, GumV8WriteOperation * self);

GUMJS_DECLARE_CONSTRUCTOR (gumjs_native_input_stream_construct)

GUMJS_DECLARE_CONSTRUCTOR (gumjs_native_output_stream_construct)

static gboolean gum_v8_native_stream_ctor_args_parse (const GumV8Args * args,
    GumStreamHandle * handle, gboolean * auto_close, GumV8Core * core);

static const GumV8Function gumjs_io_stream_functions[] =
{
  { "_close", gumjs_io_stream_close },

  { NULL, NULL }
};

static const GumV8Function gumjs_input_stream_functions[] =
{
  { "_close", gumjs_input_stream_close },
  { "_read", gumjs_input_stream_read },
  { "_readAll", gumjs_input_stream_read_all },

  { NULL, NULL }
};

static const GumV8Function gumjs_output_stream_functions[] =
{
  { "_close", gumjs_output_stream_close },
  { "_write", gumjs_output_stream_write },
  { "_writeAll", gumjs_output_stream_write_all },

  { NULL, NULL }
};

void
_gum_v8_stream_init (GumV8Stream * self,
                     GumV8Core * core,
                     Handle<ObjectTemplate> scope)
{
  auto isolate = core->isolate;

  self->core = core;

  auto module (External::New (isolate, self));

  auto io_stream = _gum_v8_create_class ("IOStream",
      gumjs_io_stream_construct, scope, module, isolate);
  _gum_v8_class_add (io_stream, gumjs_io_stream_functions, isolate);
  self->io_stream =
      new GumPersistent<FunctionTemplate>::type (isolate, io_stream);

  auto input_stream = _gum_v8_create_class ("InputStream",
      gumjs_input_stream_construct, scope, module, isolate);
  _gum_v8_class_add (input_stream, gumjs_input_stream_functions, isolate);
  self->input_stream =
      new GumPersistent<FunctionTemplate>::type (isolate, input_stream);

  auto output_stream = _gum_v8_create_class ("OutputStream",
      gumjs_output_stream_construct, scope, module, isolate);
  _gum_v8_class_add (output_stream, gumjs_output_stream_functions, isolate);
  self->output_stream =
      new GumPersistent<FunctionTemplate>::type (isolate, output_stream);

  auto native_input_stream = _gum_v8_create_class (GUM_NATIVE_INPUT_STREAM,
      gumjs_native_input_stream_construct, scope, module, isolate);
  native_input_stream->Inherit (input_stream);

  auto native_output_stream = _gum_v8_create_class (GUM_NATIVE_OUTPUT_STREAM,
      gumjs_native_output_stream_construct, scope, module, isolate);
  native_output_stream->Inherit (output_stream);
}

void
_gum_v8_stream_realize (GumV8Stream * self)
{
  gum_v8_object_manager_init (&self->objects);
}

void
_gum_v8_stream_flush (GumV8Stream * self)
{
  gum_v8_object_manager_flush (&self->objects);
}

void
_gum_v8_stream_dispose (GumV8Stream * self)
{
  gum_v8_object_manager_free (&self->objects);
}

void
_gum_v8_stream_finalize (GumV8Stream * self)
{
  delete self->io_stream;
  delete self->input_stream;
  delete self->output_stream;
  self->io_stream = nullptr;
  self->input_stream = nullptr;
  self->output_stream = nullptr;
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_io_stream_construct)
{
  auto core = args->core;
  auto isolate = core->isolate;
  auto context = isolate->GetCurrentContext ();

  GIOStream * stream;
  if (!_gum_v8_args_parse (args, "X", &stream))
    return;

  Local<Object> wrapper (args->info->Holder ());
  gum_v8_object_manager_add (&module->objects, wrapper, stream, module);

  {
    Local<FunctionTemplate> ctor (
        Local<FunctionTemplate>::New (isolate, *module->input_stream));
    Handle<Value> argv[] = {
        External::New (isolate, g_object_ref (
            g_io_stream_get_input_stream (stream)))
    };
    Local<Object> input = ctor->GetFunction ()->NewInstance (context,
        G_N_ELEMENTS (argv), argv).ToLocalChecked ();
    _gum_v8_object_set (wrapper, "input", input, core);
  }

  {
    Local<FunctionTemplate> ctor (
        Local<FunctionTemplate>::New (isolate, *module->output_stream));
    Handle<Value> argv[] = {
        External::New (isolate, g_object_ref (
            g_io_stream_get_output_stream (stream)))
    };
    Local<Object> output = ctor->GetFunction ()->NewInstance (context,
        G_N_ELEMENTS (argv), argv).ToLocalChecked ();
    _gum_v8_object_set (wrapper, "output", output, core);
  }
}

GUMJS_DEFINE_METHOD (IOStream, gumjs_io_stream_close)
{
  Local<Function> callback;
  if (!_gum_v8_args_parse (args, "F", &callback))
    return;

  GumV8CloseIOStreamOperation * op = gum_v8_object_operation_new (self,
      callback, gum_v8_close_io_stream_operation_start);

  GPtrArray * dependencies = g_ptr_array_sized_new (2);

  GumV8ObjectManager * objects = &self->module->objects;
  GIOStream * stream = self->handle;

  GumV8InputStream * input =
      gum_v8_object_manager_lookup<GInputStream, GumV8Stream> (objects,
          g_io_stream_get_input_stream (stream));
  if (input != NULL)
  {
    g_cancellable_cancel (input->cancellable);
    g_ptr_array_add (dependencies, input);
  }

  GumV8OutputStream * output =
      gum_v8_object_manager_lookup<GOutputStream, GumV8Stream> (objects,
          g_io_stream_get_output_stream (stream));
  if (output != NULL)
  {
    g_cancellable_cancel (output->cancellable);
    g_ptr_array_add (dependencies, output);
  }

  g_cancellable_cancel (self->cancellable);

  gum_v8_object_operation_schedule_when_idle (op, dependencies);

  g_ptr_array_unref (dependencies);
}

static void
gum_v8_close_io_stream_operation_start (GumV8CloseIOStreamOperation * self)
{
  g_io_stream_close_async (self->object->handle, G_PRIORITY_DEFAULT, NULL,
      (GAsyncReadyCallback) gum_v8_close_io_stream_operation_finish, self);
}

static void
gum_v8_close_io_stream_operation_finish (GIOStream * stream,
                                         GAsyncResult * result,
                                         GumV8CloseIOStreamOperation * self)
{
  GError * error = NULL;
  gboolean success;

  success = g_io_stream_close_finish (stream, result, &error);

  {
    GumV8Core * core = self->core;
    ScriptScope scope (core->script);
    Isolate * isolate = core->isolate;

    Local<Value> error_value;
    Local<Value> success_value = success ? True (isolate) : False (isolate);
    Local<Value> null_value = Null (isolate);
    if (error == NULL)
    {
      error_value = null_value;
    }
    else
    {
      error_value = Exception::Error (
          String::NewFromUtf8 (isolate, error->message));
      g_error_free (error);
    }

    Handle<Value> argv[] = { error_value, success_value };
    Local<Function> callback (Local<Function>::New (isolate, *self->callback));
    callback->Call (null_value, G_N_ELEMENTS (argv), argv);
  }

  gum_v8_object_operation_finish (self);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_input_stream_construct)
{
  GInputStream * stream;
  if (!_gum_v8_args_parse (args, "X", &stream))
    return;

  gum_v8_object_manager_add (&module->objects, args->info->Holder (), stream,
      module);
}

GUMJS_DEFINE_METHOD (InputStream, gumjs_input_stream_close)
{
  Local<Function> callback;
  if (!_gum_v8_args_parse (args, "F", &callback))
    return;

  g_cancellable_cancel (self->cancellable);

  GumV8CloseInputOperation * op = gum_v8_object_operation_new (self, callback,
      gum_v8_close_input_operation_start);
  gum_v8_object_operation_schedule_when_idle (op);
}

static void
gum_v8_close_input_operation_start (GumV8CloseInputOperation * self)
{
  g_input_stream_close_async (self->object->handle, G_PRIORITY_DEFAULT, NULL,
      (GAsyncReadyCallback) gum_v8_close_input_operation_finish, self);
}

static void
gum_v8_close_input_operation_finish (GInputStream * stream,
                                     GAsyncResult * result,
                                     GumV8CloseInputOperation * self)
{
  GError * error = NULL;
  gboolean success;

  success = g_input_stream_close_finish (stream, result, &error);

  {
    GumV8Core * core = self->core;
    ScriptScope scope (core->script);
    Isolate * isolate = core->isolate;

    Local<Value> error_value;
    Local<Value> success_value = success ? True (isolate) : False (isolate);
    Local<Value> null_value = Null (isolate);
    if (error == NULL)
    {
      error_value = null_value;
    }
    else
    {
      error_value = Exception::Error (
          String::NewFromUtf8 (isolate, error->message));
      g_error_free (error);
    }

    Handle<Value> argv[] = { error_value, success_value };
    Local<Function> callback (Local<Function>::New (isolate, *self->callback));
    callback->Call (null_value, G_N_ELEMENTS (argv), argv);
  }

  gum_v8_object_operation_finish (self);
}

GUMJS_DEFINE_METHOD (InputStream, gumjs_input_stream_read)
{
  gumjs_input_stream_read_with_strategy (self, args, GUM_V8_READ_SOME);
}

GUMJS_DEFINE_METHOD (InputStream, gumjs_input_stream_read_all)
{
  gumjs_input_stream_read_with_strategy (self, args, GUM_V8_READ_ALL);
}

static void
gumjs_input_stream_read_with_strategy (GumV8InputStream * self,
                                       const GumV8Args * args,
                                       GumV8ReadStrategy strategy)
{
  guint64 size;
  Local<Function> callback;
  if (!_gum_v8_args_parse (args, "QF", &size, &callback))
    return;

  GumV8ReadOperation * op = gum_v8_object_operation_new (self, callback,
      gum_v8_read_operation_start, gum_v8_read_operation_dispose);
  op->strategy = strategy;
  op->buffer = g_malloc (size);
  op->buffer_size = size;
  gum_v8_object_operation_schedule (op);
}

static void
gum_v8_read_operation_dispose (GumV8ReadOperation * self)
{
  g_free (self->buffer);
}

static void
gum_v8_read_operation_start (GumV8ReadOperation * self)
{
  GumV8InputStream * stream = self->object;

  if (self->strategy == GUM_V8_READ_SOME)
  {
    g_input_stream_read_async (stream->handle, self->buffer, self->buffer_size,
        G_PRIORITY_DEFAULT, stream->cancellable,
        (GAsyncReadyCallback) gum_v8_read_operation_finish, self);
  }
  else
  {
    g_assert_cmpuint (self->strategy, ==, GUM_V8_READ_ALL);

    g_input_stream_read_all_async (stream->handle, self->buffer,
        self->buffer_size, G_PRIORITY_DEFAULT, stream->cancellable,
        (GAsyncReadyCallback) gum_v8_read_operation_finish, self);
  }
}

static void
gum_v8_read_operation_finish (GInputStream * stream,
                              GAsyncResult * result,
                              GumV8ReadOperation * self)
{
  gsize bytes_read = 0;
  GError * error = NULL;

  if (self->strategy == GUM_V8_READ_SOME)
  {
    gsize n;

    n = g_input_stream_read_finish (stream, result, &error);
    if (n > 0)
      bytes_read = n;
  }
  else
  {
    g_assert_cmpuint (self->strategy, ==, GUM_V8_READ_ALL);

    g_input_stream_read_all_finish (stream, result, &bytes_read, &error);
  }

  {
    GumV8Core * core = self->core;
    ScriptScope scope (core->script);
    Isolate * isolate = core->isolate;

    Local<Value> error_value, data_value, null_value;
    null_value = Null (isolate);
    if (self->strategy == GUM_V8_READ_ALL && bytes_read != self->buffer_size)
    {
      error_value = Exception::Error (
          String::NewFromUtf8 (isolate,
              (error != NULL) ? error->message : "Short read"));
      data_value = ArrayBuffer::New (isolate, self->buffer, bytes_read,
          ArrayBufferCreationMode::kInternalized);
      self->buffer = NULL; /* steal it */
    }
    else if (error == NULL)
    {
      error_value = null_value;
      data_value = ArrayBuffer::New (isolate, self->buffer, bytes_read,
          ArrayBufferCreationMode::kInternalized);
      self->buffer = NULL; /* steal it */
    }
    else
    {
      error_value = Exception::Error (
          String::NewFromUtf8 (isolate, error->message));
      data_value = null_value;
    }

    g_clear_error (&error);

    Handle<Value> argv[] = { error_value, data_value };
    Local<Function> callback (Local<Function>::New (isolate, *self->callback));
    callback->Call (null_value, G_N_ELEMENTS (argv), argv);
  }

  gum_v8_object_operation_finish (self);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_output_stream_construct)
{
  GOutputStream * stream;
  if (!_gum_v8_args_parse (args, "X", &stream))
    return;

  gum_v8_object_manager_add (&module->objects, args->info->Holder (), stream,
      module);
}

GUMJS_DEFINE_METHOD (OutputStream, gumjs_output_stream_close)
{
  Local<Function> callback;
  if (!_gum_v8_args_parse (args, "F", &callback))
    return;

  g_cancellable_cancel (self->cancellable);

  GumV8CloseOutputOperation * op = gum_v8_object_operation_new (self, callback,
      gum_v8_close_output_operation_start);
  gum_v8_object_operation_schedule_when_idle (op);
}

static void
gum_v8_close_output_operation_start (GumV8CloseOutputOperation * self)
{
  g_output_stream_close_async (self->object->handle, G_PRIORITY_DEFAULT, NULL,
      (GAsyncReadyCallback) gum_v8_close_output_operation_finish, self);
}

static void
gum_v8_close_output_operation_finish (GOutputStream * stream,
                                      GAsyncResult * result,
                                      GumV8CloseOutputOperation * self)
{
  GError * error = NULL;
  gboolean success;

  success = g_output_stream_close_finish (stream, result, &error);

  {
    GumV8Core * core = self->core;
    ScriptScope scope (core->script);
    Isolate * isolate = core->isolate;

    Local<Value> error_value;
    Local<Value> success_value = success ? True (isolate) : False (isolate);
    Local<Value> null_value = Null (isolate);
    if (error == NULL)
    {
      error_value = null_value;
    }
    else
    {
      error_value = Exception::Error (
          String::NewFromUtf8 (isolate, error->message));
      g_error_free (error);
    }

    Handle<Value> argv[] = { error_value, success_value };
    Local<Function> callback (Local<Function>::New (isolate, *self->callback));
    callback->Call (null_value, G_N_ELEMENTS (argv), argv);
  }

  gum_v8_object_operation_finish (self);
}

GUMJS_DEFINE_METHOD (OutputStream, gumjs_output_stream_write)
{
  gumjs_output_stream_write_with_strategy (self, args, GUM_V8_WRITE_SOME);
}

GUMJS_DEFINE_METHOD (OutputStream, gumjs_output_stream_write_all)
{
  gumjs_output_stream_write_with_strategy (self, args, GUM_V8_WRITE_ALL);
}

static void
gumjs_output_stream_write_with_strategy (GumV8OutputStream * self,
                                         const GumV8Args * args,
                                         GumV8WriteStrategy strategy)
{
  GBytes * bytes;
  Local<Function> callback;
  if (!_gum_v8_args_parse (args, "BF", &bytes, &callback))
    return;

  GumV8WriteOperation * op = gum_v8_object_operation_new (self, callback,
      gum_v8_write_operation_start, gum_v8_write_operation_dispose);
  op->strategy = strategy;
  op->bytes = bytes;
  gum_v8_object_operation_schedule (op);
}

static void
gum_v8_write_operation_dispose (GumV8WriteOperation * self)
{
  g_bytes_unref (self->bytes);
}

static void
gum_v8_write_operation_start (GumV8WriteOperation * self)
{
  GumV8OutputStream * stream = self->object;

  if (self->strategy == GUM_V8_WRITE_SOME)
  {
    g_output_stream_write_bytes_async (stream->handle, self->bytes,
        G_PRIORITY_DEFAULT, stream->cancellable,
        (GAsyncReadyCallback) gum_v8_write_operation_finish, self);
  }
  else
  {
    g_assert_cmpuint (self->strategy, ==, GUM_V8_WRITE_ALL);

    gsize size;
    gconstpointer data = g_bytes_get_data (self->bytes, &size);

    g_output_stream_write_all_async (stream->handle, data, size,
        G_PRIORITY_DEFAULT, stream->cancellable,
        (GAsyncReadyCallback) gum_v8_write_operation_finish, self);
  }
}

static void
gum_v8_write_operation_finish (GOutputStream * stream,
                               GAsyncResult * result,
                               GumV8WriteOperation * self)
{
  gsize bytes_written = 0;
  GError * error = NULL;

  if (self->strategy == GUM_V8_WRITE_SOME)
  {
    gssize n;

    n = g_output_stream_write_bytes_finish (stream, result, &error);
    if (n > 0)
      bytes_written = n;
  }
  else
  {
    g_assert_cmpuint (self->strategy, ==, GUM_V8_WRITE_ALL);

    g_output_stream_write_all_finish (stream, result, &bytes_written, &error);
  }

  {
    GumV8Core * core = self->core;
    ScriptScope scope (core->script);
    Isolate * isolate = core->isolate;

    Local<Value> error_value;
    Local<Value> size_value = Integer::NewFromUnsigned (isolate, bytes_written);
    Local<Value> null_value = Null (isolate);
    if (self->strategy == GUM_V8_WRITE_ALL &&
        bytes_written != g_bytes_get_size (self->bytes))
    {
      error_value = Exception::Error (
          String::NewFromUtf8 (isolate,
              (error != NULL) ? error->message : "Short write"));
    }
    else if (error == NULL)
    {
      error_value = null_value;
    }
    else
    {
      error_value = Exception::Error (
          String::NewFromUtf8 (isolate, error->message));
    }

    g_clear_error (&error);

    Handle<Value> argv[] = { error_value, size_value };
    Local<Function> callback (Local<Function>::New (isolate, *self->callback));
    callback->Call (null_value, G_N_ELEMENTS (argv), argv);
  }

  gum_v8_object_operation_finish (self);
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_native_input_stream_construct)
{
  auto core = args->core;
  auto isolate = core->isolate;

  if (!args->info->IsConstructCall ())
  {
    _gum_v8_throw_ascii_literal (isolate, "Use `new " GUM_NATIVE_INPUT_STREAM
        "()` to create a new instance");
    return;
  }

  GumStreamHandle handle;
  gboolean auto_close;
  if (!gum_v8_native_stream_ctor_args_parse (args, &handle, &auto_close, core))
    return;

  GInputStream * stream;
#ifdef G_OS_WIN32
  stream = g_win32_input_stream_new (handle, auto_close);
#else
  stream = g_unix_input_stream_new (handle, auto_close);
#endif

  Local<FunctionTemplate> base_ctor (
      Local<FunctionTemplate>::New (isolate, *module->input_stream));
  Handle<Value> argv[] = { External::New (isolate, stream) };
  base_ctor->GetFunction ()->Call (isolate->GetCurrentContext (),
      args->info->Holder (), G_N_ELEMENTS (argv), argv).ToLocalChecked ();
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_native_output_stream_construct)
{
  auto core = args->core;
  auto isolate = core->isolate;

  if (!args->info->IsConstructCall ())
  {
    _gum_v8_throw_ascii_literal (isolate, "Use `new " GUM_NATIVE_OUTPUT_STREAM
        "()` to create a new instance");
    return;
  }

  GumStreamHandle handle;
  gboolean auto_close;
  if (!gum_v8_native_stream_ctor_args_parse (args, &handle, &auto_close, core))
    return;

  GOutputStream * stream;
#ifdef G_OS_WIN32
  stream = g_win32_output_stream_new (handle, auto_close);
#else
  stream = g_unix_output_stream_new (handle, auto_close);
#endif

  Local<FunctionTemplate> base_ctor (
      Local<FunctionTemplate>::New (isolate, *module->output_stream));
  Handle<Value> argv[] = { External::New (isolate, stream) };
  base_ctor->GetFunction ()->Call (isolate->GetCurrentContext (),
      args->info->Holder (), G_N_ELEMENTS (argv), argv).ToLocalChecked ();
}

static gboolean
gum_v8_native_stream_ctor_args_parse (const GumV8Args * args,
                                      GumStreamHandle * handle,
                                      gboolean * auto_close,
                                      GumV8Core * core)
{
  Local<Object> options;
  if (!_gum_v8_args_parse (args, GUM_NATIVE_FORMAT "|O", handle, &options))
    return FALSE;

  *auto_close = FALSE;
  if (!options.IsEmpty ())
  {
    auto auto_close_key =
        _gum_v8_string_new_from_ascii ("autoClose", core->isolate);
    if (options->Has (auto_close_key))
    {
      *auto_close =
          options->Get (auto_close_key)->ToBoolean ()->BooleanValue ();
    }
  }

  return TRUE;
}
