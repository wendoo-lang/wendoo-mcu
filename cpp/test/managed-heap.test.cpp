#include "doctest/doctest.h"

#include "core/runtime/core-func-id.h"
#include "core/runtime/core-host-functions.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/program.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/type-registry.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using wendoo::CapturesObject;
using wendoo::CoreFuncId;
using wendoo::GcMarker;
using wendoo::GcRoots;
using wendoo::HostCallEnv;
using wendoo::isTruthy;
using wendoo::kNoCaptures;
using wendoo::kStringRefIndexMask;
using wendoo::ListObject;
using wendoo::ManagedHeap;
using wendoo::MapKey;
using wendoo::MapObject;
using wendoo::ProgramImage;
using wendoo::RegionArena;
using wendoo::RegisteredStructSlotCount;
using wendoo::Span;
using wendoo::Status;
using wendoo::StringObject;
using wendoo::StringRef;
using wendoo::StructObject;
using wendoo::TypeEntry;
using wendoo::TypeRegistry;
using wendoo::TypeTag;
using wendoo::Value;

namespace {

/** A mutable root set standing in for the VM's live operand-stack slots. */
struct RootSet : GcRoots {
  std::vector<Value> roots;

  void enumerateRoots(GcMarker& marker) override {
    for (const Value& value : roots) {
      marker.mark(value);
    }
  }
};

/** A number-keyed map key. */
MapKey numKey(float n) { return MapKey{true, false, n, 0}; }

/** A borrowed-string map key referencing constant-pool string index `idx`. */
MapKey strKey(uint32_t idx) { return MapKey{false, false, 0.0f, idx}; }

} // namespace

TEST_CASE("a fresh list is empty and lengths track pushes") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value listValue;
  REQUIRE(heap.newList(7, &roots, listValue));
  REQUIRE(listValue.isList());
  CHECK(listValue.typeId() == 7u);
  ListObject* list = heap.list(listValue);
  CHECK(list->size == 0u);

  for (int i = 0; i < 5; i++) {
    REQUIRE(heap.listPush(list, Value::number(static_cast<float>(i)), &roots));
  }
  CHECK(list->size == 5u);
  CHECK(heap.listGet(list, 0).asNumber() == 0.0f);
  CHECK(heap.listGet(list, 4).asNumber() == 4.0f);
}

TEST_CASE("list reads past the ends yield nil and empty pop/shift yield nil") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value listValue;
  REQUIRE(heap.newList(0, &roots, listValue));
  ListObject* list = heap.list(listValue);

  CHECK(heap.listGet(list, 0).isNil());
  CHECK(heap.listPop(list).isNil());
  CHECK(heap.listShift(list).isNil());

  REQUIRE(heap.listPush(list, Value::number(10.0f), &roots));
  CHECK(heap.listGet(list, -1).isNil());
  CHECK(heap.listGet(list, 1).isNil());
  CHECK(heap.listGet(list, 0).asNumber() == 10.0f);
}

TEST_CASE("list set, insert, remove, shift, and swap match the array semantics") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value listValue;
  REQUIRE(heap.newList(0, &roots, listValue));
  ListObject* list = heap.list(listValue);
  for (int i = 0; i < 4; i++) {
    REQUIRE(heap.listPush(list, Value::number(static_cast<float>(i)), &roots)); // [0,1,2,3]
  }

  heap.listSet(list, 1, Value::number(9.0f)); // [0,9,2,3]
  CHECK(heap.listGet(list, 1).asNumber() == 9.0f);

  REQUIRE(heap.listInsert(list, 2, Value::number(8.0f), &roots)); // [0,9,8,2,3]
  CHECK(list->size == 5u);
  CHECK(heap.listGet(list, 2).asNumber() == 8.0f);
  CHECK(heap.listGet(list, 3).asNumber() == 2.0f);

  CHECK(heap.listRemove(list, 0).asNumber() == 0.0f); // [9,8,2,3]
  CHECK(list->size == 4u);
  CHECK(heap.listGet(list, 0).asNumber() == 9.0f);

  CHECK(heap.listShift(list).asNumber() == 9.0f); // [8,2,3]
  CHECK(list->size == 3u);

  heap.listSwap(list, 0, 2); // [3,2,8]
  CHECK(heap.listGet(list, 0).asNumber() == 3.0f);
  CHECK(heap.listGet(list, 2).asNumber() == 8.0f);

  CHECK(heap.listRemove(list, 9).isNil()); // out-of-range remove is a no-op
  CHECK(list->size == 3u);
}

