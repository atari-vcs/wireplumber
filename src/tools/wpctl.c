/* WirePlumber
 *
 * Copyright © 2019-2020 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <wp/wp.h>
#include <stdio.h>
#include <pipewire/keys.h>
#include <pipewire/extensions/session-manager/keys.h>

typedef struct _WpCtl WpCtl;
struct _WpCtl
{
  GOptionContext *context;
  GMainLoop *loop;
  WpCore *core;
  GPtrArray *apis;
  WpObjectManager *om;
  guint pending_plugins;
  gint exit_code;
};

static struct {
  union {
    struct {
      guint32 id;
      gboolean show_referenced;
      gboolean show_associated;
    } inspect;
    struct {
      guint32 id;
    } set_default;
    struct {
      guint32 id;
      gfloat volume;
    } set_volume;

    struct {
      guint32 id;
      guint mute;
    } set_mute;

    struct {
      guint32 id;
      gint index;
    } set_profile;
  };
} cmdline;

G_DEFINE_QUARK (wpctl-error, wpctl_error_domain)

static void
wp_ctl_clear (WpCtl * self)
{
  g_clear_pointer (&self->apis, g_ptr_array_unref);
  g_clear_object (&self->om);
  g_clear_object (&self->core);
  g_clear_pointer (&self->loop, g_main_loop_unref);
  g_clear_pointer (&self->context, g_option_context_free);
}

static void
async_quit (WpCore *core, GAsyncResult *res, WpCtl * self)
{
  g_main_loop_quit (self->loop);
}

/* status */

static gboolean
status_prepare (WpCtl * self, GError ** error)
{
  wp_object_manager_add_interest (self->om, WP_TYPE_CLIENT, NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_DEVICE, NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_ENDPOINT, NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_NODE, NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_PORT, NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_LINK, NULL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_GLOBAL_PROXY,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  return TRUE;
}

#define TREE_INDENT_LINE " │  "
#define TREE_INDENT_NODE " ├─ "
#define TREE_INDENT_END  " └─ "
#define TREE_INDENT_EMPTY "    "

struct print_context
{
  WpCtl *self;
  guint32 default_node;
  WpPlugin *mixer_api;
};

static void
print_controls (guint32 id, struct print_context *context)
{
  g_autoptr (GVariant) dict = NULL;

  if (context->mixer_api)
    g_signal_emit_by_name (context->mixer_api, "get-volume", id, &dict);

  if (dict) {
    gboolean mute = FALSE;
    gdouble volume = 1.0;
    if (g_variant_lookup (dict, "mute", "b", &mute) &&
        g_variant_lookup (dict, "volume", "d", &volume))
      printf (" [vol: %.2f%s", volume, mute ? " MUTED]" : "]");
  }
  printf ("\n");
}

static void
print_device (const GValue *item, gpointer data)
{
  WpPipewireObject *obj = g_value_get_object (item);
  guint32 id = wp_proxy_get_bound_id (WP_PROXY (obj));
  const gchar *api = wp_pipewire_object_get_property (obj, PW_KEY_DEVICE_API);
  const gchar *name = wp_pipewire_object_get_property (obj, PW_KEY_DEVICE_DESCRIPTION);
  if (!name)
    name = wp_pipewire_object_get_property (obj, PW_KEY_DEVICE_NAME);

  printf (TREE_INDENT_LINE "  %4u. %-35s [%s]\n", id, name, api);
}

static void
print_dev_node (const GValue *item, gpointer data)
{
  WpPipewireObject *obj = g_value_get_object (item);
  struct print_context *context = data;
  guint32 id = wp_proxy_get_bound_id (WP_PROXY (obj));
  gboolean is_default = (context->default_node == id);
  const gchar *name =
      wp_pipewire_object_get_property (obj, PW_KEY_NODE_DESCRIPTION);
  if (!name)
    name = wp_pipewire_object_get_property (obj, PW_KEY_APP_NAME);
  if (!name)
    name = wp_pipewire_object_get_property (obj, PW_KEY_NODE_NAME);

  printf (TREE_INDENT_LINE "%c %4u. %-35s", is_default ? '*' : ' ', id, name);
  print_controls (id, context);
}

static void
print_endpoint (const GValue *item, gpointer data)
{
  WpPipewireObject *obj = g_value_get_object (item);
  struct print_context *context = data;
  guint32 id = wp_proxy_get_bound_id (WP_PROXY (obj));
  guint32 node_id = -1;
  gboolean is_default = (context->default_node == id);
  const gchar *str, *name;

  if ((str = wp_pipewire_object_get_property (obj, "node.id")))
    node_id = atoi (str);

  name = wp_pipewire_object_get_property (obj, "endpoint.description");
  if (!name)
    name = wp_pipewire_object_get_property (obj, "endpoint.name");

  printf (TREE_INDENT_LINE "%c %4u. %-35s", is_default ? '*' : ' ', id, name);
  print_controls (node_id, context);
}

static void
print_stream_node (const GValue *item, gpointer data)
{
  WpCtl * self = data;
  WpPipewireObject *obj = g_value_get_object (item);
  guint32 id = wp_proxy_get_bound_id (WP_PROXY (obj));
  const gchar *name = wp_pipewire_object_get_property (obj, PW_KEY_APP_NAME);
  if (!name)
    name = wp_pipewire_object_get_property (obj, PW_KEY_NODE_NAME);

  printf (TREE_INDENT_EMPTY "  %4u. %-60s\n", id, name);

  g_autoptr (WpIterator) it = wp_object_manager_new_filtered_iterator (self->om,
      WP_TYPE_PORT, WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_NODE_ID, "=u", id,
      NULL);
  g_auto (GValue) val = G_VALUE_INIT;

  for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
    WpPort *port = g_value_get_object (&val);
    obj = WP_PIPEWIRE_OBJECT (port);
    id = wp_proxy_get_bound_id (WP_PROXY (obj));
    name = wp_pipewire_object_get_property (obj, PW_KEY_PORT_NAME);
    WpDirection dir = wp_port_get_direction (port);

    printf (TREE_INDENT_EMPTY "       %4u. %-15s", id, name);

    g_autoptr (WpLink) link = wp_object_manager_lookup (self->om, WP_TYPE_LINK,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, (dir == WP_DIRECTION_OUTPUT) ?
            PW_KEY_LINK_OUTPUT_PORT : PW_KEY_LINK_INPUT_PORT, "=u", id,
        NULL);
    if (link) {
      guint32 peer_id = -1;
      wp_link_get_linked_object_ids(link,
          NULL, (dir == WP_DIRECTION_INPUT) ? &peer_id : NULL,
          NULL, (dir == WP_DIRECTION_OUTPUT) ? &peer_id : NULL);
      g_autoptr (WpPipewireObject) peer = wp_object_manager_lookup (
          self->om, WP_TYPE_PORT,
          WP_CONSTRAINT_TYPE_G_PROPERTY, "bound-id", "=u", peer_id,
          NULL);
      name = wp_pipewire_object_get_property (peer, PW_KEY_PORT_ALIAS);

      printf (" %c %s\n", (dir == WP_DIRECTION_OUTPUT) ? '>' : '<', name);
    } else {
      printf ("\n");
    }
  }
}

