#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;
using simple_json::Value;

struct ConfigError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ProgressState {
    long long last_percent = -1;
};

struct MessageRef {
    long long chat_id{};
    long long message_id{};
};

enum class MenuState {
    Main,
    UpdateMenu,
    AwaitZip,
};

struct ChatState {
    MenuState state{MenuState::Main};
};

static size_t write_to_string(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), total);
    return total;
}

static size_t write_to_file(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* out = static_cast<std::ofstream*>(userp);
    out->write(static_cast<char*>(contents), total);
    return total;
}

std::string getenv_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

fs::path resolve_target_dir() {
    std::string target = getenv_or_empty("PC_UPDATER_TARGET_DIR");
    if (target.empty()) {
        throw ConfigError("PC_UPDATER_TARGET_DIR is not set");
    }
    fs::path path = fs::path(target);
    if (!fs::exists(path) || !fs::is_directory(path)) {
        throw ConfigError("Target directory does not exist: " + path.string());
    }
    return path;
}

fs::path resolve_target_exe(const fs::path& target_dir) {
    std::string exe_env = getenv_or_empty("PC_UPDATER_TARGET_EXE");
    if (!exe_env.empty()) {
        fs::path exe_path = fs::path(exe_env);
        if (exe_path.is_relative()) {
            exe_path = target_dir / exe_path;
        }
        if (!fs::exists(exe_path)) {
            throw ConfigError("Configured exe not found: " + exe_path.string());
        }
        return exe_path;
    }

    std::vector<fs::path> exes;
    for (const auto& entry : fs::directory_iterator(target_dir)) {
        if (entry.path().extension() == ".exe") {
            exes.push_back(entry.path());
        }
    }

    if (exes.size() == 1) {
        return exes.front();
    }
    if (exes.empty()) {
        throw ConfigError("No .exe file found in target directory");
    }
    throw ConfigError("Multiple .exe files found; set PC_UPDATER_TARGET_EXE.");
}

std::string resolve_github_repo() {
    std::string repo = getenv_or_empty("PC_UPDATER_GITHUB_REPO");
    if (repo.empty()) {
        throw ConfigError("PC_UPDATER_GITHUB_REPO is not set");
    }
    return repo;
}

std::string resolve_autostart_name(const fs::path& exe_path) {
    std::string name = getenv_or_empty("PC_UPDATER_AUTOSTART_NAME");
    if (name.empty()) {
        name = exe_path.stem().string();
    }
    return name;
}

fs::path get_self_exe_path() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (size == 0) {
        throw std::runtime_error("Failed to resolve updater exe path");
    }
    return fs::path(buffer);
#else
    return fs::current_path();
#endif
}

fs::path temp_zip_path(const std::string& prefix) {
    fs::path base = fs::temp_directory_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return base / (prefix + "-" + std::to_string(now) + ".zip");
}

std::string http_get(const std::string& url, const std::vector<std::string>& headers = {}) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to init curl");
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* header_list = nullptr;
    for (const auto& header : headers) {
        header_list = curl_slist_append(header_list, header.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    CURLcode res = curl_easy_perform(curl);
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP GET failed: " + std::string(curl_easy_strerror(res)));
    }
    curl_easy_cleanup(curl);
    return response;
}

struct DownloadContext {
    ProgressState* progress;
    std::function<void(int)> progress_callback;
};

static int curl_progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DownloadContext*>(clientp);
    if (dltotal <= 0) {
        return 0;
    }
    int percent = static_cast<int>((dlnow * 100) / dltotal);
    if (ctx->progress && percent != ctx->progress->last_percent) {
        ctx->progress->last_percent = percent;
        if (ctx->progress_callback) {
            ctx->progress_callback(percent);
        }
    }
    return 0;
}

void http_download(const std::string& url, const fs::path& out_path, std::function<void(int)> progress_cb) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to init curl");
    }
    std::ofstream out(out_path, std::ios::binary);
    if (!out.is_open()) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("Failed to open download file");
    }

    ProgressState progress;
    DownloadContext ctx{&progress, std::move(progress_cb)};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        throw std::runtime_error("Download failed: " + std::string(curl_easy_strerror(res)));
    }
}

std::string url_encode(CURL* curl, const std::string& text) {
    char* encoded = curl_easy_escape(curl, text.c_str(), static_cast<int>(text.size()));
    if (!encoded) {
        return "";
    }
    std::string result(encoded);
    curl_free(encoded);
    return result;
}

