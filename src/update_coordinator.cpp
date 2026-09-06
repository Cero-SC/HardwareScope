#include "hardwarescope/update_coordinator.hpp"

#include <process.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace hardwarescope {
namespace {

struct CoordinatorState final {
    std::atomic<bool> update_in_progress{};
    std::mutex completion_mutex;
    std::optional<UpdateCompletion> pending_completion;
};

CoordinatorState& SharedState() noexcept {
    // The worker is deliberately detached. Keep its small synchronization state
    // alive until ExitProcess so a user closing the app during WinHTTP work can
    // never race C++ static destruction.
    static auto* const state = new CoordinatorState{};
    return *state;
}

struct Work final {
    HWND window{};
    bool automatic{};
    std::optional<SemanticVersion> skipped_version;
    std::optional<UpdateManifest> download;
};

unsigned __stdcall CheckThread(void* const parameter) noexcept {
    std::unique_ptr<Work> work{static_cast<Work*>(parameter)};
    auto& state = SharedState();
    UpdateCompletion completion{};
    completion.automatic = work->automatic;
    if (work->download) {
        completion.manifest = *work->download;
        const auto installer = DownloadVerifiedInstaller(completion.manifest);
        if (installer) {
            completion.status = UpdateCompletionStatus::ready;
            completion.installer = *installer;
        } else {
            completion.status = UpdateCompletionStatus::failed;
            completion.system_error = ERROR_CRC;
        }
    } else {
        auto check = CheckStableUpdate(SemanticVersion{
            HARDWARESCOPE_VERSION_MAJOR,
            HARDWARESCOPE_VERSION_MINOR,
            HARDWARESCOPE_VERSION_PATCH});
        completion.system_error = check.system_error;
        if (check.status == UpdateCheckStatus::current) {
            completion.status = UpdateCompletionStatus::current;
        } else if (check.status == UpdateCheckStatus::available
            && work->automatic
            && work->skipped_version
            && check.manifest.version == *work->skipped_version) {
            completion.status = UpdateCompletionStatus::current;
        } else if (check.status == UpdateCheckStatus::available) {
            completion.status = UpdateCompletionStatus::available;
        }
        completion.manifest = std::move(check.manifest);
    }
    {
        const std::scoped_lock lock(state.completion_mutex);
        state.pending_completion = std::move(completion);
    }
    state.update_in_progress.store(false, std::memory_order_release);
    if (!PostMessageW(work->window, kUpdateCompletedMessage, 0U, 0U)) {
        const std::scoped_lock lock(state.completion_mutex);
        state.pending_completion.reset();
    }
    return 0U;
}

} // namespace

static bool BeginWork(
    const HWND notification_window,
    const bool automatic,
    std::optional<SemanticVersion> skipped_version,
    std::optional<UpdateManifest> download) noexcept {
    if (notification_window == nullptr) return false;
    auto& state = SharedState();
    bool expected{};
    if (!state.update_in_progress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    try {
        {
            const std::scoped_lock lock(state.completion_mutex);
            if (state.pending_completion) {
                state.update_in_progress.store(false, std::memory_order_release);
                return false; // UI must consume the previous result before another operation
            }
        }
        auto work = std::make_unique<Work>();
        work->window = notification_window;
        work->automatic = automatic;
        work->skipped_version = std::move(skipped_version);
        work->download = std::move(download);
        const auto thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0U, &CheckThread, work.get(), 0U, nullptr));
        if (thread == nullptr) {
            state.update_in_progress.store(false, std::memory_order_release);
            return false;
        }
        static_cast<void>(work.release());
        CloseHandle(thread);
        return true;
    } catch (...) {
        state.update_in_progress.store(false, std::memory_order_release);
        return false;
    }
}

bool BeginNativeUpdateCheck(HWND window, bool automatic, std::optional<SemanticVersion> skipped) noexcept {
    return BeginWork(window, automatic, std::move(skipped), std::nullopt);
}

bool BeginNativeUpdateDownload(HWND window, UpdateManifest manifest) noexcept {
    return BeginWork(window, false, std::nullopt, std::move(manifest));
}

std::optional<UpdateCompletion> TakeNativeUpdateCompletion() noexcept {
    try {
        auto& state = SharedState();
        const std::scoped_lock lock(state.completion_mutex);
        auto result = std::move(state.pending_completion);
        state.pending_completion.reset();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace hardwarescope
