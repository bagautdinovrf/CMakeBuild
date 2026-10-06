#include "platform.hpp"

#include <nana/gui/detail/bedrock.hpp>
#include <nana/gui/detail/internal_scope_guard.hpp>
#include <nana/gui/detail/window_manager.hpp>
#include <nana/gui/programming_interface.hpp>
#include <nana/gui/widgets/button.hpp>
#include <nana/gui/widgets/form.hpp>
#include <windows.h>

#include <array>
#include <atomic>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

auto& manager() { return nana::detail::bedrock::instance().wd_manager(); }

class HiddenForm : public nana::form {
public:
    HiddenForm()
        : nana::form{nana::rectangle{-32000, -32000, 120, 80},
              nana::appearance{false, false, false, true, false, false, false}} {
        cb::platform::preparePreviewWindow(native_handle());
    }
};

void dispatch() { manager().call_safe_place(GetCurrentThreadId()); }
void collect() { manager().remove_trash_handle(GetCurrentThreadId()); }
void pumpModal(nana::window window) {
    // Match modal_window's lock contract while keeping the fixture hidden.
    nana::internal_scope_guard lock;
    nana::detail::bedrock::instance().pump_event(window, true);
}

void modalCleanup() {
    unsigned calls{};
    HiddenForm modal;
    nana::button child{modal};
    child.events().destroy([&](const nana::arg_destroy& event) {
        nana::api::at_safe_place(event.window_handle, [&] { ++calls; });
    });
    const auto handle = modal.handle();
    expect(PostMessageW(reinterpret_cast<HWND>(modal.native_handle()), WM_CLOSE, 0, 0),
        "Could not post close to the hidden modal form");
    pumpModal(handle);
    // In the former implementation the modal loop collected the child but
    // never called its deferred cleanup. Check before another dispatch.
    expect(calls == 1, "Modal loop did not finish deferred destruction cleanup");
    dispatch();
    expect(calls == 1, "Modal cleanup ran more than once");
}

void collectedWindow(HiddenForm& owner) {
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        unsigned calls{};
        auto child = std::make_unique<nana::button>(owner);
        child->events().destroy([&](const nana::arg_destroy& event) {
            nana::api::at_safe_place(event.window_handle, [&] { ++calls; });
        });
        child.reset();
        collect();
        dispatch();
        dispatch();
        expect(calls == 1, "Cleanup after window collection did not run exactly once");
    }
}

void nestedDispatch(HiddenForm& owner) {
    nana::button first{owner}, second{owner}, third{owner};
    std::vector<unsigned> order;
    nana::api::at_safe_place(first, [&] {
        order.push_back(1);
        nana::api::at_safe_place(third, [&] { order.push_back(3); });
        dispatch();
        order.push_back(4);
    });
    nana::api::at_safe_place(second, [&] { order.push_back(2); });
    dispatch();
    dispatch();
    expect(order == std::vector<unsigned>{1, 2, 3, 4},
        "Nested dispatch lost FIFO order or repeated a callback");
}

void expandingQueue(HiddenForm& owner) {
    nana::button first{owner}, second{owner};
    std::array<unsigned, 128> calls{};
    std::vector<std::unique_ptr<nana::button>> children;
    for (unsigned index = 0; index < calls.size(); ++index)
        children.push_back(std::make_unique<nana::button>(owner));
    unsigned secondCalls{};
    nana::api::at_safe_place(first, [&] {
        for (unsigned index = 0; index < calls.size(); ++index)
            nana::api::at_safe_place(*children[index], [&, index] { ++calls[index]; });
    });
    nana::api::at_safe_place(second, [&] { ++secondCalls; });
    dispatch();
    dispatch();
    expect(secondCalls == 1, "Queue expansion lost the existing action");
    for (const auto count : calls)
        expect(count == 1, "Queue expansion lost or repeated an action");
}

