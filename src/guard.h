// A thread's work, guarded: a C++ exception that would leave it (std::terminate: X-Plane's whole process gone, with no word in its log)
// is caught and told instead, and only that thread's work stops. The plugin sets who is told (its log and crash record).
#pragma once
#include <exception>
#include <utility>

namespace xpt {

inline void (*g_threadFailed)(const char* name, const char* what) = nullptr;

template <class F>
auto guarded(const char* name, F f) {
    return [name, f = std::move(f)]() mutable {
        try {
            f();
        } catch (const std::exception& e) {
            if (g_threadFailed) g_threadFailed(name, e.what());
        } catch (...) {
            if (g_threadFailed) g_threadFailed(name, "(not a std::exception)");
        }
    };
}

}   // namespace xpt
