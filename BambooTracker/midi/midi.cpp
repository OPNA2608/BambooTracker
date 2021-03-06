/*
 * Copyright (C) 2019-2021 Rerrah
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "midi.hpp"
#include <stdio.h>
#include "RtMidi.h"
#include "utils.hpp"

namespace
{
// Defines
constexpr int MIDI_BUFFER_SIZE = 8192;

const std::string MIDI_INP_CLIENT_NAME = "BambooTracker Rx";
const std::string MIDI_INP_PORT_NAME = "BambooTracker MIDI In";

constexpr bool MIDI_INP_IGNORE_SYSEX = false;
constexpr bool MIDI_INP_IGNORE_TIME = false;
constexpr bool MIDI_INP_IGNORE_SENSE = true;
}

std::unique_ptr<MidiInterface> MidiInterface::instance_;

MidiInterface& MidiInterface::getInstance()
{
	if (instance_)
		return *instance_;

	MidiInterface *out = new MidiInterface;
	instance_.reset(out);
	return *out;
}

MidiInterface::MidiInterface() : hasOpenInputPort_(false) {}

MidiInterface::~MidiInterface() = default;

std::string MidiInterface::currentApiName() const
{
	RtMidi::Api api = inputClient_ ? inputClient_->getCurrentApi()
								   : RtMidi::RTMIDI_DUMMY;
	return RtMidi::getApiDisplayName(api);
}

std::vector<std::string> MidiInterface::getAvailableApis() const
{
	std::vector<RtMidi::Api> apis;
	RtMidi::getCompiledApi(apis);
	std::vector<std::string> list;
	for (const auto& apiAvailable : apis)
		list.push_back(RtMidi::getApiDisplayName(apiAvailable));
	return (list.empty() ? std::vector<std::string>({ "" }) : list);
}

bool MidiInterface::isAvailableApi(const std::string& api) const
{
	const std::vector<std::string> apis = getAvailableApis();
	return (utils::find(apis, api) != apis.end());
}

bool MidiInterface::switchApi(std::string api, std::string* errDetail)
{
	std::vector<RtMidi::Api> apis;
	RtMidi::getCompiledApi(apis);

	for (const auto& apiAvailable : apis) {
		if (api == RtMidi::getApiDisplayName(apiAvailable)) {
			if (inputClient_ && apiAvailable == inputClient_->getCurrentApi())
				return true;

			RtMidiIn *inputClient = nullptr;
			try {
				inputClient = new RtMidiIn(apiAvailable, MIDI_INP_CLIENT_NAME, MIDI_BUFFER_SIZE);
				if (errDetail) *errDetail = "";
			}
			catch (RtMidiError &error) {
				error.printMessage();
				if (errDetail) *errDetail = error.getMessage();
			}

			if (inputClient) {
				inputClient->ignoreTypes(MIDI_INP_IGNORE_SYSEX, MIDI_INP_IGNORE_TIME, MIDI_INP_IGNORE_SENSE);
				inputClient->setCallback(&onMidiInput, this);
			}
			inputClient_.reset(inputClient);
			hasOpenInputPort_ = false;

			return (inputClient != nullptr);
		}
	}

	inputClient_ = nullptr;
	if (errDetail) *errDetail = "No available midi api.";
	return false;
}

bool MidiInterface::supportsVirtualPort() const
{
	if (!inputClient_) return false;

	switch (inputClient_->getCurrentApi()) {
	case RtMidi::MACOSX_CORE: case RtMidi::LINUX_ALSA: case RtMidi::UNIX_JACK:
		return true;
	default:
		return false;
	}
}

bool MidiInterface::supportsVirtualPort(std::string api) const
{
	std::vector<RtMidi::Api> apis;
	RtMidi::getCompiledApi(apis);
	RtMidi::Api apiType = RtMidi::RTMIDI_DUMMY;
	for (const auto& apiAvailable : apis) {
		if (api == RtMidi::getApiDisplayName(apiAvailable)) {
			apiType = apiAvailable;
			break;
		}
	}

	switch (apiType) {
	case RtMidi::MACOSX_CORE: case RtMidi::LINUX_ALSA: case RtMidi::UNIX_JACK:
		return true;
	default:
		return false;
	}
}

std::vector<std::string> MidiInterface::getRealInputPorts()
{
	if (!inputClient_) return { "" };	// Error

	RtMidiIn &client = *inputClient_;
	unsigned count = client.getPortCount();

	std::vector<std::string> ports;
	ports.reserve(count);

	for (unsigned i = 0; i < count; ++i)
		ports.push_back(client.getPortName(i));
	return ports;
}

std::vector<std::string> MidiInterface::getRealInputPorts(const std::string& api)
{
	std::vector<RtMidi::Api> apis;
	RtMidi::getCompiledApi(apis);

	RtMidi::Api apiType = RtMidi::RTMIDI_DUMMY;
	for (const auto& apiAvailable : apis) {
		if (api == RtMidi::getApiDisplayName(apiAvailable)) {
			apiType = apiAvailable;
			break;
		}
	}
	if (apiType == RtMidi::RTMIDI_DUMMY) {
		return { "" };	// Error
	}

	std::vector<std::string> ports;
	try {
		auto client = std::make_unique<RtMidiIn>(apiType);
		unsigned count = client->getPortCount();
		ports.reserve(count);
		for (unsigned i = 0; i < count; ++i)
			ports.push_back(client->getPortName(i));
	}
	catch (RtMidiError& error) {
		error.printMessage();
	}
	return ports;
}

void MidiInterface::closeInputPort()
{
	if (!inputClient_) return;

	RtMidiIn &client = *inputClient_;
	if (hasOpenInputPort_) {
		client.closePort();
		hasOpenInputPort_ = false;
	}
}

bool MidiInterface::openInputPort(unsigned port, std::string* errDetail)
{
	if (!inputClient_) {
		if (errDetail) *errDetail = "Not opened input client.";
		hasOpenInputPort_ = false;
		return false;
	}

	try {
		RtMidiIn &client = *inputClient_;
		closeInputPort();

		if (port == ~0u) {
			client.openVirtualPort(MIDI_INP_PORT_NAME);
			hasOpenInputPort_ = true;
		}
		else {
			client.openPort(port, MIDI_INP_PORT_NAME);
			hasOpenInputPort_ = client.isPortOpen();
		}
		if (errDetail) *errDetail = "";
		return true;
	}
	catch (RtMidiError& error) {
		if (errDetail) *errDetail = error.getMessage();
		hasOpenInputPort_ = false;
		return false;
	}
}

bool MidiInterface::openInputPortByName(const std::string &portName, std::string* errDetail)
{
	std::vector<std::string> ports = getRealInputPorts();

	for (unsigned i = 0, n = ports.size(); i < n; ++i) {
		if (ports[i] == portName) {
			return openInputPort(i, errDetail);;
		}
	}

	if (errDetail) *errDetail = "There is no port such the name.";
	return false;
}

void MidiInterface::installInputHandler(InputHandler* handler, void* user_data)
{
	std::lock_guard<std::mutex> lock(inputHandlersMutex_);
	inputHandlers_.push_back(std::make_pair(handler, user_data));
}

void MidiInterface::uninstallInputHandler(InputHandler* handler, void* user_data)
{
	std::lock_guard<std::mutex> lock(inputHandlersMutex_);
	for (size_t i = 0, n = inputHandlers_.size(); i < n; ++i) {
		bool match = inputHandlers_[i].first == handler &&
					 inputHandlers_[i].second == user_data;
		if (match) {
			inputHandlers_.erase(inputHandlers_.begin() + i);
			return;
		}
	}
}

void MidiInterface::onMidiInput(double timestamp, std::vector<unsigned char> *message, void *user_data)
{
	MidiInterface *self = reinterpret_cast<MidiInterface *>(user_data);

	std::unique_lock<std::mutex> lock(self->inputHandlersMutex_, std::try_to_lock);
	if (!lock.owns_lock())
		return;

	const uint8_t *msg = message->data();
	size_t len = message->size();

	for (size_t i = 0, n = self->inputHandlers_.size(); i < n; ++i)
		self->inputHandlers_[i].first(timestamp, msg, len, self->inputHandlers_[i].second);
}
