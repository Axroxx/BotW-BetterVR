#pragma once

class ModSettingBase {
public:
    const char* name;

    ModSettingBase(const char* name): name(name) {}

    virtual std::string Serialize() = 0;

    virtual void Deserialize(std::string valueString) = 0;

    virtual void Reset() = 0;

    virtual ~ModSettingBase() = default;
};

template <typename T>
class ModSetting : public ModSettingBase {
private:
    std::atomic<T> m_value;

public:
    const T defaultValue;

    ModSetting(const char* name, T defaultValue): ModSettingBase(name), m_value(defaultValue), defaultValue(defaultValue) {}

    T load(std::memory_order order = std::memory_order_seq_cst) const {
        return m_value.load(order);
    }

    void store(const T value, std::memory_order order = std::memory_order_seq_cst) {
        Set(value, order);
    }

    T exchange(const T value, std::memory_order order = std::memory_order_seq_cst) {
        const T newValue = NormalizeValue(value);
        return m_value.exchange(newValue, order);
    }

    operator T() const {
        return load();
    }

    T operator=(const T value) {
        store(value);
        return load();
    }

    virtual T NormalizeValue(T value) const {
        return value;
    }

    virtual void Set(const T value, std::memory_order order = std::memory_order_seq_cst) {
        m_value.store(NormalizeValue(value), order);
    }

    void Reset() override {
        store(defaultValue);
    }
};

template <typename T>
requires(std::is_integral_v<T> && std::is_signed_v<T>)
class IntSetting : public ModSetting<T> {
public:
    using ModSetting<T>::operator=;

    const T min;
    const T max;

    IntSetting(const char* name, T defaultValue, T min = std::numeric_limits<T>::min(), T max = std::numeric_limits<T>::max()): ModSetting<T>(name, defaultValue), min(min), max(max) {
        this->store(defaultValue);
    }

    T NormalizeValue(T value) const override {
        if (value < min) {
            Log::print<ERROR>("Tried to set {} to too low value {}, setting to minimum value {} instead", this->name, value, min);
            return min;
        }
        if (value > max) {
            Log::print<ERROR>("Tried to set {} to too high value {}, setting to maximum value {} instead", this->name, value, max);
            return max;
        }
        return value;
    }

    std::string Serialize() override {
        return std::to_string(T(*this));
    }

    void Deserialize(std::string valueString) override {
        int& errnoRef = errno; //this has a nonzero cost so we pay it once
        char* parseEnd;
        errnoRef = 0;
        const long parsed = std::strtol(valueString.c_str(), &parseEnd, 10);
        if (valueString == parseEnd) { //unparsable string
            Log::print<ERROR>("{} had invalid value \"{}\". Resetting to \"{}\"", this->name, valueString, this->defaultValue);
            this->Reset();
        }
        else if ((errnoRef == ERANGE && parsed < 0) || parsed < min) {
            Log::print<ERROR>("{} had too low value \"{}\". Setting to minimum value \"{}\"", this->name, valueString, min);
            *this = min;
        }
        else if ((errnoRef == ERANGE && parsed > 0) || parsed > max) {
            Log::print<ERROR>("{} had too high value \"{}\". Setting to maximum value \"{}\"", this->name, valueString, max);
            *this = max;
        }
        else {
            *this = T(parsed);
        }
    }

    void AddSliderToGUI(bool* changed, int min, int max, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        int value = int(T(*this));
        std::string idStr = std::format("##{}", this->name);
        if (ImGui::SliderInt(idStr.c_str(), &value, min, max, format(value).c_str())) {
            *this = T(value);
            *changed = true;
        }
    }

    void AddSliderToGUI(bool* changed, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        AddSliderToGUI(changed, int(this->min), int(this->max), std::move(format));
    }

    void AddSetToGUI(bool* changed, const char* label, T value) {
        std::string idStr = std::format("{}##{}", label, this->name);
        if (ImGui::Button(idStr.c_str())) {
            *this = value;
            *changed = true;
        }
    }

    void AddResetToGUI(bool* changed) {
        std::string idStr = std::format("Reset##{}", this->name);
        if (ImGui::Button(idStr.c_str())) {
            this->Reset();
            *changed = true;
        }
    }

    void AddToGUI(bool* changed, float windowWidth, int min, int max, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        ImGui::PushItemWidth(windowWidth * 0.35f);
        AddSliderToGUI(changed, min, max, format);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        AddResetToGUI(changed);
    }

    void AddToGUI(bool* changed, float windowWidth, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        AddToGUI(changed, windowWidth, int(this->min), int(this->max), std::move(format));
    }
};

template <typename T>
requires(std::is_integral_v<T> && !std::is_signed_v<T>)
class UIntSetting : public ModSetting<T> {
public:
    using ModSetting<T>::operator=;

    const T min;
    const T max;

    UIntSetting(const char* name, T defaultValue, T min = 0, T max = std::numeric_limits<T>::max()): ModSetting<T>(name, defaultValue), min(min), max(max) {
        this->store(defaultValue);
    }

