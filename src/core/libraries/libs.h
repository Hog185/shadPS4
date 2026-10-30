// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/loader/elf.h"
#include "core/loader/symbols_resolver.h"
#include "core/tls.h"

void LinkSymbolImpl(Core::Loader::SymbolsResolver* sym, char const* nid, char const* lib,
                    u16 libversion, char const* mod, u64 symbol, Core::Loader::SymbolType sym_type);

#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    do {                                                                                           \
        Core::Devtools::HleProfiler::Register(&Core::HostCallWrapperImpl<function>::stats,         \
                                              #function, lib);                                     \
        LinkSymbolImpl(sym, nid, lib, libversion, mod, reinterpret_cast<u64>(HOST_CALL(function)), \
                       Core::Loader::SymbolType::Function);                                        \
    } while (0)

// Like LIB_FUNCTION but skips the profiler wrapper, for functions that read their return address.
#define LIB_FUNCTION_UNPROFILED(nid, lib, libversion, mod, function)                               \
    LinkSymbolImpl(sym, nid, lib, libversion, mod,                                                 \
                   reinterpret_cast<u64>(Core::HostCallWrapperImpl<function>::wrap_unprofiled),    \
                   Core::Loader::SymbolType::Function)

#define LIB_OBJ(nid, lib, libversion, mod, obj)                                                    \
    LinkSymbolImpl(sym, nid, lib, libversion, mod, reinterpret_cast<u64>(obj),                     \
                   Core::Loader::SymbolType::Object)

namespace Libraries {

void InitHLELibs(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries
