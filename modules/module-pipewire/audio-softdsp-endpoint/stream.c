﻿/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author Julian Bouzas <julian.bouzas@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <spa/param/props.h>
#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/param/audio/type-info.h>
#include <spa/utils/names.h>

#include "stream.h"

#if !defined(HAVE_AUDIOFADE)
# define SPA_PROP_audioFadeDuration 0x30001
# define SPA_PROP_audioFadeStep 0x30002
# define SPA_PROP_audioFadeDirection 0x30003
# define SPA_PROP_audioFadeType 0x30004
# define SPA_NAME_CONTROL_AUDIO_FADE_SOURCE "control.audio.fade.source"
#endif

typedef struct _WpAudioStreamPrivate WpAudioStreamPrivate;
struct _WpAudioStreamPrivate
{
  GObject parent;

  GTask *init_task;

  /* Props */
  GWeakRef endpoint;
  guint id;
  gchar *name;
  enum pw_direction direction;

  /* Stream Proxy */
  WpNode *proxy;
  WpNode *audio_fade_source;
  WpPort *proxy_control_port;
  WpPort *audio_fade_source_port;
  WpLink *control_link;

  WpObjectManager *ports_om;
  WpObjectManager *audio_fade_source_ports_om;
  GVariantBuilder port_vb;
  gboolean port_config_done;
  gboolean port_control_pending;

  /* Stream Controls */
  gfloat volume;
  gboolean mute;

  /* Fade */
  GTask *fade_task;
  GSource *fade_source;
};

enum {
  PROP_0,
  PROP_ENDPOINT,
  PROP_ID,
  PROP_NAME,
  PROP_DIRECTION,
  PROP_PROXY_NODE,
};

enum {
  SIGNAL_CONTROL_CHANGED,
  N_SIGNALS,
};

static guint32 signals[N_SIGNALS] = {0};

static void wp_audio_stream_async_initable_init (gpointer iface, gpointer iface_data);

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (WpAudioStream, wp_audio_stream, G_TYPE_OBJECT,
    G_ADD_PRIVATE (WpAudioStream)
    G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, wp_audio_stream_async_initable_init))

static void
audio_stream_event_param (WpProxy *proxy, int seq, uint32_t id,
    uint32_t index, uint32_t next, const struct spa_pod *param,
    WpAudioStream *self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpBaseEndpoint) ep = g_weak_ref_get (&priv->endpoint);

  switch (id) {
    case SPA_PARAM_Props:
    {
      struct spa_pod_prop *prop;
      struct spa_pod_object *obj = (struct spa_pod_object *) param;
      float volume = priv->volume;
      bool mute = priv->mute;

      SPA_POD_OBJECT_FOREACH(obj, prop) {
        switch (prop->key) {
        case SPA_PROP_volume:
          spa_pod_get_float(&prop->value, &volume);
          priv->volume = volume;
          g_signal_emit (self, signals[SIGNAL_CONTROL_CHANGED], 0,
              WP_ENDPOINT_CONTROL_VOLUME);
          break;
        case SPA_PROP_mute:
          spa_pod_get_bool(&prop->value, &mute);
          priv->mute = mute;
          g_signal_emit (self, signals[SIGNAL_CONTROL_CHANGED], 0,
              WP_ENDPOINT_CONTROL_MUTE);
          break;
        default:
          break;
        }
      }

      break;
    }
    default:
      break;
  }
}

static void
wp_audio_stream_sync_done (WpCore *core, GAsyncResult *res, gpointer data)
{
  WpAudioStream *self = WP_AUDIO_STREAM (data);
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);

  /* finish stream creation when ports are done */
  if (priv->port_config_done && !priv->port_control_pending)
    wp_audio_stream_init_task_finish (self, NULL);
}

