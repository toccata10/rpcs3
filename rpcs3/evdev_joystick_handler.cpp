// This makes debugging on windows less painful
//#define HAVE_LIBEVDEV

#ifdef HAVE_LIBEVDEV

#include "rpcs3qt/pad_settings_dialog.h"
#include "evdev_joystick_handler.h"
#include "Utilities/Thread.h"
#include "Utilities/Log.h"

#include <functional>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <cstdio>
#include <cmath>

evdev_joystick_handler::evdev_joystick_handler()
{
	// Define border values
	thumb_min = 0;
	thumb_max = 255;
	trigger_min = 0;
	trigger_max = 255;
	vibration_min = 0;
	vibration_max = 65535;

	// Set this handler's type and save location
	m_pad_config.cfg_type = "evdev";
	m_pad_config.cfg_name = fs::get_config_dir() + "/config_evdev.yml";

	// Set default button mapping
	m_pad_config.ls_left.def  = rev_axis_list.at(ABS_X);
	m_pad_config.ls_down.def  = axis_list.at(ABS_Y);
	m_pad_config.ls_right.def = axis_list.at(ABS_X);
	m_pad_config.ls_up.def    = rev_axis_list.at(ABS_Y);
	m_pad_config.rs_left.def  = rev_axis_list.at(ABS_RX);
	m_pad_config.rs_down.def  = axis_list.at(ABS_RY);
	m_pad_config.rs_right.def = axis_list.at(ABS_RX);
	m_pad_config.rs_up.def    = rev_axis_list.at(ABS_RY);
	m_pad_config.start.def    = button_list.at(BTN_START);
	m_pad_config.select.def   = button_list.at(BTN_SELECT);
	m_pad_config.ps.def       = button_list.at(BTN_MODE);
	m_pad_config.square.def   = button_list.at(BTN_X);
	m_pad_config.cross.def    = button_list.at(BTN_A);
	m_pad_config.circle.def   = button_list.at(BTN_B);
	m_pad_config.triangle.def = button_list.at(BTN_Y);
	m_pad_config.left.def     = rev_axis_list.at(ABS_HAT0X);
	m_pad_config.down.def     = axis_list.at(ABS_HAT0Y);
	m_pad_config.right.def    = axis_list.at(ABS_HAT0X);
	m_pad_config.up.def       = rev_axis_list.at(ABS_HAT0Y);
	m_pad_config.r1.def       = button_list.at(BTN_TR);
	m_pad_config.r2.def       = axis_list.at(ABS_RZ);
	m_pad_config.r3.def       = button_list.at(BTN_THUMBR);
	m_pad_config.l1.def       = button_list.at(BTN_TL);
	m_pad_config.l2.def       = axis_list.at(ABS_Z);
	m_pad_config.l3.def       = button_list.at(BTN_THUMBL);

	// Set default misc variables
	m_pad_config.lstickdeadzone.def = 30;   // between 0 and 255
	m_pad_config.rstickdeadzone.def = 30;   // between 0 and 255
	m_pad_config.ltriggerthreshold.def = 0; // between 0 and 255
	m_pad_config.rtriggerthreshold.def = 0; // between 0 and 255
	m_pad_config.padsquircling.def = 5000;
	m_pad_config.axis_positive_only.def = false; //xbox360 gamepads send values from -32767 to +32768, but some gamepads send values from 0 to 255

	// apply defaults
	m_pad_config.from_default();

	// set capabilities
	b_has_config = true;
	b_has_rumble = true;
	b_has_deadzones = true;

	m_trigger_threshold = trigger_max / 2;
	m_thumb_threshold = thumb_max / 2;
}

evdev_joystick_handler::~evdev_joystick_handler()
{
	Close();
}

bool evdev_joystick_handler::Init()
{
	m_pad_config.load();
	return true;
}

