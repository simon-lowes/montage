// Montage — the device layer's backends (audio/AudioDevice.h), each in a file of its own.
#pragma once

#include "AudioDevice.h"

namespace montage::audiodev::detail {

// JACK (JackDevice.cpp), its library loaded when first asked for.
bool jackLoadable();
std::vector<DeviceInfo> jackDevices(std::string* error);
std::unique_ptr<Stream> openJack(const DeviceSettings& s, int inputs, int outputs, const std::vector<std::string>& outputNames,
                                 StreamCallbacks callbacks, std::string* error);

// PortAudio (PortAudioDevice.cpp; stubs without MONTAGE_WITH_PORTAUDIO).
bool portAudioHas(Host h);
std::vector<DeviceInfo> portAudioDevices(Host h, std::string* error);
std::unique_ptr<Stream> openPortAudio(const DeviceSettings& s, int rate, int inputs, int outputs, StreamCallbacks callbacks,
                                      std::string* error);

}  // namespace montage::audiodev::detail
