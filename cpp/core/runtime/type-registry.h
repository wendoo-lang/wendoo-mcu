#pragma once

#include <cstdint>
#include <cstring>

#include "core/platform/span.h"
#include "core/runtime/program.h"
#include "core/runtime/value.h"

namespace wendoo {

/** Reads a native struct field by id, returning the field value. */
using NativeStructFieldGetter = Value (*)(const Value& source, uint32_t fieldId);

/**
 * Writes a native struct field by id. Returns false to reject the write (e.g. a
 * read-only field).
 */
using NativeStructFieldSetter = bool (*)(const Value& source, uint32_t fieldId, const Value& value);

/**
 * Materializes a native struct value's handle during deep copy (assignment):
 * resolves a lazy handle (e.g. a resolver token) to the concrete value the
 * copy stores. Mirrors `snapshotNative` in
 * external/wendoo-lang/packages/core/src/runtime/type-defs.ts.
 */
using NativeStructSnapshot = Value (*)(const Value& source);

/**
 * One native struct type's field accessors, keyed by the typeId a native struct
 * value of that type carries. The `getter` maps a source value and field id to
 * the field value; the `setter` writes a field and reports accept/reject.
 */
struct NativeStructTypeBinding {
  /** TypeId a native struct value of this type carries (its `Value::typeId`). */
  uint32_t typeId;

  /** Field reader. Must be non-null. */
  NativeStructFieldGetter getter;

  /** Field writer, or null when the type rejects all field writes. */
  NativeStructFieldSetter setter;

  /** Deep-copy handle snapshot, or null when the handle copies by reference. */
  NativeStructSnapshot snapshot = nullptr;
};

/**
 * Field storage slot count of a host-registered (atom) struct type, keyed by its
 * stable type-atom id. `STRUCT_NEW` reads it to size a managed struct allocation
 * of that type.
 */
struct RegisteredStructSlotCount {
  /** Stable type-atom id of the registered struct type. */
  uint32_t atomId;

  /** Field storage slot count (its highest field id + 1). */
  uint32_t slotCount;
};

/**
 * Resolves type identities against the decoded program image: program-local
 * struct field names and slot counts, the instantiated container types the core
 * builtins produce, and the optional native field getter/setter of a struct
 * type. Mirrors the role of `runtime.types` in
 * external/wendoo-lang/packages/core/src/runtime/vm.ts on the device.
 *
 * A program-local struct has no native getter/setter; its fields are managed
 * slab slots indexed by field id.
 */
class TypeRegistry {
public:
  explicit TypeRegistry(const ProgramImage& program,
                        Span<const NativeStructTypeBinding> nativeStructs = {})
      : program_(&program), nativeStructs_(nativeStructs) {}

  /**
   * Installs the slot counts of host-registered (atom) struct types, read by
   * {@link registeredStructSlotCount}. `bindings` must outlive the registry.
   */
  void setRegisteredStructSlotCounts(Span<const RegisteredStructSlotCount> bindings) {
    registeredStructs_ = bindings;
  }

  /**
   * Resolves the field storage slot count of the registered (atom) struct type
   * `atomId`. Writes it to `slotCount` and returns true on a match; returns false
   * when no registered struct carries that atom id.
   */
  bool registeredStructSlotCount(uint32_t atomId, uint32_t& slotCount) const {
    for (size_t i = 0; i < registeredStructs_.size(); i++) {
      if (registeredStructs_[i].atomId == atomId) {
        slotCount = registeredStructs_[i].slotCount;
        return true;
      }
    }
    return false;
  }

  /** The decoded program image this registry resolves against. */
  const ProgramImage& program() const { return *program_; }

  /**
   * Installs the native struct field accessors read by {@link
   * nativeStructGetter} and {@link nativeStructSetter}. `bindings` must outlive
   * the registry.
   */
  void setNativeStructBindings(Span<const NativeStructTypeBinding> bindings) {
    nativeStructs_ = bindings;
  }

  /** Field storage slot count of struct `typeIdx`, or 0 when it is not a struct. */
  uint32_t structSlotCount(uint32_t typeIdx) const {
    if (typeIdx >= program_->types.size()) {
      return 0;
    }
    const TypeEntry& entry = program_->types[typeIdx];
    return entry.tag == TypeTag::Struct ? entry.structOf.slotCount : 0;
  }