bool evdev_joystick_handler::update_device(EvdevDevice& device, bool use_cell)
{
	std::shared_ptr<Pad> pad = device.pad;
	const auto& path = device.path;
	libevdev*& dev = device.device;

	bool was_connected = dev != nullptr;

	if (access(path.c_str(), R_OK) == -1)
	{
		if (was_connected)
		{
			// It was disconnected.
			if (use_cell)
				pad->m_port_status |= CELL_PAD_STATUS_ASSIGN_CHANGES;

			int fd = libevdev_get_fd(dev);
			libevdev_free(dev);
			close(fd);
			dev = nullptr;
		}

		if (use_cell)
			pad->m_port_status &= ~CELL_PAD_STATUS_CONNECTED;

		LOG_ERROR(GENERAL, "Joystick %s is not present or accessible [previous status: %d]", path.c_str(), was_connected ? 1 : 0);
		return false;
	}

	if (was_connected)
		return true;  // It's already been connected, and the js is still present.

	int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
	if (fd == -1)
	{
		int err = errno;
		LOG_ERROR(GENERAL, "Failed to open joystick: %s [errno %d]", strerror(err), err);
		return false;
	}

	int ret = libevdev_new_from_fd(fd, &dev);
	if (ret < 0)
	{
		LOG_ERROR(GENERAL, "Failed to initialize libevdev for joystick: %s [errno %d]", strerror(-ret), -ret);
		return false;
	}

	LOG_NOTICE(GENERAL, "Opened joystick: '%s' at %s (fd %d)", libevdev_get_name(dev), path, fd);

	if (use_cell)
	{
		// Connection status changed from disconnected to connected.
		pad->m_port_status |= CELL_PAD_STATUS_ASSIGN_CHANGES;
		pad->m_port_status |= CELL_PAD_STATUS_CONNECTED;
	}

	return true;
}

void evdev_joystick_handler::update_devs(bool use_cell)
{
	for (auto& device : devices)
	{
		update_device(device, use_cell);
	}
}

void evdev_joystick_handler::Close()
{
	for (auto& device : devices)
	{
		auto& dev = device.device;
		if (dev != nullptr)
		{
			int fd = libevdev_get_fd(dev);
			if (device.effect_id != -1)
				ioctl(fd, EVIOCRMFF, device.effect_id);
			libevdev_free(dev);
			close(fd);
		}
	}
}

std::unordered_map<u64, std::pair<u16, bool>> evdev_joystick_handler::GetButtonValues(const EvdevDevice& device)
{
	std::unordered_map<u64, std::pair<u16, bool>> button_values;
	auto& dev = device.device;

	for (auto entry : button_list)
	{
		auto code = entry.first;
		int val = 0;
		if (libevdev_fetch_event_value(dev, EV_KEY, code, &val) == 0)
			continue;

		button_values.emplace(code, std::make_pair<u16, bool>(static_cast<u16>(val > 0 ? 255 : 0), false));
	}

	for (auto entry : axis_list)
	{
		auto code = entry.first;
		int val = 0;
		if (libevdev_fetch_event_value(dev, EV_ABS, code, &val) == 0)
			continue;

		int min = libevdev_get_abs_minimum(dev, code);
		int max = libevdev_get_abs_maximum(dev, code);

		// Triggers do not need handling of negative values
		// but some custom stick axis only have a positive range
		// (0 to 255) instead of (-32767 to +32768)
		if (min >= 0 && !m_pad_config.axis_positive_only)
		{
			float fvalue = ScaleStickInput(val, min, max);
			button_values.emplace(code, std::make_pair<u16, bool>(static_cast<u16>(fvalue), false));
			continue;
		}

		float fvalue = ScaleStickInput2(val, min, max);
		if (fvalue < 0)
			button_values.emplace(code, std::make_pair<u16, bool>(static_cast<u16>(std::abs(fvalue)), true));
		else
			button_values.emplace(code, std::make_pair<u16, bool>(static_cast<u16>(fvalue), false));
	}

	return button_values;
}

evdev_joystick_handler::EvdevDevice* evdev_joystick_handler::get_device(const std::string& device)
{
	// Add device if not yet present
	m_pad_index = add_device(device, true);
	if (m_pad_index < 0)
		return nullptr;

	EvdevDevice& dev = devices[m_pad_index];

	// Check if our device is connected
	if (!update_device(dev, false))
		return nullptr;

	return &dev;
}

