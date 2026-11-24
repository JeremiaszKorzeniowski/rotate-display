#include "resource.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"RotateDisplayCppWindow";
constexpr wchar_t kWindowTitle[] = L"Obrót monitora";
constexpr wchar_t kDefaultDevice[] = L"\\\\.\\DISPLAY3";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr int kHotkeyRotate = 1;
constexpr int kHotkeyMedia = 2;
constexpr UINT kMediaVirtualKey = 0xB4;

constexpr UINT kCommandToggle = 100;
constexpr UINT kCommandSaveLayout = 101;
constexpr UINT kCommandRestoreLayout = 102;
constexpr UINT kCommandExit = 103;
constexpr UINT kCommandMonitorBase = 1000;

struct Position {
    LONG x{};
    LONG y{};
};

using Layout = std::map<std::wstring, Position>;

struct Config {
    std::wstring device;
    std::map<std::string, Layout> layouts;
};

struct MonitorInfo {
    std::wstring device;
    std::wstring name;
    DWORD width{};
    DWORD height{};
    LONG x{};
    LONG y{};
    bool primary{};
};

enum class JsonType { Null, String, Number, Object, Array, Boolean };

struct JsonValue {
    JsonType type{JsonType::Null};
    std::string text;
    long long number{};
    bool boolean{};
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    std::optional<JsonValue> Parse() {
        SkipWhitespace();
        auto value = ParseValue();
        SkipWhitespace();
        if (!value || position_ != input_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    void SkipWhitespace() {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool Consume(char expected) {
        SkipWhitespace();
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool ConsumeLiteral(const char* literal) {
        const std::string token(literal);
        if (input_.compare(position_, token.size(), token) != 0) {
            return false;
        }
        position_ += token.size();
        return true;
    }

    static int HexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static void AppendUtf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7F) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    std::optional<std::string> ParseString() {
        if (!Consume('"')) return std::nullopt;

        std::string result;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') return result;
            if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (position_ >= input_.size()) return std::nullopt;
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (position_ + 4 > input_.size()) return std::nullopt;
                    unsigned codepoint = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int value = HexValue(input_[position_++]);
                        if (value < 0) return std::nullopt;
                        codepoint = (codepoint << 4U) | static_cast<unsigned>(value);
                    }
                    AppendUtf8(result, codepoint);
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> ParseNumber() {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        const std::size_t digits = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (digits == position_) return std::nullopt;

        try {
            JsonValue value;
            value.type = JsonType::Number;
            value.number = std::stoll(input_.substr(start, position_ - start));
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<JsonValue> ParseObject() {
        if (!Consume('{')) return std::nullopt;
        JsonValue result;
        result.type = JsonType::Object;
        SkipWhitespace();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return result;
        }

        while (true) {
            auto key = ParseString();
            if (!key || !Consume(':')) return std::nullopt;
            auto value = ParseValue();
            if (!value) return std::nullopt;
            result.object.insert_or_assign(*key, std::move(*value));
            SkipWhitespace();
            if (position_ < input_.size() && input_[position_] == '}') {
                ++position_;
                return result;
            }
            if (!Consume(',')) return std::nullopt;
        }
    }

    std::optional<JsonValue> ParseArray() {
        if (!Consume('[')) return std::nullopt;
        JsonValue result;
        result.type = JsonType::Array;
        SkipWhitespace();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return result;
        }
        while (true) {
            auto value = ParseValue();
            if (!value) return std::nullopt;
            result.array.push_back(std::move(*value));
            SkipWhitespace();
            if (position_ < input_.size() && input_[position_] == ']') {
                ++position_;
                return result;
            }
            if (!Consume(',')) return std::nullopt;
        }
    }

    std::optional<JsonValue> ParseValue() {
        SkipWhitespace();
        if (position_ >= input_.size()) return std::nullopt;
        if (input_[position_] == '{') return ParseObject();
        if (input_[position_] == '[') return ParseArray();
        if (input_[position_] == '"') {
            auto text = ParseString();
            if (!text) return std::nullopt;
            JsonValue value;
            value.type = JsonType::String;
            value.text = std::move(*text);
            return value;
        }
        if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9')) {
            return ParseNumber();
        }
        JsonValue value;
        if (ConsumeLiteral("true")) {
            value.type = JsonType::Boolean;
            value.boolean = true;
            return value;
        }
        if (ConsumeLiteral("false")) {
            value.type = JsonType::Boolean;
            value.boolean = false;
            return value;
        }
        if (ConsumeLiteral("null")) return value;
        return std::nullopt;
    }

    const std::string& input_;
    std::size_t position_{};
};

HINSTANCE gInstance{};
HWND gWindow{};
NOTIFYICONDATAW gTray{};
HICON gTrayIcon{};
Config gConfig;
std::wstring gSelected = kDefaultDevice;
std::vector<MonitorInfo> gMenuMonitors;

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), output.data(), size);
    return output;
}

