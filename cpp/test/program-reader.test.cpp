#include "doctest/doctest.h"

#include "core/runtime/bytecode.h"
#include "core/runtime/core-type-atom-id.h"
#include "core/runtime/load-error.h"
#include "core/runtime/program.h"
#include "wire-builder.h"

#include <cstdint>
#include <vector>

using wendoo::CallSiteBinding;
using wendoo::ConstValueKind;
using wendoo::CoreTypeAtomId;
using wendoo::kNoFuncId;
using wendoo::kNoTypeIdx;
using wendoo::LoadError;
using wendoo::MapKeyKind;
using wendoo::Op;
using wendoo::ProgramImage;
using wendoo::Result;
using wendoo::TypeTag;

TEST_CASE("an empty program decodes with every pool empty") {
  WireBuilder w = programHeader(0, 7);
  emptyRequiredSectionsThroughVars(w);
  w.varUint(0); // PAGE

  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = decodeOk(w, storage);
  CHECK(image.profileId == 7);
  CHECK(image.strings.size() == 0);
  CHECK(image.constantPools.stringCount == 0);
  CHECK(image.types.size() == 0);
  CHECK(image.constNumbers.size() == 0);
  CHECK(image.constValues.size() == 0);
  CHECK(image.functions.size() == 0);
  CHECK(image.variableNames.size() == 0);
  CHECK(!image.hasActions);
  CHECK(!image.hasRuleFuncIds);
  CHECK(!image.hasRuleAncestors);
  CHECK(image.pages.size() == 0);
}

TEST_CASE("strings are borrowed from the wire buffer") {
  WireBuilder w = programHeader();
  w.varUint(2).varUint(1); // CSTR: 2 strings, const prefix 1
  w.str("pool");
  w.str("aux");
  w.varUint(0).varUint(0).varUint(0).varUint(0).varUint(0); // TYPS..VARS
  w.varUint(0);                                             // PAGE

  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = decodeOk(w, storage);
  REQUIRE(image.strings.size() == 2);
  CHECK(image.constantPools.stringCount == 1);
  CHECK(image.stringData.data() == w.span().data());
  REQUIRE(image.strings[0].length == 4);
  CHECK(memcmp(image.stringData.data() + image.strings[0].offset, "pool", 4) == 0);
  REQUIRE(image.strings[1].length == 3);
  CHECK(memcmp(image.stringData.data() + image.strings[1].offset, "aux", 3) == 0);
}