static void
status_run (WpCtl * self)
{
  g_autoptr (WpIterator) it = NULL;
  g_auto (GValue) val = G_VALUE_INIT;
  g_autoptr (WpPlugin) def_nodes_api = NULL;
  struct print_context context = { .self = self };

  def_nodes_api = wp_plugin_find (self->core, "default-nodes-api");
  context.mixer_api = wp_plugin_find (self->core, "mixer-api");

  /* server + clients */
  printf ("PipeWire '%s' [%s, %s@%s, cookie:%u]\n",
      wp_core_get_remote_name (self->core),
      wp_core_get_remote_version (self->core),
      wp_core_get_remote_user_name (self->core),
      wp_core_get_remote_host_name (self->core),
      wp_core_get_remote_cookie (self->core));

  printf (TREE_INDENT_END "Clients:\n");
  it = wp_object_manager_new_filtered_iterator (self->om, WP_TYPE_CLIENT, NULL);
  for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
    WpProxy *client = g_value_get_object (&val);
    g_autoptr (WpProperties) properties =
        wp_pipewire_object_get_properties (WP_PIPEWIRE_OBJECT (client));

    printf (TREE_INDENT_EMPTY "  %4u. %-35s [%s, %s@%s, pid:%s]\n",
        wp_proxy_get_bound_id (client),
        wp_properties_get (properties, PW_KEY_APP_NAME),
        wp_properties_get (properties, PW_KEY_CORE_VERSION),
        wp_properties_get (properties, PW_KEY_APP_PROCESS_USER),
        wp_properties_get (properties, PW_KEY_APP_PROCESS_HOST),
        wp_properties_get (properties, PW_KEY_APP_PROCESS_ID));
  }
  g_clear_pointer (&it, wp_iterator_unref);
  printf ("\n");

  /* sessions */
  const gchar *MEDIA_TYPES[] = { "Audio", "Video" };

  for (guint i = 0; i < G_N_ELEMENTS (MEDIA_TYPES); i++) {
    const gchar *media_type = MEDIA_TYPES[i];
    g_autoptr (WpIterator) child_it = NULL;

    printf ("%s\n", media_type);

    if (media_type && *media_type != '\0') {
      gchar media_type_glob[16];
      gchar media_class[24];

      g_snprintf (media_type_glob, sizeof(media_type_glob), "*%s*", media_type);

      printf (TREE_INDENT_NODE "Devices:\n");
      child_it = wp_object_manager_new_filtered_iterator (self->om,
          WP_TYPE_DEVICE,
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", media_type_glob,
          NULL);
      wp_iterator_foreach (child_it, print_device, self);
      g_clear_pointer (&child_it, wp_iterator_unref);

      printf (TREE_INDENT_LINE "\n");

      printf (TREE_INDENT_NODE "Sinks:\n");
      g_snprintf (media_class, sizeof(media_class), "%s/Sink", media_type);
      context.default_node = -1;
      if (def_nodes_api)
        g_signal_emit_by_name (def_nodes_api, "get-default-node", media_class,
            &context.default_node);
      child_it = wp_object_manager_new_filtered_iterator (self->om,
          WP_TYPE_NODE,
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", "*/Sink*",
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", media_type_glob,
          NULL);
      wp_iterator_foreach (child_it, print_dev_node, (gpointer) &context);
      g_clear_pointer (&child_it, wp_iterator_unref);

      printf (TREE_INDENT_LINE "\n");

      printf (TREE_INDENT_NODE "Sink endpoints:\n");
      child_it = wp_object_manager_new_filtered_iterator (self->om,
          WP_TYPE_ENDPOINT,
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", "*/Sink*",
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", media_type_glob,
          NULL);
      wp_iterator_foreach (child_it, print_endpoint, (gpointer) &context);
      g_clear_pointer (&child_it, wp_iterator_unref);

      printf (TREE_INDENT_LINE "\n");

      printf (TREE_INDENT_NODE "Sources:\n");
      g_snprintf (media_class, sizeof(media_class), "%s/Source", media_type);
      context.default_node = -1;
      if (def_nodes_api)
        g_signal_emit_by_name (def_nodes_api, "get-default-node", media_class,
            &context.default_node);
      child_it = wp_object_manager_new_filtered_iterator (self->om,
          WP_TYPE_NODE,
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", "*/Source*",
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", media_type_glob,
          NULL);
      wp_iterator_foreach (child_it, print_dev_node, (gpointer) &context);
      g_clear_pointer (&child_it, wp_iterator_unref);

      printf (TREE_INDENT_LINE "\n");

      printf (TREE_INDENT_NODE "Source endpoints:\n");
      child_it = wp_object_manager_new_filtered_iterator (self->om,
          WP_TYPE_ENDPOINT,
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", "*/Source*",
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", media_type_glob,
          NULL);
      wp_iterator_foreach (child_it, print_endpoint, (gpointer) &context);
      g_clear_pointer (&child_it, wp_iterator_unref);

      printf (TREE_INDENT_LINE "\n");

      printf (TREE_INDENT_END "Streams:\n");
      child_it = wp_object_manager_new_filtered_iterator (self->om,
          WP_TYPE_NODE,
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", "Stream/*",
          WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", media_type_glob,
          NULL);
      wp_iterator_foreach (child_it, print_stream_node, self);
      g_clear_pointer (&child_it, wp_iterator_unref);
    }

    printf ("\n");
  }

  g_clear_object (&context.mixer_api);
  g_main_loop_quit (self->loop);
}

/* inspect */

static gboolean
inspect_parse_positional (gint argc, gchar ** argv, GError **error)
{
  if (argc < 3) {
    g_set_error (error, wpctl_error_domain_quark(), 0, "ID is required");
    return FALSE;
  }

  long id = strtol (argv[2], NULL, 10);
  if (id <= 0) {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "'%s' is not a valid number", argv[2]);
    return FALSE;
  }

  cmdline.inspect.id = id;
  return TRUE;
}

static gboolean
inspect_prepare (WpCtl * self, GError ** error)
{
  /* collect all objects */
  wp_object_manager_add_interest (self->om, WP_TYPE_GLOBAL_PROXY, NULL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_GLOBAL_PROXY,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  return TRUE;
}

static inline void
inspect_prefix_line (guint nest_level, gboolean node)
{
  for (guint i = 1; i < nest_level; i++)
    printf (TREE_INDENT_EMPTY TREE_INDENT_LINE);
  if (nest_level > 0)
    printf (TREE_INDENT_EMPTY "%s", node ? TREE_INDENT_NODE : TREE_INDENT_LINE);
}

struct {
  const gchar *key;
  const gchar *type;
} assoc_keys[] = {
  { PW_KEY_CLIENT_ID, "Client" },
  { PW_KEY_DEVICE_ID, "Device" },
  { PW_KEY_ENDPOINT_CLIENT_ID, NULL },
  { "endpoint-link.id", "EndpointLink" },
  { PW_KEY_ENDPOINT_STREAM_ID, "EndpointStream" },
  { PW_KEY_ENDPOINT_LINK_OUTPUT_ENDPOINT, NULL },
  { PW_KEY_ENDPOINT_LINK_OUTPUT_STREAM, NULL },
  { PW_KEY_ENDPOINT_LINK_INPUT_ENDPOINT, NULL },
  { PW_KEY_ENDPOINT_LINK_INPUT_STREAM, NULL },
  { PW_KEY_ENDPOINT_ID, "Endpoint" },
  { PW_KEY_LINK_INPUT_NODE, NULL },
  { PW_KEY_LINK_INPUT_PORT, NULL },
  { PW_KEY_LINK_OUTPUT_NODE, NULL },
  { PW_KEY_LINK_OUTPUT_PORT, NULL },
  { PW_KEY_LINK_ID, "Link" },
  { PW_KEY_NODE_ID, "Node" },
  { PW_KEY_PORT_ID, "Port" },
  { PW_KEY_SESSION_ID, "Session" },
};

static inline gboolean
key_is_object_reference (const gchar *key)
{
  for (guint i = 0; i < G_N_ELEMENTS (assoc_keys); i++)
    if (!g_strcmp0 (key, assoc_keys[i].key))
      return TRUE;
  return FALSE;
}

static inline const gchar *
get_association_key (WpProxy * proxy)
{
  for (guint i = 0; i < G_N_ELEMENTS (assoc_keys); i++) {
    if (assoc_keys[i].type &&
        strstr (WP_PROXY_GET_CLASS (proxy)->pw_iface_type, assoc_keys[i].type))
      return assoc_keys[i].key;
  }
  return NULL;
}

struct property_item {
  const gchar *key;
  const gchar *value;
};

static gint
property_item_compare (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (
      ((struct property_item *) a)->key,
      ((struct property_item *) b)->key);
}

static void
inspect_print_object (WpCtl * self, WpProxy * proxy, guint nest_level)
{
  g_autoptr (WpProperties) properties =
      wp_pipewire_object_get_properties (WP_PIPEWIRE_OBJECT (proxy));
  g_autoptr (WpProperties) global_p =
      wp_global_proxy_get_global_properties (WP_GLOBAL_PROXY (proxy));
  g_autoptr (GArray) array =
      g_array_new (FALSE, FALSE, sizeof (struct property_item));

  /* print basic object info */
  inspect_prefix_line (nest_level, TRUE);
  printf ("id %u, type %s\n",
      wp_proxy_get_bound_id (proxy),
      WP_PROXY_GET_CLASS (proxy)->pw_iface_type);

  /* merge the two property sets */
  properties = wp_properties_ensure_unique_owner (properties);
  wp_properties_add (properties, global_p);
  wp_properties_set (properties, "object.id", NULL);

  /* copy key/value pointers to an array for sorting */
  {
    g_autoptr (WpIterator) it = NULL;
    g_auto (GValue) item = G_VALUE_INIT;

    for (it = wp_properties_new_iterator (properties);
          wp_iterator_next (it, &item);
          g_value_unset (&item)) {
      struct property_item prop_item = {
        .key = wp_properties_iterator_item_get_key (&item),
        .value = wp_properties_iterator_item_get_value (&item),
      };
      g_array_append_val (array, prop_item);
    }
  }

  /* sort */
  g_array_sort (array, property_item_compare);

  /* print */
  for (guint i = 0; i < array->len; i++) {
    struct property_item *prop_item =
        &g_array_index (array, struct property_item, i);
    gboolean is_global =
        (wp_properties_get (global_p, prop_item->key) != NULL);

    inspect_prefix_line (nest_level, FALSE);
    printf ("  %c %s = \"%s\"\n", is_global ? '*' : ' ',
        prop_item->key, prop_item->value);

    /* if the property is referencing an object, print the object */
    if (cmdline.inspect.show_referenced && nest_level == 0 &&
        key_is_object_reference (prop_item->key))
    {
      guint id = (guint) strtol (prop_item->value, NULL, 10);
      g_autoptr (WpProxy) refer_proxy =
          wp_object_manager_lookup (self->om, WP_TYPE_GLOBAL_PROXY,
              WP_CONSTRAINT_TYPE_G_PROPERTY, "bound-id", "=u", id, NULL);

      if (refer_proxy)
        inspect_print_object (self, refer_proxy, nest_level + 1);
    }
  }

  /* print associated objects */
  if (cmdline.inspect.show_associated && nest_level == 0) {
    const gchar *lookup_key = get_association_key (proxy);
    if (lookup_key) {
      g_autoptr (WpIterator) it =
          wp_object_manager_new_filtered_iterator (self->om,
              WP_TYPE_PIPEWIRE_OBJECT, WP_CONSTRAINT_TYPE_PW_PROPERTY,
              lookup_key, "=u", wp_proxy_get_bound_id (proxy), NULL);
      g_auto (GValue) item = G_VALUE_INIT;

      inspect_prefix_line (nest_level, TRUE);
      printf ("associated objects:\n");

      for (; wp_iterator_next (it, &item); g_value_unset (&item)) {
        WpProxy *assoc_proxy = g_value_get_object (&item);
        inspect_print_object (self, assoc_proxy, nest_level + 1);
      }
    }
  }
}

static void
inspect_run (WpCtl * self)
{
  g_autoptr (WpProxy) proxy = NULL;
  guint32 id = cmdline.inspect.id;

  proxy = wp_object_manager_lookup (self->om, WP_TYPE_GLOBAL_PROXY,
      WP_CONSTRAINT_TYPE_G_PROPERTY, "bound-id", "=u", id, NULL);
  if (!proxy) {
    printf ("Object '%d' not found\n", id);
    goto out_err;
  }

  inspect_print_object (self, proxy, 0);

out:
  g_main_loop_quit (self->loop);
  return;

out_err:
  self->exit_code = 3;
  goto out;
}

/* set-default */
static gboolean
set_default_parse_positional (gint argc, gchar ** argv, GError **error)
{
  if (argc < 3) {
    g_set_error (error, wpctl_error_domain_quark(), 0, "ID is required");
    return FALSE;
  }

  long id = strtol (argv[2], NULL, 10);
  if (id <= 0) {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "'%s' is not a valid number", argv[2]);
    return FALSE;
  }

  cmdline.set_default.id = id;
  return TRUE;
}

static gboolean
set_default_prepare (WpCtl * self, GError ** error)
{
  wp_object_manager_add_interest (self->om, WP_TYPE_METADATA, NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_NODE,
      WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
      "object.id", "=u", cmdline.set_default.id,
      NULL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_METADATA,
      WP_OBJECT_FEATURES_ALL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_NODE,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  return TRUE;
}

static void
set_default_run (WpCtl * self)
{
  g_autoptr (WpMetadata) metadata = NULL;
  g_autoptr (WpProxy) proxy = NULL;
  guint32 id = cmdline.set_default.id;
  const gchar *media_class;
  const gchar *key;
  const gchar *name;
  gchar buf[1024];

  metadata = wp_object_manager_lookup (self->om, WP_TYPE_METADATA, NULL);
  if (!metadata) {
    printf ("No metadata found\n");
    goto out;
  }

  proxy = wp_object_manager_lookup (self->om, WP_TYPE_NODE, NULL);
  if (!proxy) {
    printf ("Node '%d' not found\n", id);
    goto out;
  }

  media_class = wp_pipewire_object_get_property (WP_PIPEWIRE_OBJECT (proxy),
      PW_KEY_MEDIA_CLASS);
  if (!g_strcmp0 (media_class, "Audio/Sink"))
    key = "default.configured.audio.sink";
  else if (!g_strcmp0 (media_class, "Audio/Source"))
    key = "default.configured.audio.source";
  else if (!g_strcmp0 (media_class, "Video/Source"))
    key = "default.configured.video.source";
  else {
    printf ("%u is not a device node (media.class = %s)\n",
        id, media_class);
    goto out;
  }

  name = wp_pipewire_object_get_property (WP_PIPEWIRE_OBJECT (proxy),
      PW_KEY_NODE_NAME);
  if (!name) {
    printf ("node %u does not have a valid node.name\n", id);
    goto out;
  }

  g_snprintf (buf, sizeof(buf), "{ \"name\": \"%s\" }", name);
  wp_metadata_set (metadata, 0, key, "Spa:String:JSON", buf);

  wp_core_sync (self->core, NULL, (GAsyncReadyCallback) async_quit, self);
  return;

out:
  self->exit_code = 3;
  g_main_loop_quit (self->loop);
}

/* set-volume */

static gboolean
set_volume_parse_positional (gint argc, gchar ** argv, GError **error)
{
  if (argc < 4) {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "ID and VOL are required");
    return FALSE;
  }

  long id = strtol (argv[2], NULL, 10);
  float volume = strtof (argv[3], NULL);
  if (id <= 0) {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "'%s' is not a valid number", argv[2]);
    return FALSE;
  }

  cmdline.set_volume.id = id;
  cmdline.set_volume.volume = volume;
  return TRUE;
}

static gboolean
set_volume_prepare (WpCtl * self, GError ** error)
{
  wp_object_manager_add_interest (self->om, WP_TYPE_ENDPOINT,
      WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
      "object.id", "=u", cmdline.set_volume.id,
      NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_NODE,
      WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
      "object.id", "=u", cmdline.set_volume.id,
      NULL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_GLOBAL_PROXY,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  return TRUE;
}

static void
set_volume_run (WpCtl * self)
{
  g_autoptr (WpPipewireObject) proxy = NULL;
  g_autoptr (WpPlugin) mixer_api = wp_plugin_find (self->core, "mixer-api");
  g_auto (GVariantBuilder) b = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  GVariant *variant = NULL;
  gboolean res = FALSE;
  guint32 node_id = cmdline.set_volume.id;

  proxy = wp_object_manager_lookup (self->om, WP_TYPE_GLOBAL_PROXY, NULL);
  if (!proxy) {
    printf ("Object '%d' not found\n", cmdline.set_volume.id);
    goto out;
  }

  if (WP_IS_ENDPOINT (proxy)) {
    const gchar *str = wp_pipewire_object_get_property (proxy, "node.id");
    if (!str) {
      printf ("Endpoint '%d' does not have an associated node\n",
          cmdline.set_volume.id);
      goto out;
    }
    node_id = atoi (str);
  }

  g_variant_builder_add (&b, "{sv}", "volume",
      g_variant_new_double (cmdline.set_volume.volume));
  variant = g_variant_builder_end (&b);

  g_signal_emit_by_name (mixer_api, "set-volume", node_id, variant, &res);
  if (!res) {
    printf ("Object '%d' (node %d) does not support volume\n",
        cmdline.set_volume.id, node_id);
    goto out;
  }

  wp_core_sync (self->core, NULL, (GAsyncReadyCallback) async_quit, self);
  return;

out:
  self->exit_code = 3;
  g_main_loop_quit (self->loop);
}

/* set-mute */

static gboolean
set_mute_parse_positional (gint argc, gchar ** argv, GError **error)
{
  if (argc < 4) {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "ID and one of '1', '0' or 'toggle' are required");
    return FALSE;
  }

  long id = strtol (argv[2], NULL, 10);
  if (id <= 0) {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "'%s' is not a valid number", argv[2]);
    return FALSE;
  }
  cmdline.set_mute.id = id;

  if (!g_strcmp0 (argv[3], "1"))
    cmdline.set_mute.mute = 1;
  else if (!g_strcmp0 (argv[3], "0"))
    cmdline.set_mute.mute = 0;
  else if (!g_strcmp0 (argv[3], "toggle"))
    cmdline.set_mute.mute = 2;
  else {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "'%s' is not a valid mute option", argv[3]);
    return FALSE;
  }

  return TRUE;
}