static void
wp_audio_stream_link_control_ports (WpAudioStream *self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpCore) core = NULL;
  g_autoptr (WpProperties) props = NULL;
  guint32 in_node_id, out_node_id, in_port_id, out_port_id;

  /* just return if the link was already created */
  if (priv->control_link)
    return;

  g_return_if_fail (priv->proxy);
  g_return_if_fail (priv->audio_fade_source);
  g_return_if_fail (priv->proxy_control_port);
  g_return_if_fail (priv->audio_fade_source_port);

  /* get the info */
  in_node_id = wp_proxy_get_bound_id (WP_PROXY (priv->proxy));
  out_node_id = wp_proxy_get_bound_id (WP_PROXY (priv->audio_fade_source));
  in_port_id = wp_proxy_get_bound_id (WP_PROXY (priv->proxy_control_port));
  out_port_id = wp_proxy_get_bound_id (WP_PROXY (priv->audio_fade_source_port));

  /* create the properties */
  props = wp_properties_new_empty ();
  wp_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", out_node_id);
  wp_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out_port_id);
  wp_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", in_node_id);
  wp_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", in_port_id);

  /* create the link */
  g_debug ("%s:%p linking stream control ports: (%d) %d -> (%d) %d\n",
      G_OBJECT_TYPE_NAME (self), self, out_node_id, out_port_id, in_node_id,
      in_port_id);
  core = wp_audio_stream_get_core (self);
  priv->control_link = wp_link_new_from_factory (core, "link-factory",
      g_steal_pointer (&props));
  g_return_if_fail (priv->control_link);
}

static void
wp_audio_stream_event_info (WpProxy * proxy, GParamSpec *spec,
    WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  const struct pw_node_info *info = wp_proxy_get_info (proxy);

  /* handle the different states */
  switch (info->state) {
  case PW_NODE_STATE_IDLE:
    g_clear_object (&priv->control_link);
    break;
  case PW_NODE_STATE_RUNNING:
    if (priv->audio_fade_source)
      wp_audio_stream_link_control_ports (self);
    break;
  case PW_NODE_STATE_SUSPENDED:
    break;
  default:
    break;
  }
}

static void
on_audio_fade_source_ports_added (WpObjectManager *om, WpPort *port,
    WpAudioStream *self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpCore) core = wp_audio_stream_get_core (self);
  const struct pw_port_info *info = wp_proxy_get_info (WP_PROXY (port));

  g_return_if_fail (info);
  g_return_if_fail (info->direction == PW_DIRECTION_OUTPUT);

  if (!priv->audio_fade_source_port) {
    priv->audio_fade_source_port = g_object_ref (port);
    g_signal_handlers_disconnect_by_func (priv->audio_fade_source_ports_om,
      on_audio_fade_source_ports_added, self);

    /* set the pending flag to false and sync */
    priv->port_control_pending = FALSE;
    wp_core_sync (core, NULL, (GAsyncReadyCallback)wp_audio_stream_sync_done,
        self);
  }
}

static void
on_audio_fade_source_augmented (WpProxy * proxy, GAsyncResult * res,
    WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpCore) core = wp_audio_stream_get_core (self);
  g_autoptr (GError) error = NULL;
  g_autofree gchar *node_id = NULL;
  GVariantBuilder b;

  /* Get the audmented node */
  wp_proxy_augment_finish (proxy, res, &error);
  if (error) {
    g_warning ("WpAudioStream:%p audio-fade-source failed to augment: %s", self,
        error->message);
    wp_audio_stream_init_task_finish (self, g_steal_pointer (&error));
    return;
  }

  /* Get the node id */
  node_id = g_strdup_printf ("%u", wp_proxy_get_bound_id (proxy));

  /* Create the audio fade source ports object manager */
  priv->audio_fade_source_ports_om = wp_object_manager_new ();

  /* set a constraint: the port's "node.id" must match
     the stream's underlying node id */
  g_variant_builder_init (&b, G_VARIANT_TYPE ("aa{sv}"));
  g_variant_builder_open (&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&b, "{sv}", "type",
      g_variant_new_int32 (WP_OBJECT_MANAGER_CONSTRAINT_PW_GLOBAL_PROPERTY));
  g_variant_builder_add (&b, "{sv}", "name",
      g_variant_new_string (PW_KEY_NODE_ID));
  g_variant_builder_add (&b, "{sv}", "value",
      g_variant_new_string (node_id));
  g_variant_builder_close (&b);

  /* declare interest on ports with this constraint */
  wp_object_manager_add_interest (priv->audio_fade_source_ports_om,
      WP_TYPE_PORT,
      g_variant_builder_end (&b),
      WP_PROXY_FEATURES_STANDARD);

  /* Add a handler to be triggered when ports are added */
  g_signal_connect (priv->audio_fade_source_ports_om, "object-added",
      (GCallback) on_audio_fade_source_ports_added, self);

  /* install the object manager */
  wp_core_install_object_manager (core, priv->audio_fade_source_ports_om);
}

