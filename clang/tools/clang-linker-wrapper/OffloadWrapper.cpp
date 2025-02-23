//===- OffloadWrapper.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OffloadWrapper.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Frontend/Offloading/Utility.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/OffloadBinary.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

namespace {
/// Magic number that begins the section containing the CUDA fatbinary.
constexpr unsigned CudaFatMagic = 0x466243b1;
constexpr unsigned HIPFatMagic = 0x48495046;

/// Copied from clang/CGCudaRuntime.h.
enum OffloadEntryKindFlag : uint32_t {
  /// Mark the entry as a global entry. This indicates the presense of a
  /// kernel if the size size field is zero and a variable otherwise.
  OffloadGlobalEntry = 0x0,
  /// Mark the entry as a managed global variable.
  OffloadGlobalManagedEntry = 0x1,
  /// Mark the entry as a surface variable.
  OffloadGlobalSurfaceEntry = 0x2,
  /// Mark the entry as a texture variable.
  OffloadGlobalTextureEntry = 0x3,
};

IntegerType *getSizeTTy(Module &M) {
  return M.getDataLayout().getIntPtrType(M.getContext());
}

// struct __tgt_device_image {
//   void *ImageStart;
//   void *ImageEnd;
//   __tgt_offload_entry *EntriesBegin;
//   __tgt_offload_entry *EntriesEnd;
// };
StructType *getDeviceImageTy(Module &M) {
  LLVMContext &C = M.getContext();
  StructType *ImageTy = StructType::getTypeByName(C, "__tgt_device_image");
  if (!ImageTy)
    ImageTy =
        StructType::create("__tgt_device_image", PointerType::getUnqual(C),
                           PointerType::getUnqual(C), PointerType::getUnqual(C),
                           PointerType::getUnqual(C));
  return ImageTy;
}

PointerType *getDeviceImagePtrTy(Module &M) {
  return PointerType::getUnqual(getDeviceImageTy(M));
}

// struct __tgt_bin_desc {
//   int32_t NumDeviceImages;
//   __tgt_device_image *DeviceImages;
//   __tgt_offload_entry *HostEntriesBegin;
//   __tgt_offload_entry *HostEntriesEnd;
// };
StructType *getBinDescTy(Module &M) {
  LLVMContext &C = M.getContext();
  StructType *DescTy = StructType::getTypeByName(C, "__tgt_bin_desc");
  if (!DescTy)
    DescTy = StructType::create(
        "__tgt_bin_desc", Type::getInt32Ty(C), getDeviceImagePtrTy(M),
        PointerType::getUnqual(C), PointerType::getUnqual(C));
  return DescTy;
}

PointerType *getBinDescPtrTy(Module &M) {
  return PointerType::getUnqual(getBinDescTy(M));
}

/// Creates binary descriptor for the given device images. Binary descriptor
/// is an object that is passed to the offloading runtime at program startup
/// and it describes all device images available in the executable or shared
/// library. It is defined as follows
///
/// __attribute__((visibility("hidden")))
/// extern __tgt_offload_entry *__start_omp_offloading_entries;
/// __attribute__((visibility("hidden")))
/// extern __tgt_offload_entry *__stop_omp_offloading_entries;
///
/// static const char Image0[] = { <Bufs.front() contents> };
///  ...
/// static const char ImageN[] = { <Bufs.back() contents> };
///
/// static const __tgt_device_image Images[] = {
///   {
///     Image0,                            /*ImageStart*/
///     Image0 + sizeof(Image0),           /*ImageEnd*/
///     __start_omp_offloading_entries,    /*EntriesBegin*/
///     __stop_omp_offloading_entries      /*EntriesEnd*/
///   },
///   ...
///   {
///     ImageN,                            /*ImageStart*/
///     ImageN + sizeof(ImageN),           /*ImageEnd*/
///     __start_omp_offloading_entries,    /*EntriesBegin*/
///     __stop_omp_offloading_entries      /*EntriesEnd*/
///   }
/// };
///
/// static const __tgt_bin_desc BinDesc = {
///   sizeof(Images) / sizeof(Images[0]),  /*NumDeviceImages*/
///   Images,                              /*DeviceImages*/
///   __start_omp_offloading_entries,      /*HostEntriesBegin*/
///   __stop_omp_offloading_entries        /*HostEntriesEnd*/
/// };
///
/// Global variable that represents BinDesc is returned.
GlobalVariable *createBinDesc(Module &M, ArrayRef<ArrayRef<char>> Bufs) {
  LLVMContext &C = M.getContext();
  auto [EntriesB, EntriesE] =
      offloading::getOffloadEntryArray(M, "omp_offloading_entries");

  auto *Zero = ConstantInt::get(getSizeTTy(M), 0u);
  Constant *ZeroZero[] = {Zero, Zero};

  // Create initializer for the images array.
  SmallVector<Constant *, 4u> ImagesInits;
  ImagesInits.reserve(Bufs.size());
  for (ArrayRef<char> Buf : Bufs) {
    auto *Data = ConstantDataArray::get(C, Buf);
    auto *Image = new GlobalVariable(M, Data->getType(), /*isConstant*/ true,
                                     GlobalVariable::InternalLinkage, Data,
                                     ".omp_offloading.device_image");
    Image->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    Image->setSection(".llvm.offloading");
    Image->setAlignment(Align(object::OffloadBinary::getAlignment()));

    auto *Size = ConstantInt::get(getSizeTTy(M), Buf.size());
    Constant *ZeroSize[] = {Zero, Size};

    auto *ImageB =
        ConstantExpr::getGetElementPtr(Image->getValueType(), Image, ZeroZero);
    auto *ImageE =
        ConstantExpr::getGetElementPtr(Image->getValueType(), Image, ZeroSize);

    ImagesInits.push_back(ConstantStruct::get(getDeviceImageTy(M), ImageB,
                                              ImageE, EntriesB, EntriesE));
  }

  // Then create images array.
  auto *ImagesData = ConstantArray::get(
      ArrayType::get(getDeviceImageTy(M), ImagesInits.size()), ImagesInits);

  auto *Images =
      new GlobalVariable(M, ImagesData->getType(), /*isConstant*/ true,
                         GlobalValue::InternalLinkage, ImagesData,
                         ".omp_offloading.device_images");
  Images->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  auto *ImagesB =
      ConstantExpr::getGetElementPtr(Images->getValueType(), Images, ZeroZero);

  // And finally create the binary descriptor object.
  auto *DescInit = ConstantStruct::get(
      getBinDescTy(M),
      ConstantInt::get(Type::getInt32Ty(C), ImagesInits.size()), ImagesB,
      EntriesB, EntriesE);

  return new GlobalVariable(M, DescInit->getType(), /*isConstant*/ true,
                            GlobalValue::InternalLinkage, DescInit,
                            ".omp_offloading.descriptor");
}

void createRegisterFunction(Module &M, GlobalVariable *BinDesc) {
  LLVMContext &C = M.getContext();
  auto *FuncTy = FunctionType::get(Type::getVoidTy(C), /*isVarArg*/ false);
  auto *Func = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                ".omp_offloading.descriptor_reg", &M);
  Func->setSection(".text.startup");