TEST_CASE("every TYPS entry kind decodes and interns its runs") {
  WireBuilder w = programHeader();
  w.varUint(3).varUint(0); // CSTR: three aux strings
  w.str("Point");
  w.str("Red");
  w.str("crimson");
  w.varUint(9);                                                   // TYPS
  w.u8(0).varUint(static_cast<uint32_t>(CoreTypeAtomId::Number)); // 0: atom
  w.u8(1).varUint(0);                                             // 1: list<0>
  w.u8(2).varUint(0).varUint(1);                                  // 2: map<0,1>
  w.u8(3).varUint(2).varUint(0).varUint(2);                       // 3: union(0,2)
  w.u8(4).varUint(1).varUint(0).varUint(3);                       // 4: function(0)->3
  w.u8(5).varUint(0);                                             // 5: nullable<0>
  w.u8(6).varUint(0).varUint(2).varUint(1).varUint(0).varUint(
      0); // 6: struct "Point" slots 2, field "Point"->0
  w.u8(7).varUint(1).varUint(1).u8(0).varUint(1).u8(0).varInt(2); // 7: enum "Red" ["Red" = 2]
  w.u8(7).varUint(2).varUint(1).u8(1).varUint(1).varUint(
      2);                                        // 8: enum "crimson" ["Red" = "crimson"]
  w.varUint(0).varUint(0).varUint(0).varUint(0); // CNUM..VARS
  w.varUint(0);                                  // PAGE

  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = decodeOk(w, storage);
  REQUIRE(image.types.size() == 9);
  CHECK(image.types[0].tag == TypeTag::Atom);
  CHECK(image.types[0].atom.atomId == static_cast<uint32_t>(CoreTypeAtomId::Number));
  CHECK(image.types[1].tag == TypeTag::List);
  CHECK(image.types[1].list.elem == 0);
  CHECK(image.types[2].tag == TypeTag::Map);
  CHECK(image.types[2].map.key == 0);
  CHECK(image.types[2].map.value == 1);
  CHECK(image.types[3].tag == TypeTag::Union);
  CHECK(image.types[4].tag == TypeTag::Function);
  CHECK(image.types[4].function.paramsCount == 1);
  CHECK(image.types[4].function.result == 3);
  CHECK(image.types[5].tag == TypeTag::Nullable);
  CHECK(image.types[5].nullable.base == 0);
  CHECK(image.types[6].tag == TypeTag::Struct);
  CHECK(image.types[6].structOf.nameStringIdx == 0);
  CHECK(image.types[6].structOf.slotCount == 2);
  CHECK(image.types[6].structOf.fieldsCount == 1);
  CHECK(image.structFields[image.types[6].structOf.fieldsOffset].nameStringIdx == 0);
  CHECK(image.structFields[image.types[6].structOf.fieldsOffset].fieldId == 0);
  CHECK(image.types[7].tag == TypeTag::Enum);
  CHECK(image.types[7].enumOf.nameStringIdx == 1);
  CHECK(image.types[7].enumOf.symbolsCount == 1);
  CHECK(!image.types[7].enumOf.stringValued);
  CHECK(image.types[8].tag == TypeTag::Enum);
  CHECK(image.types[8].enumOf.nameStringIdx == 2);
  CHECK(image.types[8].enumOf.symbolsCount == 1);
  CHECK(image.types[8].enumOf.stringValued);

  // The shared type-ref pool holds the union members, the function param,
  // and each enum's symbol-key run followed by its value run, in entry order.
  REQUIRE(image.typeRefs.size() == 7);
  CHECK(image.types[3].unionOf.membersOffset == 0);
  CHECK(image.types[3].unionOf.membersCount == 2);
  CHECK(image.typeRefs[0] == 0);
  CHECK(image.typeRefs[1] == 2);
  CHECK(image.types[4].function.paramsOffset == 2);
  CHECK(image.typeRefs[2] == 0);
  CHECK(image.types[7].enumOf.symbolsOffset == 3);
  CHECK(image.typeRefs[3] == 1);
  CHECK(image.types[7].enumOf.valuesOffset == 4);
  CHECK(image.typeRefs[4] == 0x40000000); // 2.0f bit pattern
  CHECK(image.types[8].enumOf.symbolsOffset == 5);
  CHECK(image.typeRefs[5] == 1);
  CHECK(image.types[8].enumOf.valuesOffset == 6);
  CHECK(image.typeRefs[6] == 2);
}

TEST_CASE("target-range atoms decode through the reader options") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0);                       // CSTR
  w.varUint(2);                                  // TYPS
  w.u8(0).varUint(1024);                         // first target atom
  w.u8(0).varUint(1027);                         // last target atom
  w.varUint(0).varUint(0).varUint(0).varUint(0); // CNUM..VARS
  w.varUint(0);                                  // PAGE

  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = decodeOk(w, storage);
  REQUIRE(image.types.size() == 2);
  CHECK(image.types[0].atom.atomId == 1024);
  CHECK(image.types[1].atom.atomId == 1027);
}

TEST_CASE("CNUM small-int and f32 entries decode at f32 precision") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0); // CSTR
  w.varUint(0);            // TYPS
  w.varUint(3);            // CNUM
  w.u8(0).varInt(-42);
  w.u8(0).varInt(255);
  w.u8(1).f32(1.5f);
  w.varUint(0).varUint(0).varUint(0); // CVAL..VARS
  w.varUint(0);                       // PAGE

  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = decodeOk(w, storage);
  REQUIRE(image.constNumbers.size() == 3);
  CHECK(image.constNumbers[0] == -42.0f);
  CHECK(image.constNumbers[1] == 255.0f);
  CHECK(image.constNumbers[2] == 1.5f);
}

