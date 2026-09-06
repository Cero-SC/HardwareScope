#include "hardwarescope/app_settings.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace hardwarescope {
namespace {

using Values = std::unordered_map<std::string, std::string>;

std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) value.remove_prefix(1U);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1U);
    return value;
}

Values Parse(const std::string& text) {
    Values values;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto line = Trim(std::string_view{text}.substr(start, end == std::string::npos ? text.size() - start : end - start));
        if (!line.empty() && line.front() != '#' && line.front() != ';') {
            const auto separator = line.find('=');
            if (separator != std::string_view::npos) {
                const auto key = Trim(line.substr(0, separator));
                const auto value = Trim(line.substr(separator + 1U));
                if (!key.empty()) values.insert_or_assign(std::string{key}, std::string{value});
            }
        }
        if (end == std::string::npos) break;
        start = end + 1U;
    }
    return values;
}

template <typename T>
T IntegerValue(const Values& values, const char* key, const T fallback, const int base = 10) noexcept {
    const auto iterator = values.find(key);
    if (iterator == values.end()) return fallback;
    T result{};
    const auto& text = iterator->second;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : fallback;
}

bool BooleanValue(const Values& values, const char* key, const bool fallback) noexcept {
    const auto iterator = values.find(key);
    if (iterator == values.end()) return fallback;
    if (iterator->second == "1" || iterator->second == "true") return true;
    if (iterator->second == "0" || iterator->second == "false") return false;
    return fallback;
}

double DoubleValue(const Values& values, const char* key, const double fallback) noexcept {
    const auto iterator = values.find(key);
    if (iterator == values.end()) return fallback;
    double result{};
    const auto& text = iterator->second;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(result)
        ? result
        : fallback;
}

template <typename Enum>
Enum EnumValue(const Values& values, const char* key, const Enum fallback, const std::uint32_t maximum) noexcept {
    const auto raw = IntegerValue<std::uint32_t>(values, key, static_cast<std::uint32_t>(fallback));
    return raw <= maximum ? static_cast<Enum>(raw) : fallback;
}

template <std::size_t Size>
std::array<std::uint64_t, Size> ParseSensorIds(
    const Values& values,
    const char* const key,
    std::uint32_t& count) noexcept {
    std::array<std::uint64_t, Size> result{};
    count = 0;
    const auto iterator = values.find(key);
    if (iterator == values.end()) return result;

    std::string_view text{iterator->second};
    while (!text.empty() && count < result.size()) {
        const auto separator = text.find(',');
        const auto token = Trim(text.substr(0, separator));
        std::uint64_t id{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), id, 10);
        if (id != 0U && parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) {
            if (std::find(result.begin(), result.begin() + count, id) == result.begin() + count) {
                result[count++] = id;
            }
        }
        if (separator == std::string_view::npos) break;
        text.remove_prefix(separator + 1U);
    }
    return result;
}

void WriteBoolean(std::ostream& stream, const char* key, const bool value) {
    stream << key << '=' << (value ? 1 : 0) << '\n';
}

void NormalizeCategoryColor(std::uint32_t& color) noexcept {
    if (color != AppSettings::kMatchAccentColor) color &= 0xFFFFFFU;
}

void WriteColor(std::ostream& stream, const char* key, const std::uint32_t color) {
    stream << key << '=' << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << color << std::dec << '\n';
}

} // namespace