    T NormalizeValue(T value) const override {
        if (value < min) {
            Log::print<ERROR>("Tried to set {} to too low value {}, setting to minimum value {} instead", this->name, value, min);
            return min;
        }
        if (value > max) {
            Log::print<ERROR>("Tried to set {} to too high value {}, setting to maximum value {} instead", this->name, value, max);
            return max;
        }
        return value;
    }

    std::string Serialize() override {
        return std::to_string(T(*this));
    }

    void Deserialize(std::string valueString) override {
        int& errnoRef = errno; //this has a nonzero cost so we pay it once
        char* parseEnd;
        errnoRef = 0;
        const unsigned long parsed = std::strtoul(valueString.c_str(), &parseEnd, 10);
        if (valueString == parseEnd) { //unparsable string
            Log::print<ERROR>("{} had invalid value \"{}\". Resetting to \"{}\"", this->name, valueString, this->defaultValue);
            this->Reset();
        }
        else if (parsed < min) {
            Log::print<ERROR>("{} had too low value \"{}\". Setting to minimum value \"{}\"", this->name, valueString, min);
            *this = min;
        }
        else if (errnoRef == ERANGE || parsed > max) {
            Log::print<ERROR>("{} had too high value \"{}\". Setting to maximum value \"{}\"", this->name, valueString, max);
            *this = max;
        }
        else {
            *this = T(parsed);
        }
    }

    void AddSliderToGUI(bool* changed, int min, int max, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        int value = int(T(*this));
        std::string idStr = std::format("##{}", this->name);
        if (ImGui::SliderInt(idStr.c_str(), &value, min, max, format(value).c_str())) {
            *this = T(value);
            *changed = true;
        }
    }

    void AddSliderToGUI(bool* changed, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        AddSliderToGUI(changed, int(this->min), int(this->max), std::move(format));
    }

    void AddSetToGUI(bool* changed, const char* label, T value) {
        std::string idStr = std::format("{}##{}", label, this->name);
        if (ImGui::Button(idStr.c_str())) {
            *this = value;
            *changed = true;
        }
    }

    void AddResetToGUI(bool* changed) {
        std::string idStr = std::format("Reset##{}", this->name);
        if (ImGui::Button(idStr.c_str())) {
            this->Reset();
            *changed = true;
        }
    }

    void AddToGUI(bool* changed, float windowWidth, int min, int max, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        ImGui::PushItemWidth(windowWidth * 0.35f);
        AddSliderToGUI(changed, min, max, format);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        AddResetToGUI(changed);
    }

    void AddToGUI(bool* changed, float windowWidth, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        AddToGUI(changed, windowWidth, int(this->min), int(this->max), std::move(format));
    }
};

template <typename T>
requires(std::is_floating_point_v<T>)
class FloatSetting : public ModSetting<T> {
public:
    using ModSetting<T>::operator=;

    const T min;
    const T max;

    FloatSetting(const char* name, T defaultValue, T min = -std::numeric_limits<T>::infinity(), T max = std::numeric_limits<T>::infinity()): ModSetting<T>(name, defaultValue), min(min), max(max) {
        this->store(defaultValue);
    }

    T NormalizeValue(T value) const override {
        if (value < min) {
            Log::print<ERROR>("Tried to set {} to too low value {}, setting to minimum value {} instead", this->name, value, min);
            return min;
        }
        if (value > max) {
            Log::print<ERROR>("Tried to set {} to too high value {}, setting to maximum value {} instead", this->name, value, max);
            return max;
        }
        return value;
    }

    std::string Serialize() override {
        //use stringstream to avoid trailing zeros
        std::stringstream ss;
        ss << T(*this);
        return ss.str();
    }

    void Deserialize(std::string valueString) override {
        int& errnoRef = errno; //this has a nonzero cost so we pay it once
        char* parseEnd;
        errnoRef = 0;
        const double parsed = std::strtod(valueString.c_str(), &parseEnd);
        if (valueString == parseEnd) { //unparsable string
            Log::print<ERROR>("{} had invalid value \"{}\". Resetting to \"{}\"", this->name, valueString, this->defaultValue);
            this->Reset();
        }
        else if (errnoRef == ERANGE) {
            Log::print<ERROR>("{} had out-of-range value \"{}\". Resetting to \"{}\"", this->name, valueString, this->defaultValue);
            this->Reset();
        }
        else if (parsed < min) {
            Log::print<ERROR>("{} had too low value \"{}\". Setting to minimum value \"{}\"", this->name, valueString, min);
            *this = min;
        }
        else if (parsed > max) {
            Log::print<ERROR>("{} had too high value \"{}\". Setting to maximum value \"{}\"", this->name, valueString, max);
            *this = max;
        }
        else {
            *this = T(parsed);
        }
    }

    void AddSliderToGUI(bool* changed, float min, float max, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        float value = float(T(*this));
        std::string idStr = std::format("##{}", this->name);
        if (ImGui::SliderFloat(idStr.c_str(), &value, min, max, format(value).c_str())) {
            *this = T(value);
            *changed = true;
        }
    }

    void AddSetToGUI(bool* changed, const char* label, T value) {
        std::string idStr = std::format("{}##{}", label, this->name);
        if (ImGui::Button(idStr.c_str())) {
            *this = value;
            *changed = true;
        }
    }

