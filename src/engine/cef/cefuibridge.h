#pragma once

#include <functional>

class QObject;

namespace eden::engine::cef {

class CefUiBridge final {
  public:
    static bool runOnUiThread(std::function<void()> task);
    static void resumeTasks();
    static void beginShutdown();
    static void cancelPendingTasks();
    static bool isOnUiThread();
    static void assertOnUiThread(const QObject *stateOwner = nullptr);
};

}