  // Get __tgt_register_lib function declaration.
  auto *RegFuncTy = FunctionType::get(Type::getVoidTy(C), getBinDescPtrTy(M),
                                      /*isVarArg*/ false);
  FunctionCallee RegFuncC =
      M.getOrInsertFunction("__tgt_register_lib", RegFuncTy);

  // Construct function body
  IRBuilder<> Builder(BasicBlock::Create(C, "entry", Func));
  Builder.CreateCall(RegFuncC, BinDesc);
  Builder.CreateRetVoid();

  // Add this function to constructors.
  // Set priority to 1 so that __tgt_register_lib is executed AFTER
  // __tgt_register_requires (we want to know what requirements have been
  // asked for before we load a libomptarget plugin so that by the time the
  // plugin is loaded it can report how many devices there are which can
  // satisfy these requirements).
  appendToGlobalCtors(M, Func, /*Priority*/ 1);
}

void createUnregisterFunction(Module &M, GlobalVariable *BinDesc) {
  LLVMContext &C = M.getContext();
  auto *FuncTy = FunctionType::get(Type::getVoidTy(C), /*isVarArg*/ false);
  auto *Func = Function::Create(FuncTy, GlobalValue::InternalLinkage,
                                ".omp_offloading.descriptor_unreg", &M);
  Func->setSection(".text.startup");

  // Get __tgt_unregister_lib function declaration.
  auto *UnRegFuncTy = FunctionType::get(Type::getVoidTy(C), getBinDescPtrTy(M),
                                        /*isVarArg*/ false);
  FunctionCallee UnRegFuncC =
      M.getOrInsertFunction("__tgt_unregister_lib", UnRegFuncTy);

  // Construct function body
  IRBuilder<> Builder(BasicBlock::Create(C, "entry", Func));
  Builder.CreateCall(UnRegFuncC, BinDesc);
  Builder.CreateRetVoid();

  // Add this function to global destructors.
  // Match priority of __tgt_register_lib
  appendToGlobalDtors(M, Func, /*Priority*/ 1);
}

