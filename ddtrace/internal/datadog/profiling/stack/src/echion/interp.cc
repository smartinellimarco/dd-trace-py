#include <echion/interp.h>

bool
for_each_interp(_PyRuntimeState* runtime, const std::function<void(InterpreterInfo& interp)>& callback)
{
    // Limit interpreter iteration to prevent infinite loops from cycles or corrupted memory.
    // This matches CPython's Tachyon profiler and is well above realistic interpreter counts.
    constexpr size_t max_interpreters = 256;

    char* interp_addr = reinterpret_cast<char*>(runtime->interpreters.head);
    char* prev_interp_addr = nullptr;

    for (size_t iteration_count = 0; iteration_count < max_interpreters && interp_addr != nullptr; ++iteration_count) {
        if (interp_addr == prev_interp_addr) {
            return false;
        }
        prev_interp_addr = interp_addr;

        InterpreterInfo interpreter_info;
        interpreter_info.address = reinterpret_cast<uintptr_t>(interp_addr);

        const bool next_valid = !copy_type(interp_addr + offsetof(PyInterpreterState, next), interpreter_info.next);

#if PY_VERSION_HEX >= 0x030e0000
        interpreter_info.code_object_generation_valid = !copy_type(
          interp_addr + offsetof(PyInterpreterState, _code_object_generation), interpreter_info.code_object_generation);
#endif

        const bool id_valid = !copy_type(interp_addr + offsetof(PyInterpreterState, id), interpreter_info.id);
#if PY_VERSION_HEX >= 0x030b0000
        const bool tstate_head_valid =
          !copy_type(interp_addr + offsetof(PyInterpreterState, threads.head), interpreter_info.tstate_head);
#else
        const bool tstate_head_valid =
          !copy_type(interp_addr + offsetof(PyInterpreterState, tstate_head), interpreter_info.tstate_head);
#endif
        interpreter_info.threads_valid = id_valid && tstate_head_valid;
        callback(interpreter_info);

        if (!next_valid) {
            return false;
        }
        interp_addr = reinterpret_cast<char*>(interpreter_info.next);
    }

    return interp_addr == nullptr;
}
