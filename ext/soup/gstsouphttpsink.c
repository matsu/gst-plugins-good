/* GStreamer
 * Copyright (C) 2011 David Schleef <ds@entropywave.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstsouphttpsink
 *
 * The souphttpsink element sends pipeline data to an HTTP server
 * using HTTP PUT commands.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc num-buffers=300 ! theoraenc ! oggmux !
 *   souphttpsink location=http://server/filename.ogv
 * ]|
 * 
 * This example encodes 10 seconds of video and sends it to the HTTP
 * server "server" using HTTP PUT commands.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include "gstsouphttpsink.h"

GST_DEBUG_CATEGORY_STATIC (gst_soup_http_sink_debug_category);
#define GST_CAT_DEFAULT gst_soup_http_sink_debug_category

/* prototypes */


static void gst_soup_http_sink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_soup_http_sink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_soup_http_sink_dispose (GObject * object);
static void gst_soup_http_sink_finalize (GObject * object);

static gboolean gst_soup_http_sink_set_caps (GstBaseSink * sink,
    GstCaps * caps);
static void gst_soup_http_sink_get_times (GstBaseSink * sink,
    GstBuffer * buffer, GstClockTime * start, GstClockTime * end);
static gboolean gst_soup_http_sink_start (GstBaseSink * sink);
static gboolean gst_soup_http_sink_stop (GstBaseSink * sink);
static gboolean gst_soup_http_sink_unlock (GstBaseSink * sink);
static gboolean gst_soup_http_sink_event (GstBaseSink * sink, GstEvent * event);
static GstFlowReturn
gst_soup_http_sink_preroll (GstBaseSink * sink, GstBuffer * buffer);
static GstFlowReturn
gst_soup_http_sink_render (GstBaseSink * sink, GstBuffer * buffer);

static void free_buffer_list (GList * list);
static void gst_soup_http_sink_reset (GstSoupHttpSink * souphttpsink);
static void authenticate (SoupSession * session, SoupMessage * msg,
    SoupAuth * auth, gboolean retrying, gpointer user_data);
static void
callback (SoupSession * session, SoupMessage * msg, gpointer user_data);

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_USER_AGENT,
  PROP_AUTOMATIC_REDIRECT,
  PROP_PROXY,
  PROP_USER_ID,
  PROP_USER_PW,
  PROP_PROXY_ID,
  PROP_PROXY_PW,
  PROP_COOKIES,
  PROP_SESSION,
};

#define DEFAULT_USER_AGENT           "GStreamer souphttpsink "

/* pad templates */

static GstStaticPadTemplate gst_soup_http_sink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);


/* class initialization */

#define DEBUG_INIT(bla) \
  GST_DEBUG_CATEGORY_INIT (gst_soup_http_sink_debug_category, "souphttpsink", 0, \
      "debug category for souphttpsink element");

GST_BOILERPLATE_FULL (GstSoupHttpSink, gst_soup_http_sink, GstBaseSink,
    GST_TYPE_BASE_SINK, DEBUG_INIT);

static void
gst_soup_http_sink_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_soup_http_sink_sink_template));

  gst_element_class_set_details_simple (element_class, "HTTP client sink",
      "Generic", "Sends streams to HTTP server via PUT",
      "David Schleef <ds@entropywave.com>");
}