TEST_CASE("a list backing is shared through every reference") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value listValue;
  REQUIRE(heap.newList(0, &roots, listValue));
  const Value alias = listValue; // a copy of the value, same handle

  REQUIRE(heap.listPush(heap.list(listValue), Value::number(1.0f), &roots));
  REQUIRE(heap.listPush(heap.list(alias), Value::number(2.0f), &roots));

  CHECK(heap.list(listValue)->size == 2u);
  CHECK(heap.list(alias)->size == 2u);
  CHECK(heap.listGet(heap.list(alias), 0).asNumber() == 1.0f);
  CHECK(heap.listGet(heap.list(listValue), 1).asNumber() == 2.0f);
}

TEST_CASE("map preserves insertion order semantics with SameValueZero number keys") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;
  Value mapValue;
  REQUIRE(heap.newMap(8, &roots, mapValue));
  MapObject* map = heap.map(mapValue);

  REQUIRE(heap.mapSet(map, numKey(1.0f), Value::number(10.0f), &roots));
  REQUIRE(heap.mapSet(map, numKey(2.0f), Value::number(20.0f), &roots));
  CHECK(map->size == 2u);

  // Updating an existing key keeps its slot, not appends.
  REQUIRE(heap.mapSet(map, numKey(1.0f), Value::number(11.0f), &roots));
  CHECK(map->size == 2u);
  CHECK(heap.mapGet(map, numKey(1.0f)).asNumber() == 11.0f);

  // +0 and -0 are one key; NaN is a usable key equal only to itself.
  REQUIRE(heap.mapSet(map, numKey(0.0f), Value::number(1.0f), &roots));
  REQUIRE(heap.mapSet(map, numKey(-0.0f), Value::number(2.0f), &roots));
  CHECK(heap.mapGet(map, numKey(0.0f)).asNumber() == 2.0f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(heap.mapSet(map, numKey(nan), Value::number(42.0f), &roots));
  CHECK(heap.mapHas(map, numKey(nan)));
  CHECK(heap.mapGet(map, numKey(nan)).asNumber() == 42.0f);

  CHECK(heap.mapHas(map, numKey(2.0f)));
  heap.mapDelete(map, numKey(2.0f));
  CHECK_FALSE(heap.mapHas(map, numKey(2.0f)));
  CHECK(heap.mapGet(map, numKey(2.0f)).isNil());
}

TEST_CASE("map string keys are identified by their constant-pool index") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value mapValue;
  REQUIRE(heap.newMap(0, &roots, mapValue));
  MapObject* map = heap.map(mapValue);

  REQUIRE(heap.mapSet(map, strKey(0), Value::number(1.0f), &roots));
  CHECK(heap.mapHas(map, strKey(0)));
  CHECK(heap.mapGet(map, strKey(0)).asNumber() == 1.0f);

  // The same index is the same key: an update keeps the single entry.
  REQUIRE(heap.mapSet(map, strKey(0), Value::number(2.0f), &roots));
  CHECK(map->size == 1u);
  CHECK(heap.mapGet(map, strKey(0)).asNumber() == 2.0f);

  // A different index is a distinct key, and a number key never matches a string key.
  CHECK_FALSE(heap.mapHas(map, strKey(1)));
  CHECK_FALSE(heap.mapHas(map, numKey(0.0f)));
}

TEST_CASE("container truthiness follows non-emptiness") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;
  const ProgramImage program{};

  Value listValue;
  REQUIRE(heap.newList(0, &roots, listValue));
  CHECK_FALSE(isTruthy(listValue, program, &heap)); // empty list is falsy
  REQUIRE(heap.listPush(heap.list(listValue), Value::number(1.0f), &roots));
  CHECK(isTruthy(listValue, program, &heap)); // non-empty list is truthy

  Value mapValue;
  REQUIRE(heap.newMap(0, &roots, mapValue));
  CHECK_FALSE(isTruthy(mapValue, program, &heap)); // empty map is falsy
  REQUIRE(heap.mapSet(heap.map(mapValue), numKey(1.0f), Value::number(2.0f), &roots));
  CHECK(isTruthy(mapValue, program, &heap)); // non-empty map is truthy
}