void evdev_joystick_handler::GetNextButtonPress(const std::string& padId, const std::function<void(u16, std::string, int[])>& callback, bool get_blacklist, std::vector<std::string> buttons)
{
	if (get_blacklist)
		blacklist.clear();

	// Get our evdev device
	EvdevDevice* device = get_device(padId);
	libevdev* dev = device->device;
	if (dev == nullptr)
		return;

	// Try to query the latest event from the joystick.
	input_event evt;
	int ret = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &evt);

	// Grab any pending sync event.
	if (ret == LIBEVDEV_READ_STATUS_SYNC)
		ret = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_SYNC, &evt);

	// return if nothing new has happened. ignore this to get the current state for blacklist
	if (!get_blacklist && ret < 0)
		return;

	auto data = GetButtonValues(*device);
	std::pair<u16, std::string> pressed_button = { 0, "" };

	for (const auto& button : button_list)
	{
		int code = button.first;
		std::string name = button.second;

		// Handle annoying useless buttons
		if (padId.find("Xbox 360") != std::string::npos && code >= BTN_TRIGGER_HAPPY)
			continue;
		if (padId.find("Sony") != std::string::npos && (code == BTN_TL2 || code == BTN_TR2))
			continue;

		if (!get_blacklist && std::find(blacklist.begin(), blacklist.end(), name) != blacklist.end())
			continue;

		u16 value = data[code].first;
		if (value > 0)
		{
			if (get_blacklist)
			{
				blacklist.emplace_back(name);
				LOG_ERROR(HLE, "Evdev Calibration: Added button [ %d = %s ] to blacklist. Value = %d", code, name, value);
			}
			else if (value > pressed_button.first)
				pressed_button = { value, name };
		}
	}

	for (const auto& button : axis_list)
	{
		int code = button.first;
		std::string name = button.second;

		if (data[code].second)
			continue;

		if (!get_blacklist && std::find(blacklist.begin(), blacklist.end(), name) != blacklist.end())
			continue;

		u16 value = data[code].first;
		if (value > 0 && value >= m_thumb_threshold)
		{
			if (get_blacklist)
			{
				int min = libevdev_get_abs_minimum(dev, code);
				int max = libevdev_get_abs_maximum(dev, code);
				blacklist.emplace_back(name);
				LOG_ERROR(HLE, "Evdev Calibration: Added axis [ %d = %s ] to blacklist. [ Value = %d ] [ Min = %d ] [ Max = %d ]", code, name, value, min, max);
			}
			else if (value > pressed_button.first)
				pressed_button = { value, name };
		}
	}

	for (const auto& button : rev_axis_list)
	{
		int code = button.first;
		std::string name = button.second;

		if (!data[code].second)
			continue;

		if (!get_blacklist && std::find(blacklist.begin(), blacklist.end(), name) != blacklist.end())
			continue;

		u16 value = data[code].first;
		if (value > 0 && value >= m_thumb_threshold)
		{
			if (get_blacklist)
			{
				int min = libevdev_get_abs_minimum(dev, code);
				int max = libevdev_get_abs_maximum(dev, code);
				blacklist.emplace_back(name);
				LOG_ERROR(HLE, "Evdev Calibration: Added rev axis [ %d = %s ] to blacklist. [ Value = %d ] [ Min = %d ] [ Max = %d ]", code, name, value, min, max);
			}
			else if (value > pressed_button.first)
				pressed_button = { value, name };
		}
	}

	if (get_blacklist)
	{
		if (blacklist.size() <= 0)
			LOG_SUCCESS(HLE, "Evdev Calibration: Blacklist is clear. No input spam detected");
		return;
	}

	auto find_value = [=](const std::string& name)
	{
		int key = FindKeyCodeByString(rev_axis_list, name, false);
		bool dir = key >= 0;
		if (key < 0)
			key = FindKeyCodeByString(axis_list, name, false);
		if (key < 0)
			key = FindKeyCodeByString(button_list, name);
		auto it = data.find(static_cast<u64>(key));
		return it != data.end() && dir == it->second.second ? it->second.first : 0;
	};

	int preview_values[6] =
	{
		find_value(buttons[0]),                          // Left Trigger
		find_value(buttons[1]),                          // Right Trigger
		find_value(buttons[3]) - find_value(buttons[2]), // Left Stick X
		find_value(buttons[5]) - find_value(buttons[4]), // Left Stick Y
		find_value(buttons[7]) - find_value(buttons[6]), // Right Stick X
		find_value(buttons[9]) - find_value(buttons[8]), // Right Stick Y
	};

	if (pressed_button.first > 0)
		return callback(pressed_button.first, pressed_button.second, preview_values);
	else
		return callback(0, "", preview_values);
}

