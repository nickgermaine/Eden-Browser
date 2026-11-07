from wpe.patch_support import replace_once


MANAGER = "Source/WebCore/platform/mediastream/gstreamer/GStreamerCaptureDeviceManager.cpp"
MANAGER_HEADER = "Source/WebCore/platform/mediastream/gstreamer/GStreamerCaptureDeviceManager.h"
VIDEO_CAPTURE_SOURCE = "Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCaptureSource.cpp"


DEVICE_MONITOR_STARTUP = r'''struct StartedGStreamerDeviceMonitor {
    GRefPtr<GstDeviceMonitor> monitor;
    Vector<GRefPtr<GstDevice>> devices;
};

class GStreamerDeviceMonitorStartup final : public ThreadSafeRefCounted<GStreamerDeviceMonitorStartup> {
public:
    static Ref<GStreamerDeviceMonitorStartup> create() { return adoptRef(*new GStreamerDeviceMonitorStartup); }

    void finish(StartedGStreamerDeviceMonitor&& started)
    {
        Locker locker { m_lock };
        m_started = WTF::move(started);
        m_finished = true;
        m_condition.notifyAll();
    }

    std::optional<StartedGStreamerDeviceMonitor> take(bool waitForStartup)
    {
        Locker locker { m_lock };
        while (waitForStartup && !m_finished)
            m_condition.wait(m_lock);
        return std::exchange(m_started, std::nullopt);
    }

private:
    GStreamerDeviceMonitorStartup() = default;

    Lock m_lock;
    Condition m_condition;
    bool m_finished WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::optional<StartedGStreamerDeviceMonitor> m_started WTF_GUARDED_BY_LOCK(m_lock);
};

static WorkQueue& deviceMonitorQueue()
{
    static NeverDestroyed<Ref<WorkQueue>> queue(WorkQueue::create("GStreamer device monitor"_s));
    return queue.get();
}

static StartedGStreamerDeviceMonitor startDeviceMonitorAndCollectDevices(OptionSet<CaptureDevice::DeviceType> types)
{
    StartedGStreamerDeviceMonitor started;
    GRefPtr monitor = adoptGRef(gst_device_monitor_new());
    if (types.contains(CaptureDevice::DeviceType::Camera))
        gst_device_monitor_add_filter(monitor.get(), "Video/Source", nullptr);
    if (types.contains(CaptureDevice::DeviceType::Microphone)) {
        GRefPtr caps = adoptGRef(gst_caps_new_empty_simple("audio/x-raw"));
        gst_device_monitor_add_filter(monitor.get(), "Audio/Source", caps.get());
    }
    if (types.contains(CaptureDevice::DeviceType::Speaker) || types.contains(CaptureDevice::DeviceType::SystemAudio)) {
        GRefPtr caps = adoptGRef(gst_caps_new_empty_simple("audio/x-raw"));
        gst_device_monitor_add_filter(monitor.get(), "Audio/Sink", caps.get());
    }

    if (!gst_device_monitor_start(monitor.get())) {
        GST_WARNING_OBJECT(monitor.get(), "Could not start device monitor");
        return started;
    }

    GRefPtr bus = adoptGRef(gst_device_monitor_get_bus(monitor.get()));
#if GST_CHECK_VERSION(1, 28, 0)
    static constexpr GstClockTime maximumProviderStartup = 5 * GST_SECOND;
    if (GRefPtr message = adoptGRef(gst_bus_timed_pop_filtered(bus.get(), maximumProviderStartup, GST_MESSAGE_DEVICE_MONITOR_STARTED)))
        GST_DEBUG_OBJECT(monitor.get(), "Device provider(s) successfully started");
    else
        GST_WARNING_OBJECT(monitor.get(), "Device provider(s) did not report startup in time, using the devices found so far");
#endif

    GList* devices = g_list_sort(gst_device_monitor_get_devices(monitor.get()), sortDevices);
    while (devices) {
        started.devices.append(adoptGRef(GST_DEVICE_CAST(devices->data)));
        devices = g_list_delete_link(devices, devices);
    }

    gst_bus_set_flushing(bus.get(), TRUE);
    gst_bus_set_flushing(bus.get(), FALSE);

    started.monitor = WTF::move(monitor);
    return started;
}

'''


