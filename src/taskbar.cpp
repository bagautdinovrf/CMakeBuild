#include "taskbar.hpp"
#include "build_progress.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

namespace cb::platform {
namespace {

UINT taskbarButtonCreated() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarButtonCreated");
    return message;
}

} // namespace

struct Taskbar::Impl {
    HWND window{};
    ITaskbarList3* taskbar{};
    std::optional<BuildProgress> progress;
    bool building{}, succeeded{};

    ~Impl() {
        if (window) RemoveWindowSubclass(window, procedure, reinterpret_cast<UINT_PTR>(this));
        releaseTaskbar();
    }

    void releaseTaskbar() {
        if (!taskbar) return;
        taskbar->Release();
        taskbar = nullptr;
    }

    void applyProgress() const {
        if (!taskbar) return;
        if (progress) taskbar->SetProgressValue(window, progress->completed, progress->total);
        else taskbar->SetProgressState(window, building ? TBPF_INDETERMINATE : TBPF_NOPROGRESS);
    }

    void applyOverlay() const {
        if (!taskbar) return;
        if (!succeeded) {
            taskbar->SetOverlayIcon(window, nullptr, nullptr);
            return;
        }
        HICON icon{};
        if (SUCCEEDED(LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(103), LIM_SMALL, &icon))) {
            taskbar->SetOverlayIcon(window, icon, L"Проект собран");
            DestroyIcon(icon);
        }
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<Impl*>(data);
        if (message == taskbarButtonCreated()) {
            // Explorer can recreate the button while a build is in progress.
            self.releaseTaskbar();
            if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                    IID_ITaskbarList3, reinterpret_cast<void**>(&self.taskbar)))) {
                if (FAILED(self.taskbar->HrInit())) self.releaseTaskbar();
                self.applyProgress();
                self.applyOverlay();
            }
            return 0;
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, procedure, id);
            self.window = nullptr;
            self.releaseTaskbar();
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
};

Taskbar::Taskbar() = default;
Taskbar::~Taskbar() = default;

void Taskbar::bind(void* nativeHandle) {
    const auto window = static_cast<HWND>(nativeHandle);
    if (!window || !taskbarButtonCreated() || (impl_ && impl_->window == window)) return;
    auto impl = std::make_unique<Impl>();
    if (SetWindowSubclass(window, Impl::procedure, reinterpret_cast<UINT_PTR>(impl.get()),
            reinterpret_cast<DWORD_PTR>(impl.get()))) {
        impl->window = window;
        impl_ = std::move(impl);
        // Receive Explorer's notification even when the panel runs elevated.
        ChangeWindowMessageFilterEx(window, taskbarButtonCreated(), MSGFLT_ALLOW, nullptr);
    }
}

void Taskbar::update(bool building, bool succeeded, const BuildProgress* progress) {
    if (!impl_) return;
    const auto nextProgress = building && progress ? std::optional{*progress} : std::nullopt;
    succeeded = !building && succeeded;
    if (impl_->building != building || impl_->progress != nextProgress) {
        impl_->building = building;
        impl_->progress = nextProgress;
        impl_->applyProgress();
    }
    if (impl_->succeeded != succeeded) {
        impl_->succeeded = succeeded;
        impl_->applyOverlay();
    }
}

} // namespace cb::platform