void AppSettings::Normalize() noexcept {
    refresh_interval_ms = std::clamp(refresh_interval_ms, 100U, 10'000U);
    interface_text_scale_percent = std::clamp(interface_text_scale_percent, 100U, 130U);
    text_color_rgb &= 0xFFFFFFU;
    NormalizeCategoryColor(cpu_temperature_color_rgb);
    NormalizeCategoryColor(cpu_usage_color_rgb);
    NormalizeCategoryColor(cpu_clock_color_rgb);
    NormalizeCategoryColor(cpu_power_color_rgb);
    NormalizeCategoryColor(graphics_color_rgb);
    NormalizeCategoryColor(storage_color_rgb);
    NormalizeCategoryColor(memory_color_rgb);
    NormalizeCategoryColor(system_color_rgb);
    osd_opacity_percent = std::clamp(osd_opacity_percent, 10U, 100U);
    osd_scale_percent = std::clamp(osd_scale_percent, 50U, 250U);
    osd_spacing_px = std::min(osd_spacing_px, 64U);
    easy_temperature_mask &= easy_cpu_package | easy_gpu_core | easy_gpu_memory_junction;
    fps_color_rgb &= 0xFFFFFFU;
    fps_scale_percent = std::clamp(fps_scale_percent, 50U, 300U);
    fps_refresh_interval_ms = std::clamp(fps_refresh_interval_ms, 50U, 500U);
    fps_smoothing_interval_ms = std::clamp(fps_smoothing_interval_ms, 250U, 1'250U);
    if (osd_graph_sensor_id == 0U) osd_graph_sensor_id = 3U;
    osd_graph_sensor_count = std::min<std::uint32_t>(
        osd_graph_sensor_count,
        static_cast<std::uint32_t>(osd_graph_sensor_ids.size()));
    if (osd_graph_sensor_count == 0U) {
        osd_graph_sensor_ids[0] = osd_graph_sensor_id;
        osd_graph_sensor_count = 1U;
    }
    std::uint32_t unique_graph_sensors{};
    for (std::uint32_t index{}; index < osd_graph_sensor_count; ++index) {
        const auto id = osd_graph_sensor_ids[index];
        if (id == 0U || std::find(
                osd_graph_sensor_ids.begin(),
                osd_graph_sensor_ids.begin() + unique_graph_sensors,
                id) != osd_graph_sensor_ids.begin() + unique_graph_sensors) continue;
        osd_graph_sensor_ids[unique_graph_sensors++] = id;
    }
    osd_graph_sensor_count = unique_graph_sensors;
    if (osd_graph_sensor_count == 0U) {
        osd_graph_sensor_ids[0] = osd_graph_sensor_id;
        osd_graph_sensor_count = 1U;
    }
    osd_graph_sensor_id = osd_graph_sensor_ids[0];
    for (auto& color : osd_graph_colors_rgb) color &= 0xFFFFFFU;
    osd_graph_history_seconds = std::clamp(osd_graph_history_seconds, 5U, 300U);
    osd_graph_refresh_interval_ms = std::clamp(osd_graph_refresh_interval_ms, 50U, 1'000U);
    osd_graph_width_px = std::clamp(osd_graph_width_px, 160U, 960U);
    osd_graph_height_px = std::clamp(osd_graph_height_px, 64U, 480U);
    osd_graph_line_thickness_px = std::clamp(osd_graph_line_thickness_px, 1U, 4U);
    if (!std::isfinite(osd_graph_custom_minimum)) osd_graph_custom_minimum = 0.0;
    if (!std::isfinite(osd_graph_custom_maximum) || osd_graph_custom_maximum <= osd_graph_custom_minimum) {
        osd_graph_custom_maximum = osd_graph_custom_minimum + 100.0;
    }
    floating_graph_width_px = std::clamp(floating_graph_width_px, 360U, 2'560U);
    floating_graph_height_px = std::clamp(floating_graph_height_px, 220U, 1'440U);
    reset_min_max_interval_minutes = std::min(reset_min_max_interval_minutes, 10'080U);
    favorite_sensor_count = std::min<std::uint32_t>(favorite_sensor_count, static_cast<std::uint32_t>(favorite_sensor_ids.size()));
    pinned_sensor_count = std::min<std::uint32_t>(pinned_sensor_count, static_cast<std::uint32_t>(pinned_sensor_ids.size()));
    osd_sensor_order_count = std::min<std::uint32_t>(osd_sensor_order_count, static_cast<std::uint32_t>(osd_sensor_order_ids.size()));
    std::uint32_t unique_osd_sensors{};
    for (std::uint32_t index{}; index < osd_sensor_order_count; ++index) {
        const auto id = osd_sensor_order_ids[index];
        if (id == 0U || std::find(osd_sensor_order_ids.begin(), osd_sensor_order_ids.begin() + unique_osd_sensors, id)
                != osd_sensor_order_ids.begin() + unique_osd_sensors) continue;
        osd_sensor_order_ids[unique_osd_sensors++] = id;
    }
    osd_sensor_order_count = unique_osd_sensors;
    collapsed_sections &= 0x01FFU;
}

bool AppSettings::PinSensor(const std::uint64_t sensor_id) noexcept {
    if (sensor_id == 0U || IsSensorPinned(sensor_id) || pinned_sensor_count >= pinned_sensor_ids.size()) return false;
    pinned_sensor_ids[pinned_sensor_count++] = sensor_id;
    return true;
}

bool AppSettings::UnpinSensor(const std::uint64_t sensor_id) noexcept {
    const auto end = pinned_sensor_ids.begin() + pinned_sensor_count;
    const auto found = std::find(pinned_sensor_ids.begin(), end, sensor_id);
    if (found == end) return false;
    std::move(found + 1, end, found);
    --pinned_sensor_count;
    pinned_sensor_ids[pinned_sensor_count] = 0U;
    return true;
}

bool AppSettings::IsSensorPinned(const std::uint64_t sensor_id) const noexcept {
    return std::find(pinned_sensor_ids.begin(), pinned_sensor_ids.begin() + pinned_sensor_count, sensor_id)
        != pinned_sensor_ids.begin() + pinned_sensor_count;
}

bool AppSettings::AddFavorite(const std::uint64_t sensor_id) noexcept {
    if (sensor_id == 0U || IsFavorite(sensor_id) || favorite_sensor_count >= favorite_sensor_ids.size()) return false;
    favorite_sensor_ids[favorite_sensor_count++] = sensor_id;
    return true;
}

bool AppSettings::RemoveFavorite(const std::uint64_t sensor_id) noexcept {
    const auto end = favorite_sensor_ids.begin() + favorite_sensor_count;
    const auto found = std::find(favorite_sensor_ids.begin(), end, sensor_id);
    if (found == end) return false;
    std::move(found + 1, end, found);
    --favorite_sensor_count;
    favorite_sensor_ids[favorite_sensor_count] = 0U;
    return true;
}

bool AppSettings::IsFavorite(const std::uint64_t sensor_id) const noexcept {
    return std::find(favorite_sensor_ids.begin(), favorite_sensor_ids.begin() + favorite_sensor_count, sensor_id)
        != favorite_sensor_ids.begin() + favorite_sensor_count;
}

SettingsStore::SettingsStore(std::filesystem::path path) noexcept : path_(std::move(path)) {}

std::filesystem::path SettingsStore::DefaultPath() noexcept {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local_app_data))) return {};
    std::filesystem::path result{local_app_data};
    CoTaskMemFree(local_app_data);
    result /= L"HardwareScope";
    result /= L"settings-v2.ini";
    return result;
}

bool SettingsStore::Load(AppSettings& destination) const noexcept {
    try {
        std::ifstream stream(path_, std::ios::binary);
        if (!stream) return false;
        // Read at most the cap plus one byte: a size check alone races a growing file.
        constexpr std::size_t maximum_bytes = 256U * 1024U;
        std::string text(maximum_bytes + 1U, '\0');
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        const auto bytes = static_cast<std::size_t>(stream.gcount());
        if (stream.bad() || bytes > maximum_bytes) return false;
        text.resize(bytes);
        const auto values = Parse(text);
        const auto schema_version = IntegerValue<std::uint32_t>(values, "schema_version", 0U);
        if (schema_version == 0U || schema_version > AppSettings::kSchemaVersion) return false;

        AppSettings loaded{};
        loaded.refresh_interval_ms = IntegerValue(values, "refresh_interval_ms", loaded.refresh_interval_ms);
        loaded.theme = EnumValue(values, "theme", loaded.theme, 2U);
        loaded.text_color_rgb = IntegerValue(values, "text_color_rgb", loaded.text_color_rgb, 16);
        loaded.cpu_temperature_color_rgb = IntegerValue(values, "cpu_temperature_color_rgb", loaded.cpu_temperature_color_rgb, 16);
        loaded.cpu_usage_color_rgb = IntegerValue(values, "cpu_usage_color_rgb", loaded.cpu_usage_color_rgb, 16);
        loaded.cpu_clock_color_rgb = IntegerValue(values, "cpu_clock_color_rgb", loaded.cpu_clock_color_rgb, 16);
        loaded.cpu_power_color_rgb = IntegerValue(values, "cpu_power_color_rgb", loaded.cpu_power_color_rgb, 16);
        loaded.graphics_color_rgb = IntegerValue(values, "graphics_color_rgb", loaded.graphics_color_rgb, 16);
        loaded.storage_color_rgb = IntegerValue(values, "storage_color_rgb", loaded.storage_color_rgb, 16);
        loaded.memory_color_rgb = IntegerValue(values, "memory_color_rgb", loaded.memory_color_rgb, 16);
        loaded.system_color_rgb = IntegerValue(values, "system_color_rgb", loaded.system_color_rgb, 16);
        loaded.interface_text_scale_percent = IntegerValue(values, "interface_text_scale_percent", loaded.interface_text_scale_percent);
        loaded.high_contrast = BooleanValue(values, "high_contrast", loaded.high_contrast);
        loaded.onboarding_completed = BooleanValue(values, "onboarding_completed", true);
        loaded.start_with_windows = BooleanValue(values, "start_with_windows", loaded.start_with_windows);
        loaded.start_minimized = BooleanValue(values, "start_minimized", loaded.start_minimized);
        loaded.reset_min_max_on_startup = BooleanValue(values, "reset_min_max_on_startup", loaded.reset_min_max_on_startup);
        loaded.reset_min_max_on_game_launch = BooleanValue(values, "reset_min_max_on_game_launch", loaded.reset_min_max_on_game_launch);
        loaded.reset_min_max_interval_minutes = IntegerValue(values, "reset_min_max_interval_minutes", loaded.reset_min_max_interval_minutes);
        loaded.show_osd = BooleanValue(values, "show_osd", loaded.show_osd);
        loaded.osd_position = EnumValue(values, "osd_position", loaded.osd_position, 3U);
        loaded.osd_layout = EnumValue(values, "osd_layout", loaded.osd_layout, 1U);
        loaded.osd_opacity_percent = IntegerValue(values, "osd_opacity_percent", loaded.osd_opacity_percent);
        loaded.osd_scale_percent = IntegerValue(values, "osd_scale_percent", loaded.osd_scale_percent);
        loaded.osd_spacing_px = IntegerValue(values, "osd_spacing_px", loaded.osd_spacing_px);
        loaded.osd_group_separators = BooleanValue(values, "osd_group_separators", loaded.osd_group_separators);
        loaded.osd_background = BooleanValue(values, "osd_background", loaded.osd_background);
        loaded.easy_temperature_enabled = BooleanValue(values, "easy_temperature_enabled", loaded.easy_temperature_enabled);
        loaded.easy_temperature_mask = IntegerValue(values, "easy_temperature_mask", loaded.easy_temperature_mask);
        loaded.fps_enabled = BooleanValue(values, "fps_enabled", loaded.fps_enabled);
        loaded.fps_game_only = BooleanValue(values, "fps_game_only", loaded.fps_game_only);
        loaded.fps_separate_position = BooleanValue(values, "fps_separate_position", loaded.fps_separate_position);
        loaded.fps_osd_position = EnumValue(values, "fps_osd_position", loaded.fps_osd_position, 3U);
        loaded.fps_refresh_interval_ms = IntegerValue(values, "fps_refresh_interval_ms", loaded.fps_refresh_interval_ms);
        loaded.fps_smoothing_interval_ms = IntegerValue(values, "fps_smoothing_interval_ms", loaded.fps_smoothing_interval_ms);
        loaded.fps_color_rgb = IntegerValue(values, "fps_color_rgb", loaded.fps_color_rgb, 16);
        loaded.fps_scale_percent = IntegerValue(values, "fps_scale_percent", loaded.fps_scale_percent);
        loaded.fps_one_percent_low_enabled = BooleanValue(values, "fps_one_percent_low_enabled", loaded.fps_one_percent_low_enabled);
        loaded.osd_graph_enabled = BooleanValue(values, "osd_graph_enabled", loaded.osd_graph_enabled);
        loaded.osd_graph_sensor_id = IntegerValue<std::uint64_t>(values, "osd_graph_sensor_id", loaded.osd_graph_sensor_id);
        loaded.osd_graph_sensor_ids = ParseSensorIds<AppSettings::kMaximumGraphSensors>(
            values,
            "osd_graph_sensor_ids",
            loaded.osd_graph_sensor_count);
        if (loaded.osd_graph_sensor_count == 0U) {
            loaded.osd_graph_sensor_ids[0] = loaded.osd_graph_sensor_id;
            loaded.osd_graph_sensor_count = 1U;
        }
        loaded.osd_graph_history_seconds = IntegerValue(values, "osd_graph_history_seconds", loaded.osd_graph_history_seconds);
        loaded.osd_graph_refresh_interval_ms = IntegerValue(values, "osd_graph_refresh_interval_ms", loaded.osd_graph_refresh_interval_ms);
        loaded.osd_graph_width_px = IntegerValue(values, "osd_graph_width_px", loaded.osd_graph_width_px);
        loaded.osd_graph_height_px = IntegerValue(values, "osd_graph_height_px", loaded.osd_graph_height_px);
        loaded.osd_graph_scale_mode = EnumValue(values, "osd_graph_scale_mode", loaded.osd_graph_scale_mode, 2U);
        loaded.osd_graph_custom_minimum = DoubleValue(values, "osd_graph_custom_minimum", loaded.osd_graph_custom_minimum);
        loaded.osd_graph_custom_maximum = DoubleValue(values, "osd_graph_custom_maximum", loaded.osd_graph_custom_maximum);
        loaded.osd_graph_line_thickness_px = IntegerValue(values, "osd_graph_line_thickness_px", loaded.osd_graph_line_thickness_px);
        loaded.osd_graph_grid = BooleanValue(values, "osd_graph_grid", loaded.osd_graph_grid);
        loaded.osd_graph_labels = BooleanValue(values, "osd_graph_labels", loaded.osd_graph_labels);
        loaded.osd_graph_colors_rgb[0] = IntegerValue(values, "osd_graph_color_1_rgb", loaded.osd_graph_colors_rgb[0], 16);
        loaded.osd_graph_colors_rgb[1] = IntegerValue(values, "osd_graph_color_2_rgb", loaded.osd_graph_colors_rgb[1], 16);
        loaded.osd_graph_colors_rgb[2] = IntegerValue(values, "osd_graph_color_3_rgb", loaded.osd_graph_colors_rgb[2], 16);
        loaded.osd_graph_colors_rgb[3] = IntegerValue(values, "osd_graph_color_4_rgb", loaded.osd_graph_colors_rgb[3], 16);
        loaded.floating_graph_enabled = BooleanValue(values, "floating_graph_enabled", loaded.floating_graph_enabled);
        loaded.floating_graph_topmost = BooleanValue(values, "floating_graph_topmost", loaded.floating_graph_topmost);
        loaded.floating_graph_x = IntegerValue(values, "floating_graph_x", loaded.floating_graph_x);
        loaded.floating_graph_y = IntegerValue(values, "floating_graph_y", loaded.floating_graph_y);
        loaded.floating_graph_width_px = IntegerValue(values, "floating_graph_width_px", loaded.floating_graph_width_px);
        loaded.floating_graph_height_px = IntegerValue(values, "floating_graph_height_px", loaded.floating_graph_height_px);
        loaded.automatic_updates = BooleanValue(values, "automatic_updates", loaded.automatic_updates);
        loaded.update_snooze_until_unix_seconds = IntegerValue(values, "update_snooze_until_unix_seconds", loaded.update_snooze_until_unix_seconds);
        loaded.skipped_update_major = IntegerValue(values, "skipped_update_major", loaded.skipped_update_major);
        loaded.skipped_update_minor = IntegerValue(values, "skipped_update_minor", loaded.skipped_update_minor);
        loaded.skipped_update_patch = IntegerValue(values, "skipped_update_patch", loaded.skipped_update_patch);
        loaded.collapsed_sections = IntegerValue(values, "collapsed_sections", loaded.collapsed_sections);
        loaded.favorites_only = BooleanValue(values, "favorites_only", loaded.easy_temperature_enabled);
        loaded.favorites_initialized = BooleanValue(values, "favorites_initialized", false);
        loaded.favorite_defaults_completed_mask = IntegerValue<std::uint32_t>(values, "favorite_defaults_completed_mask", 0U) & 7U;
        loaded.favorite_sensor_ids = ParseSensorIds<AppSettings::kMaximumFavoriteSensors>(values, "favorite_sensor_ids", loaded.favorite_sensor_count);
        loaded.pinned_sensor_ids = ParseSensorIds<AppSettings::kMaximumPinnedSensors>(values, "pinned_sensor_ids", loaded.pinned_sensor_count);
        loaded.osd_sensor_order_ids = ParseSensorIds<AppSettings::kMaximumOsdOrderSensors>(values, "osd_sensor_order_ids", loaded.osd_sensor_order_count);
        loaded.Normalize();
        destination = loaded;
        return true;
    } catch (...) {
        return false;
    }
}

bool SettingsStore::Save(const AppSettings& settings) const noexcept {
    try {
        auto normalized = settings;
        normalized.Normalize();
        const auto parent = path_.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        auto temporary = path_;
        temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId());

        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << "# HardwareScope 2.0 native settings\n";
        stream << "schema_version=" << AppSettings::kSchemaVersion << '\n';
        stream << "refresh_interval_ms=" << normalized.refresh_interval_ms << '\n';
        stream << "theme=" << static_cast<unsigned>(normalized.theme) << '\n';
        stream << "text_color_rgb=" << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << normalized.text_color_rgb << std::dec << '\n';
        WriteColor(stream, "cpu_temperature_color_rgb", normalized.cpu_temperature_color_rgb);
        WriteColor(stream, "cpu_usage_color_rgb", normalized.cpu_usage_color_rgb);
        WriteColor(stream, "cpu_clock_color_rgb", normalized.cpu_clock_color_rgb);
        WriteColor(stream, "cpu_power_color_rgb", normalized.cpu_power_color_rgb);
        WriteColor(stream, "graphics_color_rgb", normalized.graphics_color_rgb);
        WriteColor(stream, "storage_color_rgb", normalized.storage_color_rgb);
        WriteColor(stream, "memory_color_rgb", normalized.memory_color_rgb);
        WriteColor(stream, "system_color_rgb", normalized.system_color_rgb);
        stream << "interface_text_scale_percent=" << normalized.interface_text_scale_percent << '\n';
        WriteBoolean(stream, "high_contrast", normalized.high_contrast);
        WriteBoolean(stream, "onboarding_completed", normalized.onboarding_completed);
        WriteBoolean(stream, "start_with_windows", normalized.start_with_windows);
        WriteBoolean(stream, "start_minimized", normalized.start_minimized);
        WriteBoolean(stream, "reset_min_max_on_startup", normalized.reset_min_max_on_startup);
        WriteBoolean(stream, "reset_min_max_on_game_launch", normalized.reset_min_max_on_game_launch);
        stream << "reset_min_max_interval_minutes=" << normalized.reset_min_max_interval_minutes << '\n';
        WriteBoolean(stream, "show_osd", normalized.show_osd);
        stream << "osd_position=" << static_cast<unsigned>(normalized.osd_position) << '\n';
        stream << "osd_layout=" << static_cast<unsigned>(normalized.osd_layout) << '\n';
        stream << "osd_opacity_percent=" << normalized.osd_opacity_percent << '\n';
        stream << "osd_scale_percent=" << normalized.osd_scale_percent << '\n';
        stream << "osd_spacing_px=" << normalized.osd_spacing_px << '\n';
        WriteBoolean(stream, "osd_group_separators", normalized.osd_group_separators);
        WriteBoolean(stream, "osd_background", normalized.osd_background);
        WriteBoolean(stream, "easy_temperature_enabled", normalized.easy_temperature_enabled);
        stream << "easy_temperature_mask=" << normalized.easy_temperature_mask << '\n';
        WriteBoolean(stream, "fps_enabled", normalized.fps_enabled);
        WriteBoolean(stream, "fps_game_only", normalized.fps_game_only);
        WriteBoolean(stream, "fps_separate_position", normalized.fps_separate_position);
        stream << "fps_osd_position=" << static_cast<unsigned>(normalized.fps_osd_position) << '\n';
        stream << "fps_refresh_interval_ms=" << normalized.fps_refresh_interval_ms << '\n';
        stream << "fps_smoothing_interval_ms=" << normalized.fps_smoothing_interval_ms << '\n';
        stream << "fps_color_rgb=" << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << normalized.fps_color_rgb << std::dec << '\n';
        stream << "fps_scale_percent=" << normalized.fps_scale_percent << '\n';
        WriteBoolean(stream, "fps_one_percent_low_enabled", normalized.fps_one_percent_low_enabled);
        WriteBoolean(stream, "osd_graph_enabled", normalized.osd_graph_enabled);
        stream << "osd_graph_sensor_id=" << normalized.osd_graph_sensor_id << '\n';
        stream << "osd_graph_sensor_ids=";
        for (std::uint32_t index{}; index < normalized.osd_graph_sensor_count; ++index) {
            if (index != 0U) stream << ',';
            stream << normalized.osd_graph_sensor_ids[index];
        }
        stream << '\n';
        stream << "osd_graph_history_seconds=" << normalized.osd_graph_history_seconds << '\n';
        stream << "osd_graph_refresh_interval_ms=" << normalized.osd_graph_refresh_interval_ms << '\n';
        stream << "osd_graph_width_px=" << normalized.osd_graph_width_px << '\n';
        stream << "osd_graph_height_px=" << normalized.osd_graph_height_px << '\n';
        stream << "osd_graph_scale_mode=" << static_cast<unsigned>(normalized.osd_graph_scale_mode) << '\n';
        stream << "osd_graph_custom_minimum=" << normalized.osd_graph_custom_minimum << '\n';
        stream << "osd_graph_custom_maximum=" << normalized.osd_graph_custom_maximum << '\n';
        stream << "osd_graph_line_thickness_px=" << normalized.osd_graph_line_thickness_px << '\n';
        WriteBoolean(stream, "osd_graph_grid", normalized.osd_graph_grid);
        WriteBoolean(stream, "osd_graph_labels", normalized.osd_graph_labels);
        WriteColor(stream, "osd_graph_color_1_rgb", normalized.osd_graph_colors_rgb[0]);
        WriteColor(stream, "osd_graph_color_2_rgb", normalized.osd_graph_colors_rgb[1]);
        WriteColor(stream, "osd_graph_color_3_rgb", normalized.osd_graph_colors_rgb[2]);
        WriteColor(stream, "osd_graph_color_4_rgb", normalized.osd_graph_colors_rgb[3]);
        WriteBoolean(stream, "floating_graph_enabled", normalized.floating_graph_enabled);
        WriteBoolean(stream, "floating_graph_topmost", normalized.floating_graph_topmost);
        stream << "floating_graph_x=" << normalized.floating_graph_x << '\n';
        stream << "floating_graph_y=" << normalized.floating_graph_y << '\n';
        stream << "floating_graph_width_px=" << normalized.floating_graph_width_px << '\n';
        stream << "floating_graph_height_px=" << normalized.floating_graph_height_px << '\n';
        WriteBoolean(stream, "automatic_updates", normalized.automatic_updates);
        stream << "update_snooze_until_unix_seconds=" << normalized.update_snooze_until_unix_seconds << '\n';
        stream << "skipped_update_major=" << normalized.skipped_update_major << '\n';
        stream << "skipped_update_minor=" << normalized.skipped_update_minor << '\n';
        stream << "skipped_update_patch=" << normalized.skipped_update_patch << '\n';
        stream << "collapsed_sections=" << normalized.collapsed_sections << '\n';
        WriteBoolean(stream, "favorites_only", normalized.favorites_only);
        WriteBoolean(stream, "favorites_initialized", normalized.favorites_initialized);
        stream << "favorite_defaults_completed_mask=" << normalized.favorite_defaults_completed_mask << '\n';
        stream << "favorite_sensor_ids=";
        for (std::uint32_t index = 0; index < normalized.favorite_sensor_count; ++index) {
            if (index != 0U) stream << ',';
            stream << normalized.favorite_sensor_ids[index];
        }
        stream << '\n';
        stream << "pinned_sensor_ids=";
        for (std::uint32_t index = 0; index < normalized.pinned_sensor_count; ++index) {
            if (index != 0U) stream << ',';
            stream << normalized.pinned_sensor_ids[index];
        }
        stream << '\n';
        stream << "osd_sensor_order_ids=";
        for (std::uint32_t index = 0; index < normalized.osd_sensor_order_count; ++index) {
            if (index != 0U) stream << ',';
            stream << normalized.osd_sensor_order_ids[index];
        }
        stream << '\n';
        stream.flush();
        if (!stream) return false;
        stream.close();

        if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            static_cast<void>(DeleteFileW(temporary.c_str()));
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

AsyncSettingsWriter::AsyncSettingsWriter(std::filesystem::path path)
    : store_(std::move(path)), thread_([this](const std::stop_token stop) { Run(stop); }) {}

AsyncSettingsWriter::~AsyncSettingsWriter() {
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable()) thread_.join(); // flush the final value after the window closes
}

void AsyncSettingsWriter::Request(const AppSettings& settings) noexcept {
    const std::scoped_lock lock(mutex_);
    pending_ = settings;
    ++requested_;
    wake_.notify_one();
}

void AsyncSettingsWriter::Run(const std::stop_token stop) {
    for (;;) {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, stop, [this] { return pending_.has_value(); });
        if (!pending_) return;
        const auto value = *pending_;
        const auto revision = requested_.load();
        pending_.reset();
        lock.unlock();
        failed_.store(!store_.Save(value));
        completed_.store(revision);
    }
}

} // namespace hardwarescope
