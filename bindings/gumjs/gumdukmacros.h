/*
 * Copyright (C) 2015 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#ifndef __GUM_DUK_MACROS_H__
#define __GUM_DUK_MACROS_H__

#include "gumdukvalue.h"

typedef struct _GumDukPropertyEntry GumDukPropertyEntry;

struct _GumDukPropertyEntry
{
  gchar * name;
  gpointer getter;
  gpointer setter;
};

#define GUMJS_RO (DUK_DEFPROP_HAVE_WRITABLE | 0)
#define GUMJS_RW (DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_WRITABLE)

#define GUMJS_DECLARE_CONSTRUCTOR(N) \
  static int N (duk_context * ctx);
#define GUMJS_DECLARE_FINALIZER(N) \
  static int N (duk_context * ctx);
#define GUMJS_DECLARE_FUNCTION(N) \
  static int N (duk_context * ctx);
#define GUMJS_DECLARE_GETTER(N) \
  static int N (duk_context * ctx);
#define GUMJS_DECLARE_SETTER(N) \
  static int N (duk_context * ctx);

#define GUMJS_DEFINE_CONSTRUCTOR(N) \
  static int N##_impl (duk_context * ctx, GumDukArgs * args); \
  \
  static int \
  N (duk_context * ctx) \
  { \
    GumDukArgs args; \
    \
    args.count = duk_get_top (ctx); \
    \
    args.ctx = ctx; \
    \
    duk_get_global_string (ctx, "\xff" "core"); \
    args.core = duk_get_pointer (ctx, -1); \
    duk_pop (ctx); \
    \
    return N##_impl (ctx, &args); \
  } \
  \
  static int \
  N##_impl (duk_context * ctx, \
            GumDukArgs * args)
#define GUMJS_DEFINE_FINALIZER(N) \
  static int N##_impl (duk_context * ctx, GumDukArgs * args); \
  \
  static int \
  N (duk_context * ctx) \
  { \
    GumDukArgs args; \
    \
    args.count = duk_get_top (ctx); \
    \
    args.ctx = ctx; \
    \
    duk_get_global_string (ctx, "\xff" "core"); \
    args.core = duk_get_pointer (ctx, -1); \
    duk_pop (ctx); \
    \
    return N##_impl (ctx, &args); \
  } \
  \
  static int \
  N##_impl (duk_context * ctx, \
            GumDukArgs * args)
#define GUMJS_DEFINE_FUNCTION(N) \
  static int N##_impl (duk_context * ctx, GumDukArgs * args); \
  \
  static int \
  N (duk_context * ctx) \
  { \
    GumDukArgs args; \
    \
    args.count = duk_get_top (ctx); \
    \
    args.ctx = ctx; \
    \
    duk_get_global_string (ctx, "\xff" "core"); \
    args.core = duk_get_pointer (ctx, -1); \
    duk_pop (ctx); \
    \
    return N##_impl (ctx, &args); \
  } \
  \
  static int \
  N##_impl (duk_context * ctx, \
            GumDukArgs * args)
#define GUMJS_DEFINE_GETTER(N) \
  static int N##_impl (duk_context * ctx, GumDukArgs * args); \
  \
  static int \
  N (duk_context * ctx) \
  { \
    GumDukArgs args; \
    \
    args.count = duk_get_top (ctx); \
    \
    args.ctx = ctx; \
    \
    duk_get_global_string (ctx, "\xff" "core"); \
    args.core = duk_get_pointer (ctx, -1); \
    duk_pop (ctx); \
    \
    return N##_impl (ctx, &args); \
  } \
  \
  static int \
  N##_impl (duk_context * ctx, \
            GumDukArgs * args)
#define GUMJS_DEFINE_SETTER(N) \
  static int N##_impl (duk_context * ctx, GumDukArgs * args); \
  \
  static int \
  N (duk_context * ctx) \
  { \
    GumDukArgs args; \
    \
    args.count = duk_get_top (ctx); \
    \
    args.ctx = ctx; \
    \
    duk_get_global_string (ctx, "\xff" "core"); \
    args.core = duk_get_pointer (ctx, -1); \
    duk_pop (ctx); \
    \
    return N##_impl (ctx, &args); \
  } \
  \
  static int \
  N##_impl (duk_context * ctx, \
            GumDukArgs * args)

#define GUMJS_ADD_GLOBAL_FUNCTION(N, F, NARGS) \
  duk_push_c_function (ctx, F, NARGS); \
  duk_put_global_string (ctx, N);

void
inline _gumjs_duk_create_subclass (duk_context * ctx, gchar * parent, gchar * name,
    gpointer constructor, gpointer finalize)
{
    duk_push_global_object (ctx);
	// [ ... global ]
	duk_get_prop_string (ctx, -1, "Object");
	// [ ... global object ]
	duk_get_prop_string (ctx, -1, "create");
	// [ ... global object create ]

	duk_get_prop_string (ctx, -3, parent);
	// [ ... global object create parent ]
	duk_get_prop_string (ctx, -1, "prototype");
	// [ ... global object create parent parentproto ]
	duk_dup (ctx, -3);
	// [ ... global object create parent parentproto create]
	duk_dup (ctx, -2);
	// [ ... global object create parent parentproto create parentproto]
	duk_call (ctx, 1);
	// [ ... global object create parent parentproto childproto]

    if (constructor)
      duk_push_c_function (ctx, constructor, 1);
    else
      duk_push_object (ctx);
	// [ ... global object create parent parentproto childproto constructor]
	duk_dup (ctx, -2);
	// [ ... global object create parent parentproto childproto constructor childproto]
    if (finalize)
    {
      duk_push_c_function (ctx, finalize, 1);
      // [ ... global object create parent parentproto childproto constructor childproto finalize]
      duk_set_finalizer (ctx, -2);
      // [ ... global object create parent parentproto childproto constructor childproto]
    }
	duk_put_prop_string (ctx, -2, "prototype");
	// [ ... global object create parent parentproto childproto constructor]
	duk_put_prop_string (ctx, -7, name);
	// [ ... global object create parent parentproto childproto]
    duk_pop_n (ctx, 6);
}


void
inline _gumjs_duk_add_properties_to_class (duk_context * ctx, gchar * classname,
    const GumDukPropertyEntry * entries)
{
  const GumDukPropertyEntry * entry;
  duk_push_global_object (ctx);
  // [ global ]
  duk_get_prop_string (ctx, -1, classname);
  // [ global class ]

  for (entry = entries; entry->name != NULL; entry++)
  {
    int idx = 1;
    int flags = DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE;
    duk_push_string (ctx, entry->name);
    idx++;
    // [ global class propname ]
    if (entry->getter != NULL)
    {
      idx++;
      flags |= DUK_DEFPROP_HAVE_GETTER;
      duk_push_c_function (ctx, entry->getter, 0);
      // [ global class propname getter ]
    }
    if (entry->setter != NULL)
    {
      idx++;
      flags |= DUK_DEFPROP_HAVE_SETTER;
      duk_push_c_function (ctx, entry->setter, 1);
      // [ global class propname {getter} setter ]
    }

    duk_def_prop (ctx, -idx, flags);
    // [ global class ]
  }

  duk_pop_2 (ctx);
}

gboolean
inline _gumjs_is_arg0_equal_to_prototype (duk_context * ctx, gchar * classname)
{
  gboolean result;
  duk_get_global_string (ctx, classname);
  // [ arg0 ... class ]
  duk_get_prop_string (ctx, -1, "prototype");
  // [ arg0 ... class proto ]
  result = duk_equals (ctx, 0, -1);
  duk_pop_2 (ctx);
  return result;
}

GumDukHeapPtr
inline _gumjs_duk_get_this (duk_context * ctx)
{
  GumDukHeapPtr result;
  duk_push_this (ctx);
  result = duk_require_heapptr (ctx, -1);
  duk_pop (ctx);
  return result;
}

GumDukHeapPtr
inline _gumjs_duk_create_proxy_accessors (duk_context * ctx, gpointer getter,
    gpointer setter)
{
  GumDukHeapPtr result;
  duk_push_object (ctx);
  // [ targetobj ]
  duk_push_object (ctx);
  // [ targetobj handlerobj ]
  if (getter)
  {
    duk_push_c_function (ctx, getter, 3);
    // [ targetobj handlerobj getter ]
    duk_put_prop_string (ctx, -2, "get");
    // [ targetobj handlerobj ]
  }
  if (setter)
  {
    duk_push_c_function (ctx, setter, 4);
    // [ targetobj handlerobj setter ]
    duk_put_prop_string (ctx, -2, "set");
    // [ targetobj handlerobj ]
  }
  duk_get_global_string (ctx, "Proxy");
  // [ targetobj handlerobj Proxy ]
  duk_insert (ctx, -3);
  // [ Proxy targetobj handlerobj ]
  duk_new (ctx, 2);
  // [ proxyinst ]
  result = duk_require_heapptr (ctx, -1);
  duk_pop (ctx);
  return result;
}

void
inline _gumjs_duk_protect (duk_context * ctx, GumDukHeapPtr object)
{
  gchar name[256];
  sprintf (name, "\xff" "protected_%p", object);

  duk_push_global_stash (ctx);
  duk_get_prop_string (ctx, -2, name);
  if (duk_is_undefined (ctx, -1))
  {
    duk_pop (ctx);
    duk_push_heapptr (ctx, object);
    duk_put_prop_string (ctx, -2, name);
  }
  else
    duk_pop (ctx);
  duk_pop (ctx);
}

void
inline _gumjs_duk_unprotect (duk_context * ctx, GumDukHeapPtr object)
{
  gchar name[256];
  sprintf (name, "\xff" "protected_%p", object);

  duk_push_global_stash (ctx);
  duk_get_prop_string (ctx, -2, name);
  if (duk_is_undefined (ctx, -1))
  {
    duk_pop (ctx);
  }
  else
  {
    duk_pop (ctx);
    duk_del_prop_string (ctx, -1, name);
  }
  duk_pop (ctx);
}
#endif
