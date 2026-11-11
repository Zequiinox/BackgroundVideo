#include "MenuUi.hpp"
#include "BackgroundVideo.hpp"
#include "Log.hpp"

#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <cstdlib>
#include <cstdio>
#include <string>
#include <string_view>

namespace bgv::menu {
namespace {

constexpr const char* kModule = "bgv.menu_video";
constexpr const char* kAudio = "audio_enabled";
constexpr const char* kVolume = "audio_volume";
bool gRegistered = false;

bool parseBool(std::string_view v) {
    return v == "true" || v == "1" || v == "on" || v == "True";
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    if (key == kAudio) {
        video::setAudioEnabled(parseBool(value));
    } else if (key == kVolume) {
        const std::string s(value);
        video::setAudioVolume(std::strtof(s.c_str(), nullptr));
    }
}

void buildSchema() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder b;
    b.category("audio", "Audio", "Sound of the menu video")
     .defaultCategory("audio");

    ConfigNodeV2 toggle;
    toggle.id = "audio_enabled_node";
    toggle.key = kAudio;
    toggle.category = "audio";
    toggle.title = "Video audio";
    toggle.description = "Play the sound of the panorama video.";
    toggle.type = ConfigControlTypeV2::Toggle;
    toggle.defaultValue = "true";
    toggle.currentValue = video::audioEnabled() ? "true" : "false";
    toggle.hasCurrentValue = true;
    b.node(std::move(toggle));

    ConfigNodeV2 vol;
    vol.id = "audio_volume_node";
    vol.key = kVolume;
    vol.category = "audio";
    vol.title = "Volume";
    vol.type = ConfigControlTypeV2::SliderFloat;
    vol.defaultValue = "1.0";
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", video::audioVolume());
        vol.currentValue = buf;
        vol.hasCurrentValue = true;
    }
    vol.minValue = "0.0";
    vol.maxValue = "1.0";
    vol.step = "0.05";
    vol.enabledWhen.push_back({kAudio, ConfigConditionOpV2::Truthy, ""});
    b.node(std::move(vol));

    if (!setConfigSchemaJson(kModule, b.toJson())) LOGW("config schema rejected");
}

}

void registerMenu(const std::string& modId) {
    if (gRegistered) return;
    using namespace pl::modmenu;
    ModuleBuilder m(kModule, "Menu Video Background");
    m.description("Video panorama on the main menu")
     .modId(modId)
     .defaultEnabled(true)
     .onToggle([](std::string_view, bool enabled) {
         video::setEnabled(enabled);
         if (enabled) video::preload();
     })
     .onConfigChanged(onConfig)
     // v1 fallback entries, same keys
     .config(kAudio, "Video audio", ConfigType::Toggle, "true")
     .config(kVolume, "Volume", ConfigType::SliderFloat, "1.0", "0.0", "1.0");
    if (!m.registerModule()) {
        LOGW("mod menu register failed");
        return;
    }
    gRegistered = true;
    buildSchema();
}

void unregisterMenu() {
    if (!gRegistered) return;
    pl::modmenu::clearConfigSchema(kModule);
    pl::modmenu::unregisterModule(kModule);
    gRegistered = false;
}

}