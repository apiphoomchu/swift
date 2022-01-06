//===--- GenericEnvironment.h - Generic Environment AST ---------*- C++ -*-===//
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
// This file defines the GenericEnvironment class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_GENERIC_ENVIRONMENT_H
#define SWIFT_AST_GENERIC_ENVIRONMENT_H

#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/GenericParamKey.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/GenericSignature.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Debug.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TrailingObjects.h"
#include <utility>

namespace swift {

class ArchetypeType;
class ASTContext;
class GenericTypeParamType;
class OpaqueTypeDecl;
class OpenedArchetypeType;
class SILModule;
class SILType;

/// Query function suitable for use as a \c TypeSubstitutionFn that queries
/// the mapping of interface types to archetypes.
class QueryInterfaceTypeSubstitutions {
  const GenericEnvironment *self;
  
public:
  QueryInterfaceTypeSubstitutions(const GenericEnvironment *self)
  : self(self) { }
  
  Type operator()(SubstitutableType *type) const;
};

/// Describes the mapping between archetypes and interface types for the
/// generic parameters of a DeclContext.
///
/// The most frequently used method here is mapTypeIntoContext(), which
/// maps an interface type to a type written in terms of the generic
/// environment's archetypes; to go in the other direction, use
/// TypeBase::mapTypeOutOfContext().
///
class alignas(1 << DeclAlignInBits) GenericEnvironment final
    : private llvm::TrailingObjects<
        GenericEnvironment, OpaqueTypeDecl *, SubstitutionMap, Type> {
public:
  enum class Kind {
    /// A normal generic environment, determined only by its generic
    /// signature.
    Normal,
    /// A generic environment describing an opened existential archetype.
    OpenedExistential,
    /// A generic environment describing an opaque type archetype.
    Opaque,
  };

private:
  mutable llvm::PointerIntPair<GenericSignature, 2, Kind> SignatureAndKind{
      GenericSignature(), Kind::Normal};

  friend TrailingObjects;
  friend OpaqueTypeArchetypeType;

  size_t numTrailingObjects(OverloadToken<OpaqueTypeDecl *>) const;
  size_t numTrailingObjects(OverloadToken<SubstitutionMap>) const;
  size_t numTrailingObjects(OverloadToken<Type>) const;

  /// Retrieve the array containing the context types associated with the
  /// generic parameters, stored in parallel with the generic parameters of the
  /// generic signature.
  MutableArrayRef<Type> getContextTypes();

  /// Retrieve the array containing the context types associated with the
  /// generic parameters, stored in parallel with the generic parameters of the
  /// generic signature.
  ArrayRef<Type> getContextTypes() const;

  explicit GenericEnvironment(GenericSignature signature, Kind kind);
  explicit GenericEnvironment(
      GenericSignature signature, OpaqueTypeDecl *opaque, SubstitutionMap subs);

  friend ArchetypeType;
  friend QueryInterfaceTypeSubstitutions;

  Type getOrCreateArchetypeFromInterfaceType(Type depType);

  /// Retrieve the mapping for the given generic parameter, if present.
  ///
  /// This is only useful when lazily populating a generic environment.
  Optional<Type> getMappingIfPresent(GenericParamKey key) const;

  /// Get the "raw" generic signature, without substituting into opaque
  /// type's signature.
  GenericSignature getRawGenericSignature() const;

public:
  GenericSignature getGenericSignature() const;

  Kind getKind() const { return SignatureAndKind.getInt(); }

  TypeArrayView<GenericTypeParamType> getGenericParams() const;

  /// Retrieve the opaque type declaration for a generic environment describing
  /// opaque types.
  OpaqueTypeDecl *getOpaqueTypeDecl() const;

  /// Retrieve the substitutions applied to an opaque type declaration to
  /// create a generic environment.
  SubstitutionMap getOpaqueSubstitutions() const;

  /// Create a new, "incomplete" generic environment that will be populated
  /// by calls to \c addMapping().
  static
  GenericEnvironment *getIncomplete(GenericSignature signature);

  /// Create a new generic environment for an opened existential.
  static GenericEnvironment *forOpenedExistential(
      GenericSignature signature, const OpenedArchetypeType *type);

  /// Create a new generic environment for an opaque type with the given set of
  /// outer substitutions.
  static GenericEnvironment *forOpaqueType(
      OpaqueTypeDecl *opaque, SubstitutionMap subs, AllocationArena arena);

  /// Add a mapping of a generic parameter to a specific type (which may be
  /// an archetype)
  void addMapping(GenericParamKey key, Type contextType);

  /// Make vanilla new/delete illegal.
  void *operator new(size_t Bytes) = delete;
  void operator delete(void *Data) = delete;

  /// Only allow placement new.
  void *operator new(size_t Bytes, void *Mem) {
    assert(Mem); 
    return Mem; 
  }

  /// For an opaque archetype environment, apply the substitutions.
  Type maybeApplyOpaqueTypeSubstitutions(Type type) const;

  /// Compute the canonical interface type within this environment.
  Type getCanonicalInterfaceType(Type interfaceType);

  /// Map an interface type to a contextual type.
  static Type mapTypeIntoContext(GenericEnvironment *genericEnv,
                                 Type type);

  /// Map an interface type to a contextual type.
  Type mapTypeIntoContext(Type type) const;

  /// Map an interface type to a contextual type.
  Type mapTypeIntoContext(Type type,
                          LookupConformanceFn lookupConformance) const;

  /// Map a generic parameter type to a contextual type.
  Type mapTypeIntoContext(GenericTypeParamType *type) const;

  /// Map the given SIL interface type to a contextual type.
  ///
  /// This operation will also reabstract dependent types according to the
  /// abstraction level of their associated type requirements.
  SILType mapTypeIntoContext(SILModule &M, SILType type) const;

  /// Map an interface type's protocol conformance into the corresponding
  /// conformance for the contextual type.
  static std::pair<Type, ProtocolConformanceRef>
  mapConformanceRefIntoContext(GenericEnvironment *genericEnv,
                               Type conformingType,
                               ProtocolConformanceRef conformance);

  /// Map an interface type's protocol conformance into the corresponding
  /// conformance for the contextual type.
  std::pair<Type, ProtocolConformanceRef>
  mapConformanceRefIntoContext(Type conformingType,
                               ProtocolConformanceRef conformance) const;

  /// Returns a substitution map that sends every generic parameter to its
  /// corresponding archetype in this generic environment.
  SubstitutionMap getForwardingSubstitutionMap() const;

  void dump(raw_ostream &os) const;

  SWIFT_DEBUG_DUMP;
};
  
} // end namespace swift

#endif // SWIFT_AST_GENERIC_ENVIRONMENT_H