static gboolean
set_mute_prepare (WpCtl * self, GError ** error)
{
  wp_object_manager_add_interest (self->om, WP_TYPE_ENDPOINT,
      WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
      "object.id", "=u", cmdline.set_mute.id,
      NULL);
  wp_object_manager_add_interest (self->om, WP_TYPE_NODE,
      WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
      "object.id", "=u", cmdline.set_mute.id,
      NULL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_GLOBAL_PROXY,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  return TRUE;
}

static void
set_mute_run (WpCtl * self)
{
  g_autoptr (WpPipewireObject) proxy = NULL;
  g_autoptr (WpPlugin) mixer_api = wp_plugin_find (self->core, "mixer-api");
  g_auto (GVariantBuilder) b = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  GVariant *variant = NULL;
  gboolean res = FALSE;
  gboolean mute = FALSE;
  guint32 node_id = cmdline.set_mute.id;

  proxy = wp_object_manager_lookup (self->om, WP_TYPE_GLOBAL_PROXY, NULL);
  if (!proxy) {
    printf ("Object '%d' not found\n", cmdline.set_mute.id);
    goto out;
  }

  if (WP_IS_ENDPOINT (proxy)) {
    const gchar *str = wp_pipewire_object_get_property (proxy, "node.id");
    if (!str) {
      printf ("Endpoint '%d' does not have an associated node\n",
          cmdline.set_mute.id);
      goto out;
    }
    node_id = atoi (str);
  }

  g_signal_emit_by_name (mixer_api, "get-volume", node_id, &variant);
  if (!variant) {
    printf ("Object '%d' (node %d) does not support mute\n",
        cmdline.set_mute.id, node_id);
    goto out;
  }

  g_variant_lookup (variant, "mute", "b", &mute);
  g_clear_pointer (&variant, g_variant_unref);

  if (cmdline.set_mute.mute == 2)
    mute = !mute;
  else
    mute = !!cmdline.set_mute.mute;

  g_variant_builder_add (&b, "{sv}", "mute", g_variant_new_boolean (mute));
  variant = g_variant_builder_end (&b);

  g_signal_emit_by_name (mixer_api, "set-volume", node_id, variant, &res);

  wp_core_sync (self->core, NULL, (GAsyncReadyCallback) async_quit, self);
  return;

out:
  self->exit_code = 3;
  g_main_loop_quit (self->loop);
}

/* set-profile */

static gboolean
set_profile_parse_positional (gint argc, gchar ** argv, GError **error)
{
  if (argc < 4) {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "ID and INDEX are required");
    return FALSE;
  }

  long id = strtol (argv[2], NULL, 10);
  int index = atoi (argv[3]);
  if (id < 0) {
    g_set_error (error, wpctl_error_domain_quark(), 0,
        "'%s' is not a valid index", argv[2]);
    return FALSE;
  }

  cmdline.set_profile.id = id;
  cmdline.set_profile.index = index;
  return TRUE;
}