std::string telegram_post(const std::string& token, const std::string& method, const std::map<std::string, std::string>& params) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to init curl");
    }
    std::string url = "https://api.telegram.org/bot" + token + "/" + method;

    std::ostringstream body;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first) {
            body << "&";
        }
        first = false;
        body << key << "=" << url_encode(curl, value);
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.str().c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        throw std::runtime_error("Telegram request failed: " + std::string(curl_easy_strerror(res)));
    }
    return response;
}

MessageRef send_message(const std::string& token, long long chat_id, const std::string& text, const std::string& reply_markup_json = "") {
    std::map<std::string, std::string> params{{"chat_id", std::to_string(chat_id)}, {"text", text}};
    if (!reply_markup_json.empty()) {
        params.emplace("reply_markup", reply_markup_json);
    }
    std::string response = telegram_post(token, "sendMessage", params);
    Value json = simple_json::parse(response);
    const auto& result = json.at("result").as_object();
    MessageRef ref;
    ref.chat_id = static_cast<long long>(result.at("chat").at("id").as_number());
    ref.message_id = static_cast<long long>(result.at("message_id").as_number());
    return ref;
}

void edit_message(const std::string& token, const MessageRef& message, const std::string& text) {
    telegram_post(
        token,
        "editMessageText",
        {
            {"chat_id", std::to_string(message.chat_id)},
            {"message_id", std::to_string(message.message_id)},
            {"text", text},
        });
}

std::string keyboard_main() {
    return R"({"keyboard":[["Обновить"]],"resize_keyboard":true})";
}

std::string keyboard_update_menu() {
    return R"({"keyboard":[["GitHub-ом",".zip-ом"],["Выйти"]],"resize_keyboard":true})";
}

std::string render_progress(const std::string& label, int percent) {
    int bars = 10;
    int filled = std::max(0, std::min(bars, percent / 10));
    std::string bar = "[" + std::string(filled, '=') + std::string(bars - filled, ' ') + "]";
    return label + "\n" + bar + " " + std::to_string(percent) + "%";
}

fs::path extract_zip(const fs::path& zip_path) {
    fs::path extract_dir = fs::temp_directory_path() / ("pc-updater-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(extract_dir);
#ifdef _WIN32
    std::wstring command = L"powershell -Command \"Expand-Archive -Force -Path '" + zip_path.wstring() + L"' -DestinationPath '" + extract_dir.wstring() + L"'\"";
    int result = _wsystem(command.c_str());
#else
    std::string command = "unzip -o -q \"" + zip_path.string() + "\" -d \"" + extract_dir.string() + "\"";
    int result = std::system(command.c_str());
#endif
    if (result != 0) {
        throw std::runtime_error("Failed to extract zip");
    }
    return extract_dir;
}

fs::path find_content_root(const fs::path& extract_dir) {
    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(extract_dir)) {
        entries.push_back(entry.path());
    }
    if (entries.size() == 1 && fs::is_directory(entries.front())) {
        return entries.front();
    }
    return extract_dir;
}

void collect_paths(const fs::path& root, std::set<fs::path>& files, std::set<fs::path>& dirs) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        fs::path relative = fs::relative(entry.path(), root);
        if (entry.is_directory()) {
            dirs.insert(relative);
        } else {
            files.insert(relative);
            dirs.insert(relative.parent_path());
        }
    }
    dirs.erase(fs::path("."));
}

void remove_extraneous(const fs::path& target_dir, const std::set<fs::path>& files, const std::set<fs::path>& dirs) {
    std::vector<fs::path> entries;
    for (const auto& entry : fs::recursive_directory_iterator(target_dir)) {
        entries.push_back(entry.path());
    }
    std::sort(entries.begin(), entries.end(), [](const fs::path& a, const fs::path& b) {
        return a.string().size() > b.string().size();
    });

    for (const auto& path : entries) {
        fs::path relative = fs::relative(path, target_dir);
        if (fs::is_directory(path)) {
            if (dirs.find(relative) == dirs.end()) {
                std::error_code ec;
                fs::remove_all(path, ec);
            }
        } else {
            if (files.find(relative) == files.end()) {
                std::error_code ec;
                fs::remove(path, ec);
            }
        }
    }
}

