/*
 * Copyright (C) 2010-2011 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
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

#include "script-fixture.c"

TEST_LIST_BEGIN (script)
  SCRIPT_TESTENTRY (invalid_script_should_return_null)
  SCRIPT_TESTENTRY (int_can_be_sent)
  SCRIPT_TESTENTRY (string_can_be_sent)
  SCRIPT_TESTENTRY (json_can_be_sent)
  SCRIPT_TESTENTRY (runtime_error_is_caught_and_delivered_as_message)
  SCRIPT_TESTENTRY (can_read_int32_argument)
  SCRIPT_TESTENTRY (can_read_utf8_string_argument)
  SCRIPT_TESTENTRY (can_read_utf16_string_argument)
TEST_LIST_END ()

typedef struct _TwoIntegersArgs TwoIntegersArgs;
typedef struct _Utf8StringAndIntegerArgs Utf8StringAndIntegerArgs;
typedef struct _Utf16StringAndIntegerArgs Utf16StringAndIntegerArgs;

struct _TwoIntegersArgs {
  gint a;
  gint b;
};

struct _Utf8StringAndIntegerArgs {
  const gchar * str;
  gint i;
};

struct _Utf16StringAndIntegerArgs {
  const gunichar2 * str;
  gint i;
};

static void store_message (GumScript * script, const gchar * msg,
    gpointer user_data);

SCRIPT_TESTCASE (invalid_script_should_return_null)
{
  GError * err = NULL;

  g_assert (gum_script_from_string ("'", NULL) == NULL);

  g_assert (gum_script_from_string ("'", &err) == NULL);
  g_assert (err != NULL);
  g_assert_cmpstr (err->message, ==,
      "Script(line 1): SyntaxError: Unexpected token ILLEGAL");
}

SCRIPT_TESTCASE (int_can_be_sent)
{
  GumScript * script;
  GError * err = NULL;
  gchar * msg = NULL;

  script = gum_script_from_string ("send(1337);", &err);
  g_assert (script != NULL);
  g_assert (err == NULL);

  gum_script_set_message_handler (script, store_message, &msg, NULL);
  gum_script_execute (script, &fixture->invocation_context);
  g_assert (msg != NULL);
  g_assert_cmpstr (msg, ==,
      "{\"type\":\"send\",\"payload\":1337}");
  g_free (msg);

  g_object_unref (script);
}

SCRIPT_TESTCASE (string_can_be_sent)
{
  GumScript * script;
  gchar * msg = NULL;

  script = gum_script_from_string ("send('hey');", NULL);
  gum_script_set_message_handler (script, store_message, &msg, NULL);
  gum_script_execute (script, &fixture->invocation_context);
  g_assert_cmpstr (msg, ==, "{\"type\":\"send\",\"payload\":\"hey\"}");
  g_free (msg);
  g_object_unref (script);
}

SCRIPT_TESTCASE (json_can_be_sent)
{
  GumScript * script;
  gchar * msg = NULL;

  script = gum_script_from_string ("send({ 'color': 'red' });", NULL);
  gum_script_set_message_handler (script, store_message, &msg, NULL);
  gum_script_execute (script, &fixture->invocation_context);
  g_assert_cmpstr (msg, ==,
      "{\"type\":\"send\",\"payload\":{\"color\":\"red\"}}");
  g_free (msg);
  g_object_unref (script);
}

SCRIPT_TESTCASE (runtime_error_is_caught_and_delivered_as_message)
{
  GumScript * script;
  gchar * msg = NULL;

  script = gum_script_from_string ("badger();", NULL);
  gum_script_set_message_handler (script, store_message, &msg, NULL);
  gum_script_execute (script, &fixture->invocation_context);
  g_assert_cmpstr (msg, ==, "{"
      "\"type\":\"error\","
      "\"lineNumber\":1,"
      "\"description\":\"ReferenceError: badger is not defined\""
  "}");
  g_free (msg);
  g_object_unref (script);
}

SCRIPT_TESTCASE (can_read_int32_argument)
{
  GumScript * script;
  TwoIntegersArgs args;
  gchar * msg = NULL;

  script = gum_script_from_string ("send(arg[1]);", NULL);
  args.a = 1227;
  args.b = 1337;
  fixture->argument_list = &args;
  gum_script_set_message_handler (script, store_message, &msg, NULL);
  gum_script_execute (script, &fixture->invocation_context);
  g_assert_cmpstr (msg, ==, "{"
      "\"type\":\"send\","
      "\"payload\":1337"
  "}");
  g_free (msg);
  g_object_unref (script);
}

SCRIPT_TESTCASE (can_read_utf8_string_argument)
{
  GumScript * script;
  Utf8StringAndIntegerArgs args;
  gchar * msg = NULL;

  script = gum_script_from_string ("send(Memory.readUtf8String(arg[0]));", NULL);
  args.str = "Bjørheimsbygd";
  args.i = 42;
  fixture->argument_list = &args;
  gum_script_set_message_handler (script, store_message, &msg, NULL);
  gum_script_execute (script, &fixture->invocation_context);
  g_assert_cmpstr (msg, ==, "{"
      "\"type\":\"send\","
      "\"payload\":\"Bjørheimsbygd\""
  "}");
  g_free (msg);
  g_object_unref (script);
}

SCRIPT_TESTCASE (can_read_utf16_string_argument)
{
  GumScript * script;
  Utf16StringAndIntegerArgs args;
  gchar * msg = NULL;

  script = gum_script_from_string ("send(Memory.readUtf16String(arg[0]));", NULL);
  args.str = g_utf8_to_utf16 ("Bjørheimsbygd", -1, NULL, NULL, NULL);
  args.i = 42;
  fixture->argument_list = &args;
  gum_script_set_message_handler (script, store_message, &msg, NULL);
  gum_script_execute (script, &fixture->invocation_context);
  g_free ((gpointer) args.str);
  g_assert_cmpstr (msg, ==, "{"
      "\"type\":\"send\","
      "\"payload\":\"Bjørheimsbygd\""
  "}");
  g_free (msg);
  g_object_unref (script);
}

static void
store_message (GumScript * script,
               const gchar * msg,
               gpointer user_data)
{
  gchar ** testcase_msg_ptr = (gchar **) user_data;

  g_assert (*testcase_msg_ptr == NULL);
  *testcase_msg_ptr = g_strdup (msg);
}