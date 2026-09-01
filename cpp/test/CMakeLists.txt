# The desktop host test harness. Builds with exceptions enabled (doctest's
# assertion machinery requires them) while linking the exception-free core.
add_executable(wendoo-tests
  main.cpp
  accelerometer-read-parity.test.cpp
  async-handles.test.cpp
  brain-runtime.test.cpp
  buffer-value.test.cpp
  byte-cursor.test.cpp
  bytecode.test.cpp
  conformance.test.cpp
  context-field.test.cpp
  control-flow.test.cpp
  core-func-id.test.cpp
  core-host-actions.test.cpp
  core-host-fn-parity.test.cpp
  core-type-atom-id.test.cpp
  device-port.test.cpp
  device-profile-caps-parity.test.cpp
  display-scroll.test.cpp
  error-code.test.cpp
  fiber-scheduler.test.cpp
  fiber-stress.test.cpp
  fixture-paths.test.cpp
  handle-cap-accounting.test.cpp
  host-loop.test.cpp
  load-error.test.cpp
  managed-heap.test.cpp
  microbit-v2-abi.test.cpp
  numeric.test.cpp
  observable-trace.test.cpp
  on-flash-region.test.cpp
  otherwise-sensor.test.cpp
  pinned-numerics-parity.test.cpp
  pool.test.cpp
  program-dump.test.cpp
  program-reader.test.cpp
  program.test.cpp
  radio-wire.test.cpp
  region-arena.test.cpp
  result.test.cpp
  rule-chain-gate.test.cpp
  rule-completion.test.cpp
  rule-firing-record.test.cpp
  span.test.cpp
  trace-parity.test.cpp
  value.test.cpp
  vm.test.cpp
)

# wendoo-codal links wendoo-core PUBLIC, so the core library and its
# include paths reach the tests transitively. wendoo-hostkit supplies the
# host-only trace and dump emitters the parity tests render against.
target_link_libraries(wendoo-tests PRIVATE wendoo-codal wendoo-hostkit)

target_include_directories(wendoo-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(wendoo-tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

target_compile_options(wendoo-tests PRIVATE -Wall -Wextra -Werror)

# Fixture roots for parity tests, resolved relative to cpp/ inside the repo.
get_filename_component(MC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_compile_definitions(wendoo-tests PRIVATE
  MC_CORE_FIXTURES_DIR="${MC_REPO_ROOT}/external/wendoo-lang/packages/core/src/runtime/__fixtures__"
  MC_WODAL_FIXTURES_DIR="${MC_REPO_ROOT}/packages/wodal/src/targets/microbit-v2/wendoo/__fixtures__"
)

add_test(NAME wendoo-tests COMMAND wendoo-tests)
