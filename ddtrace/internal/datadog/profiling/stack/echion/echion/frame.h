// This file is part of "echion" which is released under MIT.
//
// Copyright (c) 2023 Gabriele N. Tornetta <phoenix1987@gmail.com>.

#pragma once

#define PY_SSIZE_T_CLEAN
#define Py_BUILD_CORE
#include <Python.h>

#if defined __GNUC__ && defined HAVE_STD_ATOMIC
#undef HAVE_STD_ATOMIC
#endif
#if PY_VERSION_HEX >= 0x030c0000
// https://github.com/python/cpython/issues/108216#issuecomment-1696565797
#undef _PyGC_FINALIZED
#endif
#include <frameobject.h>
#if PY_VERSION_HEX >= 0x030e0000
#include <internal/pycore_interpframe_structs.h>
#elif PY_VERSION_HEX >= 0x030b0000
#include <internal/pycore_frame.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <echion/cache.h>
#if PY_VERSION_HEX >= 0x030b0000
#include <echion/stack_chunk.h>
#endif // PY_VERSION_HEX >= 0x030b0000
#include <echion/strings.h>
#include <echion/vm.h>

// Forward declaration
class EchionSampler;

struct FrameKey
{
    uintptr_t code = 0;
    int lasti = 0;
    int firstlineno = 0;

    bool operator==(const FrameKey&) const = default;
};

struct FrameKeyHash
{
    size_t operator()(const FrameKey& key) const noexcept
    {
        uintptr_t hash = key.code;
        hash ^= static_cast<uintptr_t>(static_cast<uint32_t>(key.lasti)) * 2654435761ULL;
        hash ^= static_cast<uintptr_t>(static_cast<uint32_t>(key.firstlineno)) * 40503ULL;
        return hash;
    }
};

// ----------------------------------------------------------------------------
class Frame
{
  public:
    using Key = FrameKey;

    // ------------------------------------------------------------------------
    Key cache_key{};
    StringTable::Key filename = 0;
    StringTable::Key name = 0;

    unsigned line = 0;
    uintptr_t code_object = 0; // PyCodeObject address, matches Python's id(code)
    int lasti = -1;            // Last bytecode offset in _Py_CODEUNIT units
    int first_lineno = 0;      // co_firstlineno, used by cache and native-call lookup keys

    // ------------------------------------------------------------------------
    Frame(StringTable::Key filename, StringTable::Key name)
      : filename(filename)
      , name(name)
    {
    }
    Frame(StringTable::Key name)
      : name(name) {};
    [[nodiscard]] static Result<Frame> create(EchionSampler& echion, PyCodeObject* code, int lasti);

#if PY_VERSION_HEX >= 0x030b0000
    [[nodiscard]] static Result<Frame> read(EchionSampler& echion,
                                            _PyInterpreterFrame* frame_addr,
                                            _PyInterpreterFrame** prev_addr);
#else
    [[nodiscard]] static Result<Frame> read(EchionSampler& echion, PyObject* frame_addr, PyObject** prev_addr);
#endif

    [[nodiscard]] static Result<Frame> get(EchionSampler& echion, PyCodeObject* code_addr, int lasti);

  private:
    [[nodiscard]] Result<void> inline infer_location(PyCodeObject* code, int instr_offset);
    static inline Key key(PyCodeObject* code, int lasti, int firstlineno);
};

inline auto INVALID_FRAME = Frame(StringTable::INVALID);
inline auto UNKNOWN_FRAME = Frame(StringTable::UNKNOWN);
inline auto C_FRAME = Frame(StringTable::C_FRAME);
