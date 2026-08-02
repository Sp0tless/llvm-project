//===--- OrcIncrementalExecutor.cpp - Orc Incremental Execution -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements an Orc-based incremental code execution.
//
//===----------------------------------------------------------------------===//

#include "OrcIncrementalExecutor.h"
#include "clang/Interpreter/PartialTranslationUnit.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/Shared/OrcRTBridge.h"
#include "llvm/ExecutionEngine/Orc/Shared/SimpleRemoteEPCUtils.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#ifdef LLVM_ON_UNIX
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif // LLVM_ON_UNIX

// Force linking some of the runtimes that helps attaching to a debugger.
LLVM_ATTRIBUTE_USED void linkComponents() {
  llvm::errs() << (void *)&llvm_orc_registerJITLoaderGDBAllocAction;
}

#ifdef _WIN32
extern "C" __declspec(thread) int _Init_thread_epoch;

extern "C" int *__clang_interpreter_get_init_thread_epoch() {
  return &_Init_thread_epoch;
}

static llvm::Error rewriteMSVCRuntimeTLS(llvm::Module &M) {
  llvm::GlobalVariable *Epoch = M.getNamedGlobal("_Init_thread_epoch");
  if (!Epoch)
    return llvm::Error::success();

  llvm::FunctionCallee Getter = M.getOrInsertFunction(
      "__clang_interpreter_get_init_thread_epoch",
      llvm::FunctionType::get(Epoch->getType(), false));

  llvm::SmallVector<llvm::Use *, 4> Uses;
  for (llvm::Use &U : Epoch->uses())
    Uses.push_back(&U);

  for (llvm::Use *U : Uses) {
    auto *I = llvm::dyn_cast<llvm::Instruction>(U->getUser());
    if (!I)
      return llvm::make_error<llvm::StringError>(
          "unsupported constant use of MSVC's _Init_thread_epoch",
          llvm::inconvertibleErrorCode());

    llvm::IRBuilder<> Builder(I);
    U->set(Builder.CreateCall(Getter));
  }

  Epoch->eraseFromParent();
  return llvm::Error::success();
}

static llvm::Error rewriteMSVCDynamicTLS(
    llvm::Module &M,
    llvm::SmallVectorImpl<std::string> &DynamicTLSInitializers,
    uint64_t &NextDynamicTLSInitializerId, bool &AddedInitializer) {
  llvm::LLVMContext &Ctx = M.getContext();
  llvm::Type *I8Ty = llvm::Type::getInt8Ty(Ctx);
  llvm::FunctionType *InitTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Ctx), false);

  // MSVC normally finds these functions through the image's .CRT$XDU range.
  // Incremental JIT objects are not ordinary PE images, so give each function
  // a stable external name and call it explicitly from later PTUs instead.
  llvm::Function *TLSInit = M.getFunction("__tls_init");
  if (TLSInit && !TLSInit->isDeclaration()) {
    if (TLSInit->getFunctionType() != InitTy)
      return llvm::make_error<llvm::StringError>(
          "unexpected type for MSVC dynamic TLS initializer",
          llvm::inconvertibleErrorCode());

    std::string Id = std::to_string(NextDynamicTLSInitializerId++);
    std::string InitName = "__clang_interpreter_tls_init." + Id;
    TLSInit->setName(InitName);
    TLSInit->setLinkage(llvm::GlobalValue::ExternalLinkage);
    TLSInit->setVisibility(llvm::GlobalValue::DefaultVisibility);

    auto *InitGuard = new llvm::GlobalVariable(
        M, I8Ty, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(I8Ty, 0),
        "__clang_interpreter_tls_init_guard." + Id);
    InitGuard->setThreadLocal(true);
    InitGuard->setThreadLocalMode(
        llvm::GlobalVariable::GeneralDynamicTLSModel);

    llvm::BasicBlock *OldEntry = &TLSInit->getEntryBlock();
    llvm::BasicBlock *GuardEntry = llvm::BasicBlock::Create(
        Ctx, "jit.tls.guard", TLSInit, OldEntry);
    llvm::BasicBlock *Done =
        llvm::BasicBlock::Create(Ctx, "jit.tls.done", TLSInit);

    llvm::IRBuilder<> GuardBuilder(GuardEntry);
    llvm::Value *GuardValue = GuardBuilder.CreateLoad(I8Ty, InitGuard);
    GuardBuilder.CreateCondBr(
        GuardBuilder.CreateICmpEQ(GuardValue, GuardBuilder.getInt8(0)),
        OldEntry, Done);

    llvm::IRBuilder<> InitBuilder(&*OldEntry->getFirstInsertionPt());
    InitBuilder.CreateStore(InitBuilder.getInt8(1), InitGuard);

    llvm::IRBuilder<> DoneBuilder(Done);
    DoneBuilder.CreateRetVoid();

    DynamicTLSInitializers.push_back(std::move(InitName));
    AddedInitializer = true;
  }

  llvm::GlobalVariable *AccessGuard = M.getNamedGlobal("__tls_guard");
  if (AccessGuard && AccessGuard->isDeclaration()) {
    AccessGuard->setInitializer(llvm::ConstantInt::get(I8Ty, 0));
    AccessGuard->setLinkage(llvm::GlobalValue::InternalLinkage);
    AccessGuard->setDSOLocal(true);
    AccessGuard->setName("__clang_interpreter_tls_access_guard." +
                         std::to_string(NextDynamicTLSInitializerId++));
  }

  llvm::Function *OnDemandInit =
      M.getFunction("__dyn_tls_on_demand_init");
  if (!OnDemandInit || DynamicTLSInitializers.empty())
    return llvm::Error::success();

  llvm::SmallVector<llvm::CallInst *, 4> Calls;
  for (llvm::User *U : OnDemandInit->users()) {
    auto *Call = llvm::dyn_cast<llvm::CallInst>(U);
    if (!Call || Call->getCalledOperand()->stripPointerCasts() != OnDemandInit)
      return llvm::make_error<llvm::StringError>(
          "unsupported use of MSVC's __dyn_tls_on_demand_init",
          llvm::inconvertibleErrorCode());
    Calls.push_back(Call);
  }

  for (llvm::CallInst *Call : Calls) {
    llvm::IRBuilder<> Builder(Call);
    if (AccessGuard)
      Builder.CreateStore(Builder.getInt8(1), AccessGuard);
    for (const std::string &InitName : DynamicTLSInitializers)
      Builder.CreateCall(M.getOrInsertFunction(InitName, InitTy));
    Call->eraseFromParent();
  }

  if (OnDemandInit->use_empty())
    OnDemandInit->eraseFromParent();
  return llvm::Error::success();
}
#endif