    void AddResetToGUI(bool* changed) {
        std::string idStr = std::format("Reset##{}", this->name);
        if (ImGui::Button(idStr.c_str())) {
            this->Reset();
            *changed = true;
        }
    }

    void AddToGUI(bool* changed, float windowWidth, float min, float max, std::function<std::string(float)> format = [](float value) { return std::format("%.2f", value); }) {
        ImGui::PushItemWidth(windowWidth * 0.35f);
        AddSliderToGUI(changed, min, max, format);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        AddResetToGUI(changed);
    }

    void AddPercentToGUI(bool* changed, float windowWidth, float minPercent, float maxPercent) {
        float value = float(T(*this)) * 100.0f;
        std::string idStr = std::format("##{}", this->name);
        ImGui::PushItemWidth(windowWidth * 0.35f);
        if (ImGui::SliderFloat(idStr.c_str(), &value, minPercent, maxPercent, "%.0f%%")) {
            *this = T(value / 100.0f);
            *changed = true;
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        AddResetToGUI(changed);
    }
};

class BoolSetting : public ModSetting<bool> {
public:
    using ModSetting<bool>::operator=;

    BoolSetting(const char* name, bool defaultValue): ModSetting<bool>(name, defaultValue) {}

    std::string Serialize() override {
        return bool(*this) ? "true" : "false";
    }

    void Deserialize(std::string valueString) override {
        const char* valueChars = valueString.c_str();
        if (stricmp(valueChars, "true") == 0)
            *this = true;
        else if (stricmp(valueChars, "false") == 0)
            *this = false;
        else {
            //numeric load backup for older syntax
            char* parseEnd;
            const long parsed = std::strtol(valueChars, &parseEnd, 10);
            if (valueChars == parseEnd) {
                //Log::print<ERROR>("{} had invalid value \"{}\". Resetting to \"{}\"", this->name, valueString, this->defaultValue ? "true" : "false");
                this->Reset();
            }
            else {
                *this = parsed != 0;
            }
        }
    }

    void AddToGUI(bool* changed) {
        bool value = *this;
        std::string idStr = std::format("##{}", this->name);
        if (ImGui::Checkbox(idStr.c_str(), &value)) {
            *this = value;
            *changed = true;
        }
    }
};

class StringSetting : public ModSettingBase {
private:
    std::string m_value;

public:
    const std::string defaultValue;

    StringSetting(const char* name, std::string defaultValue): ModSettingBase(name), m_value(defaultValue), defaultValue(std::move(defaultValue)) {}

    const std::string& Get() const {
        return m_value;
    }

    operator const std::string&() const {
        return Get();
    }

    void Set(std::string value) {
        m_value = NormalizeValue(std::move(value));
    }

    std::string Serialize() override {
        return m_value;
    }

    void Deserialize(std::string valueString) override {
        Set(std::move(valueString));
    }

    void Reset() override {
        m_value = defaultValue;
    }

    virtual std::string NormalizeValue(std::string value) const {
        return value;
    }
};

template <typename T>
concept IsEnum = std::is_enum_v<T>;

template <typename T>
requires(IsEnum<T>)
class EnumSetting : public ModSetting<T> {
public:
    using ModSetting<T>::operator=;

    const char* (*getName)(T);
    const std::vector<T> values;

    EnumSetting(const char* name, T defaultValue, const char* (*getName)(T), std::initializer_list<T> values): ModSetting<T>(name, defaultValue), getName(getName), values(values) {
        for (T value : values) {
            if (value == defaultValue) {
                this->store(defaultValue);
                return;
            }
        }
        throw std::invalid_argument("Received illegal default value");
    }

    T NormalizeValue(T value) const override {
        for (T value2 : values) {
            if (value2 == value) {
                return value;
            }
        }
        Log::print<ERROR>("Tried to set {} to an invalid value, resetting to default value {} instead", this->name, getName(this->defaultValue));
        return this->defaultValue;
    }

    std::string Serialize() override {
        return std::string(getName(T(*this)));
    }

    void Deserialize(std::string valueString) override {
        const char* valueChars = valueString.c_str();
        for (T value : values) {
            if (stricmp(getName(value), valueChars) == 0) {
                *this = value;
                return;
            }
        }
        //numeric load backup for older syntax
        int& errnoRef = errno; //this has a nonzero cost so we pay it once
        char* parseEnd;
        errnoRef = 0;
        const long parsed = std::strtol(valueChars, &parseEnd, 10);
        if (valueChars == parseEnd || errnoRef == ERANGE) {
            Log::print<ERROR>("{} had invalid value \"{}\". Resetting to \"{}\"", this->name, valueString, getName(this->defaultValue));
            this->Reset();
        }
        else {
            for (T value : values) {
                if (parsed == std::to_underlying(value)) {
                    *this = value;
                    return;
                }
            }
            Log::print<ERROR>("{} had invalid value \"{}\". Resetting to \"{}\"", this->name, valueString, getName(this->defaultValue));
            this->Reset();
        }
    }

