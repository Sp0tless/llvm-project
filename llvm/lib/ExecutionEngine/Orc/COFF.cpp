//===------------------ COFF.cpp - COFF format utilities ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/COFF.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFFImportFile.h"

#define DEBUG_TYPE "orc"

namespace llvm::orc {

Expected<bool> COFFImportFileScanner::operator()(object::Archive &A,
                                                 MemoryBufferRef MemberBuf,
                                                 size_t Index) const {
  // Try to build a binary for the member.
  auto Bin = object::createBinary(MemberBuf);
  if (!Bin) {
    // If we can't then consume the error and return false (i.e. not loadable).
    consumeError(Bin.takeError());
    return false;
  }

  // If this is a COFF import file then handle it and return false (not
  // loadable).
  if ((*Bin)->isCOFFImportFile()) {
    ImportedDynamicLibraries.insert((*Bin)->getFileName().str());

    if (ImportedSymbolTypes) {
      auto &ImportFile = cast<object::COFFImportFile>(**Bin);
      const auto *Header = ImportFile.getCOFFImportHeader();
      StringRef Buffer = MemberBuf.getBuffer();
      if (Buffer.size() <= sizeof(*Header))
        return make_error<StringError>("COFF import file has no symbol name",
                                       inconvertibleErrorCode());

      StringRef TargetName =
          Buffer.drop_front(sizeof(*Header)).split('\0').first;
      if (TargetName.empty())
        return make_error<StringError>(
            "COFF import file has an empty symbol name",
            inconvertibleErrorCode());

      auto Type = static_cast<COFF::ImportType>(Header->getType());
      auto [I, Inserted] = ImportedSymbolTypes->try_emplace(TargetName, Type);
      if (!Inserted && I->second != Type)
        return make_error<StringError>(
            "conflicting COFF import types for symbol " + TargetName,
            inconvertibleErrorCode());
    }

    return false;
  }

  // Otherwise the member is loadable (at least as far as COFFImportFileScanner
  // is concerned), so return true;
  return true;
}

} // namespace llvm::orc
