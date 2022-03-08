#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gst/gst.h>


#define LOG_LEVEL 0x02
#include "peer_connection.h"
#include "signaling.h"

const char BROKER[] = "localhost";
const uint16_t PORT = 8883;
const char DEVICE_CODE[] = "test";
const char DEVICE_KEY[] = "PVqrsYIcnY_hJNnW3E34XOWTIKuKLLBbrTwQ2IviSwE";
const char CERTS[] = "root.crt";

const char VIDEO_SINK_PIPELINE[] = "v4l2src ! videorate ! video/x-raw,width=640,height=360,framerate=30/1 ! videoconvert ! queue ! x264enc bitrate=6000 speed-preset=ultrafast tune=zerolatency key-int-max=15 ! video/x-h264,profile=constrained-baseline ! rtph264pay config-interval=-1 name=video-rtp ! appsink name=video-app-sink";

const char AUDIO_SINK_PIPELINE[] = "alsasrc ! audioconvert ! audioresample ! alawenc ! rtppcmapay name=audio-rtp ! appsink name=audio-app-sink";

const char AUDIO_SRC_PIPELINE[] = "appsrc name=audio-app-src format=time ! application/x-rtp,clock-rate=8000,encoding-name=PCMA ! rtppcmadepay ! alawdec ! alsasink";

typedef struct HomeCamera {

  GstElement *audio_src_pipeline, *audio_sink_pipeline, *video_sink_pipeline;

  GstElement *audio_src, *audio_sink, *video_sink;

  GstElement *audio_rtp, *video_rtp;

  GCond cond;
  GMutex mutex;
  GMutex srtp_lock;

  Signaling *signaling;
  PeerConnection *pc;

} HomeCamera;

HomeCamera g_home_camera;

static void on_iceconnectionstatechange(IceConnectionState state, void *data) {

  if(state == FAILED) {
    LOG_INFO("Disconnect with browser... Stop streaming");
    gst_element_set_state(g_home_camera.audio_src_pipeline, GST_STATE_NULL);
    gst_element_set_state(g_home_camera.audio_sink_pipeline, GST_STATE_NULL);
    gst_element_set_state(g_home_camera.video_sink_pipeline, GST_STATE_NULL);
  }
}

static void on_icecandidate(char *sdp, void *data) {

  signaling_send_answer_to_call(g_home_camera.signaling, sdp);
  g_cond_signal(&g_home_camera.cond);
}

static void need_data(GstElement *src, guint size, void *data) {
}

static GstFlowReturn new_sample(GstElement *sink, void *data) {

  size_t size;
  uint8_t packet[1500];

  GstSample *sample;
  GstBuffer *buffer;
  GstMapInfo info;

  g_signal_emit_by_name(sink, "pull-sample", &sample);

  if(sample) {

    buffer = gst_sample_get_buffer(sample);
    gst_buffer_map(buffer, &info, GST_MAP_READ);

    size = info.size;
    memcpy(packet, info.data, info.size);

    g_mutex_lock(&g_home_camera.srtp_lock);
    peer_connection_send_rtp_packet(g_home_camera.pc, packet, size);
    g_mutex_unlock(&g_home_camera.srtp_lock);

    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
  }

  return GST_FLOW_ERROR;
}

static void on_transport_ready(void *data) {

  static int audio_pt = -1;
  static int video_pt = -1;
  int gst_pt;

  // Update payload type of rtph264pay and rtppcmapay
  gst_pt = peer_connection_get_rtpmap(g_home_camera.pc, CODEC_H264);

  if(video_pt != gst_pt) {
    video_pt = gst_pt;
    g_object_set(g_home_camera.video_rtp, "pt", video_pt, NULL);
  }

  gst_pt = peer_connection_get_rtpmap(g_home_camera.pc, CODEC_PCMA);

  if(audio_pt != gst_pt) {
    audio_pt = gst_pt;
    g_object_set(g_home_camera.audio_rtp, "pt", audio_pt, NULL);
  }

  gst_element_set_state(g_home_camera.audio_src_pipeline, GST_STATE_PLAYING);
  gst_element_set_state(g_home_camera.audio_sink_pipeline, GST_STATE_PLAYING);
  gst_element_set_state(g_home_camera.video_sink_pipeline, GST_STATE_PLAYING);

}


void on_track(uint8_t *packet, size_t bytes, void *data) {

  GstBuffer *buf;
  GstMapInfo info;

  GstFlowReturn *ret;

  buf = gst_buffer_new_and_alloc(bytes);

  gst_buffer_map(buf, &info, GST_MAP_WRITE);

  memcpy(info.data, packet, bytes);
  info.size = bytes;

  g_signal_emit_by_name(g_home_camera.audio_src, "push-buffer", buf, &ret);

  gst_buffer_unmap(buf, &info);
  gst_buffer_unref(buf);

  //printf("Got packet %ld\n", bytes);
}

