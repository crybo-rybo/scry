# Dear ImGui chat panel

This optional showcase shows how a host can display one asynchronous Scry turn
in an existing Dear ImGui application. It is example code. It is not part of the
installed API of Scry. The panel owns its current `scry::Turn`. It submits each
message directly through the harness and the conversation of the host.

The host owns the `scry::Harness`, the `scry::Conversation`, the ImGui context,
the platform and renderer backends, the window, and the main loop. The Scry
callbacks and `ChatPanel::draw()` run when the host decides to call them:

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

The harness and the conversation must have a longer lifetime than the panel.
When the panel is destroyed, it requests cancellation of an active turn. It also
disconnects the callbacks of that turn and does not wait. The callbacks hold the
shared state of the panel. Disconnection stops the delivery of more callbacks
and releases those captures.

The showcase is a standalone CMake project outside the root build of Scry.
Configure `extras/showcase` directly, or run `./.github/scripts/ci-showcase.sh`
(`just showcase`). The showcase fetches the pinned Dear ImGui core only for this
build. It does not select or link GLFW, SDL, OpenGL, or another host backend.
