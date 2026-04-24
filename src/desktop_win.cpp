#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <cstring>
#include <string>
#include <vector>

static constexpr int APP_PORT = 18080;
static constexpr UINT_PTR STATUS_TIMER = 1;

static HWND g_hwnd = nullptr;
static HWND g_status = nullptr;
static HWND g_log = nullptr;
static HWND g_start_button = nullptr;
static HWND g_stop_button = nullptr;
static HWND g_open_button = nullptr;
static HWND g_refresh_button = nullptr;
static HWND g_send_full_button = nullptr;
static HWND g_send_6m_button = nullptr;
static HWND g_send_2m_button = nullptr;
static HFONT g_font = nullptr;
static PROCESS_INFORMATION g_backend = {};

static std::wstring utf8_to_wide(const std::string &text) {
    if (text.empty()) {
        return L"";
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (needed <= 0) {
        return L"";
    }
    std::wstring out((size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), needed);
    return out;
}

static std::string json_string_value(const std::string &json, const char *key) {
    std::string pattern = std::string("\"") + key + "\":";
    size_t p = json.find(pattern);
    if (p == std::string::npos) {
        return "";
    }
    p = json.find('"', p + pattern.size());
    if (p == std::string::npos) {
        return "";
    }
    ++p;
    std::string out;
    bool escaping = false;
    for (; p < json.size(); ++p) {
        char ch = json[p];
        if (escaping) {
            out.push_back(ch == 'n' ? '\n' : ch);
            escaping = false;
        } else if (ch == '\\') {
            escaping = true;
        } else if (ch == '"') {
            break;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

static bool json_bool_value(const std::string &json, const char *key) {
    std::string pattern = std::string("\"") + key + "\":";
    size_t p = json.find(pattern);
    return p != std::string::npos && json.compare(p + pattern.size(), 4, "true") == 0;
}

static std::wstring exe_dir(void) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, path + len);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir.resize(slash);
    }
    return dir;
}

static bool file_exists(const std::wstring &path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void append_log(const std::wstring &line) {
    if (!g_log) {
        return;
    }
    std::wstring text = line + L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

static void set_status(const std::wstring &text) {
    if (g_status) {
        SetWindowTextW(g_status, text.c_str());
    }
}

static bool backend_running(void) {
    if (!g_backend.hProcess) {
        return false;
    }
    DWORD wait = WaitForSingleObject(g_backend.hProcess, 0);
    return wait == WAIT_TIMEOUT;
}

static void close_backend_handles(void) {
    if (g_backend.hThread) {
        CloseHandle(g_backend.hThread);
        g_backend.hThread = nullptr;
    }
    if (g_backend.hProcess) {
        CloseHandle(g_backend.hProcess);
        g_backend.hProcess = nullptr;
    }
    g_backend.dwProcessId = 0;
    g_backend.dwThreadId = 0;
}

static std::string http_request(const wchar_t *method, const wchar_t *path, const char *body, DWORD *status_code) {
    if (status_code) {
        *status_code = 0;
    }
    HINTERNET session = WinHttpOpen(L"PropagationDesktop/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return "";
    }
    HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", APP_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return "";
    }
    HINTERNET request = WinHttpOpenRequest(connect, method, path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return "";
    }

    DWORD body_len = body ? (DWORD)strlen(body) : 0;
    const wchar_t *headers = body_len > 0 ? L"Content-Type: application/x-www-form-urlencoded\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD header_len = body_len > 0 ? (DWORD)-1L : 0;
    BOOL ok = WinHttpSendRequest(request, headers, header_len,
        body_len > 0 ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA, body_len, body_len, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
    }

    std::string response;
    if (ok) {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr, &status, &status_size, nullptr);
        if (status_code) {
            *status_code = status;
        }
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
                break;
            }
            std::vector<char> buffer(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) {
                break;
            }
            response.append(buffer.data(), buffer.data() + read);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

static bool post_action(const wchar_t *path, const char *body, const wchar_t *label) {
    DWORD status = 0;
    std::string response = http_request(L"POST", path, body, &status);
    if (status >= 200 && status < 400) {
        append_log(std::wstring(label) + L" 已提交");
        return true;
    }
    append_log(std::wstring(label) + L" 失败，HTTP=" + std::to_wstring(status));
    if (!response.empty()) {
        append_log(utf8_to_wide(response));
    }
    return false;
}

static void start_backend(void) {
    if (backend_running()) {
        append_log(L"后台已经在运行");
        return;
    }
    close_backend_handles();

    std::wstring backend_path = exe_dir() + L"\\propagation_bot.exe";
    if (!file_exists(backend_path)) {
        append_log(L"未找到 propagation_bot.exe，请把桌面程序和后台程序放在同一目录");
        set_status(L"后台程序缺失");
        return;
    }

    std::wstring cmd = L"\"" + backend_path + L"\" --no-browser --hide-console --bind 127.0.0.1 --port " +
        std::to_wstring(APP_PORT);
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION proc = {};
    BOOL ok = CreateProcessW(backend_path.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, exe_dir().c_str(), &startup, &proc);
    if (!ok) {
        append_log(L"后台启动失败，错误码=" + std::to_wstring(GetLastError()));
        set_status(L"后台启动失败");
        return;
    }
    g_backend = proc;
    append_log(L"后台已启动：http://127.0.0.1:" + std::to_wstring(APP_PORT) + L"/");
    set_status(L"后台启动中");
}

static void stop_backend(void) {
    if (!backend_running()) {
        append_log(L"后台未运行");
        close_backend_handles();
        return;
    }
    post_action(L"/actions/shutdown", "", L"停止后台");
    if (WaitForSingleObject(g_backend.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(g_backend.hProcess, 1);
        append_log(L"后台未及时退出，已结束由桌面壳启动的进程");
    }
    close_backend_handles();
    set_status(L"后台未运行");
}

static void open_web_ui(void) {
    ShellExecuteW(g_hwnd, L"open", (L"http://127.0.0.1:" + std::to_wstring(APP_PORT) + L"/").c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);
}

static void refresh_status(void) {
    if (!backend_running()) {
        close_backend_handles();
        set_status(L"后台未运行");
        EnableWindow(g_start_button, TRUE);
        EnableWindow(g_stop_button, FALSE);
        EnableWindow(g_open_button, FALSE);
        EnableWindow(g_refresh_button, FALSE);
        EnableWindow(g_send_full_button, FALSE);
        EnableWindow(g_send_6m_button, FALSE);
        EnableWindow(g_send_2m_button, FALSE);
        return;
    }

    EnableWindow(g_start_button, FALSE);
    EnableWindow(g_stop_button, TRUE);
    EnableWindow(g_open_button, TRUE);
    EnableWindow(g_refresh_button, TRUE);
    EnableWindow(g_send_full_button, TRUE);
    EnableWindow(g_send_6m_button, TRUE);
    EnableWindow(g_send_2m_button, TRUE);

    DWORD status = 0;
    std::string body = http_request(L"GET", L"/api/status", nullptr, &status);
    if (status == 200 && !body.empty()) {
        std::wstring refresh_state = utf8_to_wide(json_string_value(body, "status"));
        std::wstring last_refresh = utf8_to_wide(json_string_value(body, "last_refreshed_text"));
        bool refreshing = json_bool_value(body, "refreshing");
        std::wstring text = L"后台运行中";
        if (!refresh_state.empty()) {
            text += L"：" + refresh_state;
        }
        if (refreshing) {
            text += L"（正在刷新）";
        }
        if (!last_refresh.empty()) {
            text += L" | 最后刷新 " + last_refresh;
        }
        set_status(text);
    } else if (status == 401) {
        set_status(L"后台运行中，状态接口需要网页登录认证");
    } else {
        set_status(L"后台运行中，等待 HTTP 服务就绪");
    }
}

static HWND make_button(HWND parent, int id, const wchar_t *text) {
    HWND hwnd = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 120, 32, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hwnd;
}

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int margin = 14;
    int top = 14;
    int button_w = 116;
    int button_h = 32;
    int gap = 8;

    MoveWindow(g_status, margin, top, width - margin * 2, 28, TRUE);
    top += 40;

    HWND buttons[] = {
        g_start_button, g_stop_button, g_open_button, g_refresh_button,
        g_send_full_button, g_send_6m_button, g_send_2m_button
    };
    int x = margin;
    int y = top;
    for (HWND button : buttons) {
        if (x + button_w > width - margin) {
            x = margin;
            y += button_h + gap;
        }
        MoveWindow(button, x, y, button_w, button_h, TRUE);
        x += button_w + gap;
    }
    top = y + button_h + 14;
    MoveWindow(g_log, margin, top, width - margin * 2, rc.bottom - top - margin, TRUE);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            g_font = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
            g_status = CreateWindowW(L"STATIC", L"后台未运行", WS_CHILD | WS_VISIBLE,
                0, 0, 100, 24, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
            g_start_button = make_button(hwnd, 1001, L"启动后台");
            g_stop_button = make_button(hwnd, 1002, L"停止后台");
            g_open_button = make_button(hwnd, 1003, L"打开网页");
            g_refresh_button = make_button(hwnd, 1004, L"立即刷新");
            g_send_full_button = make_button(hwnd, 1005, L"发完整简报");
            g_send_6m_button = make_button(hwnd, 1006, L"发 6m");
            g_send_2m_button = make_button(hwnd, 1007, L"发 2m");
            g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                0, 0, 100, 100, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font, TRUE);
            SetTimer(hwnd, STATUS_TIMER, 3000, nullptr);
            layout(hwnd);
            refresh_status();
            return 0;
        case WM_SIZE:
            layout(hwnd);
            return 0;
        case WM_TIMER:
            if (wparam == STATUS_TIMER) {
                refresh_status();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case 1001:
                    start_backend();
                    refresh_status();
                    return 0;
                case 1002:
                    stop_backend();
                    refresh_status();
                    return 0;
                case 1003:
                    open_web_ui();
                    return 0;
                case 1004:
                    post_action(L"/api/refresh?reason=desktop", "", L"立即刷新");
                    return 0;
                case 1005:
                    post_action(L"/actions/send", "kind=full", L"发送完整简报");
                    return 0;
                case 1006:
                    post_action(L"/actions/send", "kind=6m", L"发送 6m 简报");
                    return 0;
                case 1007:
                    post_action(L"/actions/send", "kind=2m", L"发送 2m 简报");
                    return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(hwnd, STATUS_TIMER);
            if (backend_running()) {
                stop_backend();
            }
            if (g_font) {
                DeleteObject(g_font);
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"PropagationDesktopWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"业余无线电传播助手",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 560,
        nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) {
        return 1;
    }
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
