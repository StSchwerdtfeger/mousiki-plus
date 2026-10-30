// app_audiodevice.cpp
// Part of class App (src/app.h): the CHOOSE AUDIO DEVICE overlay (Settings -> GENERAL -> AUDIO "Choose Audio Device", [ENTER];
// HKeyAudioDevice on the main screen, "°" / "&" by default) and handing the chosen output to the player.
//
// The choice is one value for all modes (config.txt AudioDevice=<name>, "" = the system's default; the radio reads the same line).
// It is applied once per frame by audio_device_apply(): a changed name goes to the player, and a track that plays moves to the new
// device at once (same position, pause state kept). Opened from Settings (drawn over the settings screen)
// and from the main screen alike: the choice is written to config.txt at once (that one line), so the settings screen's s / ESC
// neither save nor undo it.
#include "app.h"
#include "string_util.h"
#include "utf8_util.h"
#include "console_log.h"
#include "overlay_common.h"
#include "app_internal.h"
#include <algorithm>
#include <mutex>

namespace muisc {

std::string App::audio_device_label() const {
    if (settings_.audio_device.empty()) return "System Default";
    return settings_.audio_device;
}

void App::audio_device_open(bool from_settings) {
    adev_from_settings_ = from_settings;
    adev_err_.clear();
    adev_list_ = list_output_devices(&adev_err_);
    // the row that is in use now: 0 = the system default, 1.. = the devices in the system's order
    adev_sel_ = 0;
    for (size_t i = 0; i < adev_list_.size(); ++i)
        if (!settings_.audio_device.empty() && adev_list_[i].name == settings_.audio_device) adev_sel_ = static_cast<int>(i) + 1;
    mode_ = Mode::AudioDevice;
    force_redraw_ = true;
}

void App::audio_device_close() {
    mode_ = adev_from_settings_ ? Mode::Settings : Mode::Browse;
    force_redraw_ = true;
}

void App::audio_device_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int rows = 1 + static_cast<int>(adev_list_.size());
    if (arrow && key == 'A') { adev_sel_ = (adev_sel_ + rows - 1) % rows; return; }
    if (arrow && key == 'B') { adev_sel_ = (adev_sel_ + 1) % rows; return; }
    if (!arrow && (key == 'r' || key == 'R')) {   // look again (a headset that was just plugged in)
        const std::string keep = adev_sel_ > 0 && adev_sel_ <= static_cast<int>(adev_list_.size()) ? adev_list_[adev_sel_ - 1].name : std::string();
        adev_err_.clear();
        adev_list_ = list_output_devices(&adev_err_);
        adev_sel_ = 0;
        for (size_t i = 0; i < adev_list_.size(); ++i) if (!keep.empty() && adev_list_[i].name == keep) adev_sel_ = static_cast<int>(i) + 1;
        status_line_ = "audio devices: " + std::to_string(adev_list_.size()) + " found";
        return;
    }
    if (!arrow && (key == '\r' || key == '\n')) {
        const std::string pick = adev_sel_ > 0 && adev_sel_ <= static_cast<int>(adev_list_.size()) ? adev_list_[adev_sel_ - 1].name : std::string();
        settings_.audio_device = pick;
        audio_device_apply();   // moves a playing track there now
        const std::string now = player_.active_device();
        status_line_ = "audio device: " + audio_device_label();
        if (!pick.empty() && !now.empty() && now != pick) status_line_ += " -- could not be opened, the default device plays";
        set_config_value("AudioDevice", pick);    // saved at once, also from Settings (s / ESC there leave it as it is)
        if (adev_from_settings_) {                // what ESC in Settings goes back to, and no "unsaved changes" for it
            settings_snapshot_.audio_device = pick;
            settings_snapshot_text_ = settings_to_text(settings_snapshot_);
        }
        audio_device_close();
        return;
    }
    std::string act = resolve_hotkey_action(key);
    if (!arrow && (key == 27 || act == "HKeyAudioDevice")) audio_device_close();
}

void App::audio_device_apply() {
    if (settings_.audio_device == adev_applied_) return;
    adev_applied_ = settings_.audio_device;
    player_.set_device(adev_applied_);
    if (!has_track_) return;   // nothing open: the next track opens the new device
    device_run([this] {   // on the device worker, like every opening and closing of the device (WASAPI wants one thread)
        if (!player_.reopen_device()) ConsoleLog::instance().log_basic("audio: no audio device could be opened");
    });
}

std::vector<std::string> App::build_audio_device_panel() const {
    const int W = kAudioDevicePanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    // "> name ..... in use": the cursor, the device and its marks within `inner` columns
    auto entry = [&](bool cursor, const std::string& name, const std::string& right) {
        const std::string l = std::string(cursor ? "> " : "  ");
        const int room = std::max(4, inner - display_width(l) - display_width(right) - 1);
        const std::string n = truncate_str(name, room);
        const int gap = std::max(1, inner - display_width(l) - display_width(n) - display_width(right));
        return l + n + std::string(static_cast<size_t>(gap), ' ') + right;
    };
    const std::string active = player_.active_device();
    std::string def_name;
    for (const AudioOutputDevice& d : adev_list_) if (d.is_default) def_name = d.name;

    std::vector<std::string> lines;
    lines.push_back(box_top("Choose Audio Device", W, border));
    lines.push_back(row("", false));
    {
        const bool chosen = settings_.audio_device.empty();
        lines.push_back(row(entry(adev_sel_ == 0, "System Default" + (def_name.empty() ? std::string() : " (" + def_name + ")"), chosen ? "in use" : ""), adev_sel_ == 0));
    }
    for (size_t i = 0; i < adev_list_.size(); ++i) {
        const int r = static_cast<int>(i) + 1;
        const bool chosen = !settings_.audio_device.empty() && settings_.audio_device == adev_list_[i].name;
        std::string mark = chosen ? "in use" : "";
        if (chosen && !active.empty() && active != adev_list_[i].name) mark = "not opened";
        lines.push_back(row(entry(adev_sel_ == r, adev_list_[i].name, mark), adev_sel_ == r));
    }
    if (!settings_.audio_device.empty()) {   // the chosen device is not there (unplugged): say so instead of hiding it
        bool there = false;
        for (const AudioOutputDevice& d : adev_list_) if (d.name == settings_.audio_device) there = true;
        if (!there) lines.push_back(row("  " + truncate_str(settings_.audio_device, inner - 18) + " (not found)", false));
    }
    if (!adev_err_.empty()) lines.push_back(row("  " + adev_err_, false));
    lines.push_back(row("", false));
    for (const std::string& l : wrap_lines("The choice is the same for the music player, the radio and the audio books. A playing track moves to the new device at once.", inner, 3))
        lines.push_back(row(l, false));
    lines.push_back(row("", false));
    lines.push_back(centred_bottom(settings_, W, "[UP/DOWN] [ENTER] use  [r] look again  [ESC] close", border_bottom));
    return lines;
}

} // namespace muisc