std::string JsonEscape(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 8);
    for (const unsigned char c : text) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    output += "\\u00";
                    output.push_back(hex[(c >> 4U) & 0x0F]);
                    output.push_back(hex[c & 0x0F]);
                } else {
                    output.push_back(static_cast<char>(c));
                }
        }
    }
    return output;
}

std::filesystem::path ConfigPath() {
    PWSTR appData = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appData))) {
        path = std::filesystem::path(appData) / L"RotateDisplay" / L"config.json";
        CoTaskMemFree(appData);
    }
    return path;
}

void LoadConfig() {
    gConfig = {};
    const auto path = ConfigPath();
    if (path.empty()) return;

    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto root = JsonParser(content).Parse();
    if (!root || root->type != JsonType::Object) return;

    const auto deviceIt = root->object.find("device");
    if (deviceIt != root->object.end() && deviceIt->second.type == JsonType::String) {
        gConfig.device = Utf8ToWide(deviceIt->second.text);
    }

    const auto layoutsIt = root->object.find("layouts");
    if (layoutsIt == root->object.end() || layoutsIt->second.type != JsonType::Object) return;

    for (const auto& [mode, layoutValue] : layoutsIt->second.object) {
        if (layoutValue.type != JsonType::Object) continue;
        Layout layout;
        for (const auto& [device, positionValue] : layoutValue.object) {
            if (positionValue.type != JsonType::Object) continue;
            const auto xIt = positionValue.object.find("x");
            const auto yIt = positionValue.object.find("y");
            if (xIt == positionValue.object.end() || yIt == positionValue.object.end() ||
                xIt->second.type != JsonType::Number || yIt->second.type != JsonType::Number) {
                continue;
            }
            layout[Utf8ToWide(device)] = {
                static_cast<LONG>(xIt->second.number),
                static_cast<LONG>(yIt->second.number)
            };
        }
        if (!layout.empty()) gConfig.layouts[mode] = std::move(layout);
    }
}

bool SaveConfig() {
    const auto path = ConfigPath();
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    const auto temporary = path.wstring() + L".tmp";
    std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    if (!file) return false;

    file << "{\n  \"device\": \"" << JsonEscape(WideToUtf8(gConfig.device)) << "\",\n";
    file << "  \"layouts\": {";
    bool firstMode = true;
    for (const auto& [mode, layout] : gConfig.layouts) {
        file << (firstMode ? "\n" : ",\n");
        firstMode = false;
        file << "    \"" << JsonEscape(mode) << "\": {";
        bool firstDevice = true;
        for (const auto& [device, position] : layout) {
            file << (firstDevice ? "\n" : ",\n");
            firstDevice = false;
            file << "      \"" << JsonEscape(WideToUtf8(device)) << "\": { \"x\": "
                 << position.x << ", \"y\": " << position.y << " }";
        }
        if (!layout.empty()) file << '\n';
        file << "    }";
    }
    if (!gConfig.layouts.empty()) file << '\n';
    file << "  }\n}\n";
    file.close();
    if (!file) {
        DeleteFileW(temporary.c_str());
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::optional<DEVMODEW> CurrentMode(const std::wstring& device) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
        return std::nullopt;
    }
    return mode;
}

