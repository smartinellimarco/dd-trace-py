#include <echion/frame.h>

#include <echion/echion_sampler.h>
#include <echion/errors.h>

#include <profiling_helpers/frame_accessors.h>
#include <profiling_helpers/linetable_parser.h>

#if PY_VERSION_HEX >= 0x030b0000
#include <cstddef>
#include <limits>
#endif // PY_VERSION_HEX >= 0x030b0000

// ------------------------------------------------------------------------
Result<Frame>
Frame::create(EchionSampler& echion, PyCodeObject* code, int lasti)
{
    auto maybe_filename = echion.string_table().key(code->co_filename, StringTag::FileName);
    if (!maybe_filename) {
        return ErrorKind::FrameError;
    }

    auto maybe_name = echion.string_table().key(DataDog::get_code_name(code), StringTag::FuncName);

    if (!maybe_name) {
        return ErrorKind::FrameError;
    }

    Frame frame(*maybe_filename, *maybe_name);
    auto infer_location_success = frame.infer_location(code, lasti);
    if (!infer_location_success) {
        return ErrorKind::LocationError;
    }

    return frame;
}

// ----------------------------------------------------------------------------
Result<void>
Frame::infer_location(PyCodeObject* code_obj, int instr_offset)
{
    Py_ssize_t len = 0;

#if PY_VERSION_HEX >= 0x030a0000
    auto table = pybytes_to_bytes_and_size(code_obj->co_linetable, &len);
#else
    auto table = pybytes_to_bytes_and_size(code_obj->co_lnotab, &len);
#endif

    if (table == nullptr) {
        return ErrorKind::LocationError;
    }

    this->line = DataDog::parse_linetable(table.get(), len, instr_offset, code_obj->co_firstlineno);

    return Result<void>::ok();
}

// ------------------------------------------------------------------------
Frame::Key
Frame::key(PyCodeObject* code, int lasti, int firstlineno)
{
    return { reinterpret_cast<uintptr_t>(code), lasti, firstlineno };
}