  /**
   * Resolves field name `name` (`length` bytes) to its storage slot on struct
   * `typeIdx`. Writes the slot to `fieldId` and returns true on a match;
   * returns false when `typeIdx` is not a struct or carries no such field.
   */
  bool findStructField(uint32_t typeIdx, const char* name, uint32_t length,
                       uint32_t& fieldId) const {
    if (typeIdx >= program_->types.size()) {
      return false;
    }
    const TypeEntry& entry = program_->types[typeIdx];
    if (entry.tag != TypeTag::Struct) {
      return false;
    }
    for (uint32_t i = 0; i < entry.structOf.fieldsCount; i++) {
      const StructFieldRef& field = program_->structFields[entry.structOf.fieldsOffset + i];
      const StringRef& ref = program_->strings[field.nameStringIdx];
      if (ref.length == length &&
          (length == 0 ||
           std::memcmp(program_->stringData.data() + ref.offset, name, length) == 0)) {
        fieldId = field.fieldId;
        return true;
      }
    }
    return false;
  }

  /** Type-table index of `List<elemTypeIdx>`, or {@link kNoTypeIdx} when the program has none. */
  uint32_t findListType(uint32_t elemTypeIdx) const {
    for (uint32_t i = 0; i < program_->types.size(); i++) {
      const TypeEntry& entry = program_->types[i];
      if (entry.tag == TypeTag::List && entry.list.elem == elemTypeIdx) {
        return i;
      }
    }
    return kNoTypeIdx;
  }

  /** Type-table index of `Map<keyTypeIdx,valueTypeIdx>`, or {@link kNoTypeIdx} when absent. */
  uint32_t findMapType(uint32_t keyTypeIdx, uint32_t valueTypeIdx) const {
    for (uint32_t i = 0; i < program_->types.size(); i++) {
      const TypeEntry& entry = program_->types[i];
      if (entry.tag == TypeTag::Map && entry.map.key == keyTypeIdx &&
          entry.map.value == valueTypeIdx) {
        return i;
      }
    }
    return kNoTypeIdx;
  }

  /** Type-table index of the core/target atom type `atomId`, or {@link kNoTypeIdx} when absent. */
  uint32_t findAtomType(uint32_t atomId) const {
    for (uint32_t i = 0; i < program_->types.size(); i++) {
      const TypeEntry& entry = program_->types[i];
      if (entry.tag == TypeTag::Atom && entry.atom.atomId == atomId) {
        return i;
      }
    }
    return kNoTypeIdx;
  }

  /**
   * Whether a struct value carrying `typeId` is a managed heap struct: a
   * program struct type, or a registered atom struct type with storage slots.
   * Native struct values (an injected execution context, a device receiver)
   * carry type ids outside this set and hold no heap handle.
   */
  bool isManagedStructType(uint32_t typeId) const {
    if (typeId >= program_->types.size()) {
      return false;
    }
    const TypeEntry& entry = program_->types[typeId];
    if (entry.tag == TypeTag::Struct) {
      return true;
    }
    uint32_t slotCount = 0;
    return entry.tag == TypeTag::Atom && registeredStructSlotCount(entry.atom.atomId, slotCount);
  }

  /** Whether a struct value carrying `typeId` is of a registered native struct type. */
  bool isNativeStructType(uint32_t typeId) const { return findNativeStruct(typeId) != nullptr; }

  /** The native field getter for a struct value carrying `typeId`, or null for a managed-slot
   * struct. */
  NativeStructFieldGetter nativeStructGetter(uint32_t typeId) const {
    const NativeStructTypeBinding* binding = findNativeStruct(typeId);
    return binding != nullptr ? binding->getter : nullptr;
  }

  /** The native field setter for a struct value carrying `typeId`, or null for a managed-slot
   * struct. */
  NativeStructFieldSetter nativeStructSetter(uint32_t typeId) const {
    const NativeStructTypeBinding* binding = findNativeStruct(typeId);
    return binding != nullptr ? binding->setter : nullptr;
  }

  /** The deep-copy handle snapshot for a struct value carrying `typeId`, or null when the handle
   * copies by reference. */
  NativeStructSnapshot nativeStructSnapshot(uint32_t typeId) const {
    const NativeStructTypeBinding* binding = findNativeStruct(typeId);
    return binding != nullptr ? binding->snapshot : nullptr;
  }

private:
  const NativeStructTypeBinding* findNativeStruct(uint32_t typeId) const {
    for (size_t i = 0; i < nativeStructs_.size(); i++) {
      if (nativeStructs_[i].typeId == typeId) {
        return &nativeStructs_[i];
      }
    }
    return nullptr;
  }

  const ProgramImage* program_;
  Span<const NativeStructTypeBinding> nativeStructs_;
  Span<const RegisteredStructSlotCount> registeredStructs_;
};

} // namespace wendoo
