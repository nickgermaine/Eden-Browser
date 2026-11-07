from wpe.patch_support import replace_once


MEDIASTREAM = "Source/WebCore/platform/mediastream/"
DISPLAY_MANAGER = MEDIASTREAM + "DisplayCaptureManager.h"
SOURCE_CENTER = MEDIASTREAM + "RealtimeMediaSourceCenter.cpp"
GSTREAMER_MANAGERS = MEDIASTREAM + "gstreamer/GStreamerCaptureDeviceManager.h"


def display_manager_patch(text):
    return replace_once(
        text,
        "    virtual bool requiresCaptureDevicesEnumeration() const { return false; }\n",
        "    virtual bool requiresCaptureDevicesEnumeration() const { return false; }\n"
        "    virtual bool selectsSourceWhenCaptureStarts() const { return false; }\n",
    )


def source_center_patch(text):
    return replace_once(
        text,
        "        auto sourceOrError = displayCaptureFactory().createDisplayCaptureSource(device, MediaDeviceHashSalts { hashSalts }, &request.videoConstraints, request.pageIdentifier);\n",
        "        if (displayCaptureFactory().displayCaptureDeviceManager().selectsSourceWhenCaptureStarts()) {\n"
        "            displayDeviceInfo.append({ 1, device });\n"
        "            continue;\n"
        "        }\n\n"
        "        auto sourceOrError = displayCaptureFactory().createDisplayCaptureSource(device, MediaDeviceHashSalts { hashSalts }, &request.videoConstraints, request.pageIdentifier);\n",
    )


def gstreamer_managers_patch(text):
    return replace_once(
        text,
        "    bool requiresCaptureDevicesEnumeration() const final { return true; }\n",
        "    bool requiresCaptureDevicesEnumeration() const final { return true; }\n"
        "    bool selectsSourceWhenCaptureStarts() const final { return true; }\n",
    )


VIDEO_CAPTURE_SOURCE = MEDIASTREAM + "gstreamer/GStreamerVideoCaptureSource.cpp"


def video_capture_source_patch(text):
    text = replace_once(
        text,
        "        RealtimeMediaSourceSupportedConstraints supportedConstraints;\n"
        "        supportedConstraints.setSupportsDeviceId(true);\n"
        "        supportedConstraints.setSupportsFacingMode(true);\n",
        "        bool isDisplay = m_deviceType == CaptureDevice::DeviceType::Screen || m_deviceType == CaptureDevice::DeviceType::Window;\n"
        "        RealtimeMediaSourceSupportedConstraints supportedConstraints;\n"
        "        supportedConstraints.setSupportsDeviceId(true);\n"
        "        supportedConstraints.setSupportsFacingMode(!isDisplay);\n"
        "        if (isDisplay) {\n"
        "            supportedConstraints.setSupportsDisplaySurface(true);\n"
        "            supportedConstraints.setSupportsLogicalSurface(true);\n"
        "            settings.setDisplaySurface(m_deviceType == CaptureDevice::DeviceType::Screen ? DisplaySurfaceType::Monitor : DisplaySurfaceType::Window);\n"
        "            settings.setLogicalSurface(false);\n"
        "        }\n",
    )
    text = replace_once(
        text,
        "    m_currentSettings->setFacingMode(facingMode());\n",
        "    if (m_deviceType == CaptureDevice::DeviceType::Camera)\n"
        "        m_currentSettings->setFacingMode(facingMode());\n",
    )
    text = replace_once(
        text,
        "    RealtimeVideoCaptureSource::setSizeFrameRateAndZoom(constraints);\n"
        "    if (!constraints.width || !constraints.height)\n"
        "        return;\n\n"
        "    m_capturer->setSize({ *constraints.width, *constraints.height });\n",
        "    RealtimeVideoCaptureSource::setSizeFrameRateAndZoom(constraints);\n",
    )
    return replace_once(
        text,
        "    setIntrinsicSize(IntSize(*videoResolution), false);\n"
        "    if (m_deviceType == CaptureDevice::DeviceType::Screen)\n"
        "        ensureIntrinsicSizeMaintainsAspectRatio();\n",
        "    bool followsIntrinsicSize = size().isEmpty() || size() == intrinsicSize();\n"
        "    setIntrinsicSize(IntSize(*videoResolution), false);\n"
        "    if (m_deviceType != CaptureDevice::DeviceType::Screen && m_deviceType != CaptureDevice::DeviceType::Window)\n"
        "        return;\n"
        "    if (followsIntrinsicSize)\n"
        "        setSize(intrinsicSize());\n"
        "    else\n"
        "        ensureIntrinsicSizeMaintainsAspectRatio();\n",
    )


PATCHES = {
    VIDEO_CAPTURE_SOURCE: video_capture_source_patch,
    DISPLAY_MANAGER: display_manager_patch,
    SOURCE_CENTER: source_center_patch,
    GSTREAMER_MANAGERS: gstreamer_managers_patch,
}