namespace clang {
OrcIncrementalExecutor::OrcIncrementalExecutor(
    llvm::orc::ThreadSafeContext &TSC)
    : TSCtx(TSC) {}

OrcIncrementalExecutor::OrcIncrementalExecutor(
    llvm::orc::ThreadSafeContext &TSC, llvm::orc::LLJITBuilder &JITBuilder,
    llvm::Error &Err)
    : TSCtx(TSC) {
  using namespace llvm::orc;
  llvm::ErrorAsOutParameter EAO(&Err);

  auto JitOrErr = JITBuilder.create();
  if (!JitOrErr) {
    Err = JitOrErr.takeError();
    return;
  }

  Jit = std::move(*JitOrErr);

#ifdef _WIN32
  // Static archives may materialize code and unwind data, so they must live in
  // a COFFPlatform-registered JITDylib with a valid __ImageBase.
  auto &RuntimeJD = Jit->getMainJITDylib();

  SymbolMap HostRuntimeSymbols;
  HostRuntimeSymbols[Jit->mangleAndIntern(
      "__clang_interpreter_get_init_thread_epoch")] =
      ExecutorSymbolDef(
          ExecutorAddr::fromPtr(&__clang_interpreter_get_init_thread_epoch),
          llvm::JITSymbolFlags::Exported |
              llvm::JITSymbolFlags::Callable);
  if (auto E = RuntimeJD.define(absoluteSymbols(std::move(HostRuntimeSymbols)))) {
    Err = std::move(E);
    return;
  }

#endif
}

OrcIncrementalExecutor::~OrcIncrementalExecutor() {}

llvm::Error OrcIncrementalExecutor::addModule(PartialTranslationUnit &PTU) {
#ifdef _WIN32
  if (auto Err = rewriteMSVCRuntimeTLS(*PTU.TheModule))
    return Err;
  bool AddedDynamicTLSInitializer = false;
  if (auto Err = rewriteMSVCDynamicTLS(
          *PTU.TheModule, DynamicTLSInitializers,
          NextDynamicTLSInitializerId, AddedDynamicTLSInitializer))
    return Err;
#endif

  llvm::orc::ResourceTrackerSP RT =
      Jit->getMainJITDylib().createResourceTracker();
  ResourceTrackers[&PTU] = RT;

  llvm::Error Err =
      Jit->addIRModule(RT, {std::move(PTU.TheModule), TSCtx});
#ifdef _WIN32
  if (Err && AddedDynamicTLSInitializer)
    DynamicTLSInitializers.pop_back();
#endif
  return Err;
}

llvm::Error OrcIncrementalExecutor::removeModule(PartialTranslationUnit &PTU) {

  llvm::orc::ResourceTrackerSP RT = std::move(ResourceTrackers[&PTU]);
  if (!RT)
    return llvm::Error::success();

  ResourceTrackers.erase(&PTU);
  if (llvm::Error Err = RT->remove())
    return Err;
  return llvm::Error::success();
}

// Clean up the JIT instance.
llvm::Error OrcIncrementalExecutor::cleanUp() {
  // This calls the global dtors of registered modules.
  return Jit->deinitialize(Jit->getMainJITDylib());
}

llvm::Error OrcIncrementalExecutor::runCtors() const {
  return Jit->initialize(Jit->getMainJITDylib());
}

llvm::Expected<llvm::orc::ExecutorAddr>
OrcIncrementalExecutor::getSymbolAddress(llvm::StringRef Name,
                                         SymbolNameKind NameKind) const {
  using namespace llvm::orc;
  auto SO = makeJITDylibSearchOrder({&Jit->getMainJITDylib(),
                                     Jit->getPlatformJITDylib().get(),
                                     Jit->getProcessSymbolsJITDylib().get()});

  ExecutionSession &ES = Jit->getExecutionSession();

  auto SymOrErr = ES.lookup(SO, (NameKind == SymbolNameKind::LinkerName)
                                    ? ES.intern(Name)
                                    : Jit->mangleAndIntern(Name));
  if (auto Err = SymOrErr.takeError())
    return std::move(Err);
  return SymOrErr->getAddress();
}

llvm::Error OrcIncrementalExecutor::LoadDynamicLibrary(const char *name) {
  // FIXME: Eventually we should put each library in its own JITDylib and
  //        turn off process symbols by default.
  llvm::orc::ExecutionSession &ES = Jit->getExecutionSession();
  auto DLSGOrErr = llvm::orc::EPCDynamicLibrarySearchGenerator::Load(
      ES, Jit->getDylibMgr(), name);
  if (!DLSGOrErr)
    return DLSGOrErr.takeError();

  Jit->getProcessSymbolsJITDylib()->addGenerator(std::move(*DLSGOrErr));

  return llvm::Error::success();
}

} // namespace clang