    void AddRadioToGUI(bool* changed, const char* (*getDisplayName)(T)) {
        bool first = true;
        int val = int(std::to_underlying(T(*this)));
        for (T value : values) {
            if (first) {
                first = false;
            }
            else {
                ImGui::SameLine();
            }
            std::string idStr = std::format("{}##{}", getDisplayName(value), this->name);
            if (ImGui::RadioButton(idStr.c_str(), &val, int(std::to_underlying(value)))) {
                *this = value;
                *changed = true;
            }
        }
    }

    void AddComboToGUI(bool* changed, const char* (*getDisplayName)(T)) {
        T cur = *this;
        int index;
        std::string idStr = std::format("##{}", this->name);
        std::string idComboStr = "";
        for (int i = 0; i < values.size(); ++i) {
            T value = values[i];
            if (value == cur) {
                index = i;
            }
            idComboStr = std::format("{}{}{}", idComboStr, getDisplayName(value), '\0');
        }
        if (ImGui::Combo(idStr.c_str(), &index, idComboStr.c_str())) {
            *this = values[index];
            *changed = true;
        }
    }
};

enum class EventMode : int32_t {
    NO_EVENT = 0,
    ALWAYS_FIRST_PERSON = 1,
    FOLLOW_DEFAULT_EVENT_SETTINGS = 2,
    ALWAYS_THIRD_PERSON = 3,
};

enum class CameraMode : int32_t {
    THIRD_PERSON = 0,
    FIRST_PERSON = 1,
};

enum class PlayMode : int32_t {
    SEATED = 0,
    STANDING = 1,
};

enum class AngularVelocityFixerMode : int32_t {
    AUTO = 0, // Angular velocity fixer is automatically enabled for Oculus Link
    FORCED_ON = 1,
    FORCED_OFF = 2,
};

enum class PerformanceOverlayMode : int32_t {
    DISABLE = 0,
    WINDOW_ONLY = 1,
    WINDOW_AND_VR = 2,
    WINDOW_AND_VR_WITH_PROFILER = 3,
};

enum class WalkingDirection : int32_t {
    CAMERA = 0,
    CONTROLLER = 1,
};

enum class SwingSensitivity : int32_t {
    SWING_EASY = 0,
    SWING_NORMAL = 1,
    SWING_CUSTOM = 2,
};

enum class TurnMode : int32_t {
    SMOOTH_SLOW = 0,
    SMOOTH_NORMAL = 1,
    SMOOTH_FAST = 2,
    SNAP_30 = 3,
    SNAP_45 = 4,
    SNAP_60 = 5,
};

struct ModSettings {
    static const char* toString(EventMode eventMode) {
        switch (eventMode) {
            case EventMode::NO_EVENT:
                return "NO_EVENT";
            case EventMode::ALWAYS_FIRST_PERSON:
                return "ALWAYS_FIRST_PERSON";
            case EventMode::FOLLOW_DEFAULT_EVENT_SETTINGS:
                return "FOLLOW_DEFAULT_EVENT_SETTINGS";
            case EventMode::ALWAYS_THIRD_PERSON:
                return "ALWAYS_THIRD_PERSON";
            default:
                return "";
        }
    }

    static const char* toDisplayString(EventMode eventMode) {
        switch (eventMode) {
            case EventMode::ALWAYS_FIRST_PERSON:
                return "First Person (Always)";
            case EventMode::FOLLOW_DEFAULT_EVENT_SETTINGS:
                return "Optimal Settings (Mix Of Third/First)";
            case EventMode::ALWAYS_THIRD_PERSON:
                return "Third Person (Always)";
            default:
                return "";
        }
    }

    static const char* toString(CameraMode cameraMode) {
        switch (cameraMode) {
            case CameraMode::THIRD_PERSON:
                return "THIRD_PERSON";
            case CameraMode::FIRST_PERSON:
                return "FIRST_PERSON";
            default:
                return "";
        }
    }

    static const char* toDisplayString(CameraMode cameraMode) {
        switch (cameraMode) {
            case CameraMode::THIRD_PERSON:
                return "Third Person";
            case CameraMode::FIRST_PERSON:
                return "First Person (Recommended)";
            default:
                return "";
        }
    }

    static const char* toString(PlayMode playMode) {
        switch (playMode) {
            case PlayMode::STANDING:
                return "STANDING";
            case PlayMode::SEATED:
                return "SEATED";
            default:
                return "";
        }
    }

    static const char* toDisplayString(PlayMode playMode) {
        switch (playMode) {
            case PlayMode::STANDING:
                return "Standing";
            case PlayMode::SEATED:
                return "Seated";
            default:
                return "";
        }
    }

    static const char* toString(AngularVelocityFixerMode buggyAngularVelocity) {
        switch (buggyAngularVelocity) {
            case AngularVelocityFixerMode::AUTO:
                return "AUTO";
            case AngularVelocityFixerMode::FORCED_ON:
                return "FORCED_ON";
            case AngularVelocityFixerMode::FORCED_OFF:
                return "FORCED_OFF";
            default:
                return "";
        }
    }

    static const char* toDisplayString(AngularVelocityFixerMode buggyAngularVelocity) {
        switch (buggyAngularVelocity) {
            case AngularVelocityFixerMode::AUTO:
                return "Auto (Oculus Link)";
            case AngularVelocityFixerMode::FORCED_ON:
                return "Forced On";
            case AngularVelocityFixerMode::FORCED_OFF:
                return "Forced Off";
            default:
                return "";
        }
    }