TEST_CASE("allocation pressure collects unreachable containers and retries") {
  // A region large enough for only a handful of list objects forces the
  // collect-on-fail path on every allocation past the first few.
  std::vector<uint8_t> storage(2 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;
  roots.roots.push_back(Value::nil()); // slot 0 holds the one active list

  for (int i = 0; i < 500; i++) {
    Value current;
    REQUIRE(heap.newList(0, &roots, current));
    // Root the active list before mutating it, exactly as the VM keeps it on
    // the operand stack; the previous list is now unreachable and reclaimable.
    roots.roots[0] = current;
    REQUIRE(heap.listPush(heap.list(current), Value::number(static_cast<float>(i)), &roots));
  }
  // Only the last list stays rooted; clearing the root makes it collectable.
  CHECK(heap.liveListCount() >= 1u);
  roots.roots.clear();
  heap.collect(roots);
  CHECK(heap.liveListCount() == 0u);
}

TEST_CASE("exhaustion with all roots live faults deterministically") {
  std::vector<uint8_t> storage(2 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  // Keep every list reachable so collection can never reclaim one.
  bool exhausted = false;
  for (int i = 0; i < 100000 && !exhausted; i++) {
    Value listValue;
    if (!heap.newList(0, &roots, listValue)) {
      exhausted = true;
      break;
    }
    roots.roots.push_back(listValue);
  }
  CHECK(exhausted);
  CHECK(heap.liveListCount() == roots.roots.size());
}

TEST_CASE("the collector reclaims unreachable cycles") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value a;
  REQUIRE(heap.newList(0, &roots, a));
  roots.roots.push_back(a);
  Value b;
  REQUIRE(heap.newList(0, &roots, b));
  roots.roots.push_back(b);
  // a -> b -> a, a self-referential cycle, built while both ends are rooted.
  REQUIRE(heap.listPush(heap.list(a), b, &roots));
  REQUIRE(heap.listPush(heap.list(b), a, &roots));

  // Rooting only a keeps the whole cycle reachable.
  roots.roots.clear();
  roots.roots.push_back(a);
  heap.collect(roots);
  CHECK(heap.liveListCount() == 2u);

  // Dropping the root makes the cycle unreachable; tracing reclaims both.
  roots.roots.clear();
  heap.collect(roots);
  CHECK(heap.liveListCount() == 0u);
}

TEST_CASE("struct field slots are nil-initialized and addressed by id") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value structValue;
  REQUIRE(heap.newStruct(3, 3, &roots, structValue));
  REQUIRE(structValue.isStruct());
  CHECK(structValue.typeId() == 3u);
  StructObject* obj = heap.structOf(structValue);
  CHECK(obj->slotCount == 3u);
  for (uint32_t i = 0; i < 3; i++) {
    CHECK(heap.structGet(obj, i).isNil());
  }

  heap.structSet(obj, 0, Value::number(10.0f));
  heap.structSet(obj, 2, Value::number(20.0f));
  CHECK(heap.structGet(obj, 0).asNumber() == 10.0f);
  CHECK(heap.structGet(obj, 1).isNil()); // a retired-id hole stays nil
  CHECK(heap.structGet(obj, 2).asNumber() == 20.0f);

  // Reads and writes outside the slot range are nil / dropped, never UB.
  CHECK(heap.structGet(obj, 3).isNil());
  heap.structSet(obj, 3, Value::number(99.0f));
  CHECK(heap.structGet(obj, 3).isNil());
}

TEST_CASE("a deep copy breaks struct aliasing while sharing list backings") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value original;
  REQUIRE(heap.newStruct(1, 2, &roots, original));
  roots.roots.push_back(original);
  Value sharedList;
  REQUIRE(heap.newList(0, &roots, sharedList));
  roots.roots.push_back(sharedList);
  heap.structSet(heap.structOf(original), 0, Value::number(7.0f));
  heap.structSet(heap.structOf(original), 1, sharedList); // a non-struct field

  Value copy;
  REQUIRE(heap.deepCopy(original, &roots, copy));
  roots.roots.push_back(copy);
  REQUIRE(copy.isStruct());
  CHECK(copy.structHandle() != original.structHandle()); // a fresh struct object

  // Mutating the original's struct field leaves the copy untouched.
  heap.structSet(heap.structOf(original), 0, Value::number(99.0f));
  CHECK(heap.structGet(heap.structOf(copy), 0).asNumber() == 7.0f);

  // The list field is copied by reference, so its backing stays shared.
  CHECK(heap.structGet(heap.structOf(copy), 1).containerHandle() == sharedList.containerHandle());
}