void copy_updated_files(const fs::path& root, const fs::path& target_dir, const std::set<fs::path>& files) {
    for (const auto& relative : files) {
        fs::path source = root / relative;
        fs::path destination = target_dir / relative;
        fs::create_directories(destination.parent_path());
        std::error_code ec;
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw std::runtime_error("Failed to copy file: " + destination.string());
        }
    }
}

fs::path find_exe_in_content(const fs::path& root) {
    std::string exe_env = getenv_or_empty("PC_UPDATER_TARGET_EXE");
    if (!exe_env.empty()) {
        fs::path exe_path = fs::path(exe_env);
        if (exe_path.is_relative()) {
            exe_path = root / exe_path;
        }
        if (fs::exists(exe_path)) {
            return exe_path;
        }
    }

    std::vector<fs::path> exes;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.path().extension() == ".exe") {
            exes.push_back(entry.path());
        }
    }
    if (exes.size() == 1) {
        return exes.front();
    }
    if (exes.empty()) {
        throw std::runtime_error("Не найден .exe файл в обновлении.");
    }
    throw std::runtime_error("Найдено несколько .exe в обновлении. Укажите PC_UPDATER_TARGET_EXE.");
}

void verify_exe_launch(const fs::path& exe_path) {
#ifdef _WIN32
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    std::wstring command = L"\"" + exe_path.wstring() + L"\"";
    std::vector<wchar_t> cmdline(command.begin(), command.end());
    cmdline.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, exe_path.parent_path().c_str(), &si, &pi)) {
        throw std::runtime_error("Тестовый запуск не удался.");
    }
    DWORD wait_result = WaitForSingleObject(pi.hProcess, 5000);
    if (wait_result == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (exit_code != 0) {
            throw std::runtime_error("Тестовый запуск завершился с ошибкой.");
        }
        return;
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    std::cout << "Executable test is only supported on Windows." << std::endl;
#endif
}

struct PreparedUpdate {
    fs::path extract_dir;
    fs::path exe_path;
};

PreparedUpdate prepare_update(const fs::path& zip_path) {
    fs::path extract_dir = extract_zip(zip_path);
    fs::path content_root = find_content_root(extract_dir);
    fs::path exe_path = find_exe_in_content(content_root);
    return {extract_dir, exe_path};
}

void apply_update(const fs::path& extract_dir, const fs::path& target_dir) {
    fs::path content_root = find_content_root(extract_dir);
    std::set<fs::path> files;
    std::set<fs::path> dirs;
    collect_paths(content_root, files, dirs);
    remove_extraneous(target_dir, files, dirs);
    for (const auto& dir : dirs) {
        fs::create_directories(target_dir / dir);
    }
    copy_updated_files(content_root, target_dir, files);
}

void stop_running_exe(const fs::path& exe_path) {
#ifdef _WIN32
    std::wstring command = L"taskkill /F /IM \"" + exe_path.filename().wstring() + L"\"";
    _wsystem(command.c_str());
#else
    (void)exe_path;
#endif
}

void start_exe(const fs::path& exe_path) {
#ifdef _WIN32
    std::wstring command = L"\"" + exe_path.wstring() + L"\"";
    _wsystem(command.c_str());
#else
    (void)exe_path;
#endif
}

