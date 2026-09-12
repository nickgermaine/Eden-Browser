#pragma once

#include <cstdint>

extern "C" {

struct Servo;
struct ServoBuilder;
struct RenderingContext;
struct ServoWebViewBuilder;
struct WebView;

struct ServoEventLoopWaker {
    void (*wake_callback)();
};

struct ServoWebViewDelegate {
    void *user_data;
    void (*notify_load_status_changed)(WebView *, std::int32_t, void *);
    void (*notify_new_frame_ready)(WebView *, void *);
};

ServoBuilder *servo_builder_create();
void servo_builder_set_event_loop_waker(ServoBuilder *, ServoEventLoopWaker);
Servo *servo_builder_build(ServoBuilder *);
void servo_setup_logging(Servo *);
void servo_spin_event_loop(Servo *);
void servo_free(Servo *);

RenderingContext *servo_rendering_context_create_software(std::uint32_t, std::uint32_t);
ServoWebViewBuilder *servo_webview_builder_create(Servo *, RenderingContext *);
std::int32_t servo_webview_builder_set_url(ServoWebViewBuilder *, const char *);
void servo_webview_builder_set_delegate(ServoWebViewBuilder *, ServoWebViewDelegate);
WebView *servo_webview_builder_build(ServoWebViewBuilder *);
std::int32_t servo_webview_load(WebView *, const char *);
void servo_webview_paint(WebView *);
void servo_webview_take_screenshot(
    WebView *,
    void (*)(const std::uint8_t *, std::uint32_t, std::uint32_t, std::int32_t, void *),
    void *
);
void servo_webview_free(WebView *);
}