DEVICE_MONITOR_ADOPTION = r'''void GStreamerCaptureDeviceManager::computeCaptureDevices(CompletionHandler<void()>&& callback)
{
    if (m_deviceMonitor || m_isTearingDown || MockRealtimeMediaSourceCenter::mockRealtimeMediaSourceCenterEnabled()) {
        callback();
        return;
    }

    m_pendingComputeCallbacks.append(WTF::move(callback));
    if (!m_monitorStartup)
        startMonitorAsynchronously();
}

void GStreamerCaptureDeviceManager::startMonitorAsynchronously()
{
    Ref startup = GStreamerDeviceMonitorStartup::create();
    m_monitorStartup = startup.copyRef();
    deviceMonitorQueue().dispatch([this, startup = WTF::move(startup), types = deviceTypes()]() mutable {
        startup->finish(startDeviceMonitorAndCollectDevices(types));
        callOnMainThread([this, startup = WTF::move(startup)]() mutable {
            finishMonitorStartup(startup.get());
        });
    });
}

void GStreamerCaptureDeviceManager::finishMonitorStartup(GStreamerDeviceMonitorStartup& startup)
{
    auto started = startup.take(false);
    if (&startup == m_monitorStartup.get()) {
        m_monitorStartup = nullptr;
        if (started)
            adoptDeviceMonitor(WTF::move(*started));
    } else {
        if (started && started->monitor)
            gst_device_monitor_stop(started->monitor.get());
        if (m_monitorStartup)
            return;
        if (!m_deviceMonitor && !m_isTearingDown && !m_pendingComputeCallbacks.isEmpty()) {
            startMonitorAsynchronously();
            return;
        }
    }

    for (auto& callback : std::exchange(m_pendingComputeCallbacks, { }))
        callback();
}

void GStreamerCaptureDeviceManager::refreshCaptureDevices()
{
    GST_DEBUG_OBJECT(m_deviceMonitor.get(), "Refreshing capture devices");
    m_devices.clear();
    m_gstreamerDevices.clear();
    m_speakerDevices.clear();
    if (m_isTearingDown)
        return;

    if (!m_deviceMonitor) {
        std::optional<StartedGStreamerDeviceMonitor> started;
        if (RefPtr startup = std::exchange(m_monitorStartup, nullptr))
            started = startup->take(true);
        else
            started = startDeviceMonitorAndCollectDevices(deviceTypes());
        if (started)
            adoptDeviceMonitor(WTF::move(*started));
        return;
    }

    GList* devices = g_list_sort(gst_device_monitor_get_devices(m_deviceMonitor.get()), sortDevices);
    while (devices) {
        addDevice(adoptGRef(GST_DEVICE_CAST(devices->data)));
        devices = g_list_delete_link(devices, devices);
    }
}

void GStreamerCaptureDeviceManager::adoptDeviceMonitor(StartedGStreamerDeviceMonitor&& started)
{
    if (!started.monitor)
        return;

    if (m_isTearingDown) {
        gst_device_monitor_stop(started.monitor.get());
        return;
    }

    m_devices.clear();
    m_gstreamerDevices.clear();
    m_speakerDevices.clear();
    m_deviceMonitor = WTF::move(started.monitor);
    for (auto& device : started.devices)
        addDevice(WTF::move(device));

    GRefPtr bus = adoptGRef(gst_device_monitor_get_bus(m_deviceMonitor.get()));

'''


SORT_DEVICES_END = "    return aIsDefault > bIsDefault ? -1 : 1;\n}\n\n"
BUS_FLUSH = "    gst_bus_set_flushing(bus.get(), TRUE);\n    gst_bus_set_flushing(bus.get(), FALSE);\n\n"