TEST_CASE("nested CVAL containers decode into contiguous child runs") {
  WireBuilder w = programHeader();
  w.varUint(2).varUint(1); // CSTR: pool "k", aux "Color"
  w.str("k");
  w.str("Color");
  w.varUint(4);                                                   // TYPS
  w.u8(0).varUint(static_cast<uint32_t>(CoreTypeAtomId::Number)); // 0: atom Number
  w.u8(1).varUint(0);                                             // 1: list<0>
  w.u8(2).varUint(0).varUint(0);                                  // 2: map<0,0>
  w.u8(7).varUint(1).varUint(2).u8(0);                            // 3: enum, 2 numeric symbols
  w.varUint(0).u8(0).varInt(0);
  w.varUint(0).u8(0).varInt(1);
  w.varUint(0); // CNUM
  w.varUint(5); // CVAL: 5 pool values
  // value 0: list<1> [number 1, number 2]
  w.u8(6).varUint(1).varUint(2);
  w.u8(3).u8(0).varInt(1);
  w.u8(3).u8(0).varInt(2);
  // value 1: map<2> {1: nil, "k": bool true}
  w.u8(7).varUint(2).varUint(2);
  w.u8(0).u8(0).varInt(1);
  w.u8(1);
  w.u8(1).varUint(0);
  w.u8(2).u8(1);
  // value 2: enum (3, 1)
  w.u8(5).varUint(3).varUint(1);
  // value 3: function funcId 9, no captures
  w.u8(11).varUint(9).varUint(0);
  // value 4: function funcId 9, empty capture list
  w.u8(11).varUint(9).varUint(1);
  w.varUint(0).varUint(0); // FUNC, VARS
  w.varUint(0);            // PAGE

  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = decodeOk(w, storage);
  CHECK(image.constantPools.valueCount == 5);
  // 5 pool slots + 2 list items + 2 map entry values.
  REQUIRE(image.constValues.size() == 9);
  REQUIRE(image.constMapEntries.size() == 2);

  const auto& list = image.constValues[0];
  REQUIRE(list.kind == ConstValueKind::List);
  CHECK(list.list.typeIdx == 1);
  REQUIRE(list.list.itemsCount == 2);
  CHECK(image.constValues[list.list.itemsOffset].kind == ConstValueKind::Number);
  CHECK(image.constValues[list.list.itemsOffset].number.value == 1.0f);
  CHECK(image.constValues[list.list.itemsOffset + 1].number.value == 2.0f);

  const auto& map = image.constValues[1];
  REQUIRE(map.kind == ConstValueKind::Map);
  REQUIRE(map.map.entriesCount == 2);
  const auto& numKeyed = image.constMapEntries[map.map.entriesOffset];
  CHECK(numKeyed.keyKind == MapKeyKind::Number);
  CHECK(numKeyed.key.number == 1.0f);
  CHECK(image.constValues[numKeyed.valueIdx].kind == ConstValueKind::Nil);
  const auto& strKeyed = image.constMapEntries[map.map.entriesOffset + 1];
  CHECK(strKeyed.keyKind == MapKeyKind::String);
  CHECK(strKeyed.key.stringIdx == 0);
  CHECK(image.constValues[strKeyed.valueIdx].kind == ConstValueKind::Boolean);
  CHECK(image.constValues[strKeyed.valueIdx].boolean.value);

  const auto& enumVal = image.constValues[2];
  REQUIRE(enumVal.kind == ConstValueKind::Enum);
  CHECK(enumVal.enumVal.typeIdx == 3);
  CHECK(enumVal.enumVal.ordinal == 1);

  // Absent captures and an empty capture list stay distinct.
  const auto& noCaptures = image.constValues[3];
  REQUIRE(noCaptures.kind == ConstValueKind::Function);
  CHECK(noCaptures.function.funcId == 9);
  CHECK(!noCaptures.function.hasCaptures);
  const auto& emptyCaptures = image.constValues[4];
  REQUIRE(emptyCaptures.kind == ConstValueKind::Function);
  CHECK(emptyCaptures.function.hasCaptures);
  CHECK(emptyCaptures.function.capturesCount == 0);
}

