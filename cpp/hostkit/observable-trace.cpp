#include "hostkit/observable-trace.h"

#include "core/runtime/buffer-value.h"
#include "core/runtime/managed-heap.h"

namespace wendoo {
namespace {

/** Value token standing in for a value kind the trace format does not render. */
constexpr const char* kOpaqueValueToken = "opaque";

} // namespace

ObservableTraceWriter::ObservableTraceWriter(TextSink& sink, const ProgramImage& program)
    : w_(sink), program_(program) {
  w_.text("mctrace ");
  w_.hex(kObservableTraceFormatVersion);
  w_.nl();
  w_.text("profile ");
  w_.hex(program.profileId);
  w_.nl();
  static_assert(sizeof(mc_number_t) == 4, "the trace renders the build profile's f32 precision");
  w_.text("precision f32");
  w_.nl();
}

void ObservableTraceWriter::tick(uint32_t ordinal, mc_number_t time, mc_number_t dt) {
  w_.text("tick ");
  w_.hex(ordinal);
  w_.text(" time ");
  w_.numberBits(time);
  w_.text(" dt ");
  w_.numberBits(dt);
  w_.nl();
}

void ObservableTraceWriter::hostActionCall(uint32_t actionId, uint32_t callSiteId,
                                           Span<const Value> args, const Value& result) {
  callPrefix("action ", actionId, callSiteId, args);
  w_.text(" result ");
  valueToken(result);
  w_.nl();
}

void ObservableTraceWriter::hostActionCallAsync(uint32_t actionId, uint32_t callSiteId,
                                                Span<const Value> args) {
  callPrefix("action ", actionId, callSiteId, args);
  w_.text(" async");
  w_.nl();
}

void ObservableTraceWriter::bytecodeActionCall(uint32_t actionSlot, uint32_t callSiteId,
                                               Span<const Value> args, const Value& result) {
  callPrefix("tile ", actionSlot, callSiteId, args);
  w_.text(" result ");
  valueToken(result);
  w_.nl();
}

void ObservableTraceWriter::bytecodeActionCallAsync(uint32_t actionSlot, uint32_t callSiteId,
                                                    Span<const Value> args) {
  callPrefix("tile ", actionSlot, callSiteId, args);
  w_.text(" async");
  w_.nl();
}

void ObservableTraceWriter::callPrefix(const char* verb, uint32_t id, uint32_t callSiteId,
                                       Span<const Value> args) {
  w_.text(verb);
  w_.hex(id);
  w_.text(" site ");
  w_.hex(callSiteId);
  w_.text(" args ");
  w_.hex(static_cast<uint32_t>(args.size()));
  for (size_t i = 0; i < args.size(); i++) {
    w_.ch(' ');
    valueToken(args[i]);
  }
}

void ObservableTraceWriter::displaySetPixel(mc_number_t x, mc_number_t y, mc_number_t brightness) {
  w_.text("port display set-pixel ");
  w_.numberBits(x);
  w_.ch(' ');
  w_.numberBits(y);
  w_.ch(' ');
  w_.numberBits(brightness);
  w_.nl();
}

void ObservableTraceWriter::displayScroll(const uint8_t* bytes, uint32_t length) {
  w_.text("port display scroll ");
  quoteBytes(w_, bytes, length);
  w_.nl();
}

void ObservableTraceWriter::displayDraw(uint32_t width, uint32_t height, const uint8_t* frame) {
  w_.text("port display draw ");
  w_.hex(width);
  w_.ch(' ');
  w_.hex(height);
  w_.ch(' ');
  for (uint32_t i = 0; i < width * height; i++) {
    w_.hexDigit(static_cast<uint8_t>(frame[i] >> 4));
    w_.hexDigit(static_cast<uint8_t>(frame[i] & 0xf));
  }
  w_.nl();
}

void ObservableTraceWriter::displayClear() {
  w_.text("port display clear");
  w_.nl();
}

void ObservableTraceWriter::i2cWrite(uint32_t address, const uint8_t* bytes, uint32_t length) {
  w_.text("port i2c write ");
  w_.hex(address);
  w_.ch(' ');
  for (uint32_t i = 0; i < length; i++) {
    w_.hexDigit(static_cast<uint8_t>(bytes[i] >> 4));
    w_.hexDigit(static_cast<uint8_t>(bytes[i] & 0xf));
  }
  w_.nl();
}

void ObservableTraceWriter::i2cRead(uint32_t address, uint32_t length, const uint8_t* bytes,
                                    uint32_t byteCount) {
  w_.text("port i2c read ");
  w_.hex(address);
  w_.ch(' ');
  w_.hex(length);
  w_.ch(' ');
  for (uint32_t i = 0; i < byteCount; i++) {
    w_.hexDigit(static_cast<uint8_t>(bytes[i] >> 4));
    w_.hexDigit(static_cast<uint8_t>(bytes[i] & 0xf));
  }
  w_.nl();
}

void ObservableTraceWriter::gpioDigitalWrite(uint32_t pin, uint32_t value) {
  w_.text("port gpio digital-write ");
  w_.hex(pin);
  w_.ch(' ');
  w_.hex(value);
  w_.nl();
}

void ObservableTraceWriter::gpioDigitalRead(uint32_t pin, uint32_t value) {
  w_.text("port gpio digital-read ");
  w_.hex(pin);
  w_.ch(' ');
  w_.hex(value);
  w_.nl();
}

void ObservableTraceWriter::gpioSetPull(uint32_t pin, uint32_t mode) {
  w_.text("port gpio set-pull ");
  w_.hex(pin);
  w_.ch(' ');
  w_.hex(mode);
  w_.nl();
}

void ObservableTraceWriter::gpioServoWrite(uint32_t pin, uint32_t angle) {
  w_.text("port gpio servo-write ");
  w_.hex(pin);
  w_.ch(' ');
  w_.hex(angle);
  w_.nl();
}

void ObservableTraceWriter::gpioAnalogRead(uint32_t pin, uint32_t value) {
  w_.text("port gpio analog-read ");
  w_.hex(pin);
  w_.ch(' ');
  w_.hex(value);
  w_.nl();
}

void ObservableTraceWriter::sonarDistance(uint32_t trig, uint32_t echo, uint32_t cm) {
  w_.text("port sonar distance ");
  w_.hex(trig);
  w_.ch(' ');
  w_.hex(echo);
  w_.ch(' ');
  w_.hex(cm);
  w_.nl();
}

void ObservableTraceWriter::speakerPlay(const uint8_t* name, uint32_t length) {
  w_.text("port speaker play ");
  quoteBytes(w_, name, length);
  w_.nl();
}

void ObservableTraceWriter::speakerTone(uint32_t waveform, mc_number_t frequencyHz,
                                        uint32_t durationMs, mc_number_t volume) {
  w_.text("port speaker tone ");
  switch (waveform) {
  case 0:
    w_.text("square ");
    break;
  case 1:
    w_.text("sawtooth ");
    break;
  case 2:
    w_.text("sine ");
    break;
  default:
    w_.text("triangle ");
    break;
  }
  w_.numberBits(frequencyHz);
  w_.ch(' ');
  w_.hex(durationMs);
  w_.ch(' ');
  w_.numberBits(volume);
  w_.nl();
}

void ObservableTraceWriter::radioSend(int type, uint32_t group, mc_number_t value,
                                      const uint8_t* name, uint32_t nameLen, const uint8_t* text,
                                      uint32_t textLen, const uint8_t* bytes, uint32_t bytesLen) {
  w_.text("port radio send group ");
  w_.hex(group);
  w_.ch(' ');
  switch (type) {
  case 0: // NUMBER
    w_.text("number ");
    w_.numberBits(value);
    break;
  case 4: // DOUBLE
    w_.text("double ");
    w_.numberBits(value);
    break;
  case 2: // STRING
    w_.text("string ");
    quoteBytes(w_, text, textLen);
    break;
  case 1: // VALUE
    w_.text("value ");
    quoteBytes(w_, name, nameLen);
    w_.text(" number ");
    w_.numberBits(value);
    break;
  case 5: // DOUBLE_VALUE
    w_.text("value ");
    quoteBytes(w_, name, nameLen);
    w_.text(" double ");
    w_.numberBits(value);
    break;
  case 3: // BUFFER
    w_.text("buffer ");
    for (uint32_t i = 0; i < bytesLen; i++) {
      w_.hexDigit(static_cast<uint8_t>(bytes[i] >> 4));
      w_.hexDigit(static_cast<uint8_t>(bytes[i] & 0xf));
    }
    break;
  default: // raw datagram (-1)
    w_.text("raw ");
    for (uint32_t i = 0; i < bytesLen; i++) {
      w_.hexDigit(static_cast<uint8_t>(bytes[i] >> 4));
      w_.hexDigit(static_cast<uint8_t>(bytes[i] & 0xf));
    }
    break;
  }
  w_.nl();
}

void ObservableTraceWriter::fiberFault(uint32_t fiberId, ErrorCode code) {
  w_.text("fault ");
  w_.hex(fiberId);
  w_.ch(' ');
  w_.hex(static_cast<uint32_t>(code));
  w_.nl();
}

void ObservableTraceWriter::valueToken(const Value& value) {
  switch (value.tag()) {
  case ValueTag::Void:
    w_.text("void");
    return;
  case ValueTag::Nil:
    w_.text("nil");
    return;
  case ValueTag::Boolean:
    w_.text(value.asBoolean() ? "bool 1" : "bool 0");
    return;
  case ValueTag::Number:
    w_.text("number ");
    w_.numberBits(value.asNumber());
    return;
  case ValueTag::String: {
    const uint8_t* bytes = nullptr;
    uint32_t length = 0;
    if (value.isManagedString()) {
      const char* chars = nullptr;
      if (heap_ == nullptr || !heap_->stringContent(value, chars, length)) {
        w_.text(kOpaqueValueToken);
        return;
      }
      bytes = reinterpret_cast<const uint8_t*>(chars);
    } else if (!stringTableBytes(program_, value.borrowedStringIndex(), bytes, length)) {
      w_.text(kOpaqueValueToken);
      return;
    }
    w_.text("string ");
    quoteBytes(w_, bytes, length);
    return;
  }
  case ValueTag::Enum: {
    const uint8_t* bytes = nullptr;
    uint32_t length = 0;
    if (!enumSymbolBytes(value, bytes, length)) {
      w_.text(kOpaqueValueToken);
      return;
    }
    w_.text("enum ");
    quoteBytes(w_, bytes, length);
    return;
  }
  case ValueTag::Buffer: {
    const uint8_t* bytes = nullptr;
    uint32_t length = 0;
    if (value.isManagedBuffer()) {
      if (heap_ == nullptr || !heap_->bufferContent(value, bytes, length)) {
        w_.text(kOpaqueValueToken);
        return;
      }
    } else {
      const ByteSpan span = bufferBytes(program_, value);
      bytes = span.data();
      length = span.size();
    }
    w_.text("buffer ");
    for (uint32_t i = 0; i < length; i++) {
      w_.hexDigit(static_cast<uint8_t>(bytes[i] >> 4));
      w_.hexDigit(static_cast<uint8_t>(bytes[i] & 0xf));
    }
    return;
  }
  case ValueTag::Struct: {
    const StructObject* obj = heap_ == nullptr ? nullptr : heap_->structOf(value);
    if (obj == nullptr) {
      w_.text(kOpaqueValueToken);
      return;
    }
    w_.text("struct ");
    w_.hex(obj->slotCount);
    for (uint32_t i = 0; i < obj->slotCount; i++) {
      w_.ch(' ');
      valueToken(heap_->structGet(obj, i));
    }
    return;
  }
  case ValueTag::List: {
    const ListObject* obj = heap_ == nullptr ? nullptr : heap_->list(value);
    if (obj == nullptr) {
      w_.text(kOpaqueValueToken);
      return;
    }
    w_.text("list ");
    w_.hex(obj->size);
    for (uint32_t i = 0; i < obj->size; i++) {
      w_.ch(' ');
      valueToken(heap_->listGet(obj, static_cast<int32_t>(i)));
    }
    return;
  }
  default:
    w_.text(kOpaqueValueToken);
    return;
  }
}

bool ObservableTraceWriter::enumSymbolBytes(const Value& value, const uint8_t*& bytes,
                                            uint32_t& length) const {
  const uint32_t typeIdx = value.typeId();
  if (typeIdx >= program_.types.size() || program_.types[typeIdx].tag != TypeTag::Enum) {
    return false;
  }
  const TypeEntry::EnumOf& entry = program_.types[typeIdx].enumOf;
  const uint32_t ordinal = value.enumOrdinal();
  if (ordinal >= entry.symbolsCount || entry.symbolsOffset + ordinal >= program_.typeRefs.size()) {
    return false;
  }
  return stringTableBytes(program_, program_.typeRefs[entry.symbolsOffset + ordinal], bytes,
                          length);
}

} // namespace wendoo