void ensure_autostart_registry(const fs::path& exe_path, const std::string& name) {
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    std::wstring wname(name.begin(), name.end());
    std::wstring wpath = exe_path.wstring();
    RegSetValueExW(key, wname.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(wpath.c_str()), static_cast<DWORD>((wpath.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
#else
    (void)exe_path;
    (void)name;
#endif
}

void ensure_autostart_startup_folder(const fs::path& exe_path, const std::string& name) {
#ifdef _WIN32
    PWSTR startup_path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &startup_path) != S_OK) {
        return;
    }
    fs::path shortcut = fs::path(startup_path) / (name + ".lnk");
    CoTaskMemFree(startup_path);
    if (fs::exists(shortcut)) {
        return;
    }
    std::wstring command = L"powershell -Command \"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('" + shortcut.wstring() + L"');$s.TargetPath='" + exe_path.wstring() + L"';$s.WorkingDirectory='" + exe_path.parent_path().wstring() + L"';$s.Save()\"";
    _wsystem(command.c_str());
#else
    (void)exe_path;
    (void)name;
#endif
}

void ensure_autostart_task_scheduler(const fs::path& exe_path, const std::string& name) {
#ifdef _WIN32
    std::wstring task_name = std::wstring(name.begin(), name.end()) + L" Logon";
    std::wstring check_cmd = L"schtasks /Query /TN \"" + task_name + L"\" >nul 2>&1";
    int query = _wsystem(check_cmd.c_str());
    if (query == 0) {
        return;
    }
    std::wstring create_cmd = L"schtasks /Create /F /SC ONLOGON /RL HIGHEST /TN \"" + task_name + L"\" /TR \"" + exe_path.wstring() + L"\"";
    _wsystem(create_cmd.c_str());
#else
    (void)exe_path;
    (void)name;
#endif
}

void ensure_autostart_all(const fs::path& exe_path, const std::string& name) {
    ensure_autostart_registry(exe_path, name);
    ensure_autostart_startup_folder(exe_path, name);
    ensure_autostart_task_scheduler(exe_path, name);
}

void restart_and_autostart(const fs::path& target_dir) {
    fs::path exe_path = resolve_target_exe(target_dir);
    stop_running_exe(exe_path);
    start_exe(exe_path);
    ensure_autostart_all(exe_path, resolve_autostart_name(exe_path));
}

fs::path download_github_latest_release(const std::string& repo, const std::string& token, std::function<void(int)> progress_cb) {
    std::string api_url = "https://api.github.com/repos/" + repo + "/releases/latest";
    std::vector<std::string> headers{"Accept: application/vnd.github+json", "User-Agent: PCcUpdater"};
    if (!token.empty()) {
        headers.push_back("Authorization: Bearer " + token);
    }
    std::string response = http_get(api_url, headers);
    Value json = simple_json::parse(response);
    const auto* assets_value = json.find("assets");
    if (!assets_value || !assets_value->is_array()) {
        throw std::runtime_error("Latest release does not contain assets");
    }
    std::string download_url;
    for (const auto& asset : assets_value->as_array()) {
        if (!asset.is_object()) {
            continue;
        }
        const auto* name_value = asset.find("name");
        const auto* url_value = asset.find("browser_download_url");
        if (!name_value || !url_value) {
            continue;
        }
        if (name_value->as_string().size() >= 4 && name_value->as_string().substr(name_value->as_string().size() - 4) == ".zip") {
            download_url = url_value->as_string();
            break;
        }
    }
    if (download_url.empty()) {
        throw std::runtime_error("Latest release does not contain a .zip asset");
    }

    fs::path out_path = temp_zip_path("github-release");
    http_download(download_url, out_path, std::move(progress_cb));
    return out_path;
}

std::string telegram_get_updates(const std::string& token, long long offset) {
    std::string url = "https://api.telegram.org/bot" + token + "/getUpdates?timeout=30";
    if (offset > 0) {
        url += "&offset=" + std::to_string(offset);
    }
    return http_get(url);
}

std::string download_telegram_file(const std::string& token, const std::string& file_id, std::function<void(int)> progress_cb) {
    std::string response = telegram_post(token, "getFile", {{"file_id", file_id}});
    Value json = simple_json::parse(response);
    std::string file_path = json.at("result").at("file_path").as_string();
    std::string url = "https://api.telegram.org/file/bot" + token + "/" + file_path;
    fs::path out_path = temp_zip_path("chat-upload");
    http_download(url, out_path, std::move(progress_cb));
    return out_path.string();
}

void cleanup_path(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path)) {
        fs::remove_all(path, ec);
    } else if (fs::exists(path)) {
        fs::remove(path, ec);
    }
}