TEST_CASE("a deep copy recurses into nested struct fields") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value child;
  REQUIRE(heap.newStruct(2, 1, &roots, child));
  roots.roots.push_back(child);
  heap.structSet(heap.structOf(child), 0, Value::number(5.0f));
  Value parent;
  REQUIRE(heap.newStruct(1, 1, &roots, parent));
  roots.roots.push_back(parent);
  heap.structSet(heap.structOf(parent), 0, child);

  Value copy;
  REQUIRE(heap.deepCopy(parent, &roots, copy));
  roots.roots.push_back(copy);

  const Value copiedChild = heap.structGet(heap.structOf(copy), 0);
  REQUIRE(copiedChild.isStruct());
  CHECK(copiedChild.structHandle() != child.structHandle()); // the nested struct was copied too

  // Mutating the original nested struct leaves the deep copy untouched.
  heap.structSet(heap.structOf(child), 0, Value::number(42.0f));
  CHECK(heap.structGet(heap.structOf(copiedChild), 0).asNumber() == 5.0f);
}

TEST_CASE("a deep copy passes non-struct values through by reference") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value listValue;
  REQUIRE(heap.newList(0, &roots, listValue));
  Value copy;
  REQUIRE(heap.deepCopy(listValue, &roots, copy));
  CHECK(copy.containerHandle() == listValue.containerHandle()); // same backing

  Value number = Value::number(3.0f);
  REQUIRE(heap.deepCopy(number, &roots, copy));
  CHECK(copy.asNumber() == 3.0f);
}

TEST_CASE("a deep copy passes a native struct through by reference") {
  // A two-atom type table with a registry installed: index 0 (atom 1024) has a
  // registered slot count and is a managed struct type; index 1 (atom 2048)
  // has none and classifies as native, exactly as GC tracing classifies it.
  TypeEntry entries[2] = {};
  entries[0].tag = TypeTag::Atom;
  entries[0].atom.atomId = 1024;
  entries[1].tag = TypeTag::Atom;
  entries[1].atom.atomId = 2048;
  ProgramImage program{};
  program.types = Span<const TypeEntry>(entries, 2);

  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena, &program);
  TypeRegistry types(program);
  const RegisteredStructSlotCount registered[] = {{1024, 2}};
  types.setRegisteredStructSlotCounts(Span<const RegisteredStructSlotCount>(registered, 1));
  heap.setTypes(&types);
  RootSet roots;

  // The native struct's handle is an opaque host token no heap object backs;
  // the copy is the same value, and no heap struct is allocated for it.
  const Value native = Value::structValue(1, 0x1234);
  Value copy;
  REQUIRE(heap.deepCopy(native, &roots, copy));
  CHECK(copy.isStruct());
  CHECK(copy.typeId() == native.typeId());
  CHECK(copy.structHandle() == native.structHandle());
  CHECK(heap.liveStructCount() == 0u);

  // The registered atom struct is a managed heap object; the same call still
  // deep-copies it into a fresh object.
  Value managed;
  REQUIRE(heap.newStruct(0, 2, &roots, managed));
  roots.roots.push_back(managed);
  heap.structSet(heap.structOf(managed), 0, Value::number(7.0f));
  Value managedCopy;
  REQUIRE(heap.deepCopy(managed, &roots, managedCopy));
  REQUIRE(managedCopy.isStruct());
  CHECK(managedCopy.structHandle() != managed.structHandle());
  CHECK(heap.structGet(heap.structOf(managedCopy), 0).asNumber() == 7.0f);
}