// https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/InputCommon/ControllerInterface/evdev/evdev.cpp
// https://github.com/reicast/reicast-emulator/blob/master/core/linux-dist/evdev.cpp
// http://www.infradead.org/~mchehab/kernel_docs_pdf/linux-input.pdf
void evdev_joystick_handler::SetRumble(EvdevDevice* device, u16 large, u16 small)
{
	if (device == nullptr || !device->has_rumble || device->effect_id == -2)
		return;

	int fd = libevdev_get_fd(device->device);
	if (fd < 0)
		return;

	if (large == device->force_large && small == device->force_small)
		return;

	// XBox One Controller can't handle faster vibration updates than ~10ms. Elite is even worse.
	// So I'll use 20ms to be on the safe side. No lag was noticable.
	if (clock() - device->last_vibration < 20)
		return;

	device->last_vibration = clock();

	// delete the previous effect (which also stops it)
	if (device->effect_id != -1)
	{
		ioctl(fd, EVIOCRMFF, device->effect_id);
		device->effect_id = -1;
	}

	if (large == 0 && small == 0)
	{
		device->force_large = large;
		device->force_small = small;
		return;
	}

	ff_effect effect;

	if (libevdev_has_event_code(device->device, EV_FF, FF_RUMBLE))
	{
		effect.type = FF_RUMBLE;
		effect.id = device->effect_id;
		effect.direction = 0;
		effect.u.rumble.strong_magnitude = large;
		effect.u.rumble.weak_magnitude = small;
		effect.replay.length = 0;
		effect.replay.delay = 0;
		effect.trigger.button = 0;
		effect.trigger.interval = 0;
	}
	else
	{
		// TODO: handle other Rumble effects
		device->effect_id = -2;
		return;
	}

	if (ioctl(fd, EVIOCSFF, &effect) == -1)
	{
		LOG_ERROR(HLE, "evdev SetRumble ioctl failed! [large = %d] [small = %d] [fd = %d]", large, small, fd);
		device->effect_id = -2;
	}

	device->effect_id = effect.id;

	input_event play;
	play.type = EV_FF;
	play.code = device->effect_id;
	play.value = 1;

	if (write(fd, &play, sizeof(play)) == -1)
	{
		LOG_ERROR(HLE, "evdev SetRumble write failed! [large = %d] [small = %d] [fd = %d] [effect_id = %d]", large, small, fd, device->effect_id);
		device->effect_id = -2;
	}

	device->force_large = large;
	device->force_small = small;
}

void evdev_joystick_handler::TestVibration(const std::string& padId, u32 largeMotor, u32 smallMotor)
{
	// Get our evdev device
	EvdevDevice* dev = get_device(padId);
	if (dev == nullptr)
	{
		LOG_ERROR(HLE, "evdev TestVibration: Device [%s] not found! [largeMotor = %d] [smallMotor = %d]", padId, largeMotor, smallMotor);
		return;
	}

	if (!dev->has_rumble)
	{
		LOG_ERROR(HLE, "evdev TestVibration: Device [%s] does not support rumble features! [largeMotor = %d] [smallMotor = %d]", padId, largeMotor, smallMotor);
		return;
	}

	SetRumble(dev, largeMotor, smallMotor);
}

void evdev_joystick_handler::TranslateButtonPress(u64 keyCode, bool& pressed, u16& value, bool ignore_threshold)
{
	// Update the pad button values based on their type and thresholds.
	// With this you can use axis or triggers as buttons or vice versa
	u32 code = static_cast<u32>(keyCode);
	auto checkButton = [&](const EvdevButton& b)
	{
		return b.code == code && b.type == m_dev.cur_type && b.dir == m_dev.cur_dir;
	};
	auto checkButtons = [&](const std::vector<EvdevButton>& b)
	{
		return std::find_if(b.begin(), b.end(), checkButton) != b.end();
	};

	if (checkButton(m_dev.trigger_left))
	{
		pressed = value > m_pad_config.ltriggerthreshold;
		value = pressed ? NormalizeTriggerInput(value, m_pad_config.ltriggerthreshold) : 0;
	}
	else if (checkButton(m_dev.trigger_right))
	{
		pressed = value > m_pad_config.rtriggerthreshold;
		value = pressed ? NormalizeTriggerInput(value, m_pad_config.rtriggerthreshold) : 0;
	}
	else if (checkButtons(m_dev.axis_left))
	{
		pressed = value > (ignore_threshold ? 0 : m_pad_config.lstickdeadzone);
		value = pressed ? NormalizeStickInput(value, m_pad_config.lstickdeadzone, ignore_threshold) : 0;
	}
	else if (checkButtons(m_dev.axis_right))
	{
		pressed = value > (ignore_threshold ? 0 : m_pad_config.rstickdeadzone);
		value = pressed ? NormalizeStickInput(value, m_pad_config.rstickdeadzone, ignore_threshold) : 0;
	}
	else // normal button (should in theory also support sensitive buttons)
	{
		pressed = value > 0;
		value = pressed ? value : 0;
	}
}

