# Dear ImGui chat panel

This opt-in showcase demonstrates how a host can present one asynchronous Scry turn
inside an existing Dear ImGui application. It is example code, not part of Scry's
installed API.

The host owns the `scry::Harness`, `scry::Conversation`, ImGui context, platform and
renderer backends, window, and main loop. Both Scry callbacks and `ChatPanel::draw()`
run when the host chooses to call them:

```cpp
scry_showcase::ChatPanel chat_panel{harness, conversation};

while (application_running()) {
  poll_platform_events();
  harness.update({.max_callbacks = 32});

  begin_imgui_frame();
  chat_panel.draw();
  render_imgui_frame();
}
```

The harness and conversation must outlive the panel. Destroying the panel requests
cancellation of an active turn but never waits for it. Callback captures use weak
shared state, so queued callbacks become harmless after panel destruction.

Enable the showcase with `-DSCRY_BUILD_IMGUI_SHOWCASE=ON`, or use the `showcase`
preset. Scry fetches the pinned Dear ImGui core only for this build; it deliberately
does not select or link GLFW, SDL, OpenGL, or another host backend.
