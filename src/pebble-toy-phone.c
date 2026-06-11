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
static AppTimer *s_stop_timer;
static AppTimer *s_light_show_timer;
static AppTimer *s_vibrate_timer;
static size_t remaining;
static bool s_playing;
static bool bl_enabled;
static bool bl_fallback;
static bool third_click;

#if defined(PBL_PLATFORM_FLINT)
static bool bl_toggle;
#endif

static const uint32_t pcms[2] = {
  RESOURCE_ID_PCM_DATA_PART1,
  RESOURCE_ID_PCM_DATA_PART2
};

// Resource playback state
static ResHandle s_res_handle[sizeof(pcms)];
static size_t s_res_size[sizeof(pcms)];
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

static void start_toy_phone(void);

#if defined(PBL_TOUCH)
static void touch_handler(const TouchEvent *event, void *context) {
  switch (event->type) {
    case TouchEvent_Liftoff:
    case TouchEvent_PositionUpdate:
      break;
    case TouchEvent_Touchdown:
      start_toy_phone();
      break;
  }
}
#endif

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  start_toy_phone();
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (bl_fallback) {
    bl_enabled = light_is_on();
    bl_fallback = !light_is_on();
  }
  if (volume >= MAX_VOLUME) return;
  volume = volume + 5;
  speaker_set_volume(volume);
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (bl_fallback) {
    bl_enabled = light_is_on();
    bl_fallback = !light_is_on();
  }
  if (volume <= MIN_VOLUME) return;
  volume = volume - 5;
  speaker_set_volume(volume);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
}

static void light_show_callback(void *data);

static bool fill_stream(void) {
  for (;;) {
    remaining = s_res_size[play_count - 1] - s_res_offset;
    
    if (remaining == 0) {
      return true;
    }

    if (remaining < 61000 && play_count == 2) {
      if (!s_light_show_timer) {
        light_show_callback(NULL);
      }
    }

    size_t to_read = (remaining < BYTES_PER_CHUNK) ? remaining : BYTES_PER_CHUNK;
    resource_load_byte_range(s_res_handle[play_count - 1], s_res_offset, s_buffer, to_read);
            
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
  bitmap_layer_set_background_color(phone_layer, GColorWhite);
  #elif defined(PBL_PLATFORM_FLINT)
  bitmap_layer_set_compositing_mode(phone_layer, GCompOpAssign);
  #endif

  if (remaining == 0) {
    if (play_count > 1) {
      light_enable(false);
    }
    speaker_stream_close();
  }

  s_stop_timer = NULL;
  speaker_stop();
}

static void timer_callback(void *data) {
  if (!s_playing) {
    s_timer = NULL;
    return;
  }

  if (fill_stream()) {
    if (!s_stop_timer) {
      s_stop_timer = app_timer_register(1500, stop_callback, NULL);
    }
    s_timer = NULL;
    return;
  }

  s_timer = app_timer_register(TIMER_MS, timer_callback, NULL);
}

static void light_show_callback(void *data) {
  if (!s_playing || play_count < 2) {
    s_light_show_timer = NULL;
    return;
  }

  #if defined(PBL_PLATFORM_FLINT)
  bl_toggle = !bl_toggle;
  
  GCompOp mode = bl_toggle ? GCompOpAssignInverted : GCompOpAssign; 
  bitmap_layer_set_compositing_mode(phone_layer, mode);
  if (bl_enabled) {
    light_enable_interaction();
  }

  #elif defined(PBL_PLATFORM_EMERY)
  GColor random_color = GColorFromRGB(rand() % 4 * 85, rand() % 4 * 85, rand() % 4 * 85);
  
  if (bl_enabled) {
    bitmap_layer_set_background_color(phone_layer, GColorWhite);
    light_set_color(random_color);
    light_enable_interaction();
  } else {
    /* fallback to changing the background color if the backlight is off */
    bitmap_layer_set_background_color(phone_layer, random_color);
    bl_enabled = light_is_on();
  }

  #endif

  s_light_show_timer = app_timer_register(200, light_show_callback, NULL);
}

static void vibrate_callback(void *data) {
  if (third_click) return;
  vibes_enqueue_custom_pattern(pat);
}

static void cancel_timers(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }

  if (s_stop_timer) {
    app_timer_cancel(s_stop_timer);
    s_stop_timer = NULL;
  }

  if (s_vibrate_timer) {
    vibes_cancel();
    s_vibrate_timer = NULL;
  }

  if (s_light_show_timer) {
    app_timer_cancel(s_light_show_timer);
    s_light_show_timer = NULL;
  }
}

static void start_toy_phone(void) {
  if (play_count == 2 && remaining > 61000 && !third_click) {
    third_click = true;
  } else {
    third_click = false;
    if (play_count >= 2) {
      play_count = 0;
    }
    play_count++;
  }
 
  cancel_timers();
  stop_callback(NULL);
  
  s_res_offset = 0;
 
  if (!s_vibrate_timer && !third_click) {
    s_vibrate_timer = app_timer_register(50, vibrate_callback, NULL);
  }
  
  if (play_count == 2) {
    if (third_click) {
      s_res_offset = 20000; //jump to ay ay ay im your little butterfly
    }
    bl_enabled = light_is_on();
    bl_fallback = !light_is_on();
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

  bitmap_layer_set_compositing_mode(phone_layer, PBL_IF_COLOR_ELSE(GCompOpSet, GCompOpAssign));
  
  bitmap_layer_set_bitmap(phone_layer, phone_bitmap);
  layer_add_child(window_layer, bitmap_layer_get_layer(phone_layer));
}

static void prv_window_unload(Window *window) {
  cancel_timers();
  stop_callback(NULL);
  if (persist_exists(1)) {
    if (volume != persist_read_int(1)) {
        persist_write_int(1, volume);
    }
  } else {
    persist_write_int(1, volume);
  }
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
 
  int pcm_count = sizeof(pcms) / sizeof(pcms[0]);

  for (int i = 0; i < pcm_count; i++) {
    s_res_handle[i] = resource_get_handle(pcms[i]);
    s_res_size[i] = resource_size(s_res_handle[i]);

    if (s_res_size[i] == 0) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "toy phone pcm part %d resource is empty", i);
    }
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
