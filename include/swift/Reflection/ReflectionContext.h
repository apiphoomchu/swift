//===--- ReflectionContext.h - Swift Type Reflection Context ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Implements the context for reflection of values in the address space of a
// remote process.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REFLECTION_REFLECTIONCONTEXT_H
#define SWIFT_REFLECTION_REFLECTIONCONTEXT_H

#include "llvm/BinaryFormat/MachO.h"
#include "swift/Remote/MemoryReader.h"
#include "swift/Remote/MetadataReader.h"
#include "swift/Reflection/Records.h"
#include "swift/Reflection/TypeLowering.h"
#include "swift/Reflection/TypeRef.h"
#include "swift/Reflection/TypeRefBuilder.h"
#include "swift/Runtime/Unreachable.h"

#include <iostream>
#include <set>
#include <vector>
#include <unordered_map>
#include <utility>

namespace {

template <unsigned PointerSize> struct MachOTraits;

template <> struct MachOTraits<4> {
  using Header = const struct llvm::MachO::mach_header;
  using SegmentCmd = const struct llvm::MachO::segment_command;
  using Section = const struct llvm::MachO::section;
  static constexpr size_t MagicNumber = llvm::MachO::MH_MAGIC;
};

template <> struct MachOTraits<8> {
  using Header = const struct llvm::MachO::mach_header_64;
  using SegmentCmd = const struct llvm::MachO::segment_command_64;
  using Section = const struct llvm::MachO::section_64;
  static constexpr size_t MagicNumber = llvm::MachO::MH_MAGIC_64;
};
} // namespace