    static const char* toString(PerformanceOverlayMode performanceOverlay) {
        switch (performanceOverlay) {
            case PerformanceOverlayMode::DISABLE:
                return "DISABLE";
            case PerformanceOverlayMode::WINDOW_ONLY:
                return "WINDOW_ONLY";
            case PerformanceOverlayMode::WINDOW_AND_VR:
                return "WINDOW_AND_VR";
            case PerformanceOverlayMode::WINDOW_AND_VR_WITH_PROFILER:
                return "WINDOW_AND_VR_WITH_PROFILER";
            default:
                return "";
        }
    }

    static const char* toDisplayString(PerformanceOverlayMode performanceOverlay) {
        switch (performanceOverlay) {
            case PerformanceOverlayMode::DISABLE:
                return "Disable";
            case PerformanceOverlayMode::WINDOW_ONLY:
                return "Only show in Cemu window";
            case PerformanceOverlayMode::WINDOW_AND_VR:
                return "Show in both Cemu and VR";
            case PerformanceOverlayMode::WINDOW_AND_VR_WITH_PROFILER:
                return "Show in both Cemu and VR with profiler";
            default:
                return "";
        }
    }

    static const char* toString(WalkingDirection walkingDirection) {
        switch (walkingDirection) {
            case WalkingDirection::CAMERA:
                return "CAMERA";
            case WalkingDirection::CONTROLLER:
                return "CONTROLLER";
            default:
                return "";
        }
    }

    static const char* toDisplayString(WalkingDirection walkingDirection) {
        switch (walkingDirection) {
            case WalkingDirection::CAMERA:
                return "Camera / Headset";
            case WalkingDirection::CONTROLLER:
                return "Controller";
            default:
                return "";
        }
    }

    static const char* toString(SwingSensitivity sensitivity) {
        switch (sensitivity) {
            case SwingSensitivity::SWING_EASY:
                return "SWING_EASY";
            case SwingSensitivity::SWING_NORMAL:
                return "SWING_NORMAL";
            case SwingSensitivity::SWING_CUSTOM:
                return "SWING_CUSTOM";
            default:
                return "";
        }
    }

    static const char* toDisplayString(SwingSensitivity sensitivity) {
        switch (sensitivity) {
            case SwingSensitivity::SWING_EASY:
                return "Relaxed";
            case SwingSensitivity::SWING_NORMAL:
                return "Normal";
            case SwingSensitivity::SWING_CUSTOM:
                return "Custom";
            default:
                return "";
        }
    }

    static const char* toString(TurnMode turnMode) {
        switch (turnMode) {
            case TurnMode::SMOOTH_SLOW:
                return "SMOOTH_SLOW";
            case TurnMode::SMOOTH_NORMAL:
                return "SMOOTH_NORMAL";
            case TurnMode::SMOOTH_FAST:
                return "SMOOTH_FAST";
            case TurnMode::SNAP_30:
                return "SNAP_30";
            case TurnMode::SNAP_45:
                return "SNAP_45";
            case TurnMode::SNAP_60:
                return "SNAP_60";
            default:
                return "";
        }
    }

    static const char* toDisplayString(TurnMode turnMode) {
        switch (turnMode) {
            case TurnMode::SMOOTH_SLOW:
                return "Smooth Turn (Slow)";
            case TurnMode::SMOOTH_NORMAL:
                return "Smooth Turn (Normal)";
            case TurnMode::SMOOTH_FAST:
                return "Smooth Turn (Fast)";
            case TurnMode::SNAP_30:
                return "30 deg Snap (Recommended)";
            case TurnMode::SNAP_45:
                return "45 deg Snap";
            case TurnMode::SNAP_60:
                return "60 deg Snap";
            default:
                return "";
        }
    }

    static constexpr float kDefaultAxisThreshold = 0.5f;
    static constexpr float kDefaultStickDeadzone = 0.15f;

    // playing mode settings
    EnumSetting<CameraMode> cameraMode = EnumSetting<CameraMode>("CameraMode", CameraMode::FIRST_PERSON, ModSettings::toString, { CameraMode::FIRST_PERSON, CameraMode::THIRD_PERSON });
    EnumSetting<PlayMode> playMode = EnumSetting<PlayMode>("PlayMode", PlayMode::STANDING, ModSettings::toString, { PlayMode::STANDING, PlayMode::SEATED });
    FloatSetting<float> thirdPlayerDistance = FloatSetting<float>("ThirdPlayerDistance", 0.5f, 0.0f);
    BoolSetting thirdPersonBowCameraAim = BoolSetting("ThirdPersonBowCameraAim", true);
    EnumSetting<EventMode> cutsceneCameraMode = EnumSetting<EventMode>("CutsceneCameraMode", EventMode::FOLLOW_DEFAULT_EVENT_SETTINGS, ModSettings::toString, { EventMode::ALWAYS_FIRST_PERSON, EventMode::FOLLOW_DEFAULT_EVENT_SETTINGS, EventMode::ALWAYS_THIRD_PERSON });
    BoolSetting useBlackBarsForCutscenes = BoolSetting("UseBlackBarsForCutscenes", false);