void handle_update_flow(const std::string& token, long long chat_id, std::function<fs::path(std::function<void(int)>)> download_fn) {
    MessageRef progress = send_message(token, chat_id, render_progress("Скачивание...", 0));
    fs::path zip_path;
    fs::path extract_dir;
    try {
        zip_path = download_fn([&](int percent) {
            edit_message(token, progress, render_progress("Скачивание...", percent));
        });
        edit_message(token, progress, render_progress("Распаковка...", 0));
        PreparedUpdate prepared = prepare_update(zip_path);
        extract_dir = prepared.extract_dir;
        edit_message(token, progress, render_progress("Распаковка...", 100));
        edit_message(token, progress, render_progress("Тестирование...", 0));
        verify_exe_launch(prepared.exe_path);
        edit_message(token, progress, render_progress("Тестирование...", 100));
        edit_message(token, progress, render_progress("Установка...", 0));
        fs::path target_dir = resolve_target_dir();
        apply_update(extract_dir, target_dir);
        edit_message(token, progress, render_progress("Установка...", 100));
        restart_and_autostart(target_dir);
        ensure_autostart_all(get_self_exe_path(), "PCcUpdater");
        edit_message(token, progress, "Установка завершена.");
    } catch (const std::exception& ex) {
        send_message(token, chat_id, std::string("Ошибка при обновлении: ") + ex.what());
    }
    if (!zip_path.empty()) {
        cleanup_path(zip_path);
    }
    if (!extract_dir.empty()) {
        cleanup_path(extract_dir);
    }
}

std::string get_text_or_empty(const Value& message) {
    const auto* text = message.find("text");
    if (text && text->is_string()) {
        return text->as_string();
    }
    return "";
}

int main() {
#ifdef _WIN32
    FreeConsole();
    HWND console = GetConsoleWindow();
    if (console) {
        ShowWindow(console, SW_HIDE);
    }
#endif
    std::string token = getenv_or_empty("TELEGRAM_BOT_TOKEN");
    if (token.empty()) {
        std::cerr << "TELEGRAM_BOT_TOKEN is not set" << std::endl;
        return 1;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    std::map<long long, ChatState> chat_states;
    long long offset = 0;

    while (true) {
        try {
            std::string response = telegram_get_updates(token, offset);
            Value json = simple_json::parse(response);
            const auto* result_value = json.find("result");
            if (!result_value || !result_value->is_array()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            for (const auto& update : result_value->as_array()) {
                if (!update.is_object()) {
                    continue;
                }
                long long update_id = static_cast<long long>(update.at("update_id").as_number());
                offset = std::max(offset, update_id + 1);

                const auto* message_value = update.find("message");
                if (!message_value || !message_value->is_object()) {
                    continue;
                }
                const auto& message = message_value->as_object();
                long long chat_id = static_cast<long long>(message.at("chat").at("id").as_number());
                auto& state = chat_states[chat_id];

                std::string text = get_text_or_empty(message);
                if (text == "/start") {
                    send_message(token, chat_id, "Главное меню.", keyboard_main());
                    state.state = MenuState::Main;
                    continue;
                }

                if (!text.empty()) {
                    if (text == "Обновить") {
                        send_message(token, chat_id, "Выберите способ обновления:", keyboard_update_menu());
                        state.state = MenuState::UpdateMenu;
                        continue;
                    }
                    if (text == "GitHub-ом") {
                        state.state = MenuState::Main;
                        std::string repo = resolve_github_repo();
                        std::string gh_token = getenv_or_empty("PC_UPDATER_GITHUB_TOKEN");
                        handle_update_flow(token, chat_id, [&](std::function<void(int)> progress_cb) {
                            return download_github_latest_release(repo, gh_token, std::move(progress_cb));
                        });
                        continue;
                    }
                    if (text == ".zip-ом") {
                        state.state = MenuState::AwaitZip;
                        send_message(token, chat_id, "Отправьте .zip файлом для обновления.");
                        continue;
                    }
                    if (text == "Выйти") {
                        send_message(token, chat_id, "Главное меню.", keyboard_main());
                        state.state = MenuState::Main;
                        continue;
                    }
                }

                const auto* document_value = message_value->find("document");
                if (document_value && document_value->is_object() && state.state == MenuState::AwaitZip) {
                    const auto* file_name_value = document_value->find("file_name");
                    const auto* file_id_value = document_value->find("file_id");
                    if (file_name_value && file_id_value && file_name_value->is_string() && file_id_value->is_string()) {
                        std::string file_name = file_name_value->as_string();
                        if (file_name.size() >= 4 && file_name.substr(file_name.size() - 4) == ".zip") {
                            state.state = MenuState::Main;
                            std::string file_id = file_id_value->as_string();
                            handle_update_flow(token, chat_id, [&](std::function<void(int)> progress_cb) {
                                return fs::path(download_telegram_file(token, file_id, std::move(progress_cb)));
                            });
                        } else {
                            send_message(token, chat_id, "Нужен файл .zip. Попробуйте снова.");
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    curl_global_cleanup();
    return 0;
}