TEST_CASE("a deep copy materializes a native struct through its type's snapshot hook") {
  // One-atom type table with no registered slot count: index 0 (atom 2048)
  // classifies as native. Its binding registers a snapshot that resolves the
  // lazy host token 0x1234 to the concrete token 0x5678.
  TypeEntry entries[1] = {};
  entries[0].tag = TypeTag::Atom;
  entries[0].atom.atomId = 2048;
  ProgramImage program{};
  program.types = Span<const TypeEntry>(entries, 1);

  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena, &program);
  TypeRegistry types(program);
  const wendoo::NativeStructTypeBinding bindings[] = {{
      0,
      [](const Value&, uint32_t) { return wendoo::kNilValue; },
      nullptr,
      [](const Value& source) {
        return source.structHandle() == 0x1234 ? Value::structValue(source.typeId(), 0x5678)
                                               : source;
      },
  }};
  types.setNativeStructBindings(Span<const wendoo::NativeStructTypeBinding>(bindings, 1));
  heap.setTypes(&types);
  RootSet roots;

  const Value lazy = Value::structValue(0, 0x1234);
  Value copy;
  REQUIRE(heap.deepCopy(lazy, &roots, copy));
  REQUIRE(copy.isStruct());
  CHECK(copy.typeId() == lazy.typeId());
  CHECK(copy.structHandle() == 0x5678u);
  CHECK(heap.liveStructCount() == 0u);

  // A registered native struct type's value resolves no StructObject, and a
  // slot read through the heap API yields nil.
  CHECK(heap.structOf(lazy) == nullptr);
  CHECK(heap.structGet(heap.structOf(lazy), 0).isNil());

  // A copy of the already-concrete value passes it through unchanged.
  Value again;
  REQUIRE(heap.deepCopy(copy, &roots, again));
  CHECK(again.structHandle() == 0x5678u);
}

TEST_CASE("a deep copy of a self-referential struct terminates") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value cyclic;
  REQUIRE(heap.newStruct(1, 1, &roots, cyclic));
  roots.roots.push_back(cyclic);
  heap.structSet(heap.structOf(cyclic), 0, cyclic); // s.field0 = s

  Value copy;
  REQUIRE(heap.deepCopy(cyclic, &roots, copy));
  roots.roots.push_back(copy);
  CHECK(copy.structHandle() != cyclic.structHandle());
  // The cycle node yields the original, so the copy's self-field points back at
  // the source struct.
  CHECK(heap.structGet(heap.structOf(copy), 0).structHandle() == cyclic.structHandle());
}

TEST_CASE("the collector traces and reclaims structs and closure captures") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  // A closure capturing a struct that itself holds a list: a three-level chain
  // reachable only through the function value's captures handle.
  Value inner;
  REQUIRE(heap.newList(0, &roots, inner));
  roots.roots.push_back(inner);
  REQUIRE(heap.listPush(heap.list(inner), Value::number(8.0f), &roots));
  Value held;
  REQUIRE(heap.newStruct(1, 1, &roots, held));
  roots.roots.push_back(held);
  heap.structSet(heap.structOf(held), 0, inner);
  uint32_t capturesHandle = 0;
  REQUIRE(heap.newCaptures(1, &roots, capturesHandle));
  heap.captures(capturesHandle)->slots[0] = held;
  const Value closure = Value::function(0, capturesHandle);

  // Root only the closure; the struct, its list, and the captures survive.
  roots.roots.clear();
  roots.roots.push_back(closure);
  heap.collect(roots);
  CHECK(heap.liveCaptureCount() == 1u);
  CHECK(heap.liveStructCount() == 1u);
  CHECK(heap.liveListCount() == 1u);
  CHECK(heap.captures(capturesHandle)->slots[0].isStruct());

  // A captureless function value reaches no heap, so dropping the closure
  // reclaims the whole chain.
  roots.roots.clear();
  roots.roots.push_back(Value::function(0, kNoCaptures));
  heap.collect(roots);
  CHECK(heap.liveCaptureCount() == 0u);
  CHECK(heap.liveStructCount() == 0u);
  CHECK(heap.liveListCount() == 0u);
}

TEST_CASE("the collector reclaims unreachable self-referential structs") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value cyclic;
  REQUIRE(heap.newStruct(1, 1, &roots, cyclic));
  roots.roots.push_back(cyclic);
  heap.structSet(heap.structOf(cyclic), 0, cyclic);

  roots.roots.push_back(cyclic);
  heap.collect(roots);
  CHECK(heap.liveStructCount() == 1u); // rooted: stays

  roots.roots.clear();
  heap.collect(roots);
  CHECK(heap.liveStructCount() == 0u); // tracing reclaims the self-cycle
}