void nestedModal(HiddenForm& owner) {
    nana::button first{owner}, second{owner};
    std::unique_ptr<HiddenForm> modal;
    unsigned firstCalls{}, secondCalls{};
    bool timedOut{};
    nana::api::at_safe_place(first, [&] {
        ++firstCalls;
        modal = std::make_unique<HiddenForm>();
        const auto native = reinterpret_cast<HWND>(modal->native_handle());
        // The fallback prevents a broken queue from hanging the test. It only
        // sends messages to this test's hidden window, without user input.
        const auto timer = SetTimer(native, 0, 1000,
            [](HWND window, UINT, UINT_PTR id, DWORD) {
                KillTimer(window, id);
                PostMessageW(window, WM_CLOSE, 0, 0);
            });
        expect(timer != 0, "Could not create the nested modal timeout");
        expect(PostMessageW(native, WM_NULL, 0, 0), "Could not wake the nested modal loop");
        pumpModal(modal->handle());
        timedOut = secondCalls == 0;
    });
    nana::api::at_safe_place(second, [&] {
        ++secondCalls;
        expect(modal != nullptr, "Nested modal action ran before the modal was created");
        modal->close();
    });
    dispatch();
    dispatch();
    expect(!timedOut && firstCalls == 1 && secondCalls == 1,
        "Pending action could not close a modal opened by an earlier action");
}

void callbackException(HiddenForm& owner) {
    unsigned firstCalls{}, secondCalls{};
    nana::api::at_safe_place(owner, [&] {
        ++firstCalls;
        throw std::runtime_error("Expected callback exception");
    });
    nana::api::at_safe_place(owner, [&] { ++secondCalls; });
    bool caught{};
    try { dispatch(); }
    catch (const std::runtime_error&) { caught = true; }
    dispatch();
    dispatch();
    expect(caught && firstCalls == 1 && secondCalls == 1,
        "Callback exception repeated an action or discarded pending work");
}

void threadOwnership(HiddenForm& owner) {
    std::promise<DWORD> ready;
    auto workerThread = ready.get_future();
    std::binary_semaphore proceed{0};
    std::atomic<unsigned> workerCalls{};
    std::atomic<bool> wrongThread{};
    std::exception_ptr workerError;
    std::jthread worker{[&] {
        bool reported{};
        try {
            HiddenForm window;
            const auto thread = GetCurrentThreadId();
            nana::api::at_safe_place(window, [&, thread] {
                wrongThread = GetCurrentThreadId() != thread;
                ++workerCalls;
            });
            ready.set_value(thread);
            reported = true;
            proceed.acquire();
            dispatch();
            dispatch();
            window.close();
            collect();
        } catch (...) {
            workerError = std::current_exception();
            if (!reported) ready.set_exception(workerError);
        }
    }};
    // Release before joining even if an expectation or GUI action throws.
    struct Release {
        std::binary_semaphore& signal;
        bool done{};
        ~Release() { if (!done) signal.release(); }
    } release{proceed};
    const auto other = workerThread.get();
    unsigned mainCalls{};
    const auto mainThread = GetCurrentThreadId();
    nana::api::at_safe_place(owner, [&] {
        expect(GetCurrentThreadId() == mainThread, "Main action ran on another thread");
        ++mainCalls;
    });
    dispatch();
    const auto beforeWorkerDispatch = workerCalls.load();
    proceed.release();
    release.done = true;
    worker.join();
    if (workerError) std::rethrow_exception(workerError);
    expect(other != mainThread && mainCalls == 1 && beforeWorkerDispatch == 0
        && workerCalls == 1 && !wrongThread,
        "Dispatch consumed another GUI thread's work or changed thread ownership");
}

void lastWindowCleanup() {
    HiddenForm window;
    unsigned firstCalls{}, secondCalls{};
    nana::api::at_safe_place(window, [&] {
        ++firstCalls;
        window.events().destroy([&](const nana::arg_destroy& event) {
            nana::api::at_safe_place(event.window_handle, [&] { ++secondCalls; });
        });
        window.close();
    });
    dispatch();
    expect(window.empty() && firstCalls == 1 && secondCalls == 1,
        "Closing the last window left deferred cleanup waiting for a new message");
    collect();
}
}

int main() {
    cb::platform::initialize();
    int result{};
    try {
        // Preserve the thread context while individual fixtures close windows.
        {
            HiddenForm context;
            modalCleanup();
            collectedWindow(context);
            nestedDispatch(context);
            expandingQueue(context);
            nestedModal(context);
            callbackException(context);
            threadOwnership(context);
            context.close();
            collect();
        }
        lastWindowCleanup();
        std::cout << "Deferred GUI cleanup, nested modal loops, FIFO, exceptions and threads passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    cb::platform::shutdown();
    return result;
}
