// HostContext backend for WebAssembly (Emscripten); the native backends are in
// runtime/src/host_context.cpp.

#include "host_context.h"

#include <emscripten/emscripten.h>
#include <emscripten/stack.h>

#include <cstdint>
#include <cstdlib>

// JavaScript side: host_context_jspi.js next to this file. A context is named by its
// address.
extern "C" {
void mkw_jspi_register(void* context, uintptr_t stackBase, uintptr_t stackEnd);
void mkw_jspi_unregister(void* context);
void mkw_jspi_switch(void* from, void* to, int startTarget);
}

namespace HostContext {

// WebAssembly cannot switch stacks itself. Each context runs on its own engine stack via JSPI
// (JavaScript Promise Integration): a context is first entered through the promising export
// mkw_jspi_context_entry, and Switch() is a suspending import that resumes or starts the target
// and parks the caller until something switches back to it. JSPI only switches the engine stack,
// so every context also owns a C shadow stack, which the JS side saves and restores around each
// switch. Guest scheduling is single-threaded, so plain globals suffice.
namespace {
struct Context {
    Entry entry = nullptr;
    void* argument = nullptr;
    void* stack = nullptr;  // owned C shadow stack; null for the scheduler's original stack
    bool started = false;
};

Context* g_current = nullptr;
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void mkw_jspi_context_entry(void* target)
{
    auto* context = static_cast<Context*>(target);
    if (!context || !context->entry) {
        std::abort();
    }
    g_current = context;
    context->entry(context->argument);
    // As with the native backends, a guest fiber must leave through a Switch(); there is no
    // caller to return to.
    std::abort();
}

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->started = true;
    mkw_jspi_register(context, emscripten_stack_get_base(), emscripten_stack_get_end());
    g_current = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (!context) {
        return;
    }
    mkw_jspi_unregister(context);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    constexpr std::size_t kAlignment = 16;
    stackSize = (stackSize + kAlignment - 1) & ~(kAlignment - 1);
    void* stack = std::aligned_alloc(kAlignment, stackSize);
    if (!stack) {
        return nullptr;
    }
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;
    context->stack = stack;
    const auto end = reinterpret_cast<uintptr_t>(stack);
    mkw_jspi_register(context, end + stackSize, end);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }
    // A context parked mid-switch holds only a pending promise on the JS side; dropping its
    // resumer lets the engine collect the suspended stack.
    mkw_jspi_unregister(nativeContext);
    std::free(nativeContext->stack);
    delete nativeContext;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    const bool start = !destination->started;
    destination->started = true;
    g_current = destination;
    mkw_jspi_switch(source, destination, start ? 1 : 0);
    g_current = source;
}

} // namespace HostContext