TEST_CASE("FUNC bodies normalize locals, ctx, and operand encodings") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0); // CSTR
  w.varUint(1);            // TYPS
  w.u8(0).varUint(static_cast<uint32_t>(CoreTypeAtomId::Context));
  w.varUint(0).varUint(0); // CNUM, CVAL
  w.varUint(2);            // FUNC
  // function 0: 1 param + 2 extra locals, ctx type 0, three instructions.
  w.varUint(1).varUint(2).varUint(1).varUint(3);
  w.u8(static_cast<uint8_t>(Op::JMP)).varInt(-2);
  w.u8(static_cast<uint8_t>(Op::LIST_NEW)).varUint(3).varUint(0); // optional b absent
  w.u8(static_cast<uint8_t>(Op::RET));
  // function 1: no params, no ctx, one typed constructor.
  w.varUint(0).varUint(0).varUint(0).varUint(1);
  w.u8(static_cast<uint8_t>(Op::STRUCT_NEW)).varUint(0).varUint(1); // optional b = 0
  w.varUint(0);                                                     // VARS
  w.varUint(0);                                                     // PAGE

  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = decodeOk(w, storage);
  REQUIRE(image.functions.size() == 2);
  REQUIRE(image.instructions.size() == 4);

  const auto& fn0 = image.functions[0];
  CHECK(fn0.numParams == 1);
  CHECK(fn0.numLocals == 3);
  CHECK(fn0.injectCtxTypeIdx == 0);
  CHECK(fn0.codeOffset == 0);
  REQUIRE(fn0.codeCount == 3);
  CHECK(image.instructions[0].op == Op::JMP);
  CHECK(image.instructions[0].a == static_cast<uint32_t>(-2));
  CHECK(image.instructions[1].op == Op::LIST_NEW);
  CHECK(image.instructions[1].a == 3);
  CHECK(image.instructions[1].b == kNoTypeIdx);
  CHECK(image.instructions[2].op == Op::RET);

  const auto& fn1 = image.functions[1];
  CHECK(fn1.numParams == 0);
  CHECK(fn1.numLocals == 0);
  CHECK(fn1.injectCtxTypeIdx == kNoTypeIdx);
  CHECK(fn1.codeOffset == 3);
  CHECK(image.instructions[3].op == Op::STRUCT_NEW);
  CHECK(image.instructions[3].b == 0);
}

TEST_CASE("optional sections decode per the presence bitmask") {
  WireBuilder p = programHeader(0b111);
  p.varUint(1).varUint(0); // CSTR: one aux string
  p.str("page-a");
  p.varUint(0).varUint(0).varUint(0).varUint(0).varUint(0); // TYPS..VARS
  p.varUint(2);                                             // ACTS
  p.u8(0).varUint(4);
  p.u8(0b101).varUint(5).varUint(6).varUint(7);
  p.varUint(2).varUint(8).varUint(9); // RULF
  p.varUint(1).varUint(8).varUint(9); // RANC
  p.varUint(1);                       // PAGE: one page
  p.varUint(0).varUint(0);            // pageIndex, pageId
  p.varUint(2).varUint(8).varUint(9); // roots
  p.varUint(2);                       // call sites
  p.u8(0).varUint(0).varUint(0x400);  // host
  p.u8(1).varUint(1).varUint(0);      // bytecode

  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = decodeOk(p, storage);
  REQUIRE(image.hasActions);
  REQUIRE(image.actions.size() == 2);
  CHECK(image.actions[0].entryFuncId == 4);
  CHECK(image.actions[0].initializerFuncId == kNoFuncId);
  CHECK(image.actions[0].activationFuncId == kNoFuncId);
  CHECK(image.actions[0].deactivationFuncId == kNoFuncId);
  CHECK(image.actions[1].entryFuncId == 5);
  CHECK(image.actions[1].initializerFuncId == 6);
  CHECK(image.actions[1].activationFuncId == kNoFuncId);
  CHECK(image.actions[1].deactivationFuncId == 7);

  REQUIRE(image.hasRuleFuncIds);
  REQUIRE(image.ruleFuncIds.size() == 2);
  CHECK(image.ruleFuncIds[0] == 8);
  CHECK(image.ruleFuncIds[1] == 9);

  REQUIRE(image.hasRuleAncestors);
  REQUIRE(image.ruleAncestors.size() == 1);
  CHECK(image.ruleAncestors[0].ruleFuncId == 8);
  CHECK(image.ruleAncestors[0].parentRuleFuncId == 9);

  REQUIRE(image.pages.size() == 1);
  const auto& page = image.pages[0];
  CHECK(page.pageIndex == 0);
  CHECK(page.pageIdStringIdx == 0);
  REQUIRE(page.rootRuleFuncIdsCount == 2);
  CHECK(image.rootRuleFuncIds[page.rootRuleFuncIdsOffset] == 8);
  CHECK(image.rootRuleFuncIds[page.rootRuleFuncIdsOffset + 1] == 9);
  REQUIRE(page.callSitesCount == 2);
  const auto& host = image.callSites[page.callSitesOffset];
  CHECK(host.binding == CallSiteBinding::Host);
  CHECK(host.callSiteId == 0);
  CHECK(host.boundId == 0x400);
  const auto& bytecode = image.callSites[page.callSitesOffset + 1];
  CHECK(bytecode.binding == CallSiteBinding::Bytecode);
  CHECK(bytecode.callSiteId == 1);
  CHECK(bytecode.boundId == 0);
}