namespace swift {
namespace reflection {

using swift::remote::MemoryReader;
using swift::remote::RemoteAddress;

template <typename Runtime>
class ReflectionContext
       : public remote::MetadataReader<Runtime, TypeRefBuilder> {
  using super = remote::MetadataReader<Runtime, TypeRefBuilder>;
  using super::readMetadata;
  using super::readObjCClassName;

  std::unordered_map<typename super::StoredPointer, const TypeInfo *> Cache;

  /// All buffers we need to keep around long term. This will automatically free them
  /// when this object is destroyed.
  std::vector<MemoryReader::ReadBytesResult> savedBuffers;
  std::vector<std::tuple<RemoteAddress, RemoteAddress>> imageRanges;

public:
  using super::getBuilder;
  using super::readDemanglingForContextDescriptor;
  using super::readIsaMask;
  using super::readTypeFromMetadata;
  using super::readGenericArgFromMetadata;
  using super::readMetadataFromInstance;
  using typename super::StoredPointer;

  explicit ReflectionContext(std::shared_ptr<MemoryReader> reader)
    : super(std::move(reader)) {
    getBuilder().setSymbolicReferenceResolverReader(*this);
  }

  ReflectionContext(const ReflectionContext &other) = delete;
  ReflectionContext &operator=(const ReflectionContext &other) = delete;
  
  MemoryReader &getReader() {
    return *this->Reader;
  }

  unsigned getSizeOfHeapObject() {
    // This must match sizeof(HeapObject) for the target.
    return sizeof(StoredPointer) * 2;
  }

  void dumpAllSections(std::ostream &OS) {
    getBuilder().dumpAllSections(OS);
  }

#if defined(__APPLE__) && defined(__MACH__)
  template <typename T> bool readMachOSections(RemoteAddress ImageStart) {
    auto Buf =
        this->getReader().readBytes(ImageStart, sizeof(typename T::Header));
    if (!Buf)
      return false;
    auto Header = reinterpret_cast<typename T::Header *>(Buf.get());
    assert(Header->magic == T::MagicNumber && "invalid MachO file");

    auto NumCommands = Header->sizeofcmds;

    // The layout of the executable is such that the commands immediately follow
    // the header.
    auto CmdStartAddress =
        RemoteAddress(ImageStart.getAddressData() + sizeof(typename T::Header));
    uint32_t SegmentCmdHdrSize = sizeof(typename T::SegmentCmd);
    uint64_t Offset = 0;

    // Find the __TEXT segment.
    typename T::SegmentCmd *Command = nullptr;
    for (unsigned I = 0; I < NumCommands; ++I) {
      auto CmdBuf = this->getReader().readBytes(
          RemoteAddress(CmdStartAddress.getAddressData() + Offset),
          SegmentCmdHdrSize);
      auto CmdHdr = reinterpret_cast<typename T::SegmentCmd *>(CmdBuf.get());
      if (strncmp(CmdHdr->segname, "__TEXT", sizeof(CmdHdr->segname)) == 0) {
        Command = CmdHdr;
        break;
      }
      Offset += CmdHdr->cmdsize;
    }

    // No __TEXT segment, bail out.
    if (!Command)
      return false;

    auto SectAddress = RemoteAddress(CmdStartAddress.getAddressData() + Offset +
                                     sizeof(typename T::SegmentCmd));
    std::vector<typename T::Section *> SectionsHdr;
    for (unsigned I = 0; I < Command->nsects; ++I) {
      auto SecHdrBuf = this->getReader().readBytes(
          RemoteAddress(SectAddress.getAddressData() +
                        (I * sizeof(typename T::Section))),
          sizeof(typename T::Section));
      auto SecHdr = reinterpret_cast<typename T::Section *>(SecHdrBuf.get());
      if (strncmp(SecHdr->sectname, "__swift5", strlen("__swift5")) == 0)
        SectionsHdr.push_back(SecHdr);
    }

    auto findMachOSectionByName = [&](std::string Name)
        -> std::pair<std::pair<const char *, const char *>, uint32_t> {
      // Now for all the sections, find their name.
      for (auto &S : SectionsHdr) {
        if (strncmp(S->sectname, Name.c_str(), strlen(Name.c_str())) != 0)
          continue;
        auto SecStart = RemoteAddress(ImageStart.getAddressData() + S->offset);
        auto SecSize = S->size;
        auto SecBuf = this->getReader().readBytes(SecStart, SecSize);
        auto SecContents = reinterpret_cast<const char *>(SecBuf.get());
        return {{SecContents, SecContents + SecSize}, 0};
      }
      return {{nullptr, nullptr}, 0};
    };

    auto FieldMdSec = findMachOSectionByName("__swift5_fieldmd");
    auto AssocTySec = findMachOSectionByName("__swift5_assocty");
    auto BuiltinTySec = findMachOSectionByName("__swift5_builtin");
    auto CaptureSec = findMachOSectionByName("__swift5_capture");
    auto TypeRefMdSec = findMachOSectionByName("__swift5_typeref");
    auto ReflStrMdSec = findMachOSectionByName("__swift5_reflstr");

    if (FieldMdSec.first.first == nullptr &&
        AssocTySec.first.first == nullptr &&
        BuiltinTySec.first.first == nullptr &&
        CaptureSec.first.first == nullptr &&
        TypeRefMdSec.first.first == nullptr &&
        ReflStrMdSec.first.first == nullptr)
      return false;

    Buf = this->getReader().readBytes(ImageStart, sizeof(typename T::Header));
    auto LocalStartAddress = reinterpret_cast<uintptr_t>(Buf.get());
    auto RemoteStartAddress = static_cast<uintptr_t>(ImageStart.getAddressData());

    ReflectionInfo info = {
        {{FieldMdSec.first.first, FieldMdSec.first.second}, 0},
        {{AssocTySec.first.first, AssocTySec.first.second}, 0},
        {{BuiltinTySec.first.first, BuiltinTySec.first.second}, 0},
        {{CaptureSec.first.first, CaptureSec.first.second}, 0},
        {{TypeRefMdSec.first.first, TypeRefMdSec.first.second}, 0},
        {{ReflStrMdSec.first.first, ReflStrMdSec.first.second}, 0},
        LocalStartAddress,
        RemoteStartAddress};

    this->addReflectionInfo(info);
    return true;
  }

  bool addImage(RemoteAddress ImageStart) {
    // We start reading 4 bytes. The first 4 bytes are supposed to be
    // the magic, so we understand whether this is a 32-bit executable or
    // a 64-bit one.
    auto Buf = this->getReader().readBytes(ImageStart, sizeof(uint32_t));
    if (!Buf)
      return false;
    auto HeaderMagic = reinterpret_cast<const uint32_t *>(Buf.get());
    if (*HeaderMagic != llvm::MachO::MH_MAGIC &&
        *HeaderMagic != llvm::MachO::MH_MAGIC_64)
      return false;

    if (*HeaderMagic == llvm::MachO::MH_MAGIC)
      return readMachOSections<MachOTraits<4>>(ImageStart);
    if (*HeaderMagic == llvm::MachO::MH_MAGIC_64)
      return readMachOSections<MachOTraits<8>>(ImageStart);
    llvm_unreachable("invalid pointer size");
  }
#endif // defined(__APPLE__) && defined(__MACH__)
  
  void addReflectionInfo(ReflectionInfo I) {
    getBuilder().addReflectionInfo(I);
  }
  
  bool ownsObject(RemoteAddress ObjectAddress) {
    auto MetadataAddress = readMetadataFromInstance(ObjectAddress.getAddressData());
    if (!MetadataAddress)
      return true;
    return ownsAddress(RemoteAddress(*MetadataAddress));
  }
  
  /// Returns true if the address falls within a registered image.
  bool ownsAddress(RemoteAddress Address) {
    for (auto Range : imageRanges) {
      auto Start = std::get<0>(Range);
      auto End = std::get<1>(Range);
      if (Start.getAddressData() <= Address.getAddressData()
          && Address.getAddressData() < End.getAddressData())
        return true;
    }
  
    return false;
  }
  
  /// Return a description of the layout of a class instance with the given
  /// metadata as its isa pointer.
  const TypeInfo *getMetadataTypeInfo(StoredPointer MetadataAddress) {
    // See if we cached the layout already
    auto found = Cache.find(MetadataAddress);
    if (found != Cache.end())
      return found->second;

    auto &TC = getBuilder().getTypeConverter();

    const TypeInfo *TI = nullptr;

    auto TR = readTypeFromMetadata(MetadataAddress);
    auto kind = this->readKindFromMetadata(MetadataAddress);
    if (TR != nullptr && kind) {
      switch (*kind) {
      case MetadataKind::Class: {
        // Figure out where the stored properties of this class begin
        // by looking at the size of the superclass
        auto start =
            this->readInstanceStartAndAlignmentFromClassMetadata(MetadataAddress);

        // Perform layout
        if (start)
          TI = TC.getClassInstanceTypeInfo(TR, *start);

        break;
      }
      default:
        break;
      }
    }

    // Cache the result for future lookups
    Cache[MetadataAddress] = TI;
    return TI;
  }

  /// Return a description of the layout of a class instance with the given
  /// metadata as its isa pointer.
  const TypeInfo *getInstanceTypeInfo(StoredPointer ObjectAddress) {
    auto MetadataAddress = readMetadataFromInstance(ObjectAddress);
    if (!MetadataAddress)
      return nullptr;

    auto kind = this->readKindFromMetadata(*MetadataAddress);
    if (!kind)
      return nullptr;

    switch (*kind) {
    case MetadataKind::Class:
      return getMetadataTypeInfo(*MetadataAddress);

    case MetadataKind::HeapLocalVariable: {
      auto CDAddr = this->readCaptureDescriptorFromMetadata(*MetadataAddress);
      if (!CDAddr)
        return nullptr;

      // FIXME: Non-generic SIL boxes also use the HeapLocalVariable metadata
      // kind, but with a null capture descriptor right now (see
      // FixedBoxTypeInfoBase::allocate).
      //
      // Non-generic SIL boxes share metadata among types with compatible
      // layout, but we need some way to get an outgoing pointer map for them.
      auto *CD = getBuilder().getCaptureDescriptor(*CDAddr);
      if (CD == nullptr)
        return nullptr;

      auto Info = getBuilder().getClosureContextInfo(*CD, 0);

      return getClosureContextInfo(ObjectAddress, Info);
    }

    case MetadataKind::HeapGenericLocalVariable: {
      // Generic SIL @box type - there is always an instantiated metadata
      // pointer for the boxed type.
      if (auto Meta = readMetadata(*MetadataAddress)) {
        auto GenericHeapMeta =
          cast<TargetGenericBoxHeapMetadata<Runtime>>(Meta.getLocalBuffer());
        return getMetadataTypeInfo(GenericHeapMeta->BoxedType);
      }
      return nullptr;
    }

    case MetadataKind::ErrorObject:
      // Error boxed existential on non-Objective-C runtime target
      return nullptr;

    default:
      return nullptr;
    }
  }

  bool
  projectExistential(RemoteAddress ExistentialAddress,
                     const TypeRef *ExistentialTR,
                     const TypeRef **OutInstanceTR,
                     RemoteAddress *OutInstanceAddress) {
    if (ExistentialTR == nullptr)
      return false;

    auto ExistentialTI = getTypeInfo(ExistentialTR);
    if (ExistentialTI == nullptr)
      return false;

    auto ExistentialRecordTI = dyn_cast<const RecordTypeInfo>(ExistentialTI);
    if (ExistentialRecordTI == nullptr)
      return false;

    switch (ExistentialRecordTI->getRecordKind()) {
    // Class existentials have trivial layout.
    // It is itself the pointer to the instance followed by the witness tables.
    case RecordKind::ClassExistential:
      // This is just Builtin.UnknownObject
      *OutInstanceTR = ExistentialRecordTI->getFields()[0].TR;
      *OutInstanceAddress = ExistentialAddress;
      return true;

    // Opaque existentials fall under two cases:
    // If the value fits in three words, it starts at the beginning of the
    // container. If it doesn't, the first word is a pointer to a heap box.
    case RecordKind::OpaqueExistential: {
      auto Fields = ExistentialRecordTI->getFields();
      auto ExistentialMetadataField = std::find_if(Fields.begin(), Fields.end(),
                                   [](const FieldInfo &FI) -> bool {
        return FI.Name.compare("metadata") == 0;
      });
      if (ExistentialMetadataField == Fields.end())
        return false;

      // Get the metadata pointer for the contained instance type.
      // This is equivalent to:
      // auto PointerArray = reinterpret_cast<uintptr_t*>(ExistentialAddress);
      // uintptr_t MetadataAddress = PointerArray[Offset];
      auto MetadataAddressAddress
        = RemoteAddress(ExistentialAddress.getAddressData() +
                        ExistentialMetadataField->Offset);

      StoredPointer MetadataAddress = 0;
      if (!getReader().readInteger(MetadataAddressAddress, &MetadataAddress))
        return false;

      auto InstanceTR = readTypeFromMetadata(MetadataAddress);
      if (!InstanceTR)
        return false;

      *OutInstanceTR = InstanceTR;

      auto InstanceTI = getTypeInfo(InstanceTR);
      if (!InstanceTI)
        return false;

      if (InstanceTI->getSize() <= ExistentialMetadataField->Offset) {
        // The value fits in the existential container, so it starts at the
        // start of the container.
        *OutInstanceAddress = ExistentialAddress;
      } else {
        // Otherwise it's in a box somewhere off in the heap. The first word
        // of the container has the address to that box.
        StoredPointer BoxAddress = 0;

        if (!getReader().readInteger(ExistentialAddress, &BoxAddress))
          return false;

        // Address = BoxAddress + (sizeof(HeapObject) + alignMask) & ~alignMask)
        auto Alignment = InstanceTI->getAlignment();
        auto StartOfValue = BoxAddress + getSizeOfHeapObject();
        // Align.
        StartOfValue += Alignment - StartOfValue % Alignment;
        *OutInstanceAddress = RemoteAddress(StartOfValue);
      }
      return true;
    }
    case RecordKind::ErrorExistential: {
      // We have a pointer to an error existential, which is always heap object.

      auto MetadataAddress
        = readMetadataFromInstance(ExistentialAddress.getAddressData());

      if (!MetadataAddress)
        return false;

      bool isObjC = false;

      // If we can determine the Objective-C class name, this is probably an
      // error existential with NSError-compatible layout.
      std::string ObjCClassName;
      if (readObjCClassName(*MetadataAddress, ObjCClassName)) {
        if (ObjCClassName == "_SwiftNativeNSError")
          isObjC = true;
      } else {
        // Otherwise, we can check to see if this is a class metadata with the
        // kind value's least significant bit set, which indicates a pure
        // Swift class.
        auto Meta = readMetadata(*MetadataAddress);
        auto ClassMeta = dyn_cast<TargetClassMetadata<Runtime>>(Meta);
        if (!ClassMeta)
          return false;

        isObjC = ClassMeta->isPureObjC();
      }

      // In addition to the isa pointer and two 32-bit reference counts, if the
      // error existential is layout-compatible with NSError, we also need to
      // skip over its three word-sized fields: the error code, the domain,
      // and userInfo.
      StoredPointer InstanceMetadataAddressAddress
        = ExistentialAddress.getAddressData() +
          (isObjC ? 5 : 2) * sizeof(StoredPointer);

      // We need to get the instance's alignment info so we can get the exact
      // offset of the start of its data in the class.
      auto InstanceMetadataAddress =
        readMetadataFromInstance(InstanceMetadataAddressAddress);
      if (!InstanceMetadataAddress)
        return false;

      auto InstanceTR = readTypeFromMetadata(*InstanceMetadataAddress);
      if (!InstanceTR)
        return false;

      auto InstanceTI = getTypeInfo(InstanceTR);
      if (!InstanceTI)
        return false;

      // Now we need to skip over the instance metadata pointer and instance's
      // conformance pointer for Swift.Error.
      StoredPointer InstanceAddress = InstanceMetadataAddressAddress +
        2 * sizeof(StoredPointer);

      // Round up to alignment, and we have the start address of the
      // instance payload.
      auto Alignment = InstanceTI->getAlignment();
      InstanceAddress += Alignment - InstanceAddress % Alignment;

      *OutInstanceTR = InstanceTR;
      *OutInstanceAddress = RemoteAddress(InstanceAddress);

      return true;
    }
    default:
      return false;
    }
  }

  /// Return a description of the layout of a value with the given type.
  const TypeInfo *getTypeInfo(const TypeRef *TR) {
    return getBuilder().getTypeConverter().getTypeInfo(TR);
  }

private:
  const TypeInfo *getClosureContextInfo(StoredPointer Context,
                                        const ClosureContextInfo &Info) {
    RecordTypeInfoBuilder Builder(getBuilder().getTypeConverter(),
                                  RecordKind::ClosureContext);

    auto Metadata = readMetadataFromInstance(Context);
    if (!Metadata)
      return nullptr;

    // Calculate the offset of the first capture.
    // See GenHeap.cpp, buildPrivateMetadata().
    auto OffsetToFirstCapture =
        this->readOffsetToFirstCaptureFromMetadata(*Metadata);
    if (!OffsetToFirstCapture)
      return nullptr;

    // Initialize the builder.
    Builder.addField(*OffsetToFirstCapture, sizeof(StoredPointer),
                     /*numExtraInhabitants=*/0);

    // Skip the closure's necessary bindings struct, if it's present.
    auto SizeOfNecessaryBindings = Info.NumBindings * sizeof(StoredPointer);
    Builder.addField(SizeOfNecessaryBindings, sizeof(StoredPointer),
                     /*numExtraInhabitants=*/0);

    // FIXME: should be unordered_set but I'm too lazy to write a hash
    // functor
    std::set<std::pair<const TypeRef *, const MetadataSource *>> Done;
    GenericArgumentMap Subs;

    ArrayRef<const TypeRef *> CaptureTypes = Info.CaptureTypes;

    // Closure context element layout depends on the layout of the
    // captured types, but captured types might depend on element
    // layout (of previous elements). Use an iterative approach to
    // solve the problem.
    while (!CaptureTypes.empty()) {
      const TypeRef *OrigCaptureTR = CaptureTypes[0];

      // If we failed to demangle the capture type, we cannot proceed.
      if (OrigCaptureTR == nullptr)
        return nullptr;

      const TypeRef *SubstCaptureTR = nullptr;

      // If we have enough substitutions to make this captured value's
      // type concrete, or we know it's size anyway (because it is a
      // class reference or metatype, for example), go ahead and add
      // it to the layout.
      if (OrigCaptureTR->isConcreteAfterSubstitutions(Subs))
        SubstCaptureTR = OrigCaptureTR->subst(getBuilder(), Subs);
      else if (getBuilder().getTypeConverter().hasFixedSize(OrigCaptureTR))
        SubstCaptureTR = OrigCaptureTR;

      if (SubstCaptureTR != nullptr) {
        Builder.addField("", SubstCaptureTR);
        if (Builder.isInvalid())
          return nullptr;

        // Try the next capture type.
        CaptureTypes = CaptureTypes.slice(1);
        continue;
      }

      // Ok, we do not have enough substitutions yet. Perhaps we have
      // enough elements figured out that we can pick off some
      // metadata sources though, and use those to derive some new
      // substitutions.
      bool Progress = false;
      for (auto Source : Info.MetadataSources) {
        // Don't read a source more than once.
        if (Done.count(Source))
          continue;

        // If we don't have enough information to read this source
        // (because it is fulfilled by metadata from a capture at
        // at unknown offset), keep going.
        if (!isMetadataSourceReady(Source.second, Builder))
          continue;

        auto Metadata = readMetadataSource(Context, Source.second, Builder);
        if (!Metadata)
          return nullptr;

        auto *SubstTR = readTypeFromMetadata(*Metadata);
        if (SubstTR == nullptr)
          return nullptr;

        if (!TypeRef::deriveSubstitutions(Subs, Source.first, SubstTR))
          return nullptr;

        Done.insert(Source);
        Progress = true;
      }

      // If we failed to make any forward progress above, we're stuck
      // and cannot close out this layout.
      if (!Progress)
        return nullptr;
    }

    // Ok, we have a complete picture now.
    return Builder.build();
  }

  /// Checks if we have enough information to read the given metadata
  /// source.
  ///
  /// \param Builder Used to obtain offsets of elements known so far.
  bool isMetadataSourceReady(const MetadataSource *MS,
                             const RecordTypeInfoBuilder &Builder) {
    switch (MS->getKind()) {
    case MetadataSourceKind::ClosureBinding:
      return true;
    case MetadataSourceKind::ReferenceCapture: {
      unsigned Index = cast<ReferenceCaptureMetadataSource>(MS)->getIndex();
      return Index < Builder.getNumFields();
    }
    case MetadataSourceKind::MetadataCapture: {
      unsigned Index = cast<MetadataCaptureMetadataSource>(MS)->getIndex();
      return Index < Builder.getNumFields();
    }
    case MetadataSourceKind::GenericArgument: {
      auto Base = cast<GenericArgumentMetadataSource>(MS)->getSource();
      return isMetadataSourceReady(Base, Builder);
    }
    case MetadataSourceKind::Self:
    case MetadataSourceKind::SelfWitnessTable:
      return true;
    }

    swift_runtime_unreachable("Unhandled MetadataSourceKind in switch.");
  }

  /// Read metadata for a captured generic type from a closure context.
  ///
  /// \param Context The closure context in the remote process.
  ///
  /// \param MS The metadata source, which must be "ready" as per the
  /// above.
  ///
  /// \param Builder Used to obtain offsets of elements known so far.
  llvm::Optional<StoredPointer>
  readMetadataSource(StoredPointer Context,
                     const MetadataSource *MS,
                     const RecordTypeInfoBuilder &Builder) {
    switch (MS->getKind()) {
    case MetadataSourceKind::ClosureBinding: {
      unsigned Index = cast<ClosureBindingMetadataSource>(MS)->getIndex();

      // Skip the context's HeapObject header
      // (one word each for isa pointer and reference counts).
      //
      // Metadata and conformance tables are stored consecutively after
      // the heap object header, in the 'necessary bindings' area.
      //
      // We should only have the index of a type metadata record here.
      unsigned Offset = getSizeOfHeapObject() +
                        sizeof(StoredPointer) * Index;

      StoredPointer MetadataAddress;
      if (!getReader().readInteger(RemoteAddress(Context + Offset),
                                   &MetadataAddress))
        break;

      return MetadataAddress;
    }
    case MetadataSourceKind::ReferenceCapture: {
      unsigned Index = cast<ReferenceCaptureMetadataSource>(MS)->getIndex();

      // We should already have enough type information to know the offset
      // of this capture in the context.
      unsigned CaptureOffset = Builder.getFieldOffset(Index);

      StoredPointer CaptureAddress;
      if (!getReader().readInteger(RemoteAddress(Context + CaptureOffset),
                                   &CaptureAddress))
        break;

      // Read the requested capture's isa pointer.
      return readMetadataFromInstance(CaptureAddress);
    }
    case MetadataSourceKind::MetadataCapture: {
      unsigned Index = cast<MetadataCaptureMetadataSource>(MS)->getIndex();

      // We should already have enough type information to know the offset
      // of this capture in the context.
      unsigned CaptureOffset = Builder.getFieldOffset(Index);

      StoredPointer CaptureAddress;
      if (!getReader().readInteger(RemoteAddress(Context + CaptureOffset),
                                   &CaptureAddress))
        break;

      return CaptureAddress;
    }
    case MetadataSourceKind::GenericArgument: {
      auto *GAMS = cast<GenericArgumentMetadataSource>(MS);
      auto Base = readMetadataSource(Context, GAMS->getSource(), Builder);
      if (!Base)
        break;

      unsigned Index = GAMS->getIndex();
      auto Arg = readGenericArgFromMetadata(*Base, Index);
      if (!Arg)
        break;

      return *Arg;
    }
    case MetadataSourceKind::Self:
    case MetadataSourceKind::SelfWitnessTable:
      break;
    }

    return llvm::None;
  }
};

} // end namespace reflection
} // end namespace swift

#endif // SWIFT_REFLECTION_REFLECTIONCONTEXT_H