static void
gst_soup_http_sink_class_init (GstSoupHttpSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_soup_http_sink_set_property;
  gobject_class->get_property = gst_soup_http_sink_get_property;
  gobject_class->dispose = gst_soup_http_sink_dispose;
  gobject_class->finalize = gst_soup_http_sink_finalize;
  base_sink_class->set_caps = GST_DEBUG_FUNCPTR (gst_soup_http_sink_set_caps);
  if (0)
    base_sink_class->get_times =
        GST_DEBUG_FUNCPTR (gst_soup_http_sink_get_times);
  base_sink_class->start = GST_DEBUG_FUNCPTR (gst_soup_http_sink_start);
  base_sink_class->stop = GST_DEBUG_FUNCPTR (gst_soup_http_sink_stop);
  base_sink_class->unlock = GST_DEBUG_FUNCPTR (gst_soup_http_sink_unlock);
  base_sink_class->event = GST_DEBUG_FUNCPTR (gst_soup_http_sink_event);
  if (0)
    base_sink_class->preroll = GST_DEBUG_FUNCPTR (gst_soup_http_sink_preroll);
  base_sink_class->render = GST_DEBUG_FUNCPTR (gst_soup_http_sink_render);

  g_object_class_install_property (gobject_class,
      PROP_LOCATION,
      g_param_spec_string ("location", "Location",
          "URI to send to", "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_USER_AGENT,
      g_param_spec_string ("user-agent", "User-Agent",
          "Value of the User-Agent HTTP request header field",
          DEFAULT_USER_AGENT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_AUTOMATIC_REDIRECT,
      g_param_spec_boolean ("automatic-redirect", "automatic-redirect",
          "Automatically follow HTTP redirects (HTTP Status Code 3xx)",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
#if 0
  g_object_class_install_property (gobject_class,
      PROP_PROXY,
      g_param_spec_string ("proxy", "Proxy",
          "HTTP proxy server URI", "",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
#endif
  g_object_class_install_property (gobject_class,
      PROP_USER_ID,
      g_param_spec_string ("user-id", "user-id",
          "user id for authentication", "",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_USER_PW,
      g_param_spec_string ("user-pw", "user-pw",
          "user password for authentication", "",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PROXY_ID,
      g_param_spec_string ("proxy-id", "proxy-id",
          "user id for proxy authentication", "",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PROXY_PW,
      g_param_spec_string ("proxy-pw", "proxy-pw",
          "user password for proxy authentication", "",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SESSION,
      g_param_spec_object ("session", "session",
          "SoupSession object to use for communication",
          SOUP_TYPE_SESSION, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


}

static void
gst_soup_http_sink_init (GstSoupHttpSink * souphttpsink,
    GstSoupHttpSinkClass * souphttpsink_class)
{
#if 0
  const char *proxy;
#endif

  souphttpsink->sinkpad =
      gst_pad_new_from_static_template (&gst_soup_http_sink_sink_template,
      "sink");

  souphttpsink->mutex = g_mutex_new ();
  souphttpsink->cond = g_cond_new ();

  souphttpsink->location = NULL;
  souphttpsink->automatic_redirect = TRUE;
  souphttpsink->user_agent = g_strdup (DEFAULT_USER_AGENT);
  souphttpsink->user_id = NULL;
  souphttpsink->user_pw = NULL;
  souphttpsink->proxy_id = NULL;
  souphttpsink->proxy_pw = NULL;
  souphttpsink->prop_session = NULL;
  souphttpsink->timeout = 1;
#if 0
  proxy = g_getenv ("http_proxy");
  if (proxy && !gst_soup_http_sink_set_proxy (souphttpsink, proxy)) {
    GST_WARNING_OBJECT (souphttpsink,
        "The proxy in the http_proxy env var (\"%s\") cannot be parsed.",
        proxy);
  }
#endif

  gst_soup_http_sink_reset (souphttpsink);
}

static void
gst_soup_http_sink_reset (GstSoupHttpSink * souphttpsink)
{
  g_free (souphttpsink->reason_phrase);
  souphttpsink->reason_phrase = NULL;
  souphttpsink->status_code = 0;
  souphttpsink->offset = 0;

}

void
gst_soup_http_sink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (object);

  g_mutex_lock (souphttpsink->mutex);
  switch (property_id) {
    case PROP_SESSION:
      if (souphttpsink->prop_session) {
        g_object_unref (souphttpsink->prop_session);
      }
      souphttpsink->prop_session = g_value_get_object (value);
      break;
    case PROP_LOCATION:
      g_free (souphttpsink->location);
      souphttpsink->location = g_value_dup_string (value);
      souphttpsink->offset = 0;
      break;
    case PROP_USER_AGENT:
      g_free (souphttpsink->user_agent);
      souphttpsink->user_agent = g_value_dup_string (value);
      break;
    case PROP_AUTOMATIC_REDIRECT:
      souphttpsink->automatic_redirect = g_value_get_boolean (value);
      break;
    case PROP_USER_ID:
      g_free (souphttpsink->user_id);
      souphttpsink->user_id = g_value_dup_string (value);
      break;
    case PROP_USER_PW:
      g_free (souphttpsink->user_pw);
      souphttpsink->user_pw = g_value_dup_string (value);
      break;
    case PROP_PROXY_ID:
      g_free (souphttpsink->proxy_id);
      souphttpsink->proxy_id = g_value_dup_string (value);
      break;
    case PROP_PROXY_PW:
      g_free (souphttpsink->proxy_pw);
      souphttpsink->proxy_pw = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  g_mutex_unlock (souphttpsink->mutex);
}

void
gst_soup_http_sink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (object);

  switch (property_id) {
    case PROP_SESSION:
      g_value_set_object (value, souphttpsink->prop_session);
      break;
    case PROP_LOCATION:
      g_value_set_string (value, souphttpsink->location);
      break;
    case PROP_AUTOMATIC_REDIRECT:
      g_value_set_boolean (value, souphttpsink->automatic_redirect);
      break;
    case PROP_USER_AGENT:
      g_value_set_string (value, souphttpsink->user_agent);
      break;
    case PROP_USER_ID:
      g_value_set_string (value, souphttpsink->user_id);
      break;
    case PROP_USER_PW:
      g_value_set_string (value, souphttpsink->user_pw);
      break;
    case PROP_PROXY_ID:
      g_value_set_string (value, souphttpsink->proxy_id);
      break;
    case PROP_PROXY_PW:
      g_value_set_string (value, souphttpsink->proxy_pw);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_soup_http_sink_dispose (GObject * object)
{
  /* GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (object); */

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

void
gst_soup_http_sink_finalize (GObject * object)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (object);

  /* clean up object here */

  g_free (souphttpsink->user_agent);
  g_free (souphttpsink->user_id);
  g_free (souphttpsink->user_pw);
  g_free (souphttpsink->proxy_id);
  g_free (souphttpsink->proxy_pw);
  g_free (souphttpsink->location);

  g_cond_free (souphttpsink->cond);
  g_mutex_free (souphttpsink->mutex);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}



static gboolean
gst_soup_http_sink_set_caps (GstBaseSink * sink, GstCaps * caps)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (sink);
  GstStructure *structure;
  const GValue *value_array;
  int i, n;

  structure = gst_caps_get_structure (caps, 0);
  value_array = gst_structure_get_value (structure, "streamheader");
  if (value_array) {
    free_buffer_list (souphttpsink->streamheader_buffers);
    souphttpsink->streamheader_buffers = NULL;

    n = gst_value_array_get_size (value_array);
    for (i = 0; i < n; i++) {
      const GValue *value;
      GstBuffer *buffer;
      value = gst_value_array_get_value (value_array, i);
      buffer = GST_BUFFER (gst_value_get_buffer (value));
      souphttpsink->streamheader_buffers =
          g_list_append (souphttpsink->streamheader_buffers,
          gst_buffer_ref (buffer));
    }
  }

  return TRUE;
}

static void
gst_soup_http_sink_get_times (GstBaseSink * sink, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{

}

static gpointer
thread_func (gpointer ptr)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (ptr);

  GST_DEBUG ("thread start");

  souphttpsink->loop = g_main_loop_new (souphttpsink->context, TRUE);
  g_main_loop_run (souphttpsink->loop);

  GST_DEBUG ("thread quit");

  return NULL;
}

static gboolean
gst_soup_http_sink_start (GstBaseSink * sink)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (sink);

  if (souphttpsink->prop_session) {
    souphttpsink->session = souphttpsink->prop_session;
  } else {
    GError *error = NULL;

    souphttpsink->context = g_main_context_new ();

    souphttpsink->thread = g_thread_create (thread_func, souphttpsink,
        TRUE, &error);

    souphttpsink->session =
        soup_session_async_new_with_options (SOUP_SESSION_ASYNC_CONTEXT,
        souphttpsink->context, SOUP_SESSION_USER_AGENT,
        souphttpsink->user_agent, SOUP_SESSION_TIMEOUT, souphttpsink->timeout,
        NULL);

    //soup_session_add_feature (souphttpsink->session,
    //    SOUP_SESSION_FEATURE (soup_logger_new (SOUP_LOGGER_LOG_BODY, 100)));

    g_signal_connect (souphttpsink->session, "authenticate",
        G_CALLBACK (authenticate), souphttpsink);
  }

  return TRUE;
}

static gboolean
gst_soup_http_sink_stop (GstBaseSink * sink)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (sink);

  GST_DEBUG ("stop");

  if (souphttpsink->prop_session == NULL) {
    soup_session_abort (souphttpsink->session);
    g_object_unref (souphttpsink->session);
  }

  if (souphttpsink->loop) {
    g_main_loop_quit (souphttpsink->loop);
    g_thread_join (souphttpsink->thread);
    g_main_loop_unref (souphttpsink->loop);
    souphttpsink->loop = NULL;
  }
  if (souphttpsink->context) {
    g_main_context_unref (souphttpsink->context);
    souphttpsink->context = NULL;
  }

  gst_soup_http_sink_reset (souphttpsink);

  return TRUE;
}

static gboolean
gst_soup_http_sink_unlock (GstBaseSink * sink)
{
  GST_DEBUG ("unlock");

  return TRUE;
}

static gboolean
gst_soup_http_sink_event (GstBaseSink * sink, GstEvent * event)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (sink);

  GST_DEBUG ("event");

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    GST_DEBUG ("got eos");
    g_mutex_lock (souphttpsink->mutex);
    while (souphttpsink->message) {
      GST_DEBUG ("waiting");
      g_cond_wait (souphttpsink->cond, souphttpsink->mutex);
    }
    g_mutex_unlock (souphttpsink->mutex);
    GST_DEBUG ("finished eos");
  }

  return TRUE;
}

static GstFlowReturn
gst_soup_http_sink_preroll (GstBaseSink * sink, GstBuffer * buffer)
{
  GST_DEBUG ("preroll");

  return GST_FLOW_OK;
}

static void
free_buffer_list (GList * list)
{
  GList *g;
  for (g = list; g; g = g_list_next (g)) {
    GstBuffer *buffer = g->data;
    gst_buffer_unref (buffer);
  }
  g_list_free (list);
}

static void
send_message_locked (GstSoupHttpSink * souphttpsink)
{
  GList *g;
  guint64 n;

  if (souphttpsink->queued_buffers == NULL || souphttpsink->message) {
    return;
  }

  /* If the URI went away, drop all these buffers */
  if (souphttpsink->location == NULL) {
    free_buffer_list (souphttpsink->queued_buffers);
    souphttpsink->queued_buffers = NULL;
    return;
  }

  souphttpsink->message = soup_message_new ("PUT", souphttpsink->location);

  //soup_message_body_set_accumulate (souphttpsink->message->request_body, TRUE);

  n = 0;
  if (souphttpsink->offset == 0) {
    for (g = souphttpsink->streamheader_buffers; g; g = g_list_next (g)) {
      GstBuffer *buffer = g->data;
      soup_message_body_append (souphttpsink->message->request_body,
          SOUP_MEMORY_STATIC, GST_BUFFER_DATA (buffer),
          GST_BUFFER_SIZE (buffer));
      n += GST_BUFFER_SIZE (buffer);
    }
  }

  for (g = souphttpsink->queued_buffers; g; g = g_list_next (g)) {
    GstBuffer *buffer = g->data;
    if (!GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_IN_CAPS)) {
      soup_message_body_append (souphttpsink->message->request_body,
          SOUP_MEMORY_STATIC, GST_BUFFER_DATA (buffer),
          GST_BUFFER_SIZE (buffer));
      n += GST_BUFFER_SIZE (buffer);
    }
  }

  if (souphttpsink->offset != 0) {
    char *s;
    s = g_strdup_printf ("bytes %" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT "/*",
        souphttpsink->offset, souphttpsink->offset + n - 1);
    soup_message_headers_append (souphttpsink->message->request_headers,
        "Content-Range", s);
    g_free (s);
  }

  if (n == 0) {
    free_buffer_list (souphttpsink->queued_buffers);
    souphttpsink->queued_buffers = NULL;
    g_object_unref (souphttpsink->message);
    souphttpsink->message = NULL;
    return;
  }

  souphttpsink->sent_buffers = souphttpsink->queued_buffers;
  souphttpsink->queued_buffers = NULL;

  GST_DEBUG ("queue message %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
      souphttpsink->offset, n);
  soup_session_queue_message (souphttpsink->session,
      souphttpsink->message, callback, souphttpsink);

  souphttpsink->offset += n;
}

static gboolean
send_message (GstSoupHttpSink * souphttpsink)
{
  g_mutex_lock (souphttpsink->mutex);
  send_message_locked (souphttpsink);
  g_mutex_unlock (souphttpsink->mutex);

  return FALSE;
}

static void
callback (SoupSession * session, SoupMessage * msg, gpointer user_data)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (user_data);

  GST_DEBUG_OBJECT (souphttpsink, "callback status=%d %s",
      msg->status_code, msg->reason_phrase);

  g_mutex_lock (souphttpsink->mutex);
  g_cond_signal (souphttpsink->cond);
  souphttpsink->message = NULL;

  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code)) {
    souphttpsink->status_code = msg->status_code;
    souphttpsink->reason_phrase = g_strdup (msg->reason_phrase);
    g_mutex_unlock (souphttpsink->mutex);
    return;
  }

  free_buffer_list (souphttpsink->sent_buffers);
  souphttpsink->sent_buffers = NULL;

  send_message_locked (souphttpsink);
  g_mutex_unlock (souphttpsink->mutex);
}

static GstFlowReturn
gst_soup_http_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (sink);
  GSource *source;
  gboolean wake;

  if (souphttpsink->status_code != 0) {
    GST_ELEMENT_ERROR (souphttpsink, RESOURCE, WRITE,
        ("Could not write to HTTP URI"),
        ("error: %d %s", souphttpsink->status_code,
            souphttpsink->reason_phrase));
    return GST_FLOW_ERROR;
  }

  g_mutex_lock (souphttpsink->mutex);
  if (souphttpsink->location != NULL) {
    wake = (souphttpsink->queued_buffers == NULL);
    souphttpsink->queued_buffers =
        g_list_append (souphttpsink->queued_buffers, gst_buffer_ref (buffer));

    if (wake) {
      source = g_idle_source_new ();
      //g_source_set_priority (source, G_PRIORITY_DEFAULT);
      g_source_set_callback (source, (GSourceFunc) (send_message),
          souphttpsink, NULL);
      g_source_attach (source, souphttpsink->context);
      g_source_unref (source);
    }
  }
  g_mutex_unlock (souphttpsink->mutex);

  return GST_FLOW_OK;
}

static void
authenticate (SoupSession * session, SoupMessage * msg,
    SoupAuth * auth, gboolean retrying, gpointer user_data)
{
  GstSoupHttpSink *souphttpsink = GST_SOUP_HTTP_SINK (user_data);

  if (!retrying) {
    if (souphttpsink->user_id && souphttpsink->user_pw) {
      soup_auth_authenticate (auth,
          souphttpsink->user_id, souphttpsink->user_pw);
    }
  }
}