#pragma once

namespace wendoo::test {

/**
 * Absolute path to the wendoo-lang core runtime fixture directory
 * (committed `.mcprogram` / `.mcprogram.bin` artifacts plus their golden
 * dumps). Wired by the test tree's CMake from the repo-relative location of
 * `cpp/`.
 */
inline constexpr const char* kCoreFixturesDir = MC_CORE_FIXTURES_DIR;

/**
 * Absolute path to the wodal microbit-v2 fixture directory (committed
 * `.mcprogram` / `.mcprogram.bin` artifacts plus their golden dumps). Wired by
 * the test tree's CMake from the repo-relative location of `cpp/`.
 */
inline constexpr const char* kWodalFixturesDir = MC_WODAL_FIXTURES_DIR;

/**
 * Absolute path to the shared cross-VM conformance corpus directory in the
 * wendoo-lang submodule (the manifest, the per-precision `.program.bin`
 * binaries, and the committed `.trace` goldens). Wired by the test tree's CMake
 * from the repo-relative location of `cpp/`.
 */
inline constexpr const char* kConformanceCorpusDir = MC_CONFORMANCE_CORPUS_DIR;

} // namespace wendoo::test
