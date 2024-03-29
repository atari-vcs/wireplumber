/* WirePlumber
 *
 * Copyright © 2020 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <wp/wp.h>
#include <wplua/wplua.h>

static gboolean
load_components (lua_State *L, WpCore * core, GError ** error)
{
  lua_getglobal (L, "SANDBOX_COMMON_ENV");

  switch (lua_getfield (L, -1, "components")) {
  case LUA_TTABLE:
    break;
  case LUA_TNIL:
    wp_debug ("no components specified");
    goto done;
  default:
    g_set_error (error, WP_DOMAIN_LIBRARY, WP_LIBRARY_ERROR_INVALID_ARGUMENT,
        "Expected 'components' to be a table");
    return FALSE;
  }

  lua_pushnil (L);
  while (lua_next (L, -2)) {
    /* value must be a table */
    if (lua_type (L, -1) != LUA_TTABLE) {
      g_set_error (error, WP_DOMAIN_LIBRARY, WP_LIBRARY_ERROR_INVALID_ARGUMENT,
          "'components' must be a table with tables as values");
      return FALSE;
    }

    /* record indexes to the current key and value of the components table */
    int key = lua_absindex (L, -2);
    int table = lua_absindex (L, -1);

    /* get component */
    if (lua_geti (L, table, 1) != LUA_TSTRING) {
      g_set_error (error, WP_DOMAIN_LIBRARY, WP_LIBRARY_ERROR_INVALID_ARGUMENT,
          "components['%s'] has a non-string or unspecified component name",
          lua_tostring (L, key));
      return FALSE;
    }
    const char * component = lua_tostring (L, -1);

    /* get component type */
    if (lua_getfield (L, table, "type") != LUA_TSTRING) {
      g_set_error (error, WP_DOMAIN_LIBRARY, WP_LIBRARY_ERROR_INVALID_ARGUMENT,
          "components['%s'] has a non-string or unspecified component type",
          lua_tostring (L, key));
      return FALSE;
    }
    const char * type = lua_tostring (L, -1);

    /* optional component arguments */
    GVariant *args = NULL;
    if (lua_getfield (L, table, "args") == LUA_TTABLE) {
      args = wplua_lua_to_gvariant (L, -1);
    }

    wp_debug ("load component: %s (%s)", component, type);

    if (!wp_core_load_component (core, component, type, args, error))
      return FALSE;

    /* clear the stack up to the key */
    lua_settop (L, key);
  }

done:
  lua_pop (L, 2); /* pop components & SANDBOX_COMMON_ENV */
  return TRUE;
}

static gint
sort_filelist (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(const gchar **) a, *(const gchar **) b);
}

gboolean
wp_lua_scripting_load_configuration (const gchar * conf_file,
    WpCore * core, GError ** error)
{
  g_autofree gchar * path = NULL;
  g_autoptr (lua_State) L = wplua_new ();
  gboolean found = FALSE;

  wplua_enable_sandbox (L, WP_LUA_SANDBOX_MINIMAL_STD);

  /* load conf_file itself */
  path = g_build_filename (wp_get_config_dir (), conf_file, NULL);
  if (g_file_test (path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)) {
    wp_info ("loading config file: %s", path);
    if (!wplua_load_path (L, path, 0, 0, error))
      return FALSE;
    found = TRUE;
  }
  g_clear_pointer (&path, g_free);

  /* aggregate split files from the ${conf_file}.d subdirectory */
  path = g_strdup_printf ("%s" G_DIR_SEPARATOR_S "%s.d",
      wp_get_config_dir (), conf_file);
  if (g_file_test (path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR)) {
    g_autoptr (GDir) conf_dir = g_dir_open (path, 0, error);
    if (!conf_dir)
      return FALSE;

    /* sort files before loading them */
    g_autoptr (GPtrArray) filenames = g_ptr_array_new ();
    const gchar *filename = NULL;
    while ((filename = g_dir_read_name (conf_dir))) {
      /* Only parse files that have the proper extension */
      if (g_str_has_suffix (filename, ".lua")) {
        g_ptr_array_add (filenames, (gpointer) filename);
      }
    }
    g_ptr_array_sort (filenames, sort_filelist);

    /* load */
    for (guint i = 0; i < filenames->len; i++) {
      g_autofree gchar * file = g_build_filename (path,
          g_ptr_array_index (filenames, i), NULL);
      wp_info ("loading config file: %s", file);
      if (!wplua_load_path (L, file, 0, 0, error))
        return FALSE;
      found = TRUE;
    }
  }

  if (!found) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "Could not locate configuration file '%s'", conf_file);
    return FALSE;
  }

  if (!load_components (L, core, error))
    return FALSE;

  return TRUE;
}