static gboolean
is_stream_control_port (WpPort *port)
{
  const struct pw_port_info *info = wp_proxy_get_info (WP_PROXY (port));
  const char *port_name;

  g_return_val_if_fail (info, FALSE);

  if (info->direction == PW_DIRECTION_OUTPUT)
    return FALSE;

  port_name = spa_dict_lookup (info->props, PW_KEY_PORT_NAME);
  if (g_strcmp0 (port_name, "control") != 0)
    return FALSE;

  return TRUE;
}

static void
on_ports_added (WpObjectManager *om, WpPort *port, WpAudioStream *self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpCore) core = wp_audio_stream_get_core (self);
  g_autoptr (WpProperties) props = NULL;

  /* only handle control ports */
  if (!is_stream_control_port (port))
    return;

  if (!priv->proxy_control_port) {
    priv->proxy_control_port = g_object_ref (port);
    g_signal_handlers_disconnect_by_func (priv->ports_om, on_ports_added, self);

    /* set the pending flag so that the stream does not finish yet */
    priv->port_control_pending = TRUE;

    /* create the audio-fade-source node */
    props = wp_properties_new_empty ();
    wp_properties_setf (props, PW_KEY_NODE_NAME, "%s",
        SPA_NAME_CONTROL_AUDIO_FADE_SOURCE);
    wp_properties_set (props, SPA_KEY_LIBRARY_NAME, "control/libspa-control");
    wp_properties_set (props, SPA_KEY_FACTORY_NAME,
        SPA_NAME_CONTROL_AUDIO_FADE_SOURCE);
    priv->audio_fade_source = wp_node_new_from_factory (core,
        "spa-node-factory", g_steal_pointer (&props));
    g_return_if_fail (priv->audio_fade_source);

    wp_proxy_augment (WP_PROXY (priv->audio_fade_source),
        WP_PROXY_FEATURES_STANDARD, NULL,
        (GAsyncReadyCallback) on_audio_fade_source_augmented, self);
  }
}

static void
on_ports_changed (WpObjectManager *om, WpAudioStream *self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpCore) core = wp_audio_stream_get_core (self);

  if (priv->port_config_done) {
    g_debug ("%s:%p port config done", G_OBJECT_TYPE_NAME (self), self);
    g_signal_handlers_disconnect_by_func (priv->ports_om, on_ports_changed,
        self);

    /* sync before finishing */
    wp_core_sync (core, NULL, (GAsyncReadyCallback)wp_audio_stream_sync_done,
        self);
  }
}

static void
on_node_proxy_augmented (WpProxy * proxy, GAsyncResult * res,
    WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpCore) core = wp_audio_stream_get_core (self);
  g_autoptr (GError) error = NULL;
  GVariantBuilder b;
  const struct pw_node_info *info = NULL;
  g_autofree gchar *node_id = NULL;

  wp_proxy_augment_finish (proxy, res, &error);
  if (error) {
    g_warning ("WpAudioStream:%p Node proxy failed to augment: %s", self,
        error->message);
    wp_audio_stream_init_task_finish (self, g_steal_pointer (&error));
    return;
  }

  g_signal_connect_object (proxy, "param",
      (GCallback) audio_stream_event_param, self, 0);
  wp_proxy_subscribe_params (proxy, 1, SPA_PARAM_Props);

  priv->ports_om = wp_object_manager_new ();

  /* Get the node id */
  info = wp_proxy_get_info (proxy);
  node_id = g_strdup_printf ("%u", info->id);

  /* set a constraint: the port's "node.id" must match
     the stream's underlying node id */
  g_variant_builder_init (&b, G_VARIANT_TYPE ("aa{sv}"));
  g_variant_builder_open (&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&b, "{sv}", "type",
      g_variant_new_int32 (WP_OBJECT_MANAGER_CONSTRAINT_PW_GLOBAL_PROPERTY));
  g_variant_builder_add (&b, "{sv}", "name",
      g_variant_new_string (PW_KEY_NODE_ID));
  g_variant_builder_add (&b, "{sv}", "value",
      g_variant_new_string (node_id));
  g_variant_builder_close (&b);

  /* declare interest on ports with this constraint */
  wp_object_manager_add_interest (priv->ports_om, WP_TYPE_PORT,
      g_variant_builder_end (&b),
      WP_PROXY_FEATURE_PW_PROXY | WP_PROXY_FEATURE_INFO);

  g_signal_connect (priv->ports_om, "objects-changed",
      (GCallback) on_ports_changed, self);
  g_signal_connect (priv->ports_om, "object-added",
      (GCallback) on_ports_added, self);

  /* install the object manager */
  wp_core_install_object_manager (core, priv->ports_om);
}

