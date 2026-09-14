/**
 * The target-package version this app bundle is published as and the language
 * build it bundles, replaced at build time so the bundle states its own build
 * to the assistant service. Declared by the production app build and undeclared
 * in a dev server and a source run, so read it through a `typeof` guard.
 */
declare const CLIENT_BUILD: import("@wendoo/core").ClientBuild;
