#include <pebble.h>

// Audio constants — must match the raw PCM file format
// Expected format: 8-bit signed mono at 8kHz
#define SAMPLE_RATE 8000
#define TIMER_MS 50
#define SAMPLES_PER_CHUNK (SAMPLE_RATE * TIMER_MS / 1000) // 400
#define BYTES_PER_CHUNK (SAMPLES_PER_CHUNK * 1)            // 400

#if defined(PBL_PLATFORM_FLINT)
#define MIN_VOLUME 65
#define MAX_VOLUME 95
#elif defined(PBL_PLATFORM_EMERY)
#define MIN_VOLUME 5
#define MAX_VOLUME 40
#endif

static Window *s_window;

static BitmapLayer *phone_layer;
static GBitmap *phone_bitmap;

// State
static AppTimer *s_timer;
static AppTimer *s_light_show_timer;
static bool s_playing;

#if defined(PBL_PLATFORM_FLINT)
static bool bl_toggle;
#endif

// Resource playback state
static ResHandle s_res_handle_1;
static ResHandle s_res_handle_2;
static size_t s_res_size;
static size_t s_res_offset;

// Sample buffer
static uint8_t s_buffer[SAMPLES_PER_CHUNK];

static uint8_t volume;
static uint8_t play_count;

static const uint32_t segments[] = {
    400, 
    400, 
    400 
};

VibePattern pat = {
  .durations = segments,
  .num_segments = ARRAY_LENGTH(segments),
};

static void start_playback(void);

#if defined(PBL_TOUCH)
static void touch_handler(const TouchEvent *event, void *context) {
  switch (event->type) {
    case TouchEvent_Touchdown:
    case TouchEvent_PositionUpdate:
      break;
    case TouchEvent_Liftoff:
      start_playback();
      break;
  }
}
#endif

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  start_playback();
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (volume >= MAX_VOLUME) return;
  volume = volume + 5;
  speaker_set_volume(volume);
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (volume <= MIN_VOLUME) return;
  volume = volume - 5;
  speaker_set_volume(volume);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
}

static bool fill_stream(void) {
  for (;;) {
    size_t remaining = s_res_size - s_res_offset;
    if (remaining == 0) {
      return true;
    }

    size_t to_read = (remaining < BYTES_PER_CHUNK) ? remaining : BYTES_PER_CHUNK;
    if (play_count == 1) {
        resource_load_byte_range(s_res_handle_1, s_res_offset, s_buffer, to_read);
    } else if (play_count == 2) {
        resource_load_byte_range(s_res_handle_2, s_res_offset, s_buffer, to_read);
    }
    
    uint32_t written = speaker_stream_write(s_buffer, to_read);
    s_res_offset += written;

    if (written < to_read) {
      return false;
    }
  }
}

static void stop_callback(void *data) {
  s_playing = false;
  #if defined(PBL_PLATFORM_EMERY)
  light_set_system_color();
  #endif
  if (play_count > 1) {
    light_enable(false);
  }
}

static void timer_callback(void *data) {
  if (!s_playing) return;

  if (fill_stream()) {
    app_timer_register(1500, stop_callback, NULL);
    speaker_stream_close();
    return;
  }

  s_timer = app_timer_register(TIMER_MS, timer_callback, NULL);
}

static void light_show_callback(void *data) {
  if (!s_playing || play_count < 2) return;

  #if defined(PBL_PLATFORM_FLINT)
  bl_toggle = !bl_toggle;
  light_enable(bl_toggle);
  #elif defined(PBL_PLATFORM_EMERY)
  light_set_color(GColorFromRGB(rand() % 4 * 85, rand() % 4 * 85, rand() % 4 * 85));
  light_enable(true);
  #endif

  s_light_show_timer = app_timer_register(200, light_show_callback, NULL);
}

static void vibrate_callback(void *data) {
  vibes_enqueue_custom_pattern(pat);
}

static void start_playback(void) {
  if (s_playing) return;

  if (play_count >= 2) {
    play_count = 0;
  }
  
  play_count++;
  
  s_res_handle_1 = resource_get_handle(RESOURCE_ID_PCM_DATA_PART1);
  s_res_handle_2 = resource_get_handle(RESOURCE_ID_PCM_DATA_PART2);
  
  app_timer_register(50, vibrate_callback, NULL);

  if (play_count == 1) {
    s_res_size = resource_size(s_res_handle_1);
  } else if (play_count == 2) {
    s_light_show_timer = app_timer_register(2600, light_show_callback, NULL);
    s_res_size = resource_size(s_res_handle_2);
  }

  s_res_offset = 0;

  if (s_res_size == 0) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "toy phone pcm resource is empty");
    return;
  }

  if (!speaker_stream_open(SpeakerPcmFormat_8kHz_8bit, 0)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to open speaker stream");
    return;
  }
  
  speaker_set_volume(volume);

  s_playing = true;
  fill_stream();
  s_timer = app_timer_register(TIMER_MS, timer_callback, NULL);
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
 
  phone_bitmap = gbitmap_create_with_resource(PBL_IF_COLOR_ELSE(RESOURCE_ID_IMAGE_PHONE_COLOR, 
                                                                RESOURCE_ID_IMAGE_PHONE_BW));
  phone_layer = bitmap_layer_create(bounds);

  bitmap_layer_set_bitmap(phone_layer, phone_bitmap);
  layer_add_child(window_layer, bitmap_layer_get_layer(phone_layer));
}

static void prv_window_unload(Window *window) {
  s_playing = false;
  speaker_stream_close();
  persist_write_int(1, volume);
}

static void prv_init(void) {
  s_window = window_create();
  window_set_click_config_provider(s_window, prv_click_config_provider);
  
  #if defined(PBL_TOUCH)
  if (touch_service_is_enabled()) {
    touch_service_subscribe(touch_handler, NULL);
  }
  #endif

  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });

  if (persist_exists(1)) {
    volume = persist_read_int(1);
  } else {
    volume = MAX_VOLUME;
  }
  
  const bool animated = true;
  window_stack_push(s_window, animated);
}

static void prv_deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