static void
wp_audio_stream_finalize (GObject * object)
{
  WpAudioStreamPrivate *priv =
      wp_audio_stream_get_instance_private (WP_AUDIO_STREAM (object));

  if (priv->fade_source)
    g_source_destroy (priv->fade_source);
  g_clear_pointer (&priv->fade_source, g_source_unref);
  g_clear_object (&priv->fade_task);
  g_clear_object (&priv->control_link);
  g_clear_object (&priv->audio_fade_source_port);
  g_clear_object (&priv->proxy_control_port);
  g_clear_object (&priv->audio_fade_source);
  g_clear_object (&priv->proxy);
  g_clear_object (&priv->audio_fade_source_ports_om);
  g_clear_object (&priv->ports_om);

  /* Clear the endpoint weak reference */
  g_weak_ref_clear (&priv->endpoint);

  /* Clear the name */
  g_clear_pointer (&priv->name, g_free);

  g_clear_object (&priv->init_task);

  G_OBJECT_CLASS (wp_audio_stream_parent_class)->finalize (object);
}

static void
wp_audio_stream_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  WpAudioStreamPrivate *priv =
      wp_audio_stream_get_instance_private (WP_AUDIO_STREAM (object));

  switch (property_id) {
  case PROP_ENDPOINT:
    g_weak_ref_set (&priv->endpoint, g_value_get_object (value));
    break;
  case PROP_ID:
    priv->id = g_value_get_uint(value);
    break;
  case PROP_NAME:
    priv->name = g_value_dup_string (value);
    break;
  case PROP_DIRECTION:
    priv->direction = g_value_get_uint(value);
    break;
  case PROP_PROXY_NODE:
    priv->proxy = g_value_dup_object (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_audio_stream_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  WpAudioStreamPrivate *priv =
      wp_audio_stream_get_instance_private (WP_AUDIO_STREAM (object));

  switch (property_id) {
    case PROP_ENDPOINT:
    g_value_take_object (value, g_weak_ref_get (&priv->endpoint));
    break;
  case PROP_ID:
    g_value_set_uint (value, priv->id);
    break;
  case PROP_NAME:
    g_value_set_string (value, priv->name);
    break;
  case PROP_DIRECTION:
    g_value_set_uint (value, priv->direction);
    break;
  case PROP_PROXY_NODE:
    g_value_set_object (value, priv->proxy);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_audio_stream_init_async (GAsyncInitable *initable, int io_priority,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  WpAudioStream *self = WP_AUDIO_STREAM(initable);
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpBaseEndpoint) ep = g_weak_ref_get (&priv->endpoint);
  g_autoptr (WpCore) core = wp_audio_stream_get_core (self);
  g_autoptr (WpProperties) props = NULL;

  g_debug ("WpBaseEndpoint:%p init stream %s (%s:%p)", ep, priv->name,
      G_OBJECT_TYPE_NAME (self), self);

  priv->init_task = g_task_new (initable, cancellable, callback, data);

  g_return_if_fail (priv->proxy);

  g_signal_connect_object (priv->proxy, "notify::info",
      (GCallback) wp_audio_stream_event_info, self, 0);

  wp_proxy_augment (WP_PROXY (priv->proxy),
      WP_PROXY_FEATURES_STANDARD, cancellable,
      (GAsyncReadyCallback) on_node_proxy_augmented, self);
}

static gboolean
wp_audio_stream_init_finish (GAsyncInitable *initable, GAsyncResult *result,
    GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, initable), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
wp_audio_stream_async_initable_init (gpointer iface, gpointer iface_data)
{
  GAsyncInitableIface *ai_iface = iface;

  ai_iface->init_async = wp_audio_stream_init_async;
  ai_iface->init_finish = wp_audio_stream_init_finish;
}

static void
wp_audio_stream_init (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);

  /* Controls */
  priv->volume = 1.0;
  priv->mute = FALSE;
}

static void
wp_audio_stream_class_init (WpAudioStreamClass * klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;

  object_class->finalize = wp_audio_stream_finalize;
  object_class->set_property = wp_audio_stream_set_property;
  object_class->get_property = wp_audio_stream_get_property;

  /* Install the properties */
  g_object_class_install_property (object_class, PROP_ENDPOINT,
      g_param_spec_object ("endpoint", "endpoint",
          "The endpoint this audio stream belongs to", WP_TYPE_BASE_ENDPOINT,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ID,
      g_param_spec_uint ("id", "id", "The Id of the audio stream", 0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_NAME,
      g_param_spec_string ("name", "name", "The name of the audio stream", NULL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_DIRECTION,
      g_param_spec_uint ("direction", "direction",
          "The direction of the audio stream", 0, 1, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_PROXY_NODE,
      g_param_spec_object ("node", "node",
          "The node proxy of the stream", WP_TYPE_NODE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_CONTROL_CHANGED] = g_signal_new (
      "control-changed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

WpAudioStream *
wp_audio_stream_new_finish (GObject *initable, GAsyncResult *res,
    GError **error)
{
  GAsyncInitable *ai = G_ASYNC_INITABLE(initable);
  return WP_AUDIO_STREAM (g_async_initable_new_finish(ai, res, error));
}

const char *
wp_audio_stream_get_name (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);

  return priv->name;
}

enum pw_direction
wp_audio_stream_get_direction (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);

  return priv->direction;
}

WpNode *
wp_audio_stream_get_node (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);

  return priv->proxy;
}

const struct pw_node_info *
wp_audio_stream_get_info (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);

  return wp_proxy_get_info (WP_PROXY (priv->proxy));
}

static void
port_proxies_foreach_func(gpointer data, gpointer user_data)
{
  WpProxy *port = WP_PROXY (data);
  WpAudioStream *self = WP_AUDIO_STREAM (user_data);
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  const struct pw_node_info *node_info;
  const struct pw_port_info *port_info;
  g_autoptr (WpProperties) props = NULL;
  const gchar *channel;
  uint32_t channel_n = SPA_AUDIO_CHANNEL_UNKNOWN;

  node_info = wp_proxy_get_info (WP_PROXY (priv->proxy));
  g_return_if_fail (node_info);

  port_info = wp_proxy_get_info (port);
  g_return_if_fail (port_info);

  props = wp_proxy_get_properties (port);

  /* skip control ports */
  if (is_stream_control_port (WP_PORT (port)))
    return;

  channel = wp_properties_get (props, PW_KEY_AUDIO_CHANNEL);
  if (channel) {
    const struct spa_type_info *t = spa_type_audio_channel;
    for (; t && t->name; t++) {
      const char *name = t->name + strlen(SPA_TYPE_INFO_AUDIO_CHANNEL_BASE);
      if (!g_strcmp0 (channel, name)) {
        channel_n = t->type;
        break;
      }
    }
  }

  /* tuple format:
      uint32 node_id;
      uint32 port_id;
      uint32 channel;  // enum spa_audio_channel
      uint8 direction; // enum spa_direction
   */
  g_variant_builder_add (&priv->port_vb, "(uuuy)", node_info->id,
      port_info->id, channel_n, (guint8) port_info->direction);
}

gboolean
wp_audio_stream_prepare_link (WpAudioStream * self, GVariant ** properties,
    GError ** error)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (GPtrArray) port_proxies =
      wp_object_manager_get_objects (priv->ports_om, 0);

  /* Create a variant array with all the ports */
  g_variant_builder_init (&priv->port_vb, G_VARIANT_TYPE ("a(uuuy)"));
  g_ptr_array_foreach (port_proxies, port_proxies_foreach_func, self);
  *properties = g_variant_builder_end (&priv->port_vb);

  return TRUE;
}

gfloat
wp_audio_stream_get_volume (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  return priv->volume;
}

gboolean
wp_audio_stream_get_mute (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  return priv->mute;
}

void
wp_audio_stream_set_volume (WpAudioStream * self, gfloat volume)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  char buf[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

  /* Make sure the proxy is valid */
  g_return_if_fail (priv->proxy);

  wp_proxy_set_param (WP_PROXY (priv->proxy),
      SPA_PARAM_Props, 0,
      spa_pod_builder_add_object (&b,
          SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
          SPA_PROP_volume, SPA_POD_Float(volume)));
}

void
wp_audio_stream_set_mute (WpAudioStream * self, gboolean mute)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  char buf[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

  /* Make sure the proxy is valid */
  g_return_if_fail (priv->proxy);

  wp_proxy_set_param (WP_PROXY (priv->proxy),
      SPA_PARAM_Props, 0,
      spa_pod_builder_add_object (&b,
          SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
          SPA_PROP_mute, SPA_POD_Bool(mute)));
}

static gboolean
fade_timeout_callback (gpointer user_data)
{
  WpAudioStream *self = user_data;
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);

  g_return_val_if_fail (priv->fade_task, G_SOURCE_REMOVE);

  g_task_return_boolean (priv->fade_task, TRUE);
  g_clear_object (&priv->fade_task);
  return G_SOURCE_REMOVE;
}

void
wp_audio_stream_begin_fade (WpAudioStream * self, guint duration,
    gfloat step, guint direction, guint type, GCancellable * cancellable,
    GAsyncReadyCallback callback, gpointer user_data)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpCore) core = wp_audio_stream_get_core (self);
  GTask *task = NULL;
  struct spa_pod *props;
  uint8_t buffer[1024];
  struct spa_pod_builder b = { 0 };

  /* Create the fade callback */
  task = g_task_new (self, cancellable, callback, user_data);

  /* Just trigger the callback if the stream does not have a control node */
  if (!priv->audio_fade_source) {
    g_task_return_boolean (task, TRUE);
    g_clear_object (&task);
    return;
  }

  /* Destroy the pending source */
  if (priv->fade_source)
    g_source_destroy (priv->fade_source);
  g_clear_pointer (&priv->fade_source, g_source_unref);

  /* Finish the pending task */
  if (priv->fade_task) {
    g_task_return_boolean (priv->fade_task, FALSE);
    g_clear_object (&priv->fade_task);
  }

  /* Set the new task */
  priv->fade_task = task;

  /* Send the fade */
  spa_pod_builder_init(&b, buffer, sizeof(buffer));
  props = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, 0,
      SPA_PROP_audioFadeDuration,  SPA_POD_Int(duration),
      SPA_PROP_audioFadeStep,      SPA_POD_Double(step),
      SPA_PROP_audioFadeDirection, SPA_POD_Id(direction),
      SPA_PROP_audioFadeType,      SPA_POD_Id(type));
  wp_proxy_set_param (WP_PROXY (priv->audio_fade_source), SPA_PARAM_Props, 0,
      props);

  /* TODO: for now we always trigger the callback after 1 second, which should
   * be enough for the fade to finish. However, we should trigger the callback
   * when channelmix finishes processing the control sequence, via node event
   * param for example */
  wp_core_timeout_add (core, &priv->fade_source, 1000, fade_timeout_callback,
      g_object_ref (self), g_object_unref);
}

WpCore *
wp_audio_stream_get_core (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (WpBaseEndpoint) ep = NULL;
  g_autoptr (WpCore) core = NULL;

  ep = g_weak_ref_get (&priv->endpoint);
  core = wp_base_endpoint_get_core (ep);
  return g_steal_pointer (&core);
}

void
wp_audio_stream_init_task_finish (WpAudioStream * self, GError * err)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  g_autoptr (GError) error = err;

  if (!priv->init_task)
    return;

  if (error)
    g_task_return_error (priv->init_task, g_steal_pointer (&error));
  else
    g_task_return_boolean (priv->init_task, TRUE);

  g_clear_object (&priv->init_task);
}

void
wp_audio_stream_set_port_config (WpAudioStream * self,
    const struct spa_pod * param)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);

  wp_proxy_set_param (WP_PROXY (priv->proxy), SPA_PARAM_PortConfig, 0, param);
}

void
wp_audio_stream_finish_port_config (WpAudioStream * self)
{
  WpAudioStreamPrivate *priv = wp_audio_stream_get_instance_private (self);
  priv->port_config_done = TRUE;
}
