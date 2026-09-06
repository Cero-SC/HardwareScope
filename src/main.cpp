#include "hardwarescope/native_window.hpp"

#include <windows.h>

#include <shellscalingapi.h>

#include <memory>
#include <new>

namespace {

#if HARDWARESCOPE_INTERNAL_TEST_HOOKS
constexpr wchar_t kInstanceMutex[] = L"Local\\HardwareScope.2.0.Instrumented";
#else
constexpr wchar_t kInstanceMutex[] = L"Local\\HardwareScope.2.0";
#endif

void ActivateExistingInstance() noexcept {
    if (const auto existing = FindWindowW(hardwarescope::NativeWindow::kWindowClass, nullptr); existing != nullptr) {
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        ShowWindow(existing, SW_SHOW);
        SetForegroundWindow(existing);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, const int show_command) {
    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const auto com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const auto mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (mutex == nullptr) {
        if (SUCCEEDED(com_result)) CoUninitialize();
        return 2;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        CloseHandle(mutex);
        if (SUCCEEDED(com_result)) CoUninitialize();
        return 0;
    }

    std::unique_ptr<hardwarescope::NativeWindow> application(new (std::nothrow) hardwarescope::NativeWindow(instance));
    const auto result = application != nullptr ? application->Run(show_command) : 3;
    // Flush background settings and destroy COM resources while the instance
    // mutex and COM apartment still belong to this process.
    application.reset();

    ReleaseMutex(mutex);
    CloseHandle(mutex);
    if (SUCCEEDED(com_result)) CoUninitialize();
    return result;
}