int evdev_joystick_handler::GetButtonInfo(const input_event& evt, const EvdevDevice& device, int& value)
{
	int code = evt.code;
	int val = evt.value;
	m_is_button_or_trigger = false;

	switch (evt.type)
	{
	case EV_KEY:
	{
		m_is_button_or_trigger = true;

		// get the button value and return its code
		if (code < BTN_MISC)
			return -1;

		value = val > 0 ? 255 : 0;
		return code;
	}
	case EV_ABS:
	{
		auto& dev = device.device;
		int min = libevdev_get_abs_minimum(dev, code);
		int max = libevdev_get_abs_maximum(dev, code);

		// Triggers do not need handling of negative values
		// but some custom stick axis only have a positive range
		// (0 to 255) instead of (-32767 to +32768)
		if (min >= 0 && !m_pad_config.axis_positive_only)
		{
			m_is_negative = false;
			m_is_button_or_trigger = true;
			value = static_cast<u16>(ScaleStickInput(val, min, max));
			return code;
		}

		float fvalue = ScaleStickInput2(val, min, max);
		m_is_negative = fvalue < 0;
		value = static_cast<u16>(std::abs(fvalue));
		return code;
	}
	default:
		return -1;
	}
}

std::vector<std::string> evdev_joystick_handler::ListDevices()
{
	Init();
	std::vector<std::string> evdev_joystick_list;
	fs::dir devdir{"/dev/input/"};
	fs::dir_entry et;

	while (devdir.read(et))
	{
		// Check if the entry starts with event (a 5-letter word)
		if (et.name.size() > 5 && et.name.compare(0, 5,"event") == 0)
		{
			int fd = open(("/dev/input/" + et.name).c_str(), O_RDWR | O_NONBLOCK);
			struct libevdev *dev = NULL;
			int rc = libevdev_new_from_fd(fd, &dev);
			if (rc < 0)
			{
				// If it's just a bad file descriptor, don't bother logging, but otherwise, log it.
				if (rc != -9)
					LOG_WARNING(GENERAL, "Failed to connect to device at %s, the error was: %s", "/dev/input/" + et.name, strerror(-rc));
				libevdev_free(dev);
				close(fd);
				continue;
			}
			if (libevdev_has_event_type(dev, EV_KEY) &&
				libevdev_has_event_code(dev, EV_ABS, ABS_X) &&
				libevdev_has_event_code(dev, EV_ABS, ABS_Y))
			{
				// It's a joystick.
				evdev_joystick_list.push_back(libevdev_get_name(dev));
			}
			libevdev_free(dev);
			close(fd);
		}
	}
	return evdev_joystick_list;
}

int evdev_joystick_handler::add_device(const std::string& device, bool in_settings)
{
	if (in_settings && m_pad_index >= 0)
		return m_pad_index;

	// Now we need to find the device with the same name, and make sure not to grab any duplicates.
	fs::dir devdir{ "/dev/input/" };
	fs::dir_entry et;
	while (devdir.read(et))
	{
		// Check if the entry starts with event (a 5-letter word)
		if (et.name.size() > 5 && et.name.compare(0, 5, "event") == 0)
		{
			std::string path = "/dev/input/" + et.name;
			int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
			struct libevdev *dev = NULL;
			int rc = libevdev_new_from_fd(fd, &dev);
			if (rc < 0)
			{
				// If it's just a bad file descriptor, don't bother logging, but otherwise, log it.
				if (rc != -9)
					LOG_WARNING(GENERAL, "Failed to connect to device at %s, the error was: %s", path, strerror(-rc));
				libevdev_free(dev);
				close(fd);
				continue;
			}
			const std::string name = libevdev_get_name(dev);
			if (libevdev_has_event_type(dev, EV_KEY) &&
				libevdev_has_event_code(dev, EV_ABS, ABS_X) &&
				libevdev_has_event_code(dev, EV_ABS, ABS_Y) &&
				name == device)
			{
				// It's a joystick. Now let's make sure we don't already have this one.
				auto it = std::find_if(devices.begin(), devices.end(), [&path](const EvdevDevice &device) { return path == device.path; });
				if (it != devices.end())
				{
					libevdev_free(dev);
					close(fd);
					continue;
				}

				// Alright, now that we've confirmed we haven't added this joystick yet, les do dis.
				m_dev.device = dev;
				m_dev.path = path;
				m_dev.has_rumble = libevdev_has_event_type(dev, EV_FF);
				devices.push_back(m_dev);
				return devices.size() - 1;
			}
			libevdev_free(dev);
			close(fd);
		}
	}
	return -1;
}