TEST_CASE("an empty buffer fails Truncated") {
  CHECK(decodeError(WireBuilder()) == LoadError::Truncated);
}

TEST_CASE("a truncated section body fails Truncated") {
  WireBuilder w = programHeader();
  w.varUint(1).varUint(1); // CSTR declares one string, then the buffer ends
  CHECK(decodeError(w) == LoadError::Truncated);

  WireBuilder cut = programHeader();
  cut.varUint(1).varUint(1).varUint(5).u8('a'); // 5 declared bytes, 1 present
  CHECK(decodeError(cut) == LoadError::Truncated);
}

TEST_CASE("bad magic fails InvalidMagic") {
  WireBuilder w;
  w.u8(0x89).u8('M').u8('B').u8('Q').u8(2).varUint(0).u8(0);
  CHECK(decodeError(w) == LoadError::InvalidMagic);

  WireBuilder json;
  json.u8('{').u8('"').u8('p').u8('r');
  CHECK(decodeError(json) == LoadError::InvalidMagic);
}

TEST_CASE("format versions other than 3 fail UnsupportedFormatVersion") {
  for (const uint8_t version : {0, 1, 2, 255}) {
    WireBuilder w;
    w.u8(0x89).u8('M').u8('B').u8('P').u8(version).varUint(0).u8(0);
    emptyRequiredSectionsThroughVars(w);
    w.varUint(0);
    CHECK(decodeError(w) == LoadError::UnsupportedFormatVersion);
  }
}

TEST_CASE("a var-int needing more than 32 bits fails VarIntOverflow") {
  // Six continuation-flagged bytes in the profileId position.
  WireBuilder sixBytes;
  sixBytes.u8(0x89).u8('M').u8('B').u8('P').u8(wendoo::kBinaryProgramFormatVersion);
  for (int i = 0; i < 5; i++) {
    sixBytes.u8(0x80);
  }
  sixBytes.u8(0x01);
  CHECK(decodeError(sixBytes) == LoadError::VarIntOverflow);

  // Five bytes whose 5th carries payload above bit 31.
  WireBuilder highBits;
  highBits.u8(0x89).u8('M').u8('B').u8('P').u8(wendoo::kBinaryProgramFormatVersion);
  highBits.u8(0xff).u8(0xff).u8(0xff).u8(0xff).u8(0x1f);
  CHECK(decodeError(highBits) == LoadError::VarIntOverflow);
}

TEST_CASE("an f64 numeric entry fails F64OnF32Target") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0); // CSTR
  w.varUint(0);            // TYPS
  w.varUint(1);            // CNUM
  w.u8(2);                 // f64 discriminant
  for (int i = 0; i < 8; i++) {
    w.u8(0);
  }
  CHECK(decodeError(w) == LoadError::F64OnF32Target);
}

