// This file is part of "echion" which is released under MIT.
//
// Copyright (c) 2023 Gabriele N. Tornetta <phoenix1987@gmail.com>.

#pragma once

#define PY_SSIZE_T_CLEAN

#define Py_BUILD_CORE
#include <Python.h>

#if PY_VERSION_HEX >= 0x03090000
#if defined __GNUC__ && defined HAVE_STD_ATOMIC
#undef HAVE_STD_ATOMIC
#endif
#include <internal/pycore_interp.h>
#endif

#include <functional>

#include <echion/state.h>
#include <echion/vm.h>

class InterpreterInfo
{
  public:
    int64_t id = 0;
    void* tstate_head = NULL;
    void* next = NULL;
    uintptr_t address = 0;
    uint64_t code_object_generation = 0;
    bool code_object_generation_valid = false;
    bool threads_valid = false;
};

[[nodiscard]] bool
for_each_interp(_PyRuntimeState* runtime, const std::function<void(InterpreterInfo& interp)>& callback);
