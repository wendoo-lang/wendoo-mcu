#include "core/runtime/managed-heap.h"

#include <cstring>

#include "core/runtime/type-registry.h"

namespace wendoo {
namespace {

/**
 * SameValueZero equality at the profile precision: NaN equals NaN (so NaN is a
 * usable key) and +0 equals -0 (one key). Mirrors the TS `Dict` number-key
 * semantics.
 */
bool sameValueZero(mc_number_t a, mc_number_t b) { return a == b || (a != a && b != b); }

} // namespace

uint32_t SlabAllocator::orderForBytes(size_t bytes) {
  uint32_t order = kMinOrder;
  while ((static_cast<size_t>(1) << order) < bytes && order < 62) {
    order++;
  }
  return order;
}

void* SlabAllocator::allocate(size_t bytes) {
  if (bytes == 0) {
    return nullptr;
  }
  const uint32_t order = orderForBytes(bytes);
  if (order > kMaxOrder) {
    return nullptr;
  }
  if (bins_[order] != nullptr) {
    void* block = bins_[order];
    void* next = nullptr;
    memcpy(&next, block, sizeof(void*));
    bins_[order] = next;
    return block;
  }
  return arena_.allocateBytes(static_cast<size_t>(1) << order, alignof(MapEntry));
}

void SlabAllocator::release(void* block, size_t bytes) {
  if (block == nullptr || bytes == 0) {
    return;
  }
  const uint32_t order = orderForBytes(bytes);
  if (order > kMaxOrder) {
    return;
  }
  memcpy(block, &bins_[order], sizeof(void*));
  bins_[order] = block;
}

ManagedHeap::ManagedHeap(RegionArena& arena, const ProgramImage* program)
    : base_(arena.base()), program_(program), slabs_(arena), lists_(arena), maps_(arena),
      structs_(arena), captures_(arena), strings_(arena), buffers_(arena) {}

bool ManagedHeap::keyStringContent(const MapKey& key, const char*& bytes, uint32_t& length) const {
  if (key.isManagedString) {
    const StringObject* obj = static_cast<const StringObject*>(fromHandle(key.stringRef));
    bytes = obj->bytes;
    length = obj->length;
    return true;
  }
  if (program_ == nullptr || key.stringRef >= program_->strings.size()) {
    return false;
  }
  const StringRef& ref = program_->strings[key.stringRef];
  bytes = reinterpret_cast<const char*>(program_->stringData.data()) + ref.offset;
  length = ref.length;
  return true;
}

bool ManagedHeap::keyEqual(const MapKey& a, const MapKey& b) const {
  if (a.isNumber != b.isNumber) {
    return false;
  }
  if (a.isNumber) {
    return sameValueZero(a.number, b.number);
  }
  // Borrowed keys are content-deduplicated upstream, so equal-content borrowed
  // keys share one index; that fast path also covers the no-program case.
  if (!a.isManagedString && !b.isManagedString) {
    return a.stringRef == b.stringRef;
  }
  // At least one managed key: compare by byte content across representations.
  const char* aBytes = nullptr;
  const char* bBytes = nullptr;
  uint32_t aLen = 0;
  uint32_t bLen = 0;
  if (!keyStringContent(a, aBytes, aLen) || !keyStringContent(b, bBytes, bLen)) {
    return false;
  }
  return aLen == bLen && (aLen == 0 || memcmp(aBytes, bBytes, aLen) == 0);
}

ListObject* ManagedHeap::allocListObject(GcRoots* roots) {
  ListObject* obj = lists_.alloc();
  if (obj == nullptr && roots != nullptr) {
    collect(*roots);
    obj = lists_.alloc();
  }
  return obj;
}

MapObject* ManagedHeap::allocMapObject(GcRoots* roots) {
  MapObject* obj = maps_.alloc();
  if (obj == nullptr && roots != nullptr) {
    collect(*roots);
    obj = maps_.alloc();
  }
  return obj;
}

bool ManagedHeap::newList(uint32_t typeId, GcRoots* roots, Value& out) {
  ListObject* obj = allocListObject(roots);
  if (obj == nullptr) {
    return false;
  }
  obj->items = nullptr;
  obj->size = 0;
  obj->capacity = 0;
  obj->mark = false;
  out = Value::list(typeId, handleOf(obj));
  return true;
}

bool ManagedHeap::newMap(uint32_t typeId, GcRoots* roots, Value& out) {
  MapObject* obj = allocMapObject(roots);
  if (obj == nullptr) {
    return false;
  }
  obj->entries = nullptr;
  obj->size = 0;
  obj->capacity = 0;
  obj->mark = false;
  out = Value::map(typeId, handleOf(obj));
  return true;
}

bool ManagedHeap::newStruct(uint32_t typeId, uint32_t slotCount, GcRoots* roots, Value& out) {
  // Back the slots before drawing the pool object: an orphan StructObject with
  // no valid backing would be swept by a collection the slab allocation could
  // trigger. A raw slab block is never traced or swept, so it survives the
  // pool-allocation collection in a C++ local untouched.
  const size_t bytes = static_cast<size_t>(slotCount) * sizeof(Value);
  Value* slots = nullptr;
  if (slotCount > 0) {
    slots = static_cast<Value*>(slabs_.allocate(bytes));
    if (slots == nullptr && roots != nullptr) {
      collect(*roots);
      slots = static_cast<Value*>(slabs_.allocate(bytes));
    }
    if (slots == nullptr) {
      return false;
    }
  }
  StructObject* obj = structs_.alloc();
  if (obj == nullptr && roots != nullptr) {
    collect(*roots);
    obj = structs_.alloc();
  }
  if (obj == nullptr) {
    if (slots != nullptr) {
      slabs_.release(slots, bytes);
    }
    return false;
  }
  for (uint32_t i = 0; i < slotCount; i++) {
    slots[i] = kNilValue;
  }
  obj->slots = slots;
  obj->slotCount = slotCount;
  obj->mark = false;
  obj->copying = false;
  out = Value::structValue(typeId, handleOf(obj));
  return true;
}

bool ManagedHeap::newCaptures(uint32_t count, GcRoots* roots, uint32_t& out) {
  const size_t bytes = static_cast<size_t>(count) * sizeof(Value);
  Value* slots = nullptr;
  if (count > 0) {
    slots = static_cast<Value*>(slabs_.allocate(bytes));
    if (slots == nullptr && roots != nullptr) {
      collect(*roots);
      slots = static_cast<Value*>(slabs_.allocate(bytes));
    }
    if (slots == nullptr) {
      return false;
    }
  }
  CapturesObject* obj = captures_.alloc();
  if (obj == nullptr && roots != nullptr) {
    collect(*roots);
    obj = captures_.alloc();
  }
  if (obj == nullptr) {
    if (slots != nullptr) {
      slabs_.release(slots, bytes);
    }
    return false;
  }
  for (uint32_t i = 0; i < count; i++) {
    slots[i] = kNilValue;
  }
  obj->slots = slots;
  obj->count = count;
  obj->mark = false;
  out = handleOf(obj);
  return true;
}

bool ManagedHeap::allocString(uint32_t length, GcRoots* roots, Value& out, char*& bytesOut) {
  // Back the bytes before drawing the pool object: a raw slab block is never
  // traced or swept, so it survives a collection the pool allocation triggers
  // (the same orphan-avoidance order as newStruct).
  char* bytes = nullptr;
  if (length > 0) {
    bytes = static_cast<char*>(slabs_.allocate(length));
    if (bytes == nullptr && roots != nullptr) {
      collect(*roots);
      bytes = static_cast<char*>(slabs_.allocate(length));
    }
    if (bytes == nullptr) {
      return false;
    }
  }
  StringObject* obj = strings_.alloc();
  if (obj == nullptr && roots != nullptr) {
    collect(*roots);
    obj = strings_.alloc();
  }
  if (obj == nullptr) {
    if (bytes != nullptr) {
      slabs_.release(bytes, length);
    }
    return false;
  }
  obj->bytes = bytes;
  obj->length = length;
  obj->mark = false;
  out = Value::managedString(handleOf(obj));
  bytesOut = bytes;
  return true;
}

bool ManagedHeap::newString(const char* data, uint32_t length, GcRoots* roots, Value& out) {
  char* bytes = nullptr;
  if (!allocString(length, roots, out, bytes)) {
    return false;
  }
  if (length > 0) {
    memcpy(bytes, data, length);
  }
  return true;
}

StringObject* ManagedHeap::stringObject(const Value& value) const {
  return static_cast<StringObject*>(fromHandle(value.managedStringHandle()));
}

bool ManagedHeap::stringContent(const Value& value, const char*& bytes, uint32_t& length) const {
  if (!value.isString()) {
    return false;
  }
  if (value.isManagedString()) {
    const StringObject* obj = stringObject(value);
    bytes = obj->bytes;
    length = obj->length;
    return true;
  }
  if (program_ == nullptr || value.borrowedStringIndex() >= program_->strings.size()) {
    return false;
  }
  const StringRef& ref = program_->strings[value.borrowedStringIndex()];
  bytes = reinterpret_cast<const char*>(program_->stringData.data()) + ref.offset;
  length = ref.length;
  return true;
}

bool ManagedHeap::allocBuffer(uint32_t length, GcRoots* roots, Value& out, uint8_t*& bytesOut) {
  // A raw slab block is never traced or swept, so back the bytes before
  // drawing the pool object that will own them.
  uint8_t* bytes = nullptr;
  if (length > 0) {
    bytes = static_cast<uint8_t*>(slabs_.allocate(length));
    if (bytes == nullptr && roots != nullptr) {
      collect(*roots);
      bytes = static_cast<uint8_t*>(slabs_.allocate(length));
    }
    if (bytes == nullptr) {
      return false;
    }
  }
  BufferObject* obj = buffers_.alloc();
  if (obj == nullptr && roots != nullptr) {
    collect(*roots);
    obj = buffers_.alloc();
  }
  if (obj == nullptr) {
    if (bytes != nullptr) {
      slabs_.release(bytes, length);
    }
    return false;
  }
  obj->bytes = bytes;
  obj->length = length;
  obj->mark = false;
  out = Value::managedBuffer(handleOf(obj), length);
  bytesOut = bytes;
  return true;
}

bool ManagedHeap::newBuffer(const uint8_t* data, uint32_t length, GcRoots* roots, Value& out) {
  uint8_t* bytes = nullptr;
  if (!allocBuffer(length, roots, out, bytes)) {
    return false;
  }
  if (length > 0) {
    memcpy(bytes, data, length);
  }
  return true;
}

BufferObject* ManagedHeap::bufferObject(const Value& value) const {
  return static_cast<BufferObject*>(fromHandle(value.managedBufferHandle()));
}

bool ManagedHeap::bufferContent(const Value& value, const uint8_t*& bytes, uint32_t& length) const {
  if (!value.isBuffer()) {
    return false;
  }
  if (value.isManagedBuffer()) {
    const BufferObject* obj = bufferObject(value);
    bytes = obj->bytes;
    length = obj->length;
    return true;
  }
  const uint32_t offset = value.bufferOffset();
  const uint32_t count = value.bufferLength();
  if (program_ == nullptr || offset > program_->stringData.size() ||
      count > program_->stringData.size() - offset) {
    return false;
  }
  bytes = program_->stringData.data() + offset;
  length = count;
  return true;
}

bool ManagedHeap::buffersEqual(const Value& a, const Value& b) const {
  if (!a.isBuffer() || !b.isBuffer()) {
    return false;
  }
  const uint8_t* aBytes = nullptr;
  uint32_t aLen = 0;
  const uint8_t* bBytes = nullptr;
  uint32_t bLen = 0;
  if (!bufferContent(a, aBytes, aLen) || !bufferContent(b, bBytes, bLen)) {
    return false;
  }
  if (aLen != bLen) {
    return false;
  }
  return aLen == 0 || memcmp(aBytes, bBytes, aLen) == 0;
}

ListObject* ManagedHeap::list(const Value& value) const {
  return static_cast<ListObject*>(fromHandle(value.containerHandle()));
}

MapObject* ManagedHeap::map(const Value& value) const {
  return static_cast<MapObject*>(fromHandle(value.containerHandle()));
}

StructObject* ManagedHeap::structOf(const Value& value) const {
  // A value of a registered native struct type carries an opaque host token,
  // not a heap handle; there is no StructObject to resolve for it.
  if (types_ != nullptr && types_->isNativeStructType(value.typeId())) {
    return nullptr;
  }
  return static_cast<StructObject*>(fromHandle(value.structHandle()));
}

CapturesObject* ManagedHeap::captures(uint32_t handle) const {
  return static_cast<CapturesObject*>(fromHandle(handle));
}

Value ManagedHeap::structGet(const StructObject* obj, uint32_t fieldId) const {
  return obj != nullptr && fieldId < obj->slotCount ? obj->slots[fieldId] : kNilValue;
}

void ManagedHeap::structSet(StructObject* obj, uint32_t fieldId, const Value& value) {
  if (obj != nullptr && fieldId < obj->slotCount) {
    obj->slots[fieldId] = value;
  }
}

bool ManagedHeap::deepCopyInto(const Value& value, DeepCopyRoots& roots, Value& out) {
  if (!value.isStruct()) {
    // Lists, maps, primitives, enums, functions: copied by reference.
    out = value;
    return true;
  }
  // A native struct value (an injected execution context, a device receiver)
  // is not a heap object and holds no heap handle; it is copied by reference,
  // unless its type registers a snapshot, which materializes the handle.
  if (types_ != nullptr && !types_->isManagedStructType(value.typeId())) {
    const NativeStructSnapshot snapshot = types_->nativeStructSnapshot(value.typeId());
    out = snapshot != nullptr ? snapshot(value) : value;
    return true;
  }
  StructObject* src = structOf(value);
  if (src->copying) {
    // A node already in the current copy chain yields the original (cycle guard).
    out = value;
    return true;
  }
  const uint32_t slotCount = src->slotCount;
  Value copyValue;
  if (!newStruct(value.typeId(), slotCount, &roots, copyValue)) {
    return false;
  }
  // The pool and slabs never relocate live objects, so these stay valid for the
  // recursion: `value` is rooted by the caller and `copyValue` is pinned below.
  StructObject* source = structOf(value);
  StructObject* dest = structOf(copyValue);
  source->copying = true;
  PinNode pin{copyValue, pinHead_};
  pinHead_ = &pin;
  bool ok = true;
  for (uint32_t i = 0; i < slotCount; i++) {
    Value childOut;
    if (!deepCopyInto(source->slots[i], roots, childOut)) {
      ok = false;
      break;
    }
    dest->slots[i] = childOut;
  }
  pinHead_ = pin.next;
  source->copying = false;
  out = copyValue;
  return ok;
}

bool ManagedHeap::deepCopy(const Value& value, GcRoots* roots, Value& out) {
  DeepCopyRoots dcRoots(roots);
  return deepCopyInto(value, dcRoots, out);
}

bool ManagedHeap::listEnsureCapacity(ListObject* obj, uint32_t needed, GcRoots* roots) {
  if (needed <= obj->capacity) {
    return true;
  }
  uint32_t newCap = obj->capacity == 0 ? 2 : obj->capacity;
  while (newCap < needed) {
    newCap *= 2;
  }
  const size_t bytes = static_cast<size_t>(newCap) * sizeof(Value);
  void* block = slabs_.allocate(bytes);
  if (block == nullptr && roots != nullptr) {
    collect(*roots);
    block = slabs_.allocate(bytes);
  }
  if (block == nullptr) {
    return false;
  }
  Value* items = static_cast<Value*>(block);
  for (uint32_t i = 0; i < obj->size; i++) {
    items[i] = obj->items[i];
  }
  if (obj->items != nullptr) {
    slabs_.release(obj->items, static_cast<size_t>(obj->capacity) * sizeof(Value));
  }
  obj->items = items;
  obj->capacity = newCap;
  return true;
}

bool ManagedHeap::listReserve(ListObject* obj, uint32_t capacity, GcRoots* roots) {
  return listEnsureCapacity(obj, capacity, roots);
}

bool ManagedHeap::listPush(ListObject* obj, const Value& item, GcRoots* roots) {
  if (!listEnsureCapacity(obj, obj->size + 1, roots)) {
    return false;
  }
  obj->items[obj->size++] = item;
  return true;
}

Value ManagedHeap::listGet(const ListObject* obj, int32_t index) const {
  if (index < 0 || static_cast<uint32_t>(index) >= obj->size) {
    return kNilValue;
  }
  return obj->items[static_cast<uint32_t>(index)];
}

void ManagedHeap::listSet(ListObject* obj, int32_t index, const Value& value) {
  if (index >= 0 && static_cast<uint32_t>(index) < obj->size) {
    obj->items[static_cast<uint32_t>(index)] = value;
  }
}

Value ManagedHeap::listPop(ListObject* obj) {
  if (obj->size == 0) {
    return kNilValue;
  }
  return obj->items[--obj->size];
}

Value ManagedHeap::listShift(ListObject* obj) {
  if (obj->size == 0) {
    return kNilValue;
  }
  const Value head = obj->items[0];
  for (uint32_t i = 1; i < obj->size; i++) {
    obj->items[i - 1] = obj->items[i];
  }
  obj->size--;
  return head;
}

Value ManagedHeap::listRemove(ListObject* obj, int32_t index) {
  if (index < 0 || static_cast<uint32_t>(index) >= obj->size) {
    return kNilValue;
  }
  const uint32_t i = static_cast<uint32_t>(index);
  const Value removed = obj->items[i];
  for (uint32_t k = i + 1; k < obj->size; k++) {
    obj->items[k - 1] = obj->items[k];
  }
  obj->size--;
  return removed;
}

bool ManagedHeap::listInsert(ListObject* obj, int32_t index, const Value& value, GcRoots* roots) {
  uint32_t i = index < 0 ? 0 : static_cast<uint32_t>(index);
  if (i > obj->size) {
    i = obj->size;
  }
  if (!listEnsureCapacity(obj, obj->size + 1, roots)) {
    return false;
  }
  for (uint32_t k = obj->size; k > i; k--) {
    obj->items[k] = obj->items[k - 1];
  }
  obj->items[i] = value;
  obj->size++;
  return true;
}

void ManagedHeap::listSwap(ListObject* obj, int32_t i, int32_t j) {
  if (i >= 0 && static_cast<uint32_t>(i) < obj->size && j >= 0 &&
      static_cast<uint32_t>(j) < obj->size) {
    const Value tmp = obj->items[static_cast<uint32_t>(i)];
    obj->items[static_cast<uint32_t>(i)] = obj->items[static_cast<uint32_t>(j)];
    obj->items[static_cast<uint32_t>(j)] = tmp;
  }
}

bool ManagedHeap::mapEnsureCapacity(MapObject* obj, uint32_t needed, GcRoots* roots) {
  if (needed <= obj->capacity) {
    return true;
  }
  uint32_t newCap = obj->capacity == 0 ? 2 : obj->capacity;
  while (newCap < needed) {
    newCap *= 2;
  }
  const size_t bytes = static_cast<size_t>(newCap) * sizeof(MapEntry);
  void* block = slabs_.allocate(bytes);
  if (block == nullptr && roots != nullptr) {
    collect(*roots);
    block = slabs_.allocate(bytes);
  }
  if (block == nullptr) {
    return false;
  }
  MapEntry* entries = static_cast<MapEntry*>(block);
  for (uint32_t i = 0; i < obj->size; i++) {
    entries[i] = obj->entries[i];
  }
  if (obj->entries != nullptr) {
    slabs_.release(obj->entries, static_cast<size_t>(obj->capacity) * sizeof(MapEntry));
  }
  obj->entries = entries;
  obj->capacity = newCap;
  return true;
}

uint32_t ManagedHeap::mapFind(const MapObject* obj, const MapKey& key) const {
  for (uint32_t i = 0; i < obj->size; i++) {
    if (keyEqual(obj->entries[i].key, key)) {
      return i;
    }
  }
  return obj->size;
}

bool ManagedHeap::mapSet(MapObject* obj, const MapKey& key, const Value& value, GcRoots* roots) {
  const uint32_t found = mapFind(obj, key);
  if (found < obj->size) {
    obj->entries[found].value = value;
    return true;
  }
  if (!mapEnsureCapacity(obj, obj->size + 1, roots)) {
    return false;
  }
  obj->entries[obj->size].key = key;
  obj->entries[obj->size].value = value;
  obj->size++;
  return true;
}

Value ManagedHeap::mapGet(const MapObject* obj, const MapKey& key) const {
  const uint32_t found = mapFind(obj, key);
  return found < obj->size ? obj->entries[found].value : kNilValue;
}

bool ManagedHeap::mapHas(const MapObject* obj, const MapKey& key) const {
  return mapFind(obj, key) < obj->size;
}

void ManagedHeap::mapDelete(MapObject* obj, const MapKey& key) {
  const uint32_t found = mapFind(obj, key);
  if (found >= obj->size) {
    return;
  }
  for (uint32_t k = found + 1; k < obj->size; k++) {
    obj->entries[k - 1] = obj->entries[k];
  }
  obj->size--;
}

void ManagedHeap::mark(const Value& value) {
  if (value.isList()) {
    ListObject* obj = list(value);
    if (!obj->mark) {
      obj->mark = true;
      for (uint32_t i = 0; i < obj->size; i++) {
        mark(obj->items[i]);
      }
    }
  } else if (value.isManagedString()) {
    // An immutable managed string has no outgoing references; marking the
    // object is enough to keep its byte backing alive through the sweep.
    stringObject(value)->mark = true;
  } else if (value.isManagedBuffer()) {
    // An immutable managed buffer has no outgoing references; marking the
    // object keeps its byte backing alive through the sweep.
    bufferObject(value)->mark = true;
  } else if (value.isMap()) {
    MapObject* obj = map(value);
    if (!obj->mark) {
      obj->mark = true;
      // Number keys carry no heap and borrowed string keys are never collected;
      // managed-string keys are heap objects and must be traced alongside the
      // values.
      for (uint32_t i = 0; i < obj->size; i++) {
        const MapEntry& entry = obj->entries[i];
        if (!entry.key.isNumber && entry.key.isManagedString) {
          static_cast<StringObject*>(fromHandle(entry.key.stringRef))->mark = true;
        }
        mark(entry.value);
      }
    }
  } else if (value.isStruct()) {
    // A native struct value (an injected execution context, a device receiver)
    // is not a heap object and holds no heap handle; tracing must not resolve
    // it.
    if (types_ != nullptr && !types_->isManagedStructType(value.typeId())) {
      return;
    }
    StructObject* obj = structOf(value);
    if (!obj->mark) {
      obj->mark = true;
      for (uint32_t i = 0; i < obj->slotCount; i++) {
        mark(obj->slots[i]);
      }
    }
  } else if (value.isFunction()) {
    const uint32_t handle = value.functionCaptures();
    if (handle != kNoCaptures) {
      CapturesObject* obj = captures(handle);
      if (!obj->mark) {
        obj->mark = true;
        for (uint32_t i = 0; i < obj->count; i++) {
          mark(obj->slots[i]);
        }
      }
    }
  }
}

void ManagedHeap::collect(GcRoots& roots) {
  lists_.forEachLive([](ListObject& obj) { obj.mark = false; });
  maps_.forEachLive([](MapObject& obj) { obj.mark = false; });
  structs_.forEachLive([](StructObject& obj) { obj.mark = false; });
  captures_.forEachLive([](CapturesObject& obj) { obj.mark = false; });
  strings_.forEachLive([](StringObject& obj) { obj.mark = false; });
  buffers_.forEachLive([](BufferObject& obj) { obj.mark = false; });

  roots.enumerateRoots(*this);
  // Pinned in-flight results (deep copies, host-function container builds) are
  // extra roots for the duration of their pin.
  for (PinNode* node = pinHead_; node != nullptr; node = node->next) {
    mark(node->value);
  }

  lists_.forEachLive([this](ListObject& obj) {
    if (!obj.mark) {
      if (obj.items != nullptr) {
        slabs_.release(obj.items, static_cast<size_t>(obj.capacity) * sizeof(Value));
      }
      lists_.free(&obj);
    }
  });
  maps_.forEachLive([this](MapObject& obj) {
    if (!obj.mark) {
      if (obj.entries != nullptr) {
        slabs_.release(obj.entries, static_cast<size_t>(obj.capacity) * sizeof(MapEntry));
      }
      maps_.free(&obj);
    }
  });
  structs_.forEachLive([this](StructObject& obj) {
    if (!obj.mark) {
      if (obj.slots != nullptr) {
        slabs_.release(obj.slots, static_cast<size_t>(obj.slotCount) * sizeof(Value));
      }
      structs_.free(&obj);
    }
  });
  captures_.forEachLive([this](CapturesObject& obj) {
    if (!obj.mark) {
      if (obj.slots != nullptr) {
        slabs_.release(obj.slots, static_cast<size_t>(obj.count) * sizeof(Value));
      }
      captures_.free(&obj);
    }
  });
  strings_.forEachLive([this](StringObject& obj) {
    if (!obj.mark) {
      if (obj.bytes != nullptr) {
        slabs_.release(obj.bytes, obj.length);
      }
      strings_.free(&obj);
    }
  });
  buffers_.forEachLive([this](BufferObject& obj) {
    if (!obj.mark) {
      if (obj.bytes != nullptr) {
        slabs_.release(obj.bytes, obj.length);
      }
      buffers_.free(&obj);
    }
  });
}

} // namespace wendoo