TEST_CASE("an undeclared CNUM discriminant fails InvalidNumberDiscriminant") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0).varUint(0); // CSTR, TYPS
  w.varUint(1).u8(3);                 // CNUM discriminant 3
  CHECK(decodeError(w) == LoadError::InvalidNumberDiscriminant);
}

TEST_CASE("an undeclared TYPS tag fails InvalidTypeEntryTag") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0); // CSTR
  w.varUint(1).u8(8);      // TYPS tag 8
  CHECK(decodeError(w) == LoadError::InvalidTypeEntryTag);
}

TEST_CASE("a TYPS child at or beyond its parent fails TypeForwardReference") {
  // Entry 0 referencing child 0 references itself.
  WireBuilder self = programHeader();
  self.varUint(0).varUint(0);       // CSTR
  self.varUint(1).u8(1).varUint(0); // list<0> at index 0
  CHECK(decodeError(self) == LoadError::TypeForwardReference);

  WireBuilder forward = programHeader();
  forward.varUint(0).varUint(0); // CSTR
  forward.varUint(2);            // TYPS
  forward.u8(0).varUint(static_cast<uint32_t>(CoreTypeAtomId::Number));
  forward.u8(5).varUint(2); // nullable<2> at index 1
  CHECK(decodeError(forward) == LoadError::TypeForwardReference);
}

TEST_CASE("a TYPS atom outside the core and target ranges fails UnknownTypeAtom") {
  // The target range ends at the next unassigned target atom id.
  const uint32_t pastTargetRange =
      wendoo::TARGET_TYPE_ATOM_BASE + wendoo::kMicroBitV2TypeAtomIdCount;
  for (const uint32_t atomId : {12u, 999u, pastTargetRange, 0xffffffffu}) {
    WireBuilder w = programHeader();
    w.varUint(0).varUint(0);            // CSTR
    w.varUint(1).u8(0).varUint(atomId); // TYPS atom
    CHECK(decodeError(w) == LoadError::UnknownTypeAtom);
  }
}

TEST_CASE("a TYPS enum value-kind byte outside 0/1 fails InvalidEnumValueKind") {
  WireBuilder w = programHeader();
  w.varUint(1).varUint(0); // CSTR: aux enum name
  w.str("Color");
  w.varUint(1);                        // TYPS
  w.u8(7).varUint(0).varUint(1).u8(2); // enum, one symbol, bad value kind
  CHECK(decodeError(w) == LoadError::InvalidEnumValueKind);
}

TEST_CASE("a CVAL enum ordinal outside its symbol list fails EnumOrdinalOutOfRange") {
  WireBuilder w = programHeader();
  w.varUint(1).varUint(0); // CSTR: aux enum name
  w.str("Color");
  w.varUint(1);                        // TYPS
  w.u8(7).varUint(0).varUint(2).u8(0); // enum, 2 numeric symbols
  w.varUint(0).u8(0).varInt(0);
  w.varUint(0).u8(0).varInt(1);
  w.varUint(0);                  // CNUM
  w.varUint(1);                  // CVAL
  w.u8(5).varUint(0).varUint(2); // enum (0, ordinal 2)
  CHECK(decodeError(w) == LoadError::EnumOrdinalOutOfRange);
}

TEST_CASE("a CVAL enum over a structural type entry fails InvalidValueTag") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0); // CSTR
  w.varUint(2);            // TYPS
  w.u8(0).varUint(static_cast<uint32_t>(CoreTypeAtomId::Number));
  w.u8(1).varUint(0);            // list<0>
  w.varUint(0);                  // CNUM
  w.varUint(1);                  // CVAL
  w.u8(5).varUint(1).varUint(0); // enum over the list entry
  CHECK(decodeError(w) == LoadError::InvalidValueTag);
}

TEST_CASE("a CVAL type index outside the table fails TypeIndexOutOfRange") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0);       // CSTR
  w.varUint(0);                  // TYPS
  w.varUint(0);                  // CNUM
  w.varUint(1);                  // CVAL
  w.u8(6).varUint(0).varUint(0); // list with typeIdx 0 into an empty table
  CHECK(decodeError(w) == LoadError::TypeIndexOutOfRange);
}