TEST_CASE("struct allocation pressure collects unreachable structs and retries") {
  // A region sized for only a few struct objects forces the collect-on-fail
  // path past the first handful.
  std::vector<uint8_t> storage(2 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;
  roots.roots.push_back(Value::nil()); // slot 0 holds the one active struct

  for (int i = 0; i < 500; i++) {
    Value current;
    REQUIRE(heap.newStruct(1, 2, &roots, current));
    roots.roots[0] = current; // the previous struct is now unreachable
    heap.structSet(heap.structOf(current), 0, Value::number(static_cast<float>(i)));
  }
  CHECK(heap.liveStructCount() >= 1u);
  roots.roots.clear();
  heap.collect(roots);
  CHECK(heap.liveStructCount() == 0u);
}

TEST_CASE("nested containers stay reachable through a rooted parent") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value outer;
  REQUIRE(heap.newList(0, &roots, outer));
  roots.roots.push_back(outer);
  Value inner;
  REQUIRE(heap.newList(0, &roots, inner));
  roots.roots.push_back(inner);
  Value innerMap;
  REQUIRE(heap.newMap(0, &roots, innerMap));
  roots.roots.push_back(innerMap);
  REQUIRE(heap.listPush(heap.list(inner), Value::number(7.0f), &roots));
  REQUIRE(heap.mapSet(heap.map(innerMap), numKey(1.0f), Value::number(5.0f), &roots));
  REQUIRE(heap.listPush(heap.list(outer), inner, &roots));
  REQUIRE(heap.listPush(heap.list(outer), innerMap, &roots));

  // Keep only the outer container rooted; the nested ones survive through it.
  roots.roots.clear();
  roots.roots.push_back(outer);
  heap.collect(roots);
  CHECK(heap.liveListCount() == 2u);
  CHECK(heap.liveMapCount() == 1u);
  // The nested values survive and stay intact.
  CHECK(heap.listGet(heap.list(inner), 0).asNumber() == 7.0f);
  CHECK(heap.mapGet(heap.map(innerMap), numKey(1.0f)).asNumber() == 5.0f);
}

namespace {

/** Content of a managed string value as a std::string, for test assertions. */
std::string contentOf(const ManagedHeap& heap, const Value& value) {
  const char* bytes = nullptr;
  uint32_t length = 0;
  REQUIRE(heap.stringContent(value, bytes, length));
  return std::string(bytes, length);
}

} // namespace

TEST_CASE("managed strings hold their content and report a managed reference") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value hello;
  REQUIRE(heap.newString("hello", 5, &roots, hello));
  CHECK(hello.isString());
  CHECK(hello.isManagedString());
  CHECK(heap.liveStringCount() == 1u);
  CHECK(contentOf(heap, hello) == "hello");

  Value empty;
  REQUIRE(heap.newString("", 0, &roots, empty));
  CHECK(empty.isManagedString());
  CHECK(contentOf(heap, empty).empty());
  CHECK(heap.stringObject(empty)->bytes == nullptr);
}