void evdev_joystick_handler::ThreadProc()
{
	update_devs();

	int padnum = 0;
	for (auto& device : devices)
	{
		m_dev = device;
		auto pad = device.pad;
		auto axis_orientations = device.axis_orientations;
		auto& dev = device.device;
		if (dev == nullptr)
		{
			if (last_connection_status[padnum] == true)
			{
				LOG_ERROR(HLE, "evdev device %d disconnected", padnum);
				last_connection_status[padnum] = false;
				connected--;
			}
			padnum++;
			continue;
		}

		if (last_connection_status[padnum] == false)
		{
			LOG_ERROR(HLE, "evdev device %d reconnected", padnum);
			last_connection_status[padnum] = true;
			connected++;
		}
		padnum++;

		// Handle vibration
		int idx_l = m_pad_config.switch_vibration_motors ? 1 : 0;
		int idx_s = m_pad_config.switch_vibration_motors ? 0 : 1;
		u16 force_large = m_pad_config.enable_vibration_motor_large ? pad->m_vibrateMotors[idx_l].m_value * 257 : vibration_min;
		u16 force_small = m_pad_config.enable_vibration_motor_small ? pad->m_vibrateMotors[idx_s].m_value * 257 : vibration_min;
		SetRumble(&device, force_large, force_small);

		// Try to query the latest event from the joystick.
		input_event evt;
		int ret = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &evt);

		// Grab any pending sync event.
		if (ret == LIBEVDEV_READ_STATUS_SYNC)
		{
			LOG_NOTICE(GENERAL, "Captured sync event");
			ret = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_SYNC, &evt);
		}

		if (ret < 0)
		{
			// -EAGAIN signifies no available events, not an actual *error*.
			if (ret != -EAGAIN)
				LOG_ERROR(GENERAL, "Failed to read latest event from joystick: %s [errno %d]", strerror(-ret), -ret);
			continue;
		}

		m_dev.cur_type = evt.type;

		int value;
		int button_code = GetButtonInfo(evt, device, value);
		if (button_code < 0 || value < 0)
			continue;

		// Translate any corresponding keycodes to our normal DS3 buttons and triggers
		for (int i = 0; i < static_cast<int>(pad->m_buttons.size() - 1); i++) // skip reserved button
		{
			if (pad->m_buttons[i].m_keyCode != button_code)
				continue;

			// Be careful to handle mapped axis specially
			if (evt.type == EV_ABS)
			{
				// get axis direction and skip on error or set to 0 if the stick/hat is actually pointing to the other direction.
				// maybe mimic on error, needs investigation. FindAxisDirection should ideally never return -1 anyway
				int direction = FindAxisDirection(axis_orientations, i);
				m_dev.cur_dir = direction;

				if (direction < 0)
				{
					LOG_ERROR(HLE, "FindAxisDirection = %d, Button Nr.%d, value = %d", direction, i, value);
					continue;
				}
				else if (direction != (m_is_negative ? 1 : 0))
				{
					pad->m_buttons[i].m_value = 0;
					pad->m_buttons[i].m_pressed = 0;
					continue;
				}
			}

			pad->m_buttons[i].m_value = static_cast<u16>(value);
			TranslateButtonPress(button_code, pad->m_buttons[i].m_pressed, pad->m_buttons[i].m_value);
		}

		// Translate any corresponding keycodes to our two sticks. (ignoring thresholds for now)
		for (int idx = 0; idx < static_cast<int>(pad->m_sticks.size()); idx++)
		{
			bool pressed_min = false, pressed_max = false;

			// m_keyCodeMin is the mapped key for left or down
			if (pad->m_sticks[idx].m_keyCodeMin == button_code)
			{
				bool is_direction_min = false;

				if (!m_is_button_or_trigger && evt.type == EV_ABS)
				{
					int index = BUTTON_COUNT + (idx * 2) + 1;
					int min_direction = FindAxisDirection(axis_orientations, index);
					m_dev.cur_dir = min_direction;

					if (min_direction < 0)
						LOG_ERROR(HLE, "keyCodeMin FindAxisDirection = %d, Axis Nr.%d, Button Nr.%d, value = %d", min_direction, idx, index, value);
					else
						is_direction_min = m_is_negative == (min_direction == 1);
				}

				if (m_is_button_or_trigger || is_direction_min)
				{
					device.val_min[idx] = value;
					TranslateButtonPress(button_code, pressed_min, device.val_min[idx], true);
				}
				else // set to 0 to avoid remnant counter axis values
					device.val_min[idx] = 0;
			}

			// m_keyCodeMax is the mapped key for right or up
			if (pad->m_sticks[idx].m_keyCodeMax == button_code)
			{
				bool is_direction_max = false;

				if (!m_is_button_or_trigger && evt.type == EV_ABS)
				{
					int index = BUTTON_COUNT + (idx * 2);
					int max_direction = FindAxisDirection(axis_orientations, index);
					m_dev.cur_dir = max_direction;

					if (max_direction < 0)
						LOG_ERROR(HLE, "keyCodeMax FindAxisDirection = %d, Axis Nr.%d, Button Nr.%d, value = %d", max_direction, idx, index, value);
					else
						is_direction_max = m_is_negative == (max_direction == 1);
				}

				if (m_is_button_or_trigger || is_direction_max)
				{
					device.val_max[idx] = value;
					TranslateButtonPress(button_code, pressed_max, device.val_max[idx], true);
				}
				else // set to 0 to avoid remnant counter axis values
					device.val_max[idx] = 0;
			}

			// cancel out opposing values and get the resulting difference. if there was no change, use the old value.
			device.stick_val[idx] = device.val_max[idx] - device.val_min[idx];
		}

		// Normalize our two stick's axis based on the thresholds
		u16 lx, ly, rx, ry;

		// Normalize our two stick's axis based on the thresholds
		std::tie(lx, ly) = NormalizeStickDeadzone(device.stick_val[0], device.stick_val[1], m_pad_config.lstickdeadzone);
		std::tie(rx, ry) = NormalizeStickDeadzone(device.stick_val[2], device.stick_val[3], m_pad_config.rstickdeadzone);

		if (m_pad_config.padsquircling != 0)
		{
			std::tie(lx, ly) = ConvertToSquirclePoint(lx, ly, m_pad_config.padsquircling);
			std::tie(rx, ry) = ConvertToSquirclePoint(rx, ry, m_pad_config.padsquircling);
		}

		pad->m_sticks[0].m_value = lx;
		pad->m_sticks[1].m_value = 255 - ly;
		pad->m_sticks[2].m_value = rx;
		pad->m_sticks[3].m_value = 255 - ry;
	}
}