def manager_patch(text):
    refresh = "void GStreamerCaptureDeviceManager::refreshCaptureDevices()\n"
    if text.count(refresh) != 1 or text.count(BUS_FLUSH) != 1:
        raise RuntimeError("The pinned source does not match the capture device enumeration patch")
    start = text.index(refresh)
    end = text.index(BUS_FLUSH, start) + len(BUS_FLUSH)
    text = text[:start] + DEVICE_MONITOR_ADOPTION + text[end:]
    text = replace_once(
        text,
        '#include "GStreamerMockDeviceProvider.h"\n',
        '#include "GStreamerMockDeviceProvider.h"\n#include "MockRealtimeMediaSourceCenter.h"\n',
    )
    text = replace_once(
        text,
        "#include <wtf/glib/GMallocString.h>\n",
        "#include <wtf/Condition.h>\n#include <wtf/Lock.h>\n#include <wtf/MainThread.h>\n#include <wtf/NeverDestroyed.h>\n#include <wtf/ThreadSafeRefCounted.h>\n#include <wtf/WorkQueue.h>\n#include <wtf/glib/GMallocString.h>\n",
    )
    text = replace_once(text, SORT_DEVICES_END, SORT_DEVICES_END + DEVICE_MONITOR_STARTUP)
    text = replace_once(
        text,
        "void GStreamerCaptureDeviceManager::stopMonitor()\n{\n    if (!m_deviceMonitor)\n        return;\n",
        "void GStreamerCaptureDeviceManager::stopMonitor()\n{\n    m_monitorStartup = nullptr;\n    if (!m_deviceMonitor)\n        return;\n",
    )
    text = replace_once(
        text,
        "    stopMonitor();\n    m_gstreamerDevices.clear();\n    m_devices.clear();\n}\n",
        "    stopMonitor();\n    m_gstreamerDevices.clear();\n    m_devices.clear();\n    m_speakerDevices.clear();\n}\n",
    )
    return text


def manager_header_patch(text):
    text = replace_once(
        text,
        "void teardownGStreamerCaptureDeviceManagers();\n",
        "void teardownGStreamerCaptureDeviceManagers();\n\nclass GStreamerDeviceMonitorStartup;\nstruct StartedGStreamerDeviceMonitor;\n",
    )
    text = replace_once(
        text,
        "    const Vector<CaptureDevice>& captureDevices() final;\n",
        "    const Vector<CaptureDevice>& captureDevices() final;\n    void computeCaptureDevices(CompletionHandler<void()>&&) override;\n",
    )
    text = replace_once(
        text,
        "    void refreshCaptureDevices();\n",
        "    void refreshCaptureDevices();\n    void startMonitorAsynchronously();\n    void finishMonitorStartup(GStreamerDeviceMonitorStartup&);\n    void adoptDeviceMonitor(StartedGStreamerDeviceMonitor&&);\n",
    )
    return replace_once(
        text,
        "    RefPtr<GStreamerCapturer> m_defaultCapturer;\n",
        "    RefPtr<GStreamerCapturer> m_defaultCapturer;\n    RefPtr<GStreamerDeviceMonitorStartup> m_monitorStartup;\n    Vector<CompletionHandler<void()>> m_pendingComputeCallbacks;\n",
    )


def video_capture_source_patch(text):
    return replace_once(
        text,
        "    m_devices.clear();\n"
        "    m_pipewireCaptureDeviceManager->computeCaptureDevices(WTF::move(callback));\n",
        "    m_devices.clear();\n"
        "    m_pipewireCaptureDeviceManager->computeCaptureDevices([this, callback = WTF::move(callback)]() mutable {\n"
        "        if (!m_devices.isEmpty()) {\n"
        "            callback();\n"
        "            return;\n"
        "        }\n"
        "        GStreamerCaptureDeviceManager::computeCaptureDevices(WTF::move(callback));\n"
        "    });\n",
    )


PATCHES = {
    MANAGER: manager_patch,
    MANAGER_HEADER: manager_header_patch,
    VIDEO_CAPTURE_SOURCE: video_capture_source_patch,
}
