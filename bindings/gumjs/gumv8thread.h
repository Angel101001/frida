/*
 * Copyright (C) 2010-2016 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#ifndef __GUM_V8_THREAD_H__
#define __GUM_V8_THREAD_H__

#include "gumv8core.h"

#include <gum/gumbacktracer.h>
#include <v8.h>

struct GumV8Thread
{
  GumV8Core * core;

  GumBacktracer * accurate_backtracer;
  GumBacktracer * fuzzy_backtracer;

  GumPersistent<v8::Symbol>::type * accurate_enum_value;
  GumPersistent<v8::Symbol>::type * fuzzy_enum_value;
};

G_GNUC_INTERNAL void _gum_v8_thread_init (GumV8Thread * self,
    GumV8Core * core, v8::Handle<v8::ObjectTemplate> scope);
G_GNUC_INTERNAL void _gum_v8_thread_realize (GumV8Thread * self);
G_GNUC_INTERNAL void _gum_v8_thread_dispose (GumV8Thread * self);
G_GNUC_INTERNAL void _gum_v8_thread_finalize (GumV8Thread * self);

#endif