TEST_CASE("collection reclaims unreachable managed strings and keeps reachable ones") {
  std::vector<uint8_t> storage(16 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value kept;
  REQUIRE(heap.newString("kept", 4, &roots, kept));
  Value dropped;
  REQUIRE(heap.newString("dropped", 7, &roots, dropped));
  CHECK(heap.liveStringCount() == 2u);

  // Root only `kept`; `dropped` is unreachable.
  roots.roots.push_back(kept);
  heap.collect(roots);

  CHECK(heap.liveStringCount() == 1u);
  CHECK(contentOf(heap, kept) == "kept");
}

TEST_CASE("a managed string survives collection through a list element") {
  std::vector<uint8_t> storage(16 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value list;
  REQUIRE(heap.newList(6, &roots, list));
  roots.roots.push_back(list);
  Value element;
  REQUIRE(heap.newString("inside", 6, &roots, element));
  REQUIRE(heap.listPush(heap.list(list), element, &roots));

  // `element` is held only by the rooted list; an unreferenced sibling is not.
  Value orphan;
  REQUIRE(heap.newString("orphan", 6, &roots, orphan));
  CHECK(heap.liveStringCount() == 2u);

  heap.collect(roots);

  CHECK(heap.liveStringCount() == 1u);
  CHECK(contentOf(heap, heap.listGet(heap.list(list), 0)) == "inside");
}

TEST_CASE("managed-string map keys are traced and compare by content") {
  std::vector<uint8_t> storage(16 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value map;
  REQUIRE(heap.newMap(7, &roots, map));
  roots.roots.push_back(map);

  Value keyA;
  REQUIRE(heap.newString("name", 4, &roots, keyA));
  const MapKey managedKeyA{false, true, 0.0f, keyA.stringRef() & kStringRefIndexMask};
  REQUIRE(heap.mapSet(heap.map(map), managedKeyA, Value::number(42.0f), &roots));

  // A distinct managed string with equal content is the same key (content, not
  // identity), and the key string is reachable through the map.
  Value keyB;
  REQUIRE(heap.newString("name", 4, &roots, keyB));
  const MapKey managedKeyB{false, true, 0.0f, keyB.stringRef() & kStringRefIndexMask};
  CHECK(heap.mapHas(heap.map(map), managedKeyB));
  CHECK(heap.mapGet(heap.map(map), managedKeyB).asNumber() == 42.0f);

  heap.collect(roots);

  // keyA is traced as the live key; keyB (unrooted) is reclaimed.
  CHECK(heap.liveStringCount() == 1u);
  CHECK(heap.mapGet(heap.map(map), managedKeyA).asNumber() == 42.0f);
  CHECK(contentOf(heap, keyA) == "name");
}

TEST_CASE("string equality and map keys unify borrowed and managed content") {
  // A one-string constant pool so a borrowed reference resolves to "hello".
  const char* pool = "hello";
  ProgramImage program{};
  program.stringData = wendoo::ByteSpan(reinterpret_cast<const uint8_t*>(pool), 5);
  const StringRef refs[] = {{0, 5}};
  program.strings = Span<const StringRef>(refs, 1);

  std::vector<uint8_t> storage(16 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena, &program);
  RootSet roots;

  const Value borrowed = Value::borrowedString(0);
  CHECK_FALSE(borrowed.isManagedString());
  Value managed;
  REQUIRE(heap.newString("hello", 5, &roots, managed));

  // == operator: borrowed and managed with equal content compare equal.
  const HostCallEnv env{&heap, &roots, nullptr};
  const Value eqArgs[] = {borrowed, managed};
  Value eq;
  REQUIRE(callCoreHostFunction(CoreFuncId::OpEqualToString, Span<const Value>(eqArgs, 2), env, eq)
              .isOk());
  CHECK(eq.asBoolean());

  // Map keys: insert under the borrowed key, look up under the managed key.
  Value map;
  REQUIRE(heap.newMap(7, &roots, map));
  const MapKey borrowedKey{false, false, 0.0f, 0};
  REQUIRE(heap.mapSet(heap.map(map), borrowedKey, Value::number(9.0f), &roots));
  const MapKey managedKey{false, true, 0.0f, managed.stringRef() & kStringRefIndexMask};
  CHECK(heap.mapHas(heap.map(map), managedKey));
  CHECK(heap.mapGet(heap.map(map), managedKey).asNumber() == 9.0f);
}

TEST_CASE("a pinned value survives collection through the pin and is reclaimed once released") {
  std::vector<uint8_t> storage(16 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots; // deliberately empty: the list is reachable only through the pin

  Value list;
  REQUIRE(heap.newList(6, &roots, list));
  {
    ManagedHeap::Pin pin(heap, list);
    // An unrooted, unpinned orphan the collection must reclaim, which confirms
    // the collection actually fired.
    Value orphan;
    REQUIRE(heap.newString("orphan", 6, &roots, orphan));
    CHECK(heap.liveListCount() == 1u);
    CHECK(heap.liveStringCount() == 1u);

    heap.collect(roots);

    // The list survived a collection with no external root, by the pin alone;
    // the orphan did not. This is the invariant the list-building host functions
    // rely on while allocating their elements.
    CHECK(heap.liveListCount() == 1u);
    CHECK(heap.liveStringCount() == 0u);
    REQUIRE(heap.listPush(heap.list(list), Value::number(5.0f), &roots));
    CHECK(heap.list(list)->size == 1u);
  }
  // The pin is released; nothing roots the list now.
  heap.collect(roots);
  CHECK(heap.liveListCount() == 0u);
}

namespace {

/** Content of a buffer value as a byte string, for test assertions. */
std::string bufferContentOf(const ManagedHeap& heap, const Value& value) {
  const uint8_t* bytes = nullptr;
  uint32_t length = 0;
  REQUIRE(heap.bufferContent(value, bytes, length));
  return std::string(reinterpret_cast<const char*>(bytes), length);
}

} // namespace

TEST_CASE("managed buffers hold their content and report a managed reference") {
  std::vector<uint8_t> storage(8 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  const uint8_t data[] = {0x00, 0x7f, 0x80, 0xff};
  Value buf;
  REQUIRE(heap.newBuffer(data, 4, &roots, buf));
  CHECK(buf.isManagedBuffer());
  CHECK(buf.bufferLength() == 4u);
  CHECK(heap.liveBufferCount() == 1u);
  CHECK(bufferContentOf(heap, buf) == std::string(reinterpret_cast<const char*>(data), 4));

  Value empty;
  REQUIRE(heap.newBuffer(nullptr, 0, &roots, empty));
  CHECK(empty.isManagedBuffer());
  CHECK(empty.bufferLength() == 0u);
  CHECK(bufferContentOf(heap, empty).empty());
  CHECK(heap.bufferObject(empty)->bytes == nullptr);
}

TEST_CASE("collection reclaims unreachable managed buffers and keeps reachable ones") {
  std::vector<uint8_t> storage(16 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  const uint8_t keptData[] = {1, 2, 3};
  const uint8_t droppedData[] = {4, 5};
  Value kept;
  REQUIRE(heap.newBuffer(keptData, 3, &roots, kept));
  Value dropped;
  REQUIRE(heap.newBuffer(droppedData, 2, &roots, dropped));
  CHECK(heap.liveBufferCount() == 2u);

  // Root only `kept`; `dropped` is unreachable.
  roots.roots.push_back(kept);
  heap.collect(roots);

  CHECK(heap.liveBufferCount() == 1u);
  CHECK(bufferContentOf(heap, kept) == std::string(reinterpret_cast<const char*>(keptData), 3));
}

TEST_CASE("a managed buffer survives collection through a list element") {
  std::vector<uint8_t> storage(16 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena);
  RootSet roots;

  Value list;
  REQUIRE(heap.newList(6, &roots, list));
  roots.roots.push_back(list);
  const uint8_t insideData[] = {7, 8, 9};
  Value element;
  REQUIRE(heap.newBuffer(insideData, 3, &roots, element));
  REQUIRE(heap.listPush(heap.list(list), element, &roots));

  // `element` is held only by the rooted list; an unreferenced sibling is not.
  const uint8_t orphanData[] = {0xaa};
  Value orphan;
  REQUIRE(heap.newBuffer(orphanData, 1, &roots, orphan));
  CHECK(heap.liveBufferCount() == 2u);

  heap.collect(roots);

  CHECK(heap.liveBufferCount() == 1u);
  CHECK(bufferContentOf(heap, heap.listGet(heap.list(list), 0)) ==
        std::string(reinterpret_cast<const char*>(insideData), 3));
}

TEST_CASE("buffer content-equality holds across borrowed and managed references") {
  const uint8_t pool[] = {0x10, 0x20, 0x30};
  ProgramImage program{};
  program.stringData = wendoo::ByteSpan(pool, 3);

  std::vector<uint8_t> storage(16 * 1024);
  RegionArena arena(Span<uint8_t>(storage.data(), storage.size()));
  ManagedHeap heap(arena, &program);
  RootSet roots;

  const Value borrowed = Value::borrowedBuffer(0, 3);
  CHECK_FALSE(borrowed.isManagedBuffer());
  Value managed;
  REQUIRE(heap.newBuffer(pool, 3, &roots, managed));
  CHECK(managed.isManagedBuffer());

  // Borrowed and managed buffers with equal content compare equal, both orders.
  CHECK(heap.buffersEqual(borrowed, managed));
  CHECK(heap.buffersEqual(managed, borrowed));

  const uint8_t otherData[] = {0x10, 0x20, 0x31};
  Value other;
  REQUIRE(heap.newBuffer(otherData, 3, &roots, other));
  CHECK_FALSE(heap.buffersEqual(borrowed, other));
}