TEST_CASE("an undeclared CVAL value tag fails InvalidValueTag") {
  // 9 (Any) and 10 (Union) are type tags, never value tags; 13 is undeclared.
  for (const uint8_t tag : {9, 10, 13, 200}) {
    WireBuilder w = programHeader();
    w.varUint(0).varUint(0); // CSTR
    w.varUint(0);            // TYPS
    w.varUint(0);            // CNUM
    w.varUint(1).u8(tag);    // CVAL
    CHECK(decodeError(w) == LoadError::InvalidValueTag);
  }
}

TEST_CASE("a CVAL buffer whose byte count overruns the section fails Truncated") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0); // CSTR
  w.varUint(0);            // TYPS
  w.varUint(0);            // CNUM
  w.varUint(1);            // CVAL
  w.u8(12).varUint(8);     // buffer claiming 8 bytes with none following
  CHECK(decodeError(w) == LoadError::Truncated);
}

TEST_CASE("an undeclared CVAL map key tag fails InvalidValueTag") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0); // CSTR
  w.varUint(2);            // TYPS
  w.u8(0).varUint(static_cast<uint32_t>(CoreTypeAtomId::Number));
  w.u8(2).varUint(0).varUint(0); // map<0,0>
  w.varUint(0);                  // CNUM
  w.varUint(1);                  // CVAL
  w.u8(7).varUint(1).varUint(1); // map with one entry
  w.u8(2);                       // key tag 2
  CHECK(decodeError(w) == LoadError::InvalidValueTag);
}

TEST_CASE("an instruction with an undeclared opcode fails UnknownOpcode") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0);                       // CSTR
  w.varUint(0).varUint(0).varUint(0);            // TYPS, CNUM, CVAL
  w.varUint(1);                                  // FUNC
  w.varUint(0).varUint(0).varUint(0).varUint(1); // one instruction
  w.u8(7);                                       // opcode 7 is undeclared
  CHECK(decodeError(w) == LoadError::UnknownOpcode);
}

TEST_CASE("an undeclared call-site binding tag fails InvalidValueTag") {
  WireBuilder w = programHeader();
  w.varUint(1).varUint(0); // CSTR: page id
  w.str("p");
  w.varUint(0).varUint(0).varUint(0).varUint(0).varUint(0); // TYPS..VARS
  w.varUint(1);                                             // PAGE
  w.varUint(0).varUint(0);                                  // pageIndex, pageId
  w.varUint(0);                                             // roots
  w.varUint(1).u8(2);                                       // binding tag 2
  CHECK(decodeError(w) == LoadError::InvalidValueTag);
}

TEST_CASE("a const-string prefix larger than the table fails InvalidValueTag") {
  WireBuilder w = programHeader();
  w.varUint(1).varUint(2); // CSTR: constStringCount 2 > total 1
  w.str("x");
  CHECK(decodeError(w) == LoadError::InvalidValueTag);
}

TEST_CASE("a string reference outside the table fails InvalidValueTag") {
  WireBuilder w = programHeader();
  w.varUint(0).varUint(0);                       // CSTR
  w.varUint(0).varUint(0).varUint(0).varUint(0); // TYPS..FUNC
  w.varUint(1).varUint(0);                       // VARS: name index 0, empty table
  CHECK(decodeError(w) == LoadError::InvalidValueTag);
}

TEST_CASE("an arena too small for the decoded pools fails ArenaExhausted") {
  WireBuilder w = programHeader();
  w.varUint(2).varUint(1); // CSTR
  w.str("pool");
  w.str("aux");
  w.varUint(0).varUint(0).varUint(0).varUint(0).varUint(0); // TYPS..VARS
  w.varUint(0);                                             // PAGE

  std::vector<uint8_t> storage(4);
  const Result<ProgramImage, LoadError> result = decode(w, storage);
  REQUIRE(!result.isOk());
  CHECK(result.error() == LoadError::ArenaExhausted);
}