std::vector<MonitorInfo> ListMonitors() {
    std::vector<MonitorInfo> result;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) break;
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;

        auto mode = CurrentMode(adapter.DeviceName);
        if (!mode) continue;

        std::wstring name = adapter.DeviceString;
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, 0) && monitor.DeviceString[0] != L'\0') {
            name = monitor.DeviceString;
        }

        result.push_back({
            adapter.DeviceName,
            name,
            mode->dmPelsWidth,
            mode->dmPelsHeight,
            mode->dmPosition.x,
            mode->dmPosition.y,
            (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0
        });
    }

    std::sort(result.begin(), result.end(), [](const MonitorInfo& left, const MonitorInfo& right) {
        if (left.x != right.x) return left.x < right.x;
        return left.device < right.device;
    });
    return result;
}

bool IsPortrait(const DEVMODEW& mode) {
    return mode.dmDisplayOrientation == DMDO_90 || mode.dmDisplayOrientation == DMDO_270;
}

std::string ModeName(const std::wstring& device) {
    const auto mode = CurrentMode(device);
    return mode && IsPortrait(*mode) ? "portrait" : "landscape";
}

Layout SnapshotLayout() {
    Layout layout;
    for (const auto& monitor : ListMonitors()) {
        layout[monitor.device] = {monitor.x, monitor.y};
    }
    return layout;
}

bool SaveLayout(const std::string& mode) {
    gConfig.layouts[mode] = SnapshotLayout();
    return SaveConfig();
}