    // first-person settings
    FloatSetting<float> playerHeightOffset = FloatSetting<float>("PlayerHeightOffset", 0.0f);
    BoolSetting leftHanded = BoolSetting("LeftHanded", false);
    BoolSetting uiFollowsGaze = BoolSetting("UiFollowsGaze", true);
    FloatSetting<float> hudDistance = FloatSetting<float>("HudDistance", 1.85f, 0.5f, 2.5f);
    FloatSetting<float> hudSize = FloatSetting<float>("HudSize", 0.85f, 0.4f, 1.75f);
    FloatSetting<float> bowArcOpacity = FloatSetting<float>("BowArcTransparency", 0.7f, 0.0f, 1.0f);

    // advanced settings
    BoolSetting enableDebuggerTools = BoolSetting("EnableDebugOverlay", false);
    BoolSetting debugShowEntityBoxesIn3DView = BoolSetting("DebugShowEntityBoxesIn3DView", false);
    BoolSetting debugShowRoomscalePhysics = BoolSetting("DebugShowRoomscalePhysics", false);
    BoolSetting debugShowRaycastLines = BoolSetting("DebugShowRaycastLines", false);
    BoolSetting debugShowWeaponAxes = BoolSetting("DebugShowWeaponAxes", false);
    BoolSetting alwaysPreventFirstPersonCutsceneCameraMovement = BoolSetting("AlwaysPreventFirstPersonCutsceneCameraMovement", false);
    BoolSetting preventFirstPersonRagdoll = BoolSetting("PreventFirstPersonRagdoll", true);
    EnumSetting<AngularVelocityFixerMode> buggyAngularVelocity = EnumSetting<AngularVelocityFixerMode>("BuggyAngularVelocity", AngularVelocityFixerMode::AUTO, ModSettings::toString, { AngularVelocityFixerMode::AUTO, AngularVelocityFixerMode::FORCED_ON, AngularVelocityFixerMode::FORCED_OFF });
    EnumSetting<PerformanceOverlayMode> performanceOverlay = EnumSetting<PerformanceOverlayMode>("PerformanceOverlay", PerformanceOverlayMode::DISABLE, ModSettings::toString, { PerformanceOverlayMode::DISABLE, PerformanceOverlayMode::WINDOW_ONLY, PerformanceOverlayMode::WINDOW_AND_VR, PerformanceOverlayMode::WINDOW_AND_VR_WITH_PROFILER });
    UIntSetting<uint32_t> performanceOverlayFrequency = UIntSetting<uint32_t>("PerformanceOverlayFrequency", 90);
    BoolSetting tutorialPromptShown = BoolSetting("TutorialPromptShown", false);
    BoolSetting bootDirectlyIntoGame = BoolSetting("BootDirectlyIntoGame", false);
    StringSetting bootDirectlyTitleId = StringSetting("BootDirectlyTitleId", "");

    // Input settings
    FloatSetting<float> axisThreshold = FloatSetting<float>("AxisThreshold", kDefaultAxisThreshold, 0.0f, 1.0f);
    FloatSetting<float> stickDeadzone = FloatSetting<float>("StickDeadzone", kDefaultStickDeadzone, 0.0f, 1.0f);
    EnumSetting<WalkingDirection> walkingDirection = EnumSetting<WalkingDirection>("WalkingDirection", WalkingDirection::CAMERA, ModSettings::toString, { WalkingDirection::CAMERA, WalkingDirection::CONTROLLER });
    EnumSetting<TurnMode> turnMode = EnumSetting<TurnMode>("TurnMode", TurnMode::SMOOTH_NORMAL, ModSettings::toString, { TurnMode::SMOOTH_SLOW, TurnMode::SMOOTH_NORMAL, TurnMode::SMOOTH_FAST, TurnMode::SNAP_30, TurnMode::SNAP_45, TurnMode::SNAP_60 });
    EnumSetting<SwingSensitivity> swingSensitivity = EnumSetting<SwingSensitivity>("SwingSensitivity", SwingSensitivity::SWING_NORMAL, ModSettings::toString, { SwingSensitivity::SWING_EASY, SwingSensitivity::SWING_NORMAL, SwingSensitivity::SWING_CUSTOM });
    FloatSetting<float> customStabSpeedThreshold = FloatSetting<float>("CustomStabSpeedThreshold", 0.05f, 0.01f, 0.50f);
    FloatSetting<float> customStabAccThreshold = FloatSetting<float>("CustomStabAccThreshold", 7.0f, 1.0f, 15.0f);
    FloatSetting<float> customStabSteadinessCone = FloatSetting<float>("CustomStabSteadinessCone", 30.0f, 15.0f, 85.0f);
    FloatSetting<float> customStabAngularSteadiness = FloatSetting<float>("CustomStabAngularSteadiness", 4.5f, 1.0f, 15.0f);
    FloatSetting<float> customStabTravelDistance = FloatSetting<float>("CustomStabTravelDistance", 0.20f, 0.05f, 0.50f);
    FloatSetting<float> customMinGoodStabDuration = FloatSetting<float>("CustomMinGoodStabDuration", 0.040f, 0.005f, 0.100f);
    FloatSetting<float> customSlashSpeedThreshold = FloatSetting<float>("CustomSlashSpeedThreshold", 1.5f, 0.1f, 5.0f);
    FloatSetting<float> customSlashAccThreshold = FloatSetting<float>("CustomSlashAccThreshold", 20.0f, 3.0f, 40.0f);
    FloatSetting<float> customSlashVelocityThreshold = FloatSetting<float>("CustomSlashVelocityThreshold", 7.0f, 1.0f, 15.0f);
    FloatSetting<float> customSlashAccDriftThreshold = FloatSetting<float>("CustomSlashAccDriftThreshold", 10.0f, 2.0f, 30.0f);
    FloatSetting<float> customSlashTravelAngle = FloatSetting<float>("CustomSlashTravelAngle", 36.0f, 10.0f, 90.0f);
    FloatSetting<float> customMinGoodSwingDuration = FloatSetting<float>("CustomMinGoodSwingDuration", 0.040f, 0.005f, 0.100f);
    FloatSetting<float> customMaxBadDuration = FloatSetting<float>("CustomMaxBadDuration", 0.022f, 0.005f, 0.100f);
    FloatSetting<float> customGoodSampleGracePeriod = FloatSetting<float>("CustomGoodSampleGracePeriod", 40.0f, 10.0f, 200.0f);
    FloatSetting<float> customSmoothingTimeConstant = FloatSetting<float>("CustomSmoothingTimeConstant", 0.020f, 0.005f, 0.100f);
    FloatSetting<float> customAngularDriftMinVelocity = FloatSetting<float>("CustomAngularDriftMinVelocity", 0.5f, 0.1f, 3.0f);
    FloatSetting<float> customDamageOutputScale = FloatSetting<float>("CustomDamageOutputScale", 1.0f, 0.10f, 2.00f);