// ------------------------------------------------------------------------
#if PY_VERSION_HEX >= 0x030b0000
Result<Frame>
Frame::read(EchionSampler& echion, _PyInterpreterFrame* frame_addr, _PyInterpreterFrame** prev_addr)
#else
Result<Frame>
Frame::read(EchionSampler& echion, PyObject* frame_addr, PyObject** prev_addr)
#endif
{
    if (frame_addr == nullptr) {
        return ErrorKind::FrameError;
    }

#if PY_VERSION_HEX >= 0x030b0000
    _PyInterpreterFrame iframe;
    auto resolved_addr =
      stack_chunk ? reinterpret_cast<_PyInterpreterFrame*>(stack_chunk->resolve(frame_addr)) : frame_addr;

    if (resolved_addr != frame_addr) {
        if (resolved_addr == nullptr) {
            return ErrorKind::FrameError;
        }

        // The frame is in the stack chunk, try to copy it into the local frame object.
        // Note: resolved_addr points into the stack chunk's local buffer and may not be
        // aligned to alignof(_PyInterpreterFrame). Copy into the aligned local
        // iframe before accessing any fields to avoid undefined behaviour.
        std::memcpy(&iframe, resolved_addr, sizeof(iframe));
    } else {
        // The frame is not in the stack chunk, directly copy the frame object.
        if (copy_type(frame_addr, iframe)) {
            return ErrorKind::FrameError;
        }
    }
    frame_addr = &iframe;

#if PY_VERSION_HEX >= 0x030c0000
#if PY_VERSION_HEX >= 0x030e0000
    // Python 3.14 introduced FRAME_OWNED_BY_INTERPRETER, and frames of this
    // type are also ignored by the upstream profiler.
    // See
    // https://github.com/python/cpython/blob/ebf955df7a89ed0c7968f79faec1de49f61ed7cb/Modules/_remote_debugging_module.c#L2134
    if (frame_addr->owner == FRAME_OWNED_BY_CSTACK || frame_addr->owner == FRAME_OWNED_BY_INTERPRETER) {
#else
    if (frame_addr->owner == FRAME_OWNED_BY_CSTACK) {
#endif // PY_VERSION_HEX >= 0x030e0000
        *prev_addr = frame_addr->previous;
        // This is a C frame, we just need to ignore it
        return C_FRAME;
    }

    if (frame_addr->owner != FRAME_OWNED_BY_THREAD && frame_addr->owner != FRAME_OWNED_BY_GENERATOR) {
        return ErrorKind::FrameError;
    }
#endif // PY_VERSION_HEX >= 0x030c0000

    auto compute_lasti = [](uintptr_t instr_addr, uintptr_t code_obj_addr) -> Result<int> {
        constexpr uintptr_t code_unit_size = sizeof(_Py_CODEUNIT);
        constexpr uintptr_t code_unit_alignment = alignof(_Py_CODEUNIT);

        if ((instr_addr % code_unit_alignment) != 0 || (code_obj_addr % code_unit_alignment) != 0) {
            return ErrorKind::FrameError;
        }

        const uintptr_t code_start_addr = code_obj_addr + offsetof(PyCodeObject, co_code_adaptive);
        if ((code_start_addr % code_unit_alignment) != 0) {
            return ErrorKind::FrameError;
        }

        if (instr_addr < code_start_addr) {
            return ErrorKind::FrameError;
        }

        const uintptr_t delta = instr_addr - code_start_addr;
        if ((delta % code_unit_size) != 0) {
            return ErrorKind::FrameError;
        }

        const uintptr_t lasti_index = delta / code_unit_size;
        if (lasti_index > static_cast<uintptr_t>(std::numeric_limits<int>::max())) {
            return ErrorKind::FrameError;
        }

        return static_cast<int>(lasti_index);
    };

    // We cannot use _PyInterpreterFrame_LASTI because _PyCode_CODE reads
    // from the code object, which is a remote address here.  Use offsetof
    // arithmetic instead to avoid dereferencing it.
#if PY_VERSION_HEX >= 0x030d0000
    // DataDog::get_code_from_frame() handles both Python 3.13 (untagged
    // f_executable) and 3.14+ (tagged _PyStackRef f_executable) transparently.
    PyCodeObject* code_obj = DataDog::get_code_from_frame(frame_addr);
    if (code_obj == nullptr || frame_addr->instr_ptr == nullptr) {
        return ErrorKind::FrameError;
    }

    // In Python 3.13+, instr_ptr points to the current instruction.
    // Compute lasti with raw addresses to avoid UB on misaligned fuzzed pointers.
    auto maybe_lasti =
      compute_lasti(reinterpret_cast<uintptr_t>(frame_addr->instr_ptr), reinterpret_cast<uintptr_t>(code_obj));
    if (!maybe_lasti) {
        return ErrorKind::FrameError;
    }
    const int lasti = *maybe_lasti;

    auto maybe_frame = Frame::get(echion, code_obj, lasti);
    if (!maybe_frame) {
        return ErrorKind::FrameError;
    }

    auto frame = *maybe_frame;
#else
    if (frame_addr->f_code == nullptr || frame_addr->prev_instr == nullptr) {
        return ErrorKind::FrameError;
    }

    auto maybe_lasti = compute_lasti(reinterpret_cast<uintptr_t>(frame_addr->prev_instr),
                                     reinterpret_cast<uintptr_t>(frame_addr->f_code));
    if (!maybe_lasti) {
        return ErrorKind::FrameError;
    }
    const int lasti = *maybe_lasti;

    auto maybe_frame = Frame::get(echion, frame_addr->f_code, lasti);
    if (!maybe_frame) {
        return ErrorKind::FrameError;
    }

    auto frame = *maybe_frame;
#endif // PY_VERSION_HEX >= 0x030d0000
    *prev_addr = frame.name == StringTable::INVALID ? NULL : frame_addr->previous;

#else  // PY_VERSION_HEX < 0x030b0000
    // Unwind the stack from leaf to root and store it in a stack. This way we
    // can print it from root to leaf.
    PyFrameObject py_frame;

    if (copy_type(frame_addr, py_frame)) {
        return ErrorKind::FrameError;
    }

    auto maybe_frame = Frame::get(echion, py_frame.f_code, py_frame.f_lasti);
    if (!maybe_frame) {
        return ErrorKind::FrameError;
    }

    auto frame = *maybe_frame;
    *prev_addr = (frame.name == StringTable::INVALID) ? NULL : reinterpret_cast<PyObject*>(py_frame.f_back);
#endif // PY_VERSION_HEX >= 0x030b0000

    return frame;
}

// ----------------------------------------------------------------------------
Result<Frame>
Frame::get(EchionSampler& echion, PyCodeObject* code_addr, int lasti)
{
    // Read co_firstlineno before the cache lookup so equality checks the complete
    // (code address, instruction offset, first line) identity after hashing.
    int firstlineno;
    {
        auto* firstlineno_addr =
          reinterpret_cast<decltype(PyCodeObject::co_firstlineno)*>( // NOLINT(performance-no-int-to-ptr)
            reinterpret_cast<uintptr_t>(code_addr) + offsetof(PyCodeObject, co_firstlineno));
        if (copy_type(firstlineno_addr, firstlineno)) {
            return INVALID_FRAME;
        }
    }

    auto frame_key = Frame::key(code_addr, lasti, firstlineno);

    if (echion.persistent_frame_cache_enabled()) {
        auto maybe_frame = echion.frame_cache().lookup(frame_key);
        if (maybe_frame) {
            return maybe_frame->get();
        }
    }

    PyCodeObject code;
    if (copy_type(code_addr, code)) {
        return INVALID_FRAME;
    }

    auto maybe_new_frame = Frame::create(echion, &code, lasti);
    if (!maybe_new_frame) {
        return INVALID_FRAME;
    }

    auto new_frame = *maybe_new_frame;
    new_frame.cache_key = frame_key;
    new_frame.code_object = reinterpret_cast<uintptr_t>(code_addr);
    new_frame.lasti = lasti;
    new_frame.first_lineno = firstlineno;
    if (echion.persistent_frame_cache_enabled()) {
        echion.frame_cache().store(frame_key, std::make_unique<Frame>(new_frame));
    }
    return new_frame;
}