// Search axis_orientations map for the direction by index, returns -1 if not found, 0 for positive and 1 for negative
int evdev_joystick_handler::FindAxisDirection(const std::unordered_map<int, bool>& map, int index)
{
	auto it = map.find(index);
	if (it == map.end())
		return -1;
	else
		return it->second;
};

bool evdev_joystick_handler::bindPadToDevice(std::shared_ptr<Pad> pad, const std::string& device)
{
	Init();

	std::unordered_map<int, bool> axis_orientations;
	int i = 0; // increment to know the axis location (17-24). Be careful if you ever add more find_key() calls in here (BUTTON_COUNT = 17)
	int last_type = EV_ABS;

	auto find_key = [&](const cfg::string& name)
	{
		int type = EV_ABS;
		int key = FindKeyCode(axis_list, name, false);
		if (key >= 0)
			axis_orientations.emplace(i, false);

		if (key < 0)
		{
			key = FindKeyCode(rev_axis_list, name, false);
			if (key >= 0)
				axis_orientations.emplace(i, true);
		}

		if (key < 0)
		{
			key = FindKeyCode(button_list, name);
			type = EV_KEY;
		}

		last_type = type;
		i++;
		return static_cast<u32>(key);
	};

	auto evdevbutton = [&](const cfg::string& name)
	{
		int index = i;
		EvdevButton button;
		button.code = find_key(name);
		button.type = last_type;
		button.dir = axis_orientations[index];
		return button;
	};

	pad->Init
	(
		CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES,
		CELL_PAD_SETTING_PRESS_OFF | CELL_PAD_SETTING_SENSOR_OFF,
		CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE | CELL_PAD_CAPABILITY_HP_ANALOG_STICK | CELL_PAD_CAPABILITY_ACTUATOR | CELL_PAD_CAPABILITY_SENSOR_MODE,
		CELL_PAD_DEV_TYPE_STANDARD
	);

	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, find_key(m_pad_config.triangle), CELL_PAD_CTRL_TRIANGLE);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, find_key(m_pad_config.circle),   CELL_PAD_CTRL_CIRCLE);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, find_key(m_pad_config.cross),    CELL_PAD_CTRL_CROSS);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, find_key(m_pad_config.square),   CELL_PAD_CTRL_SQUARE);

	m_dev.trigger_left = evdevbutton(m_pad_config.l2);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, m_dev.trigger_left.code,         CELL_PAD_CTRL_L2);

	m_dev.trigger_right = evdevbutton(m_pad_config.r2);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, m_dev.trigger_right.code,        CELL_PAD_CTRL_R2);

	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, find_key(m_pad_config.l1),       CELL_PAD_CTRL_L1);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, find_key(m_pad_config.r1),       CELL_PAD_CTRL_R1);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, find_key(m_pad_config.start),    CELL_PAD_CTRL_START);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, find_key(m_pad_config.select),   CELL_PAD_CTRL_SELECT);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, find_key(m_pad_config.l3),       CELL_PAD_CTRL_L3);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, find_key(m_pad_config.r3),       CELL_PAD_CTRL_R3);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, find_key(m_pad_config.ps),       0x100/*CELL_PAD_CTRL_PS*/);// TODO: PS button support
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, find_key(m_pad_config.up),       CELL_PAD_CTRL_UP);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, find_key(m_pad_config.down),     CELL_PAD_CTRL_DOWN);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, find_key(m_pad_config.left),     CELL_PAD_CTRL_LEFT);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, find_key(m_pad_config.right),    CELL_PAD_CTRL_RIGHT);
	pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, 0, 0x0); // Reserved

	m_dev.axis_left[0]  = evdevbutton(m_pad_config.ls_right);
	m_dev.axis_left[1]  = evdevbutton(m_pad_config.ls_left);
	m_dev.axis_left[2]  = evdevbutton(m_pad_config.ls_up);
	m_dev.axis_left[3]  = evdevbutton(m_pad_config.ls_down);
	m_dev.axis_right[0] = evdevbutton(m_pad_config.rs_right);
	m_dev.axis_right[1] = evdevbutton(m_pad_config.rs_left);
	m_dev.axis_right[2] = evdevbutton(m_pad_config.rs_up);
	m_dev.axis_right[3] = evdevbutton(m_pad_config.rs_down);

	pad->m_sticks.emplace_back(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X,  m_dev.axis_left[1].code,  m_dev.axis_left[0].code);
	pad->m_sticks.emplace_back(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y,  m_dev.axis_left[3].code,  m_dev.axis_left[2].code);
	pad->m_sticks.emplace_back(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, m_dev.axis_right[1].code, m_dev.axis_right[0].code);
	pad->m_sticks.emplace_back(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, m_dev.axis_right[3].code, m_dev.axis_right[2].code);

	pad->m_sensors.emplace_back(CELL_PAD_BTN_OFFSET_SENSOR_X, 512);
	pad->m_sensors.emplace_back(CELL_PAD_BTN_OFFSET_SENSOR_Y, 399);
	pad->m_sensors.emplace_back(CELL_PAD_BTN_OFFSET_SENSOR_Z, 512);
	pad->m_sensors.emplace_back(CELL_PAD_BTN_OFFSET_SENSOR_G, 512);

	pad->m_vibrateMotors.emplace_back(true, 0);
	pad->m_vibrateMotors.emplace_back(false, 0);

	m_dev.pad = pad;
	m_dev.axis_orientations = axis_orientations;

	if (!add_device(device, false))
		LOG_WARNING(HLE, "evdev add_device in bindPadToDevice failed for device %s", device);

	update_devs();
	return true;
}

#endif