static gboolean
set_profile_prepare (WpCtl * self, GError ** error)
{
  wp_object_manager_add_interest (self->om, WP_TYPE_GLOBAL_PROXY,
      WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
      "object.id", "=u", cmdline.set_profile.id,
      NULL);
  wp_object_manager_request_object_features (self->om, WP_TYPE_GLOBAL_PROXY,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  return TRUE;
}

static void
set_profile_run (WpCtl * self)
{
  g_autoptr (WpPipewireObject) proxy = NULL;

  proxy = wp_object_manager_lookup (self->om, WP_TYPE_GLOBAL_PROXY, NULL);
  if (!proxy) {
    printf ("Object '%d' not found\n", cmdline.set_profile.id);
    goto out;
  }
  wp_pipewire_object_set_param (proxy, "Profile", 0,
      wp_spa_pod_new_object (
        "Spa:Pod:Object:Param:Profile", "Profile",
        "index", "i", cmdline.set_profile.index,
        NULL));
  wp_core_sync (self->core, NULL, (GAsyncReadyCallback) async_quit, self);
  return;

out:
  self->exit_code = 3;
  g_main_loop_quit (self->loop);
}

#define N_ENTRIES 3

static const struct subcommand {
  /* the name to match on the command line */
  const gchar *name;
  /* description of positional arguments, shown in the help message */
  const gchar *positional_args;
  /* short description, shown at the top of the help message */
  const gchar *summary;
  /* long description, shown at the bottom of the help message */
  const gchar *description;
  /* additional cmdline arguments for this subcommand */
  const GOptionEntry entries[N_ENTRIES];
  /* function to parse positional arguments */
  gboolean (*parse_positional) (gint, gchar **, GError **);
  /* function to prepare the object manager */
  gboolean (*prepare) (WpCtl *, GError **);
  /* function to run after the object manager is installed */
  void (*run) (WpCtl *);
} subcommands[] = {
  {
    .name = "status",
    .positional_args = "",
    .summary = "Displays the current state of objects in PipeWire",
    .description = NULL,
    .entries = { { NULL } },
    .parse_positional = NULL,
    .prepare = status_prepare,
    .run = status_run,
  },
  {
    .name = "inspect",
    .positional_args = "ID",
    .summary = "Displays information about the specified object",
    .description = NULL,
    .entries = {
      { "referenced", 'r', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
        &cmdline.inspect.show_referenced,
        "Show objects that are referenced in properties", NULL },
      { "associated", 'a', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
        &cmdline.inspect.show_associated, "Show associated objects", NULL },
      { NULL }
    },
    .parse_positional = inspect_parse_positional,
    .prepare = inspect_prepare,
    .run = inspect_run,
  },
  {
    .name = "set-default",
    .positional_args = "ID",
    .summary = "Sets ID to be the default endpoint of its kind "
               "(capture/playback) in its session",
    .description = NULL,
    .entries = { { NULL } },
    .parse_positional = set_default_parse_positional,
    .prepare = set_default_prepare,
    .run = set_default_run,
  },
  {
    .name = "set-volume",
    .positional_args = "ID VOL",
    .summary = "Sets the volume of ID to VOL (floating point, 1.0 is 100%%)",
    .description = NULL,
    .entries = { { NULL } },
    .parse_positional = set_volume_parse_positional,
    .prepare = set_volume_prepare,
    .run = set_volume_run,
  },
  {
    .name = "set-mute",
    .positional_args = "ID 1|0|toggle",
    .summary = "Changes the mute state of ID",
    .description = NULL,
    .entries = { { NULL } },
    .parse_positional = set_mute_parse_positional,
    .prepare = set_mute_prepare,
    .run = set_mute_run,
  },
  {
    .name = "set-profile",
    .positional_args = "ID INDEX",
    .summary = "Sets the profile of ID to INDEX (integer, 0 is 'off')",
    .description = NULL,
    .entries = { { NULL } },
    .parse_positional = set_profile_parse_positional,
    .prepare = set_profile_prepare,
    .run = set_profile_run,
  }
};

static void
on_plugin_activated (WpObject * p, GAsyncResult * res, WpCtl * ctl)
{
  g_autoptr (GError) error = NULL;

  if (!wp_object_activate_finish (p, res, &error)) {
    fprintf (stderr, "%s", error->message);
    ctl->exit_code = 1;
    g_main_loop_quit (ctl->loop);
    return;
  }

  if (--ctl->pending_plugins == 0)
    wp_core_install_object_manager (ctl->core, ctl->om);
}

gint
main (gint argc, gchar **argv)
{
  WpCtl ctl = {0};
  const struct subcommand *cmd = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *summary = NULL;

  wp_init (WP_INIT_ALL);

  ctl.context = g_option_context_new (
      "COMMAND [COMMAND_OPTIONS] - WirePlumber Control CLI");
  ctl.loop = g_main_loop_new (NULL, FALSE);
  ctl.core = wp_core_new (NULL, NULL);
  ctl.apis = g_ptr_array_new_with_free_func (g_object_unref);
  ctl.om = wp_object_manager_new ();

  /* find the subcommand */
  if (argc > 1) {
    for (guint i = 0; i < G_N_ELEMENTS (subcommands); i++) {
      if (!g_strcmp0 (argv[1], subcommands[i].name)) {
        cmd = &subcommands[i];
        break;
      }
    }
  }

  /* prepare the subcommand options */
  if (cmd) {
    GOptionGroup *group;

    /* options */
    group = g_option_group_new (cmd->name, NULL, NULL, &ctl, NULL);
    g_option_group_add_entries (group, cmd->entries);
    g_option_context_set_main_group (ctl.context, group);

    /* summary */
    summary = g_strdup_printf ("Command: %s %s\n  %s",
        cmd->name, cmd->positional_args, cmd->summary);
    g_option_context_set_summary (ctl.context, summary);

    /* description */
    if (cmd->description)
      g_option_context_set_description (ctl.context, cmd->description);
  }
  else {
    /* build the generic summary */
    GString *summary_str = g_string_new ("Commands:");
    for (guint i = 0; i < G_N_ELEMENTS (subcommands); i++) {
      g_string_append_printf (summary_str, "\n  %s %s", subcommands[i].name,
          subcommands[i].positional_args);
    }
    summary = g_string_free (summary_str, FALSE);
    g_option_context_set_summary (ctl.context, summary);
    g_option_context_set_description (ctl.context, "Pass -h after a command "
        "to see command-specific options\n");
  }

  /* parse options */
  if (!g_option_context_parse (ctl.context, &argc, &argv, &error) ||
      (cmd && cmd->parse_positional &&
          !cmd->parse_positional (argc, argv, &error))) {
    fprintf (stderr, "Error: %s\n\n", error->message);
    cmd = NULL;
  }

  /* no active subcommand, show usage and exit */
  if (!cmd) {
    g_autofree gchar *help =
        g_option_context_get_help (ctl.context, FALSE, NULL);
    printf ("%s", help);
    return 1;
  }

  /* prepare the subcommand */
  if (!cmd->prepare (&ctl, &error)) {
    fprintf (stderr, "%s\n", error->message);
    return 1;
  }

  /* load required API modules */
  if (!wp_core_load_component (ctl.core,
      "libwireplumber-module-default-nodes-api", "module", NULL, &error)) {
    fprintf (stderr, "%s\n", error->message);
    return 1;
  }
  if (!wp_core_load_component (ctl.core,
      "libwireplumber-module-mixer-api", "module", NULL, &error)) {
    fprintf (stderr, "%s\n", error->message);
    return 1;
  }
  g_ptr_array_add (ctl.apis, wp_plugin_find (ctl.core, "default-nodes-api"));
  g_ptr_array_add (ctl.apis, ({
    WpPlugin *p = wp_plugin_find (ctl.core, "mixer-api");
    g_object_set (G_OBJECT (p), "scale", 1 /* cubic */, NULL);
    p;
  }));

  /* connect */
  if (!wp_core_connect (ctl.core)) {
    fprintf (stderr, "Could not connect to PipeWire\n");
    return 2;
  }

  /* run */
  g_signal_connect_swapped (ctl.core, "disconnected",
      (GCallback) g_main_loop_quit, ctl.loop);
  g_signal_connect_swapped (ctl.om, "installed",
      (GCallback) cmd->run, &ctl);

  for (guint i = 0; i < ctl.apis->len; i++) {
    WpPlugin *plugin = g_ptr_array_index (ctl.apis, i);
    ctl.pending_plugins++;
    wp_object_activate (WP_OBJECT (plugin), WP_PLUGIN_FEATURE_ENABLED, NULL,
        (GAsyncReadyCallback) on_plugin_activated, &ctl);
  }

  g_main_loop_run (ctl.loop);

  wp_ctl_clear (&ctl);
  return ctl.exit_code;
}