// struct fatbin_wrapper {
//  int32_t magic;
//  int32_t version;
//  void *image;
//  void *reserved;
//};
StructType *getFatbinWrapperTy(Module &M) {
  LLVMContext &C = M.getContext();
  StructType *FatbinTy = StructType::getTypeByName(C, "fatbin_wrapper");
  if (!FatbinTy)
    FatbinTy = StructType::create(
        "fatbin_wrapper", Type::getInt32Ty(C), Type::getInt32Ty(C),
        PointerType::getUnqual(C), PointerType::getUnqual(C));
  return FatbinTy;
}

/// Embed the image \p Image into the module \p M so it can be found by the
/// runtime.
GlobalVariable *createFatbinDesc(Module &M, ArrayRef<char> Image, bool IsHIP) {
  LLVMContext &C = M.getContext();
  llvm::Type *Int8PtrTy = PointerType::getUnqual(C);
  llvm::Triple Triple = llvm::Triple(M.getTargetTriple());

  // Create the global string containing the fatbinary.
  StringRef FatbinConstantSection =
      IsHIP ? ".hip_fatbin"
            : (Triple.isMacOSX() ? "__NV_CUDA,__nv_fatbin" : ".nv_fatbin");
  auto *Data = ConstantDataArray::get(C, Image);
  auto *Fatbin = new GlobalVariable(M, Data->getType(), /*isConstant*/ true,
                                    GlobalVariable::InternalLinkage, Data,
                                    ".fatbin_image");
  Fatbin->setSection(FatbinConstantSection);

  // Create the fatbinary wrapper
  StringRef FatbinWrapperSection = IsHIP               ? ".hipFatBinSegment"
                                   : Triple.isMacOSX() ? "__NV_CUDA,__fatbin"
                                                       : ".nvFatBinSegment";
  Constant *FatbinWrapper[] = {
      ConstantInt::get(Type::getInt32Ty(C), IsHIP ? HIPFatMagic : CudaFatMagic),
      ConstantInt::get(Type::getInt32Ty(C), 1),
      ConstantExpr::getPointerBitCastOrAddrSpaceCast(Fatbin, Int8PtrTy),
      ConstantPointerNull::get(PointerType::getUnqual(C))};

  Constant *FatbinInitializer =
      ConstantStruct::get(getFatbinWrapperTy(M), FatbinWrapper);

  auto *FatbinDesc =
      new GlobalVariable(M, getFatbinWrapperTy(M),
                         /*isConstant*/ true, GlobalValue::InternalLinkage,
                         FatbinInitializer, ".fatbin_wrapper");
  FatbinDesc->setSection(FatbinWrapperSection);
  FatbinDesc->setAlignment(Align(8));

  return FatbinDesc;
}

