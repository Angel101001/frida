/*
 * Copyright (C) 2015 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumdukinterceptor.h"

#include "gumdukmacros.h"
#include "gumdukscript-priv.h"

#ifdef G_OS_WIN32
# define GUM_SYSTEM_ERROR_FIELD "lastError"
#else
# define GUM_SYSTEM_ERROR_FIELD "errno"
#endif

typedef struct _GumDukAttachEntry GumDukAttachEntry;
typedef struct _GumDukReplaceEntry GumDukReplaceEntry;

struct _GumDukInvocationArgs
{
  GumDukHeapPtr object;
  GumInvocationContext * ic;

  duk_context * ctx;
};

struct _GumDukInvocationReturnValue
{
  GumDukNativePointer parent;

  GumDukHeapPtr object;
  GumInvocationContext * ic;

  duk_context * ctx;
};

struct _GumDukAttachEntry
{
  GumDukHeapPtr on_enter;
  GumDukHeapPtr on_leave;
  duk_context * ctx;
};

struct _GumDukReplaceEntry
{
  GumInterceptor * interceptor;
  gpointer target;
  GumDukHeapPtr replacement;
  GumDukCore * core;
};

GUMJS_DECLARE_CONSTRUCTOR (gumjs_interceptor_construct)
GUMJS_DECLARE_FUNCTION (gumjs_interceptor_attach)
static void gum_duk_attach_entry_free (GumDukAttachEntry * entry);
GUMJS_DECLARE_FUNCTION (gumjs_interceptor_detach_all)
static void gum_duk_interceptor_detach_all (GumDukInterceptor * self);
GUMJS_DECLARE_FUNCTION (gumjs_interceptor_replace)
static void gum_duk_replace_entry_free (GumDukReplaceEntry * entry);
GUMJS_DECLARE_FUNCTION (gumjs_interceptor_revert)
static GumDukInvocationArgs * gum_duk_interceptor_obtain_invocation_args (
    GumDukInterceptor * self);
static void gum_duk_interceptor_release_invocation_args (
    GumDukInterceptor * self, GumDukInvocationArgs * args);
static GumDukInvocationReturnValue *
gum_duk_interceptor_obtain_invocation_return_value (GumDukInterceptor * self);
static void gum_duk_interceptor_release_invocation_return_value (
    GumDukInterceptor * self, GumDukInvocationReturnValue * retval);

static GumDukInvocationContext * gum_duk_invocation_context_new (
    GumDukInterceptor * parent);
static void gum_duk_invocation_context_release (GumDukInvocationContext * self);
GUMJS_DECLARE_CONSTRUCTOR (gumjs_invocation_context_construct)
GUMJS_DECLARE_FINALIZER (gumjs_invocation_context_finalize)
GUMJS_DECLARE_GETTER (gumjs_invocation_context_get_return_address)
GUMJS_DECLARE_GETTER (gumjs_invocation_context_get_cpu_context)
GUMJS_DECLARE_GETTER (gumjs_invocation_context_get_system_error)
GUMJS_DECLARE_SETTER (gumjs_invocation_context_set_system_error)
GUMJS_DECLARE_GETTER (gumjs_invocation_context_get_thread_id)
GUMJS_DECLARE_GETTER (gumjs_invocation_context_get_depth)
GUMJS_DECLARE_SETTER (gumjs_invocation_context_set_property)

static GumDukInvocationArgs * gum_duk_invocation_args_new (
    GumDukInterceptor * parent);
static void gum_duk_invocation_args_release (GumDukInvocationArgs * self);
static void gum_duk_invocation_args_reset (GumDukInvocationArgs * self,
    GumInvocationContext * ic);
GUMJS_DECLARE_CONSTRUCTOR (gumjs_invocation_args_construct)
GUMJS_DECLARE_FINALIZER (gumjs_invocation_args_finalize)
GUMJS_DECLARE_GETTER (gumjs_invocation_args_get_property)
GUMJS_DECLARE_SETTER (gumjs_invocation_args_set_property)

static GumDukInvocationReturnValue * gum_duk_invocation_return_value_new (
    GumDukInterceptor * parent);
static void gum_duk_invocation_return_value_release (
    GumDukInvocationReturnValue * self);
static void gum_duk_invocation_return_value_reset (
    GumDukInvocationReturnValue * self, GumInvocationContext * ic);
GUMJS_DECLARE_CONSTRUCTOR (gumjs_invocation_return_value_construct)
GUMJS_DECLARE_FINALIZER (gumjs_invocation_return_value_finalize)
GUMJS_DECLARE_FUNCTION (gumjs_invocation_return_value_replace)

static const duk_function_list_entry gumjs_interceptor_functions[] =
{
  { "_attach", gumjs_interceptor_attach, 2 },
  { "detachAll", gumjs_interceptor_detach_all, 0 },
  { "_replace", gumjs_interceptor_replace, 2 },
  { "revert", gumjs_interceptor_revert, 1 },

  { NULL, NULL, 0 }
};

static const GumDukPropertyEntry gumjs_invocation_context_values[] =
{
  {
    "returnAddress",
    gumjs_invocation_context_get_return_address,
    NULL
  },
  {
    "context",
    gumjs_invocation_context_get_cpu_context,
    NULL
  },
  {
    GUM_SYSTEM_ERROR_FIELD,
    gumjs_invocation_context_get_system_error,
    gumjs_invocation_context_set_system_error
  },
  {
    "threadId",
    gumjs_invocation_context_get_thread_id,
    NULL
  },
  {
    "depth",
    gumjs_invocation_context_get_depth,
    NULL
  },

  { NULL, NULL, NULL }
};

static const duk_function_list_entry gumjs_invocation_return_value_functions[] =
{
  { "replace", gumjs_invocation_return_value_replace, 1 },

  { NULL, NULL, 0 }
};

void
_gum_duk_interceptor_init (GumDukInterceptor * self,
                           GumDukCore * core)
{
  duk_context * ctx = core->ctx;

  self->core = core;

  self->interceptor = gum_interceptor_obtain ();

  self->attach_entries = g_queue_new ();
  self->replacement_by_address = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) gum_duk_replace_entry_free);

  duk_push_c_function (ctx, gumjs_interceptor_construct, 0);
  duk_push_object (ctx);
  duk_put_function_list (ctx, -1, gumjs_interceptor_functions);
  duk_put_prop_string (ctx, -2, "prototype");
  duk_new (ctx, 0);
  _gum_duk_put_data (ctx, -1, self);
  duk_put_global_string (ctx, "Interceptor");

  duk_push_c_function (ctx, gumjs_invocation_context_construct, 0);
  duk_push_object (ctx);
  duk_push_c_function (ctx, gumjs_invocation_context_finalize, 1);
  duk_set_finalizer (ctx, -2);
  duk_put_prop_string (ctx, -2, "prototype");
  self->invocation_context = _gum_duk_require_heapptr (ctx, -1);
  duk_put_global_string (ctx, "InvocationContext");
  _gum_duk_add_properties_to_class (ctx, "InvocationContext",
      gumjs_invocation_context_values);

  duk_push_c_function (ctx, gumjs_invocation_args_construct, 0);
  duk_push_object (ctx);
  duk_push_c_function (ctx, gumjs_invocation_args_finalize, 1);
  duk_set_finalizer (ctx, -2);
  duk_put_prop_string (ctx, -2, "prototype");
  self->invocation_args = _gum_duk_require_heapptr (ctx, -1);
  duk_put_global_string (ctx, "InvocationArgs");

  _gum_duk_create_subclass (ctx, "NativePointer", "InvocationReturnValue",
      gumjs_invocation_return_value_construct, 1, NULL);
  duk_get_global_string (ctx, "InvocationReturnValue");
  duk_get_prop_string (ctx, -1, "prototype");
  duk_push_c_function (ctx, gumjs_invocation_return_value_finalize, 1);
  duk_set_finalizer (ctx, -2);
  duk_put_function_list (ctx, -1, gumjs_invocation_return_value_functions);
  duk_pop (ctx);
  self->invocation_retval = _gum_duk_require_heapptr (ctx, -1);
  duk_pop (ctx);

  self->cached_invocation_context = gum_duk_invocation_context_new (self);
  self->cached_invocation_context_in_use = FALSE;

  self->cached_invocation_args = gum_duk_invocation_args_new (self);
  self->cached_invocation_args_in_use = FALSE;

  self->cached_invocation_return_value = gum_duk_invocation_return_value_new (
      self);
  self->cached_invocation_return_value_in_use = FALSE;
}

void
_gum_duk_interceptor_flush (GumDukInterceptor * self)
{
  gum_duk_interceptor_detach_all (self);

  g_hash_table_remove_all (self->replacement_by_address);
}

void
_gum_duk_interceptor_dispose (GumDukInterceptor * self)
{
  duk_context * ctx = self->core->ctx;

  gum_duk_invocation_context_release (self->cached_invocation_context);
  gum_duk_invocation_args_release (self->cached_invocation_args);
  gum_duk_invocation_return_value_release (
      self->cached_invocation_return_value);

  _gum_duk_release_heapptr (ctx, self->invocation_context);
  _gum_duk_release_heapptr (ctx, self->invocation_args);
  _gum_duk_release_heapptr (ctx, self->invocation_retval);
}

void
_gum_duk_interceptor_finalize (GumDukInterceptor * self)
{
  g_clear_pointer (&self->attach_entries, g_queue_free);
  g_clear_pointer (&self->replacement_by_address, g_hash_table_unref);

  g_clear_pointer (&self->interceptor, g_object_unref);
}

static GumDukInterceptor *
gumjs_interceptor_from_args (const GumDukArgs * args)
{
  duk_context * ctx = args->ctx;
  GumDukInterceptor * self;

  duk_push_this (ctx);
  self = _gum_duk_require_data (ctx, -1);
  duk_pop (ctx);

  return self;
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_interceptor_construct)
{
  (void) ctx;
  (void) args;

  return 0;
}

GUMJS_DEFINE_FUNCTION (gumjs_interceptor_attach)
{
  GumDukInterceptor * self;
  GumDukCore * core = args->core;
  gpointer target;
  GumDukHeapPtr on_enter, on_leave;
  GumDukAttachEntry * entry;
  GumAttachReturn attach_ret;

  self = gumjs_interceptor_from_args (args);

  _gum_duk_args_parse (args, "pF{onEnter?,onLeave?}", &target,
      &on_enter, &on_leave);

  entry = g_slice_new (GumDukAttachEntry);
  _gum_duk_protect (ctx, on_enter);
  entry->on_enter = on_enter;
  _gum_duk_protect (ctx, on_leave);
  entry->on_leave = on_leave;
  entry->ctx = core->ctx;

  attach_ret = gum_interceptor_attach_listener (self->interceptor, target,
      GUM_INVOCATION_LISTENER (core->script), entry);

  if (attach_ret != GUM_ATTACH_OK)
    goto unable_to_attach;

  g_queue_push_tail (self->attach_entries, entry);

  return 0;

unable_to_attach:
  {
    gum_duk_attach_entry_free (entry);

    switch (attach_ret)
    {
      case GUM_ATTACH_WRONG_SIGNATURE:
        _gum_duk_throw (ctx, "unable to intercept function at %p; "
            "please file a bug", target);
      case GUM_ATTACH_ALREADY_ATTACHED:
        _gum_duk_throw (ctx, "already attached to this function");
      default:
        g_assert_not_reached ();
    }

    return 0;
  }
}

static void
gum_duk_attach_entry_free (GumDukAttachEntry * entry)
{
  _gum_duk_unprotect (entry->ctx, entry->on_enter);
  _gum_duk_unprotect (entry->ctx, entry->on_leave);

  g_slice_free (GumDukAttachEntry, entry);
}

GUMJS_DEFINE_FUNCTION (gumjs_interceptor_detach_all)
{
  GumDukInterceptor * self = gumjs_interceptor_from_args (args);

  (void) ctx;

  gum_duk_interceptor_detach_all (self);

  return 0;
}

static void
gum_duk_interceptor_detach_all (GumDukInterceptor * self)
{
  gum_interceptor_detach_listener (self->interceptor,
      GUM_INVOCATION_LISTENER (self->core->script));

  while (!g_queue_is_empty (self->attach_entries))
  {
    GumDukAttachEntry * entry = g_queue_pop_tail (self->attach_entries);

    gum_duk_attach_entry_free (entry);
  }
}

GUMJS_DEFINE_FUNCTION (gumjs_interceptor_replace)
{
  GumDukInterceptor * self;
  GumDukCore * core = args->core;
  gpointer target, replacement;
  GumDukHeapPtr replacement_value;
  GumDukReplaceEntry * entry;
  GumReplaceReturn replace_ret;

  self = gumjs_interceptor_from_args (args);

  _gum_duk_args_parse (args, "pO", &target, &replacement_value);

  duk_push_heapptr (ctx, replacement_value);
  if (!_gum_duk_get_pointer (ctx, -1, core, &replacement))
    _gum_duk_throw (ctx, "expected a pointer");
  duk_pop (ctx);

  entry = g_slice_new (GumDukReplaceEntry);
  entry->interceptor = self->interceptor;
  entry->target = target;
  entry->replacement = replacement_value;
  entry->core = core;

  replace_ret = gum_interceptor_replace_function (self->interceptor, target,
      replacement, NULL);
  if (replace_ret != GUM_REPLACE_OK)
    goto unable_to_replace;

  _gum_duk_protect (ctx, replacement_value);

  g_hash_table_insert (self->replacement_by_address, target, entry);

  return 0;

unable_to_replace:
  {
    g_slice_free (GumDukReplaceEntry, entry);

    switch (replace_ret)
    {
      case GUM_REPLACE_WRONG_SIGNATURE:
        _gum_duk_throw (ctx, "unable to intercept function at %p; "
            "please file a bug", target);
      case GUM_REPLACE_ALREADY_REPLACED:
        _gum_duk_throw (ctx, "already replaced this function");
      default:
        g_assert_not_reached ();
    }

    return 0;
  }
}

static void
gum_duk_replace_entry_free (GumDukReplaceEntry * entry)
{
  GumDukCore * core = entry->core;

  gum_interceptor_revert_function (entry->interceptor, entry->target);

  _gum_duk_unprotect (core->ctx, entry->replacement);

  g_slice_free (GumDukReplaceEntry, entry);
}

GUMJS_DEFINE_FUNCTION (gumjs_interceptor_revert)
{
  GumDukInterceptor * self;
  gpointer target;

  (void) ctx;

  self = gumjs_interceptor_from_args (args);

  _gum_duk_args_parse (args, "p", &target);

  g_hash_table_remove (self->replacement_by_address, target);

  return 0;
}

void
_gum_duk_interceptor_on_enter (GumDukInterceptor * self,
                               GumInvocationContext * ic)
{
  GumDukAttachEntry * entry;
  gint * depth;

  if (gum_script_backend_is_ignoring (
      gum_invocation_context_get_thread_id (ic)))
    return;

  entry = gum_invocation_context_get_listener_function_data (ic);
  depth = GUM_LINCTX_GET_THREAD_DATA (ic, gint);

  if (entry->on_enter != NULL)
  {
    GumDukCore * core = self->core;
    duk_context * ctx = core->ctx;
    GumDukScope scope;
    GumDukInvocationContext * jic;
    GumDukInvocationArgs * args;

    _gum_duk_scope_enter (&scope, core);

    jic = _gum_duk_interceptor_obtain_invocation_context (self);
    _gum_duk_invocation_context_reset (jic, ic, *depth);

    args = gum_duk_interceptor_obtain_invocation_args (self);
    gum_duk_invocation_args_reset (args, ic);

    duk_push_heapptr (ctx, entry->on_enter);
    duk_push_heapptr (ctx, jic->object);
    duk_push_heapptr (ctx, args->object);
    _gum_duk_scope_call_method (&scope, 1);
    duk_pop (ctx);

    gum_duk_invocation_args_reset (args, NULL);
    gum_duk_interceptor_release_invocation_args (self, args);

    _gum_duk_invocation_context_reset (jic, NULL, 0);
    if (entry->on_leave != NULL)
    {
      *GUM_LINCTX_GET_FUNC_INVDATA (ic, GumDukHeapPtr) = jic;
    }
    else
    {
      _gum_duk_interceptor_release_invocation_context (self, jic);
    }

    _gum_duk_scope_leave (&scope);
  }

  (*depth)++;
}

void
_gum_duk_interceptor_on_leave (GumDukInterceptor * self,
                               GumInvocationContext * ic)
{
  GumDukAttachEntry * entry;
  gint * depth;

  if (gum_script_backend_is_ignoring (
      gum_invocation_context_get_thread_id (ic)))
    return;

  entry = gum_invocation_context_get_listener_function_data (ic);
  depth = GUM_LINCTX_GET_THREAD_DATA (ic, gint);

  (*depth)--;

  if (entry->on_leave != NULL)
  {
    GumDukCore * core = self->core;
    duk_context * ctx = core->ctx;
    GumDukScope scope;
    GumDukInvocationContext * jic;
    GumDukInvocationReturnValue * retval;

    _gum_duk_scope_enter (&scope, core);

    jic = (entry->on_enter != NULL)
        ? *GUM_LINCTX_GET_FUNC_INVDATA (ic, GumDukInvocationContext *)
        : NULL;
    if (jic == NULL)
    {
      jic = _gum_duk_interceptor_obtain_invocation_context (self);
    }
    _gum_duk_invocation_context_reset (jic, ic, *depth);

    retval = gum_duk_interceptor_obtain_invocation_return_value (self);
    gum_duk_invocation_return_value_reset (retval, ic);

    duk_push_heapptr (ctx, entry->on_leave);
    duk_push_heapptr (ctx, jic->object);
    duk_push_heapptr (ctx, retval->object);
    _gum_duk_scope_call_method (&scope, 1);
    duk_pop (ctx);

    gum_duk_invocation_return_value_reset (retval, NULL);
    gum_duk_interceptor_release_invocation_return_value (self, retval);

    _gum_duk_invocation_context_reset (jic, NULL, 0);
    _gum_duk_interceptor_release_invocation_context (self, jic);

    _gum_duk_scope_leave (&scope);
  }
}

GumDukInvocationContext *
_gum_duk_interceptor_obtain_invocation_context (GumDukInterceptor * self)
{
  GumDukInvocationContext * jic;

  if (!self->cached_invocation_context_in_use)
  {
    jic = self->cached_invocation_context;
    self->cached_invocation_context_in_use = TRUE;
  }
  else
  {
    jic = gum_duk_invocation_context_new (self);
  }

  return jic;
}

void
_gum_duk_interceptor_release_invocation_context (GumDukInterceptor * self,
                                                 GumDukInvocationContext * jic)
{
  if (jic == self->cached_invocation_context)
    self->cached_invocation_context_in_use = FALSE;
  else
    gum_duk_invocation_context_release (jic);
}

static GumDukInvocationArgs *
gum_duk_interceptor_obtain_invocation_args (GumDukInterceptor * self)
{
  GumDukInvocationArgs * args;

  if (!self->cached_invocation_args_in_use)
  {
    args = self->cached_invocation_args;
    self->cached_invocation_args_in_use = TRUE;
  }
  else
  {
    args = gum_duk_invocation_args_new (self);
  }

  return args;
}

static void
gum_duk_interceptor_release_invocation_args (GumDukInterceptor * self,
                                             GumDukInvocationArgs * args)
{
  if (args == self->cached_invocation_args)
    self->cached_invocation_args_in_use = FALSE;
  else
    gum_duk_invocation_args_release (args);
}

static GumDukInvocationReturnValue *
gum_duk_interceptor_obtain_invocation_return_value (GumDukInterceptor * self)
{
  GumDukInvocationReturnValue * retval;

  if (!self->cached_invocation_return_value_in_use)
  {
    retval = self->cached_invocation_return_value;
    self->cached_invocation_return_value_in_use = TRUE;
  }
  else
  {
    retval = gum_duk_invocation_return_value_new (self);
  }

  return retval;
}

static void
gum_duk_interceptor_release_invocation_return_value (
    GumDukInterceptor * self,
    GumDukInvocationReturnValue * retval)
{
  if (retval == self->cached_invocation_return_value)
    self->cached_invocation_return_value_in_use = FALSE;
  else
    gum_duk_invocation_return_value_release (retval);
}

static GumDukInvocationContext *
gum_duk_invocation_context_new (GumDukInterceptor * parent)
{
  duk_context * ctx = parent->core->ctx;
  GumDukInvocationContext * jic;

  jic = g_slice_new (GumDukInvocationContext);

  duk_push_heapptr (ctx, parent->invocation_context);
  duk_new (ctx, 0);

  _gum_duk_put_data (ctx, -1, jic);

  _gum_duk_push_proxy (ctx, -1, NULL, gumjs_invocation_context_set_property);
  jic->object = _gum_duk_require_heapptr (ctx, -1);

  duk_pop_2 (ctx);

  jic->handle = NULL;
  jic->cpu_context = NULL;
  jic->depth = 0;

  jic->interceptor = parent;

  return jic;
}

static void
gum_duk_invocation_context_release (GumDukInvocationContext * self)
{
  _gum_duk_release_heapptr (self->interceptor->core->ctx, self->object);
}

void
_gum_duk_invocation_context_reset (GumDukInvocationContext * self,
                                   GumInvocationContext * handle,
                                   gint depth)
{
  self->handle = handle;
  self->depth = depth;

  if (self->cpu_context != NULL)
  {
    duk_context * ctx = self->interceptor->core->ctx;

    _gum_duk_cpu_context_make_read_only (self->cpu_context);
    self->cpu_context = NULL;

    duk_push_heapptr (ctx, self->object);
    duk_push_null (ctx);
    duk_put_prop_string (ctx, -2, "\xff" "cc");
    duk_pop (ctx);
  }
}

static GumDukInvocationContext *
gumjs_invocation_context_from_args (const GumDukArgs * args)
{
  duk_context * ctx = args->ctx;
  GumDukInvocationContext * self;

  duk_push_this (ctx);
  self = _gum_duk_require_data (ctx, -1);
  duk_pop (ctx);

  if (self->handle == NULL)
    _gum_duk_throw (ctx, "invalid operation");

  return self;
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_invocation_context_construct)
{
  (void) ctx;
  (void) args;

  return 0;
}

GUMJS_DEFINE_FINALIZER (gumjs_invocation_context_finalize)
{
  GumDukInvocationContext * self;

  (void) args;

  if (_gum_duk_is_arg0_equal_to_prototype (ctx, "InvocationContext"))
    return 0;

  self = _gum_duk_steal_data (ctx, 0);
  if (self == NULL)
    return 0;

  g_slice_free (GumDukInvocationContext, self);

  return 0;
}

GUMJS_DEFINE_GETTER (gumjs_invocation_context_get_return_address)
{
  GumDukInvocationContext * self = gumjs_invocation_context_from_args (args);

  _gum_duk_push_native_pointer (ctx,
      gum_invocation_context_get_return_address (self->handle), args->core);
  return 1;
}

GUMJS_DEFINE_GETTER (gumjs_invocation_context_get_cpu_context)
{
  GumDukInvocationContext * self = gumjs_invocation_context_from_args (args);

  if (self->cpu_context == NULL)
  {
    duk_push_this (ctx);
    self->cpu_context = _gum_duk_push_cpu_context (ctx,
        self->handle->cpu_context, GUM_CPU_CONTEXT_READWRITE, args->core);
    duk_put_prop_string (ctx, -2, "\xff" "cc");
    duk_pop (ctx);
  }

  duk_push_heapptr (ctx, self->cpu_context->object);
  return 1;
}

GUMJS_DEFINE_GETTER (gumjs_invocation_context_get_system_error)
{
  GumDukInvocationContext * self = gumjs_invocation_context_from_args (args);

  duk_push_number (ctx, self->handle->system_error);
  return 1;
}

GUMJS_DEFINE_SETTER (gumjs_invocation_context_set_system_error)
{
  GumDukInvocationContext * self;
  gint value;

  (void) ctx;

  self = gumjs_invocation_context_from_args (args);

  _gum_duk_args_parse (args, "i", &value);

  self->handle->system_error = value;
  return 0;
}

GUMJS_DEFINE_GETTER (gumjs_invocation_context_get_thread_id)
{
  GumDukInvocationContext * self = gumjs_invocation_context_from_args (args);

  duk_push_number (ctx,
      gum_invocation_context_get_thread_id (self->handle));
  return 1;
}

GUMJS_DEFINE_GETTER (gumjs_invocation_context_get_depth)
{
  GumDukInvocationContext * self = gumjs_invocation_context_from_args (args);

  duk_push_number (ctx, self->depth);
  return 1;
}

GUMJS_DEFINE_SETTER (gumjs_invocation_context_set_property)
{
  GumDukInvocationContext * self;
  GumDukHeapPtr receiver;
  GumDukInterceptor * interceptor;

  (void) args;

  self = _gum_duk_require_data (ctx, 0);
  receiver = _gum_duk_require_heapptr (ctx, 3);
  interceptor = self->interceptor;

  duk_dup (ctx, 1);
  duk_dup (ctx, 2);
  duk_put_prop (ctx, 0);

  if (receiver == interceptor->cached_invocation_context->object)
  {
    interceptor->cached_invocation_context =
        gum_duk_invocation_context_new (interceptor);
    interceptor->cached_invocation_context_in_use = FALSE;
  }

  duk_push_true (ctx);
  return 1;
}

static GumDukInvocationArgs *
gum_duk_invocation_args_new (GumDukInterceptor * parent)
{
  duk_context * ctx = parent->core->ctx;
  GumDukInvocationArgs * args;

  args = g_slice_new (GumDukInvocationArgs);

  duk_push_heapptr (ctx, parent->invocation_args);
  duk_new (ctx, 0);
  _gum_duk_put_data (ctx, -1, args);
  args->object = _gum_duk_require_heapptr (ctx, -1);
  duk_pop (ctx);

  args->ic = NULL;
  args->ctx = ctx;

  return args;
}

static void
gum_duk_invocation_args_release (GumDukInvocationArgs * self)
{
  _gum_duk_release_heapptr (self->ctx, self->object);
}

static void
gum_duk_invocation_args_reset (GumDukInvocationArgs * self,
                               GumInvocationContext * ic)
{
  self->ic = ic;
}

static GumInvocationContext *
gumjs_invocation_args_require_context (duk_context * ctx,
                                       duk_idx_t index)
{
  GumDukInvocationArgs * self = _gum_duk_require_data (ctx, index);

  if (self->ic == NULL)
    _gum_duk_throw (self->ctx, "invalid operation");

  return self->ic;
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_invocation_args_construct)
{
  (void) args;

  duk_push_this (ctx);
  _gum_duk_push_proxy (ctx, -1, gumjs_invocation_args_get_property,
      gumjs_invocation_args_set_property);
  return 1;
}

GUMJS_DEFINE_FINALIZER (gumjs_invocation_args_finalize)
{
  GumDukInvocationArgs * self;

  (void) args;

  if (_gum_duk_is_arg0_equal_to_prototype (ctx, "InvocationArgs"))
    return 0;

  self = _gum_duk_steal_data (ctx, 0);
  if (self == NULL)
    return 0;

  g_slice_free (GumDukInvocationArgs, self);

  return 0;
}

GUMJS_DEFINE_GETTER (gumjs_invocation_args_get_property)
{
  GumInvocationContext * ic;
  guint n;

  if (duk_is_string (ctx, 1) &&
      strcmp (duk_require_string (ctx, 1), "toJSON") == 0)
  {
    duk_push_string (ctx, "invocation-args");
    return 1;
  }

  ic = gumjs_invocation_args_require_context (ctx, 0);
  n = _gum_duk_require_index (ctx, 1);

  _gum_duk_push_native_pointer (ctx,
      gum_invocation_context_get_nth_argument (ic, n), args->core);
  return 1;
}

GUMJS_DEFINE_SETTER (gumjs_invocation_args_set_property)
{
  GumInvocationContext * ic;
  guint n;
  gpointer value;

  ic = gumjs_invocation_args_require_context (ctx, 0);
  n = _gum_duk_require_index (ctx, 1);
  if (!_gum_duk_get_pointer (ctx, 2, args->core, &value))
  {
    duk_push_false (ctx);
    return 1;
  }

  gum_invocation_context_replace_nth_argument (ic, n, value);

  duk_push_true (ctx);
  return 1;
}

static GumDukInvocationReturnValue *
gum_duk_invocation_return_value_new (GumDukInterceptor * parent)
{
  duk_context * ctx = parent->core->ctx;
  GumDukInvocationReturnValue * retval;
  GumDukNativePointer * ptr;

  retval = g_slice_new (GumDukInvocationReturnValue);

  ptr = &retval->parent;
  ptr->value = NULL;

  duk_push_heapptr (ctx, parent->invocation_retval);
  duk_new (ctx, 0);
  _gum_duk_put_data (ctx, -1, retval);
  retval->object = _gum_duk_require_heapptr (ctx, -1);
  duk_pop (ctx);

  retval->ic = NULL;
  retval->ctx = ctx;

  return retval;
}

static void
gum_duk_invocation_return_value_release (GumDukInvocationReturnValue * self)
{
  _gum_duk_release_heapptr (self->ctx, self->object);
}

static void
gum_duk_invocation_return_value_reset (GumDukInvocationReturnValue * self,
                                       GumInvocationContext * ic)
{
  GumDukNativePointer * ptr;

  ptr = &self->parent;
  if (ic != NULL)
    ptr->value = gum_invocation_context_get_return_value (ic);
  else
    ptr->value = NULL;

  self->ic = ic;
}

GUMJS_DEFINE_CONSTRUCTOR (gumjs_invocation_return_value_construct)
{
  (void) ctx;
  (void) args;

  return 0;
}

GUMJS_DEFINE_FINALIZER (gumjs_invocation_return_value_finalize)
{
  GumDukInvocationReturnValue * self;

  (void) args;

  if (_gum_duk_is_arg0_equal_to_prototype (ctx, "InvocationReturnValue"))
    return 0;

  self = _gum_duk_steal_data (ctx, 0);
  if (self == NULL)
    return 0;

  g_slice_free (GumDukInvocationReturnValue, self);

  return 0;
}

GUMJS_DEFINE_FUNCTION (gumjs_invocation_return_value_replace)
{
  GumDukInvocationReturnValue * self;
  GumDukNativePointer * ptr;

  duk_push_this (ctx);
  self = _gum_duk_require_data (ctx, -1);
  duk_pop (ctx);

  if (self->ic == NULL)
    _gum_duk_throw (ctx, "invalid operation");

  ptr = &self->parent;
  _gum_duk_args_parse (args, "p~", &ptr->value);

  gum_invocation_context_replace_return_value (self->ic, ptr->value);

  return 0;
}
