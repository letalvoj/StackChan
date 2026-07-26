// voj-chan: WASM Settings implementation using browser localStorage
// Replaces ESP-IDF NVS flash storage with persistent browser key-value store
#include <settings.h>
#include <emscripten.h>
#include <stdio.h>
#include <string>

Settings::Settings(const std::string& ns, bool read_write)
    : ns_(ns), read_write_(read_write) {
}

Settings::~Settings() {
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    std::string full_key = ns_ + "." + key;
    char* result = (char*)EM_ASM_INT({
        var k = UTF8ToString($0);
        var val = localStorage.getItem(k);
        if (val === null) return 0;
        var len = lengthBytesUTF8(val) + 1;
        var buf = _malloc(len);
        stringToUTF8(val, buf, len);
        return buf;
    }, full_key.c_str());

    if (result) {
        std::string val(result);
        free(result);
        printf("[WASM_NVS] GetString('%s') = '%s'\n", full_key.c_str(), val.c_str());
        return val;
    }
    return default_value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    std::string full_key = ns_ + "." + key;
    printf("[WASM_NVS] SetString('%s', '%s') -> localStorage\n", full_key.c_str(), value.c_str());
    EM_ASM({
        localStorage.setItem(UTF8ToString($0), UTF8ToString($1));
    }, full_key.c_str(), value.c_str());
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    std::string full_key = ns_ + "." + key;
    int found = EM_ASM_INT({
        var val = localStorage.getItem(UTF8ToString($0));
        return (val !== null) ? 1 : 0;
    }, full_key.c_str());

    if (found) {
        int32_t val = EM_ASM_INT({
            return parseInt(localStorage.getItem(UTF8ToString($0)), 10) || 0;
        }, full_key.c_str());
        printf("[WASM_NVS] GetInt('%s') = %d\n", full_key.c_str(), val);
        return val;
    }
    return default_value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    std::string full_key = ns_ + "." + key;
    printf("[WASM_NVS] SetInt('%s', %d) -> localStorage\n", full_key.c_str(), value);
    EM_ASM({
        localStorage.setItem(UTF8ToString($0), String($1));
    }, full_key.c_str(), value);
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    std::string full_key = ns_ + "." + key;
    int found = EM_ASM_INT({
        var val = localStorage.getItem(UTF8ToString($0));
        return (val !== null) ? 1 : 0;
    }, full_key.c_str());

    if (found) {
        int val = EM_ASM_INT({
            return (localStorage.getItem(UTF8ToString($0)) === "1") ? 1 : 0;
        }, full_key.c_str());
        printf("[WASM_NVS] GetBool('%s') = %s\n", full_key.c_str(), val ? "true" : "false");
        return val != 0;
    }
    return default_value;
}

void Settings::SetBool(const std::string& key, bool value) {
    std::string full_key = ns_ + "." + key;
    printf("[WASM_NVS] SetBool('%s', %s) -> localStorage\n", full_key.c_str(), value ? "true" : "false");
    EM_ASM({
        localStorage.setItem(UTF8ToString($0), $1 ? "1" : "0");
    }, full_key.c_str(), value ? 1 : 0);
}

void Settings::EraseKey(const std::string& key) {
    std::string full_key = ns_ + "." + key;
    printf("[WASM_NVS] EraseKey('%s')\n", full_key.c_str());
    EM_ASM({
        localStorage.removeItem(UTF8ToString($0));
    }, full_key.c_str());
}

void Settings::EraseAll() {
    printf("[WASM_NVS] EraseAll(namespace='%s') — clearing all keys with prefix\n", ns_.c_str());
    EM_ASM({
        var prefix = UTF8ToString($0) + ".";
        var toRemove = [];
        for (var i = 0; i < localStorage.length; i++) {
            var k = localStorage.key(i);
            if (k && k.startsWith(prefix)) toRemove.push(k);
        }
        toRemove.forEach(function(k) { localStorage.removeItem(k); });
    }, ns_.c_str());
}