/// Create the register globals function. We will iterate all of the offloading
/// entries stored at the begin / end symbols and register them according to
/// their type. This creates the following function in IR:
///
/// extern struct __tgt_offload_entry __start_cuda_offloading_entries;
/// extern struct __tgt_offload_entry __stop_cuda_offloading_entries;
///
/// extern void __cudaRegisterFunction(void **, void *, void *, void *, int,
///                                    void *, void *, void *, void *, int *);
/// extern void __cudaRegisterVar(void **, void *, void *, void *, int32_t,
///                               int64_t, int32_t, int32_t);
///
/// void __cudaRegisterTest(void **fatbinHandle) {
///   for (struct __tgt_offload_entry *entry = &__start_cuda_offloading_entries;
///        entry != &__stop_cuda_offloading_entries; ++entry) {
///     if (!entry->size)
///       __cudaRegisterFunction(fatbinHandle, entry->addr, entry->name,
///                              entry->name, -1, 0, 0, 0, 0, 0);
///     else
///       __cudaRegisterVar(fatbinHandle, entry->addr, entry->name, entry->name,
///                         0, entry->size, 0, 0);
///   }
/// }
Function *createRegisterGlobalsFunction(Module &M, bool IsHIP) {
  LLVMContext &C = M.getContext();
  auto [EntriesB, EntriesE] = offloading::getOffloadEntryArray(
      M, IsHIP ? "hip_offloading_entries" : "cuda_offloading_entries");

  // Get the __cudaRegisterFunction function declaration.
  PointerType *Int8PtrTy = PointerType::get(C, 0);
  PointerType *Int8PtrPtrTy = PointerType::get(C, 0);
  PointerType *Int32PtrTy = PointerType::get(C, 0);
  auto *RegFuncTy = FunctionType::get(
      Type::getInt32Ty(C),
      {Int8PtrPtrTy, Int8PtrTy, Int8PtrTy, Int8PtrTy, Type::getInt32Ty(C),
       Int8PtrTy, Int8PtrTy, Int8PtrTy, Int8PtrTy, Int32PtrTy},
      /*isVarArg*/ false);
  FunctionCallee RegFunc = M.getOrInsertFunction(
      IsHIP ? "__hipRegisterFunction" : "__cudaRegisterFunction", RegFuncTy);

  // Get the __cudaRegisterVar function declaration.
  auto *RegVarTy = FunctionType::get(
      Type::getVoidTy(C),
      {Int8PtrPtrTy, Int8PtrTy, Int8PtrTy, Int8PtrTy, Type::getInt32Ty(C),
       getSizeTTy(M), Type::getInt32Ty(C), Type::getInt32Ty(C)},
      /*isVarArg*/ false);
  FunctionCallee RegVar = M.getOrInsertFunction(
      IsHIP ? "__hipRegisterVar" : "__cudaRegisterVar", RegVarTy);

  auto *RegGlobalsTy = FunctionType::get(Type::getVoidTy(C), Int8PtrPtrTy,
                                         /*isVarArg*/ false);
  auto *RegGlobalsFn =
      Function::Create(RegGlobalsTy, GlobalValue::InternalLinkage,
                       IsHIP ? ".hip.globals_reg" : ".cuda.globals_reg", &M);
  RegGlobalsFn->setSection(".text.startup");

  // Create the loop to register all the entries.
  IRBuilder<> Builder(BasicBlock::Create(C, "entry", RegGlobalsFn));
  auto *EntryBB = BasicBlock::Create(C, "while.entry", RegGlobalsFn);
  auto *IfThenBB = BasicBlock::Create(C, "if.then", RegGlobalsFn);
  auto *IfElseBB = BasicBlock::Create(C, "if.else", RegGlobalsFn);
  auto *SwGlobalBB = BasicBlock::Create(C, "sw.global", RegGlobalsFn);
  auto *SwManagedBB = BasicBlock::Create(C, "sw.managed", RegGlobalsFn);
  auto *SwSurfaceBB = BasicBlock::Create(C, "sw.surface", RegGlobalsFn);
  auto *SwTextureBB = BasicBlock::Create(C, "sw.texture", RegGlobalsFn);
  auto *IfEndBB = BasicBlock::Create(C, "if.end", RegGlobalsFn);
  auto *ExitBB = BasicBlock::Create(C, "while.end", RegGlobalsFn);

  auto *EntryCmp = Builder.CreateICmpNE(EntriesB, EntriesE);
  Builder.CreateCondBr(EntryCmp, EntryBB, ExitBB);
  Builder.SetInsertPoint(EntryBB);
  auto *Entry = Builder.CreatePHI(PointerType::getUnqual(C), 2, "entry");
  auto *AddrPtr =
      Builder.CreateInBoundsGEP(offloading::getEntryTy(M), Entry,
                                {ConstantInt::get(getSizeTTy(M), 0),
                                 ConstantInt::get(Type::getInt32Ty(C), 0)});
  auto *Addr = Builder.CreateLoad(Int8PtrTy, AddrPtr, "addr");
  auto *NamePtr =
      Builder.CreateInBoundsGEP(offloading::getEntryTy(M), Entry,
                                {ConstantInt::get(getSizeTTy(M), 0),
                                 ConstantInt::get(Type::getInt32Ty(C), 1)});
  auto *Name = Builder.CreateLoad(Int8PtrTy, NamePtr, "name");
  auto *SizePtr =
      Builder.CreateInBoundsGEP(offloading::getEntryTy(M), Entry,
                                {ConstantInt::get(getSizeTTy(M), 0),
                                 ConstantInt::get(Type::getInt32Ty(C), 2)});
  auto *Size = Builder.CreateLoad(getSizeTTy(M), SizePtr, "size");
  auto *FlagsPtr =
      Builder.CreateInBoundsGEP(offloading::getEntryTy(M), Entry,
                                {ConstantInt::get(getSizeTTy(M), 0),
                                 ConstantInt::get(Type::getInt32Ty(C), 3)});
  auto *Flags = Builder.CreateLoad(Type::getInt32Ty(C), FlagsPtr, "flag");
  auto *FnCond =
      Builder.CreateICmpEQ(Size, ConstantInt::getNullValue(getSizeTTy(M)));
  Builder.CreateCondBr(FnCond, IfThenBB, IfElseBB);

  // Create kernel registration code.
  Builder.SetInsertPoint(IfThenBB);
  Builder.CreateCall(RegFunc, {RegGlobalsFn->arg_begin(), Addr, Name, Name,
                               ConstantInt::get(Type::getInt32Ty(C), -1),
                               ConstantPointerNull::get(Int8PtrTy),
                               ConstantPointerNull::get(Int8PtrTy),
                               ConstantPointerNull::get(Int8PtrTy),
                               ConstantPointerNull::get(Int8PtrTy),
                               ConstantPointerNull::get(Int32PtrTy)});
  Builder.CreateBr(IfEndBB);
  Builder.SetInsertPoint(IfElseBB);

  auto *Switch = Builder.CreateSwitch(Flags, IfEndBB);
  // Create global variable registration code.
  Builder.SetInsertPoint(SwGlobalBB);
  Builder.CreateCall(RegVar, {RegGlobalsFn->arg_begin(), Addr, Name, Name,
                              ConstantInt::get(Type::getInt32Ty(C), 0), Size,
                              ConstantInt::get(Type::getInt32Ty(C), 0),
                              ConstantInt::get(Type::getInt32Ty(C), 0)});
  Builder.CreateBr(IfEndBB);
  Switch->addCase(Builder.getInt32(OffloadGlobalEntry), SwGlobalBB);

  // Create managed variable registration code.
  Builder.SetInsertPoint(SwManagedBB);
  Builder.CreateBr(IfEndBB);
  Switch->addCase(Builder.getInt32(OffloadGlobalManagedEntry), SwManagedBB);

  // Create surface variable registration code.
  Builder.SetInsertPoint(SwSurfaceBB);
  Builder.CreateBr(IfEndBB);
  Switch->addCase(Builder.getInt32(OffloadGlobalSurfaceEntry), SwSurfaceBB);

  // Create texture variable registration code.
  Builder.SetInsertPoint(SwTextureBB);
  Builder.CreateBr(IfEndBB);
  Switch->addCase(Builder.getInt32(OffloadGlobalTextureEntry), SwTextureBB);

  Builder.SetInsertPoint(IfEndBB);
  auto *NewEntry = Builder.CreateInBoundsGEP(
      offloading::getEntryTy(M), Entry, ConstantInt::get(getSizeTTy(M), 1));
  auto *Cmp = Builder.CreateICmpEQ(
      NewEntry,
      ConstantExpr::getInBoundsGetElementPtr(
          ArrayType::get(offloading::getEntryTy(M), 0), EntriesE,
          ArrayRef<Constant *>({ConstantInt::get(getSizeTTy(M), 0),
                                ConstantInt::get(getSizeTTy(M), 0)})));
  Entry->addIncoming(
      ConstantExpr::getInBoundsGetElementPtr(
          ArrayType::get(offloading::getEntryTy(M), 0), EntriesB,
          ArrayRef<Constant *>({ConstantInt::get(getSizeTTy(M), 0),
                                ConstantInt::get(getSizeTTy(M), 0)})),
      &RegGlobalsFn->getEntryBlock());
  Entry->addIncoming(NewEntry, IfEndBB);
  Builder.CreateCondBr(Cmp, ExitBB, EntryBB);
  Builder.SetInsertPoint(ExitBB);
  Builder.CreateRetVoid();

  return RegGlobalsFn;
}