    auto GetOptions() {
        return std::to_array<ModSettingBase*>({ 
            &cameraMode,
            &playMode,
            &thirdPlayerDistance,
            &thirdPersonBowCameraAim,
            &cutsceneCameraMode,
            &useBlackBarsForCutscenes,
            &playerHeightOffset,
            &leftHanded,
            &uiFollowsGaze,
            &hudDistance,
            &hudSize,
            &bowArcOpacity,
            &enableDebuggerTools,
            &debugShowEntityBoxesIn3DView,
            &debugShowRaycastLines,
            &debugShowWeaponAxes,
            &debugShowRoomscalePhysics,
            &alwaysPreventFirstPersonCutsceneCameraMovement,
            &preventFirstPersonRagdoll,
            &buggyAngularVelocity,
            &performanceOverlay,
            &performanceOverlayFrequency,
            &tutorialPromptShown,
            &bootDirectlyIntoGame,
            &bootDirectlyTitleId,
            &axisThreshold,
            &stickDeadzone,
            &walkingDirection,
            &turnMode,
            &swingSensitivity,
            &customStabSpeedThreshold,
            &customStabAccThreshold,
            &customStabSteadinessCone,
            &customStabAngularSteadiness,
            &customStabTravelDistance,
            &customMinGoodStabDuration,
            &customSlashSpeedThreshold,
            &customSlashAccThreshold,
            &customSlashVelocityThreshold,
            &customSlashAccDriftThreshold,
            &customSlashTravelAngle,
            &customMinGoodSwingDuration,
            &customMaxBadDuration,
            &customGoodSampleGracePeriod,
            &customSmoothingTimeConstant,
            &customAngularDriftMinVelocity,
            &customDamageOutputScale
        });
    }

    void ResetCustomWeaponSensitivity() {
        customStabSpeedThreshold.Reset();
        customStabAccThreshold.Reset();
        customStabSteadinessCone.Reset();
        customStabAngularSteadiness.Reset();
        customStabTravelDistance.Reset();
        customMinGoodStabDuration.Reset();
        customSlashSpeedThreshold.Reset();
        customSlashAccThreshold.Reset();
        customSlashVelocityThreshold.Reset();
        customSlashAccDriftThreshold.Reset();
        customSlashTravelAngle.Reset();
        customMinGoodSwingDuration.Reset();
        customMaxBadDuration.Reset();
        customGoodSampleGracePeriod.Reset();
        customSmoothingTimeConstant.Reset();
        customAngularDriftMinVelocity.Reset();
        customDamageOutputScale.Reset();
    }

    CameraMode GetCameraMode() const { return cameraMode; }

    PlayMode GetPlayMode() const { return playMode; }
    bool DoesUIFollowGaze() const { return uiFollowsGaze; }
    bool IsLeftHanded() const { return leftHanded; }
    float GetPlayerHeightOffset() const {
        // disable height offset in third-person mode
        return GetCameraMode() == CameraMode::THIRD_PERSON ? 0.0f : playerHeightOffset;
    }
    EventMode GetCutsceneCameraMode() const {
        // if in third-person mode, always use third-person cutscene camera
        return GetCameraMode() == CameraMode::THIRD_PERSON ? EventMode::ALWAYS_THIRD_PERSON : cutsceneCameraMode;
    }
    bool UseBlackBarsForCutscenes() const { return useBlackBarsForCutscenes; }

    float GetBowArcOpacity() const { return bowArcOpacity; }
    bool ShouldAimThirdPersonBowFromCamera() const { return thirdPersonBowCameraAim; }
    bool AlwaysPreventFirstPersonCutsceneCameraMovement() const { return alwaysPreventFirstPersonCutsceneCameraMovement; }
    bool ShouldPreventFirstPersonRagdoll() const { return preventFirstPersonRagdoll; }
    bool ShouldBootDirectlyIntoGame() const { return bootDirectlyIntoGame; }
    AngularVelocityFixerMode AngularVelocityFixer_GetMode() const { return buggyAngularVelocity; }
    SwingSensitivity GetSwingSensitivity() const { return swingSensitivity; }
    WalkingDirection GetWalkingDirection() const { return walkingDirection; }
    int32_t GetSnapTurnAngle() const {
        switch (turnMode.load()) {
            case TurnMode::SNAP_30: return 30;
            case TurnMode::SNAP_45: return 45;
            case TurnMode::SNAP_60: return 60;
            default: return 0;
        }
    }
    float GetSmoothTurnSpeed() const {
        switch (turnMode.load()) {
            case TurnMode::SMOOTH_SLOW: return 60.0f;
            case TurnMode::SMOOTH_NORMAL: return 120.0f;
            case TurnMode::SMOOTH_FAST: return 240.0f;
            default: return 0.0f;
        }
    }
    float GetWeaponDamageOutputScale() const { return GetSwingSensitivity() == SwingSensitivity::SWING_CUSTOM ? customDamageOutputScale : 1.0f; }

    bool IsDebuggingToolsEnabled() const;
    bool ShouldShowRoomPhysics() const { return IsDebuggingToolsEnabled() && debugShowRoomscalePhysics; }
    bool ShouldShowEntityBoxesIn3DView() const { return IsDebuggingToolsEnabled() && debugShowEntityBoxesIn3DView; }
    bool ShouldShowRaycastLines() const { return IsDebuggingToolsEnabled() && debugShowRaycastLines; }
    bool ShouldShowWeaponAxes() const { return IsDebuggingToolsEnabled() && debugShowWeaponAxes; }

    // By default BotW's camera uses 0.1f for near plane and 25000.0f for far plane, except maybe some indoor areas? But for simplicity, we'll use the default values everywhere.
    float GetZNear() const { return 0.1f; }
    float GetZFar() const { return 25000.0f; }

    std::string ToString() const {
        std::string buffer = "";
        std::format_to(std::back_inserter(buffer), " - Camera Mode: {}\n", toDisplayString(GetCameraMode()));
        std::format_to(std::back_inserter(buffer), " - Left Handed: {}\n", IsLeftHanded() ? "Yes" : "No");
        std::format_to(std::back_inserter(buffer), " - GUI Follow Setting: {}\n", DoesUIFollowGaze() ? "Follow Looking Direction" : "Fixed");
        std::format_to(std::back_inserter(buffer), " - Player Height: {} meters\n", GetPlayerHeightOffset());
        std::format_to(std::back_inserter(buffer), " - Boot Directly Into BotW: {}\n", ShouldBootDirectlyIntoGame() ? "Enabled" : "Disabled");
        if (!bootDirectlyTitleId.Get().empty()) {
            std::format_to(std::back_inserter(buffer), " - Saved Direct Boot Title ID: {}\n", bootDirectlyTitleId.Get());
        }
        std::format_to(std::back_inserter(buffer), " - Cutscene Camera Mode: {}\n", toDisplayString(GetCutsceneCameraMode()));
        std::format_to(std::back_inserter(buffer), " - Show Black Bars for Third-Person Cutscenes: {}\n", UseBlackBarsForCutscenes() ? "Yes" : "No");
        std::format_to(std::back_inserter(buffer), " - Performance Overlay: {}\n", toDisplayString(performanceOverlay));
        std::format_to(std::back_inserter(buffer), " - Performance Overlay Frequency: {} Hz\n", uint32_t(performanceOverlayFrequency));
        std::format_to(std::back_inserter(buffer), " - Stick Direction Threshold: {}\n", float(axisThreshold));
        std::format_to(std::back_inserter(buffer), " - Thumbstick Deadzone: {}\n", float(stickDeadzone));
        std::format_to(std::back_inserter(buffer), " - Weapon Sensitivity: {}\n", toDisplayString(GetSwingSensitivity()));
        std::format_to(std::back_inserter(buffer), " - Walking Direction: {}\n", toDisplayString(GetWalkingDirection()));
        std::format_to(std::back_inserter(buffer), " - Turn Mode: {}\n", toDisplayString(turnMode.load()));
        std::format_to(std::back_inserter(buffer), " - Prevent Ragdolling In First-Person: {}\n", ShouldPreventFirstPersonRagdoll() ? "Yes" : "No");
        return buffer;
    }
};

extern ModSettings& GetSettings();
extern void InitSettings();
extern void Settings_LogSavedSettings();
