// HostContext backend for WebAssembly: symmetric coroutines on JSPI (JavaScript Promise
// Integration). See the __EMSCRIPTEN__ branch of runtime/src/host_context.cpp.
//
// Every context runs on its own engine stack and is named by its address (a Context* in
// host_context.cpp). A context is started by calling the promising export
// _mkw_jspi_context_entry(context), which runs until the context first switches away. A switch
// is the suspending import mkw_jspi_switch: it records how to resume the caller, then wakes the
// target from a microtask (after the caller's stack has actually suspended), and finally awaits
// until some other context switches back to the caller.
//
// JSPI switches only the engine stack. The C shadow stack (the __stack_pointer global and the
// limits used by stack-overflow checks) is shared by everything, so each context owns a region of
// linear memory and its stack pointer and limits are saved and restored around every switch.
// Link with -sJSPI -sJSPI_EXPORTS=mkw_jspi_context_entry --js-library <this file>.

addToLibrary({
  $MkwJspiContexts: {
    // context address -> { base, end, resume }. `resume` is set while the context is parked in a
    // switch.
    byAddress: new Map(),
  },

  // Installs a context's C stack limits. With STACK_OVERFLOW_CHECK=2, Binaryen's StackCheck pass
  // keeps its own copy of the limits, which Emscripten's Fibers update the same way. That pass
  // adds __set_stack_limits after linking, so it cannot be listed as a link-time dependency.
  $mkwJspiSetLimits__deps: ['emscripten_stack_set_limits'],
  $mkwJspiSetLimits: (base, end) => {
    _emscripten_stack_set_limits(base, end);
#if STACK_OVERFLOW_CHECK >= 2
    ___set_stack_limits(base, end);
#endif
  },

  mkw_jspi_register__deps: ['$MkwJspiContexts'],
  mkw_jspi_register: (context, stackBase, stackEnd) => {
    MkwJspiContexts.byAddress.set(context, { base: stackBase, end: stackEnd, resume: null });
  },

  mkw_jspi_unregister__deps: ['$MkwJspiContexts'],
  mkw_jspi_unregister: (context) => {
    MkwJspiContexts.byAddress.delete(context);
  },

  mkw_jspi_switch__deps: [
    '$MkwJspiContexts', '$stackSave', '$stackRestore', '$mkwJspiSetLimits',
    'mkw_jspi_context_entry',
  ],
  mkw_jspi_switch__async: true,
  mkw_jspi_switch: async (fromAddress, toAddress, startTarget) => {
    const contexts = MkwJspiContexts.byAddress;
    const from = contexts.get(fromAddress);
    const to = contexts.get(toAddress);
    if (!from || !to) {
      abort(`HostContext: switch ${fromAddress} -> ${toAddress} names an unknown context`);
    }
    const perf = Module['mkwPerf'];
    if (perf) perf.switches++;
    // No wasm runs between this point and the target resuming: that span is the switch's cost.
    MkwJspiContexts.switchStarted = performance.now();
    const savedStackPointer = stackSave();
    await new Promise((resolve) => {
      from.resume = resolve;
      queueMicrotask(() => {
        if (startTarget) {
          mkwJspiSetLimits(to.base, to.end);
          stackRestore(to.base);
#if STACK_OVERFLOW_CHECK
          writeStackCookie();  // runtime function (runtime_stack_check.js), not a library symbol
#endif
          // A context's entry never returns; a rejection here is a crash inside it.
          _mkw_jspi_context_entry(toAddress).catch((error) => {
            err(`HostContext: context ${toAddress} failed: ${error}`);
            abort(error);
          });
        } else {
          const resume = to.resume;
          if (!resume) {
            abort(`HostContext: context ${toAddress} is not parked and cannot be resumed`);
          }
          to.resume = null;
          resume();
        }
      });
    });
    if (perf) perf.switchCost += performance.now() - MkwJspiContexts.switchStarted;
    // Back on the caller's engine stack: put its C stack back before returning into wasm.
    mkwJspiSetLimits(from.base, from.end);
    stackRestore(savedStackPointer);
  },
});