// Create the constructor and destructor to register the fatbinary with the CUDA
// runtime.
void createRegisterFatbinFunction(Module &M, GlobalVariable *FatbinDesc,
                                  bool IsHIP) {
  LLVMContext &C = M.getContext();
  auto *CtorFuncTy = FunctionType::get(Type::getVoidTy(C), /*isVarArg*/ false);
  auto *CtorFunc =
      Function::Create(CtorFuncTy, GlobalValue::InternalLinkage,
                       IsHIP ? ".hip.fatbin_reg" : ".cuda.fatbin_reg", &M);
  CtorFunc->setSection(".text.startup");

  auto *DtorFuncTy = FunctionType::get(Type::getVoidTy(C), /*isVarArg*/ false);
  auto *DtorFunc =
      Function::Create(DtorFuncTy, GlobalValue::InternalLinkage,
                       IsHIP ? ".hip.fatbin_unreg" : ".cuda.fatbin_unreg", &M);
  DtorFunc->setSection(".text.startup");

  // Get the __cudaRegisterFatBinary function declaration.
  auto *RegFatTy = FunctionType::get(PointerType::getUnqual(C)->getPointerTo(),
                                     PointerType::getUnqual(C),
                                     /*isVarArg*/ false);
  FunctionCallee RegFatbin = M.getOrInsertFunction(
      IsHIP ? "__hipRegisterFatBinary" : "__cudaRegisterFatBinary", RegFatTy);
  // Get the __cudaRegisterFatBinaryEnd function declaration.
  auto *RegFatEndTy = FunctionType::get(
      Type::getVoidTy(C), PointerType::getUnqual(C)->getPointerTo(),
      /*isVarArg*/ false);
  FunctionCallee RegFatbinEnd =
      M.getOrInsertFunction("__cudaRegisterFatBinaryEnd", RegFatEndTy);
  // Get the __cudaUnregisterFatBinary function declaration.
  auto *UnregFatTy = FunctionType::get(
      Type::getVoidTy(C), PointerType::getUnqual(C)->getPointerTo(),
      /*isVarArg*/ false);
  FunctionCallee UnregFatbin = M.getOrInsertFunction(
      IsHIP ? "__hipUnregisterFatBinary" : "__cudaUnregisterFatBinary",
      UnregFatTy);

  auto *AtExitTy =
      FunctionType::get(Type::getInt32Ty(C), DtorFuncTy->getPointerTo(),
                        /*isVarArg*/ false);
  FunctionCallee AtExit = M.getOrInsertFunction("atexit", AtExitTy);

  auto *BinaryHandleGlobal = new llvm::GlobalVariable(
      M, PointerType::getUnqual(C)->getPointerTo(), false,
      llvm::GlobalValue::InternalLinkage,
      llvm::ConstantPointerNull::get(PointerType::getUnqual(C)->getPointerTo()),
      IsHIP ? ".hip.binary_handle" : ".cuda.binary_handle");

  // Create the constructor to register this image with the runtime.
  IRBuilder<> CtorBuilder(BasicBlock::Create(C, "entry", CtorFunc));
  CallInst *Handle = CtorBuilder.CreateCall(
      RegFatbin, ConstantExpr::getPointerBitCastOrAddrSpaceCast(
                     FatbinDesc, PointerType::getUnqual(C)));
  CtorBuilder.CreateAlignedStore(
      Handle, BinaryHandleGlobal,
      Align(M.getDataLayout().getPointerTypeSize(PointerType::getUnqual(C))));
  CtorBuilder.CreateCall(createRegisterGlobalsFunction(M, IsHIP), Handle);
  if (!IsHIP)
    CtorBuilder.CreateCall(RegFatbinEnd, Handle);
  CtorBuilder.CreateCall(AtExit, DtorFunc);
  CtorBuilder.CreateRetVoid();

  // Create the destructor to unregister the image with the runtime. We cannot
  // use a standard global destructor after CUDA 9.2 so this must be called by
  // `atexit()` intead.
  IRBuilder<> DtorBuilder(BasicBlock::Create(C, "entry", DtorFunc));
  LoadInst *BinaryHandle = DtorBuilder.CreateAlignedLoad(
      PointerType::getUnqual(C)->getPointerTo(), BinaryHandleGlobal,
      Align(M.getDataLayout().getPointerTypeSize(PointerType::getUnqual(C))));
  DtorBuilder.CreateCall(UnregFatbin, BinaryHandle);
  DtorBuilder.CreateRetVoid();

  // Add this function to constructors.
  appendToGlobalCtors(M, CtorFunc, /*Priority*/ 1);
}

} // namespace

Error wrapOpenMPBinaries(Module &M, ArrayRef<ArrayRef<char>> Images) {
  GlobalVariable *Desc = createBinDesc(M, Images);
  if (!Desc)
    return createStringError(inconvertibleErrorCode(),
                             "No binary descriptors created.");
  createRegisterFunction(M, Desc);
  createUnregisterFunction(M, Desc);
  return Error::success();
}

Error wrapCudaBinary(Module &M, ArrayRef<char> Image) {
  GlobalVariable *Desc = createFatbinDesc(M, Image, /* IsHIP */ false);
  if (!Desc)
    return createStringError(inconvertibleErrorCode(),
                             "No fatinbary section created.");

  createRegisterFatbinFunction(M, Desc, /* IsHIP */ false);
  return Error::success();
}

Error wrapHIPBinary(Module &M, ArrayRef<char> Image) {
  GlobalVariable *Desc = createFatbinDesc(M, Image, /* IsHIP */ true);
  if (!Desc)
    return createStringError(inconvertibleErrorCode(),
                             "No fatinbary section created.");

  createRegisterFatbinFunction(M, Desc, /* IsHIP */ true);
  return Error::success();
}
