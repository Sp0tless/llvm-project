//===-------------- COFF.h - COFF format utilities --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Contains utilities for load COFF relocatable object files.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_COFF_H
#define LLVM_EXECUTIONENGINE_ORC_COFF_H

#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <set>
#include <string>

namespace llvm {

namespace object {
class Archive;
} // namespace object

namespace orc {

using COFFImportSymbolTypes = StringMap<COFF::ImportType>;

class COFFImportFileScanner {
public:
  COFFImportFileScanner(
      std::set<std::string> &ImportedDynamicLibraries,
      COFFImportSymbolTypes *ImportedSymbolTypes = nullptr)
      : ImportedDynamicLibraries(ImportedDynamicLibraries),
        ImportedSymbolTypes(ImportedSymbolTypes) {}
  LLVM_ABI Expected<bool>
  operator()(object::Archive &A, MemoryBufferRef MemberBuf, size_t Index) const;

private:
  std::set<std::string> &ImportedDynamicLibraries;
  COFFImportSymbolTypes *ImportedSymbolTypes;
};

} // namespace orc
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_MACHO_H