bool ApplyLayout(const std::string& mode) {
    const auto layoutIt = gConfig.layouts.find(mode);
    if (layoutIt == gConfig.layouts.end() || layoutIt->second.empty()) return false;

    const auto monitors = ListMonitors();
    std::map<std::wstring, MonitorInfo> current;
    for (const auto& monitor : monitors) current[monitor.device] = monitor;
    if (current.size() != layoutIt->second.size()) return false;
    for (const auto& [device, position] : layoutIt->second) {
        if (!current.contains(device)) return false;
    }

    bool changed = false;
    for (const auto& [device, position] : layoutIt->second) {
        const auto& monitor = current.at(device);
        if (monitor.x == position.x && monitor.y == position.y) continue;
        auto displayMode = CurrentMode(device);
        if (!displayMode) return false;
        displayMode->dmPosition.x = position.x;
        displayMode->dmPosition.y = position.y;
        displayMode->dmFields = DM_POSITION;
        const LONG status = ChangeDisplaySettingsExW(
            device.c_str(), &*displayMode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        if (status != DISP_CHANGE_SUCCESSFUL) return false;
        changed = true;
    }

    if (changed) {
        return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL;
    }
    return true;
}

bool Rotate(const std::wstring& device, DWORD target) {
    auto mode = CurrentMode(device);
    if (!mode) return false;
    const bool wasPortrait = IsPortrait(*mode);
    const bool willBePortrait = target == DMDO_90 || target == DMDO_270;
    if (wasPortrait != willBePortrait) {
        std::swap(mode->dmPelsWidth, mode->dmPelsHeight);
    }
    mode->dmDisplayOrientation = target;
    mode->dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
    if (ChangeDisplaySettingsExW(device.c_str(), &*mode, nullptr, CDS_TEST, nullptr) !=
        DISP_CHANGE_SUCCESSFUL) {
        return false;
    }
    return ChangeDisplaySettingsExW(device.c_str(), &*mode, nullptr, CDS_UPDATEREGISTRY, nullptr) ==
           DISP_CHANGE_SUCCESSFUL;
}

void Notify(const std::wstring& message, DWORD flags = NIIF_INFO) {
    NOTIFYICONDATAW notification = gTray;
    notification.uFlags = NIF_INFO;
    notification.dwInfoFlags = flags;
    lstrcpynW(notification.szInfoTitle, kWindowTitle, ARRAYSIZE(notification.szInfoTitle));
    lstrcpynW(notification.szInfo, message.c_str(), ARRAYSIZE(notification.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &notification);
}

void ToggleOrientation() {
    const auto monitors = ListMonitors();
    const bool available = std::any_of(monitors.begin(), monitors.end(), [](const MonitorInfo& monitor) {
        return monitor.device == gSelected;
    });
    if (!available) {
        Notify(L"Wybrany monitor nie jest dostępny. Wybierz inny z menu.", NIIF_WARNING);
        return;
    }

    const auto current = CurrentMode(gSelected);
    if (!current) return;
    const bool portrait = IsPortrait(*current);
    const std::string before = portrait ? "portrait" : "landscape";
    const std::string after = portrait ? "landscape" : "portrait";
    SaveLayout(before);
    if (!Rotate(gSelected, portrait ? DMDO_DEFAULT : DMDO_90)) {
        Notify(L"Nie udało się zmienić orientacji.", NIIF_ERROR);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ApplyLayout(after);
}

HICON CreateTrayIcon() {
    constexpr int size = 32;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return nullptr;

    auto* pixels = static_cast<std::uint32_t*>(bits);
    std::fill(pixels, pixels + size * size, 0U);
    constexpr std::uint32_t blue = 0xFF00A2E8U;
    const auto setPixel = [&](int x, int y) {
        if (x >= 0 && x < size && y >= 0 && y < size) pixels[y * size + x] = blue;
    };
    const auto outline = [&](int left, int top, int right, int bottom, int width) {
        for (int offset = 0; offset < width; ++offset) {
            for (int x = left; x <= right; ++x) {
                setPixel(x, top + offset);
                setPixel(x, bottom - offset);
            }
            for (int y = top; y <= bottom; ++y) {
                setPixel(left + offset, y);
                setPixel(right - offset, y);
            }
        }
    };
    outline(6, 1, 25, 30, 3);
    outline(1, 6, 30, 25, 3);

    std::uint8_t maskBits[(size * size) / 8]{};
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void AddTrayIcon() {
    gTray = {};
    gTray.cbSize = sizeof(gTray);
    gTray.hWnd = gWindow;
    gTray.uID = kTrayIconId;
    gTray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    gTray.uCallbackMessage = kTrayCallback;
    gTrayIcon = CreateTrayIcon();
    gTray.hIcon = gTrayIcon ? gTrayIcon : LoadIconW(gInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    lstrcpynW(gTray.szTip, L"Obrót monitora (Ctrl+Alt+R)", ARRAYSIZE(gTray.szTip));
    Shell_NotifyIconW(NIM_ADD, &gTray);
    gTray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &gTray);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &gTray);
    if (gTrayIcon) {
        DestroyIcon(gTrayIcon);
        gTrayIcon = nullptr;
    }
}

std::wstring MonitorLabel(const MonitorInfo& monitor, std::size_t index) {
    std::wostringstream label;
    label << (index + 1) << L". " << monitor.name << L"  " << monitor.width << L'x'
          << monitor.height << L"  [" << monitor.device << L']';
    if (monitor.primary) label << L"  (główny)";
    return label.str();
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    HMENU monitorMenu = CreatePopupMenu();
    if (!menu || !monitorMenu) {
        if (monitorMenu) DestroyMenu(monitorMenu);
        if (menu) DestroyMenu(menu);
        return;
    }

    gMenuMonitors = ListMonitors();
    if (gMenuMonitors.empty()) {
        AppendMenuW(monitorMenu, MF_STRING | MF_GRAYED, 0, L"Brak wykrytych monitorów");
    } else {
        for (std::size_t index = 0; index < gMenuMonitors.size(); ++index) {
            UINT flags = MF_STRING;
            if (gMenuMonitors[index].device == gSelected) flags |= MF_CHECKED;
            AppendMenuW(monitorMenu, flags, kCommandMonitorBase + static_cast<UINT>(index),
                        MonitorLabel(gMenuMonitors[index], index).c_str());
        }
    }

    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kCommandToggle, L"Przełącz orientację");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(monitorMenu), L"Monitor do obsługi");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandSaveLayout, L"Zapisz bieżący układ monitorów");
    AppendMenuW(menu, MF_STRING, kCommandRestoreLayout, L"Przywróć zapisany układ");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandExit, L"Zakończ");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(gWindow);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   cursor.x, cursor.y, 0, gWindow, nullptr);
    PostMessageW(gWindow, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void SelectInitialMonitor() {
    const auto monitors = ListMonitors();
    if (!gConfig.device.empty()) gSelected = gConfig.device;
    const auto selected = std::find_if(monitors.begin(), monitors.end(), [](const MonitorInfo& monitor) {
        return monitor.device == gSelected;
    });
    if (selected != monitors.end()) return;

    const auto fallback = std::find_if(monitors.begin(), monitors.end(), [](const MonitorInfo& monitor) {
        return monitor.device == kDefaultDevice;
    });
    if (fallback != monitors.end()) {
        gSelected = fallback->device;
        return;
    }
    const auto primary = std::find_if(monitors.begin(), monitors.end(), [](const MonitorInfo& monitor) {
        return monitor.primary;
    });
    if (primary != monitors.end()) gSelected = primary->device;
}

void HandleCommand(UINT command) {
    if (command >= kCommandMonitorBase &&
        command < kCommandMonitorBase + gMenuMonitors.size()) {
        gSelected = gMenuMonitors[command - kCommandMonitorBase].device;
        gConfig.device = gSelected;
        if (!SaveConfig()) Notify(L"Nie udało się zapisać wyboru monitora.", NIIF_WARNING);
        return;
    }

    switch (command) {
        case kCommandToggle:
            ToggleOrientation();
            break;
        case kCommandSaveLayout: {
            const std::string mode = ModeName(gSelected);
            if (SaveLayout(mode)) {
                Notify(mode == "portrait" ? L"Zapisano układ dla trybu pionowego."
                                           : L"Zapisano układ dla trybu poziomego.");
            } else {
                Notify(L"Nie udało się zapisać układu monitorów.", NIIF_WARNING);
            }
            break;
        }
        case kCommandRestoreLayout:
            if (!ApplyLayout(ModeName(gSelected))) {
                Notify(L"Brak zgodnego zapisanego układu monitorów.", NIIF_WARNING);
            }
            break;
        case kCommandExit:
            DestroyWindow(gWindow);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND:
            HandleCommand(LOWORD(wParam));
            return 0;
        case WM_HOTKEY:
            if (wParam == kHotkeyRotate || wParam == kHotkeyMedia) ToggleOrientation();
            return 0;
        case kTrayCallback:
            if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) {
                ShowTrayMenu();
            } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                ToggleOrientation();
            }
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(window, kHotkeyRotate);
            UnregisterHotKey(window, kHotkeyMedia);
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    gInstance = instance;

    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\RotateDisplayCppSingleInstance");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleInstance) CloseHandle(singleInstance);
        return 0;
    }

    LoadConfig();
    SelectInitialMonitor();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        CloseHandle(singleInstance);
        return 1;
    }

    gWindow = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPED,
                              0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!gWindow) {
        CloseHandle(singleInstance);
        return 1;
    }

    AddTrayIcon();
    if (!RegisterHotKey(gWindow, kHotkeyRotate, MOD_ALT | MOD_CONTROL | MOD_NOREPEAT, 'R')) {
        Notify(L"Skrót Ctrl+Alt+R jest już zajęty.", NIIF_WARNING);
    }
    if (!RegisterHotKey(gWindow, kHotkeyMedia, MOD_NOREPEAT, kMediaVirtualKey)) {
        Notify(L"Klawisz multimedialny jest już zajęty.", NIIF_WARNING);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CloseHandle(singleInstance);
    return static_cast<int>(message.wParam);
}