void on_call_event(SignalingEvent signaling_event, char *msg, void *data) {

  if(signaling_event == SIGNALING_EVENT_GET_OFFER) {

    printf("Get offer from singaling\n");
    g_mutex_lock(&g_home_camera.mutex);

    gst_element_set_state(g_home_camera.video_sink_pipeline, GST_STATE_NULL);
    gst_element_set_state(g_home_camera.audio_src_pipeline, GST_STATE_NULL);
    gst_element_set_state(g_home_camera.audio_sink_pipeline, GST_STATE_NULL);
    if(g_home_camera.pc)
      peer_connection_destroy(g_home_camera.pc);

    g_home_camera.pc = peer_connection_create();

    MediaStream *media_stream = media_stream_new();
    media_stream_add_track(media_stream, CODEC_H264);
    media_stream_add_track(media_stream, CODEC_PCMA);
    peer_connection_add_stream(g_home_camera.pc, media_stream);

    Transceiver transceiver = {.video = SENDRECV, .audio = SENDRECV};
    peer_connection_add_transceiver(g_home_camera.pc, transceiver);

    peer_connection_onicecandidate(g_home_camera.pc, on_icecandidate, NULL);
    peer_connection_ontrack(g_home_camera.pc, on_track, NULL);
    peer_connection_oniceconnectionstatechange(g_home_camera.pc, &on_iceconnectionstatechange, NULL);
    peer_connection_set_on_transport_ready(g_home_camera.pc, &on_transport_ready, NULL);
    peer_connection_set_remote_description(g_home_camera.pc, msg);
    peer_connection_create_answer(g_home_camera.pc);

    g_cond_wait(&g_home_camera.cond, &g_home_camera.mutex);
    g_mutex_unlock(&g_home_camera.mutex);
  }
}


void signal_handler(int signal) {

  gst_element_set_state(g_home_camera.video_sink_pipeline, GST_STATE_NULL);
  gst_element_set_state(g_home_camera.audio_src_pipeline, GST_STATE_NULL);
  gst_element_set_state(g_home_camera.audio_sink_pipeline, GST_STATE_NULL);

  gst_object_unref(g_home_camera.audio_src);
  gst_object_unref(g_home_camera.audio_sink);
  gst_object_unref(g_home_camera.video_sink);

  gst_object_unref(g_home_camera.audio_src_pipeline);
  gst_object_unref(g_home_camera.audio_sink_pipeline);
  gst_object_unref(g_home_camera.video_sink_pipeline);

  if(g_home_camera.signaling)
    signaling_destroy(g_home_camera.signaling);

  if(g_home_camera.pc)
    peer_connection_destroy(g_home_camera.pc);

  exit(0);
}


int main(int argc, char **argv) {

  signal(SIGINT, signal_handler);
  gst_init(&argc, &argv);

  g_home_camera.audio_src_pipeline = gst_parse_launch(AUDIO_SRC_PIPELINE, NULL);
  g_home_camera.audio_sink_pipeline = gst_parse_launch(AUDIO_SINK_PIPELINE, NULL);
  g_home_camera.video_sink_pipeline = gst_parse_launch(VIDEO_SINK_PIPELINE, NULL);


  g_home_camera.audio_src = gst_bin_get_by_name(GST_BIN(g_home_camera.audio_src_pipeline), "audio-app-src");
  g_home_camera.audio_sink = gst_bin_get_by_name(GST_BIN(g_home_camera.audio_sink_pipeline), "audio-app-sink");
  g_home_camera.video_sink = gst_bin_get_by_name(GST_BIN(g_home_camera.video_sink_pipeline), "video-app-sink");

  g_signal_connect(g_home_camera.audio_src, "need-data", G_CALLBACK(need_data), NULL );
  g_signal_connect(g_home_camera.audio_sink, "new-sample", G_CALLBACK(new_sample), NULL);
  g_signal_connect(g_home_camera.video_sink, "new-sample", G_CALLBACK(new_sample), NULL);

  g_object_set(g_home_camera.audio_src, "emit-signals", TRUE, NULL);
  g_object_set(g_home_camera.audio_sink, "emit-signals", TRUE, NULL);
  g_object_set(g_home_camera.video_sink, "emit-signals", TRUE, NULL);

  g_home_camera.video_rtp = gst_bin_get_by_name(GST_BIN(g_home_camera.video_sink_pipeline), "video-rtp");
  g_home_camera.audio_rtp = gst_bin_get_by_name(GST_BIN(g_home_camera.audio_sink_pipeline), "audio-rtp");

  char topic[128] = {0};
  snprintf(topic, sizeof(topic), "/call/%s", DEVICE_CODE);
  SignalingOption signaling_option = {SIGNALING_PROTOCOL_MQTT, BROKER, topic, PORT, NULL, CERTS, DEVICE_CODE, DEVICE_KEY};

  g_home_camera.signaling = signaling_create(signaling_option);
  if(!g_home_camera.signaling) {
    printf("Create signaling service failed\n");
    return 0;
  }

  signaling_on_call_event(g_home_camera.signaling, &on_call_event, NULL);
  signaling_dispatch(g_home_camera.signaling);

  return 0;
}
