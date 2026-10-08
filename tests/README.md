# Engine integration tests

The standalone harness uses the public `Engine` API and creates disposable CMake
projects under `%TEMP%`. It does not read or write panel settings or existing
projects. Qt is required only by the generated fixtures; the harness has no Qt
dependency. Use Windows, CMake 3.24+, C++23, MSVC, and/or MinGW matching a native
Qt 6.11+ kit. The build-option suites use plain C++23 fixtures and do not
require Qt.

Build once, then run both compiler suites:

```powershell
cmake -S tests -B build/engine-tests -G 'Visual Studio 17 2022' -A x64
cmake --build build/engine-tests --config Release
ctest --test-dir build/engine-tests -C Release --parallel 2 --output-on-failure
```

The defaults are `C:/Qt/6.11.2/msvc2022_64`, `C:/Qt/6.11.2/mingw_64`, and
`C:/Qt/Tools/mingw1310_64/bin`. Override the corresponding
`CMAKEBUILD_TEST_*` CMake cache variables for other installed kits. A suite can
also run directly:

```powershell
.\build\engine-tests\Release\engine_qt_discovery.exe --toolchain msvc
.\build\engine-tests\Release\engine_qt_discovery.exe --toolchain mingw --mingw-bin C:\Qt\Tools\mingw1310_64\bin
.\build\engine-tests\Release\engine_qt_discovery.exe --toolchain mingw --discover-mingw
```

Optional arguments are `--qt-prefix <kit>`, `--cmake <cmake.exe>`, and `--keep`.
Failed fixtures are retained automatically and their location is printed.
`--discover-mingw` removes inherited GCC paths and asks Engine to discover the
compiler itself; `--mingw-bin` then specifies the expected compiler, rather than
adding it to the test process PATH.

Each suite checks a non-Qt build/run; a nested `find_package(Qt6 6.11)` with the
executable outside the build tree; dynamic `find_package(QT NAMES ...)`; and
preservation of explicit `Qt6_DIR`, cached `CMAKE_PREFIX_PATH`, and environment
`CMAKE_PREFIX_PATH`. Explicit paths use a temporary forwarding package backed by
the real installed kit, making unwanted substitution detectable. Every executable
runs through a fresh Engine instance, with inherited Qt package hints and DLL
directories removed from the test process environment. Runtime and compiled Qt
versions must match the expected kit. Cases finish without modifying machine or
user environment settings.

The `engine_build_tests` harness checks the default exclusion of tests with
genuinely invalid C++ sources, opt-in compilation of valid tests, and replacement
of an existing `ON` cache with `OFF` after returning to the default. Its fixtures
cover both standard `BUILD_TESTING` through `include(CTest)` and project-specific
`BUILD_TESTS`, each guarding a separate executable target. A control build with
tests enabled must fail at the deliberate `#error`, proving that the broken test
sources would otherwise stop the build.

An additional fixture reproduces a project-specific `DESIGNER_BUILD_TESTS=ON`
option that ignores both standard switches. Its deliberately broken test must
fail without an explicit override and disappear after passing
`-DDESIGNER_BUILD_TESTS=OFF` through `BuildSettings::cmakeArguments`. Real cache
and generated-file checks cover quoted spaces, Unicode, literal quotes, trailing
backslashes, and shell metacharacters. Fresh Engine instances must reuse unchanged
arguments, reconfigure changed or cleared arguments, and honor explicit `-D`/`-U`
overrides without repeatedly configuring because they differ from the checkbox.
Standalone configure accepts the same arguments without compiling targets. The
harness also rejects CMake operation, generator, and source/build directory
overrides before creating a build directory or launching any process.
This includes mode/help tokens consumed as option values and reserved toolchain
or build-tree cache definitions in joined, separate, and typed `-D` forms. Invalid
quotes, controls, missing values, and oversized strings also fail before side
effects. Explicit `CMAKE_BUILD_TYPE` definitions and unsets cannot replace the
panel configuration field; repeating them still reuses the configured tree.

The MSVC suite additionally holds a real executable image lock using a suspended
test-owned process, changes its source, and verifies the `LNK1168` failure explains
that the running application must be closed. It then releases the lock and
verifies that the same project builds successfully. The fixture process is always
terminated by its owner; existing applications are never stopped by the harness.

Run only these regression suites, without the Qt suites:

```powershell
ctest --test-dir build/engine-tests -C Release -L build-options --parallel 2 --output-on-failure
.\build\engine-tests\Release\engine_build_tests.exe --toolchain msvc
.\build\engine-tests\Release\engine_build_tests.exe --toolchain mingw --mingw-bin C:\Qt\Tools\mingw1310_64\bin
```

The build-option harness accepts `--cmake <cmake.exe>` and `--keep`. Each run
uses a separate temporary directory and a fresh Engine for every build, including
the cache reuse case. It does not write to the panel's settings or existing projects.

The `journal_ui`, `run_ui`, and `native_dpi_ui` harnesses construct the actual FLTK `Panel`,
`SettingsDialog`, buttons, executable menu, and text display. They link the same
UI, state, platform, and Engine sources as the application, with the same pinned static FLTK
and the panel's Windows DPI manifest. CMake obtains the pinned dependency using
`../cmake/fltk.cmake`; `CMAKEBUILD_FLTK_SOURCE_DIR` can point to the matching pinned FLTK
checkout for an offline build. The UI fixtures remain hidden and use their
own UTF-16 INI in `%TEMP%`; they never read or write the user's panel settings.

`journal_ui` checks expanded/collapsed size constraints and remembered height,
real button callbacks, vertical and horizontal scroll preservation, text selection,
resumed autoscroll, resize at the bottom and while reading, wrapped output,
Unicode-safe truncation under large output, and automatic opening after failure.
It also changes light/dark palettes and verifies unchanged geometry, fonts, focus,
text buffer, selection, and viewing position. An offscreen pixel regression
updates a real panel button and Settings input using child damage, verifying
that the changed child redraws while every surrounding pixel is preserved.

`run_ui` inspects the actual FLTK executable menu through a popup presenter hook,
so no interactive popup opens. Only the popup presentation is intercepted: real
button callbacks and the actual widget keyboard handler process Down, F4,
Alt+Down, and Alt+F4. The menu tests cover the selected checkmark, selection
without launch, main-button launch and working directory, target reordering,
missing EXEs, independent choices/build directories for two projects, path aliases,
and restoration after restarting the panel. Synthetic CMake File API replies
include unbuilt executables and nonexecutable artifacts. Cache/reply source
ownership mismatches must not expose another project's targets or cached EXE.
Saved existing EXEs remain available without replies; missing saved EXEs do not.

It also exercises the actual recent-project menu: switching restores complete
project profiles, case/dot aliases do not duplicate history, only the ten newest
projects remain, and a deleted project retains its history entry without replacing
the active project. Complete build settings and launch profiles for two targets
survive restart and legacy INI migration, including arguments longer than an INI
read buffer. The Settings dialog retains separate target drafts, theme changes
preserve edited launch inputs, and cancellation leaves saved launch profiles intact.

Configured launches run a disposable copy of the harness and inspect its actual
Unicode argv, quoted spaces, literal quotes, trailing backslashes, absolute and
relative working directories, and child environment. Overrides must not change
the parent environment or leak into a later default launch. A real compiled fixture
checks the combined Build-and-run menu action and local F5/F6/Ctrl+F5 handlers;
failed and cancelled builds cannot launch an older executable, and subsequent
ordinary builds cannot inherit a pending launch request.

The first two Build menu entries select the persistent main-button mode as well
as execute their action. Actual repeated main-button builds and a restarted panel
prove that combined mode launches the remembered executable exactly once with its
saved launch profile. F5/F6 keep their explicit actions without changing that
preference. Clean, Rebuild and CMake execute while combined mode is selected,
preserve it, and do not automatically launch. Project switches retain this global
preference. Legacy settings default to build-only; only `BuildAndRun=1` enables
combined mode, saves normalize to `0`/`1`, and unknown INI keys remain preserved.

A real build target waits for a test-owned release file while the main Run button
and Ctrl+F5 queue a launch. It checks the first build without an existing EXE or
chosen target, full rebuild, repeated requests producing one acknowledgement and
one actual launch, and delivery of the chosen target's arguments/directory/environment.
The first request disables Run and changes its caption to «Ожидание» without
changing its geometry; combined F5/menu actions enter this state immediately.
Target selection stays disabled while Run waits. Success/failure/cancellation
restore «Запустить», with idle availability determined by the executable inventory.
Failure/cancellation discard the
request; direct callbacks after cancellation cannot restore it, and later ordinary
builds remain build-only. Clean and configure-only reject queued Run requests.

The appended `CMake` action configures a real project without invoking its build
target. Independent configure/build counters prove that unchanged ordinary builds
skip explicit configuration and that a successful manual CMake makes the generated
build reusable. Cache updates, cancellation, recovery and configure failures use
real child processes; project/run menus remain disabled while CMake works, and
saved launch profiles survive each outcome. Configure results have their own CMake
duration in the journal. Offscreen FLTK raster checks compare the complete CMake
duration caption at minimum/wider widths in both themes and 100/125/150/200% scales.

A real hidden FLTK Settings dialog checks preservation of a temporarily missing
launch choice while another setting changes, an explicit selection change, and
Escape cancellation. A legacy INI verifies global Panel-key migration and
preservation of unknown keys and unrelated sections. Unicode paths and labels are
used throughout. Settings also verifies borderless styling, application fonts,
and theme changes retaining edited values, text selection, focus, and geometry.

The lifecycle cases run a real slow CMake configuration, then cancel it through
the Build button. They verify enabled queued Run, disabled selection, and the Cancel/Build
labels. Disposable executable copies launch a test-owned child process; the Run
button must stop both processes, and closing the panel must join Engine and stop
the entire process tree. A second slow CMake configuration starts the owned
parent/child fixture through `execute_process()`; closing the panel while building
must promptly stop both descendants and join Engine. Actual UTF-8 process output must reach the FLTK journal
through the UI event bridge. Fixture executables only create marker/PID files in
the temporary tree and are always owned by the harness.

Build results also verify worker-measured duration, journal output after success,
failure and real cancellation, and formatting at second/minute/hour boundaries.
The build-option suites check that timing belongs only to the single terminal
build event and does not exceed the duration observed by the caller.

Progress tests recognize checked percentage and Ninja count prefixes, ANSI colors,
split CR/LF and UTF-8 reads, duplicate reports and long-line continuations. Configure
and application output must not report build progress; a 100% prefix followed by
a failed process must remain a failure. UI cases check measured/unknown progress,
invalid and regressing reports, terminal reset and Build dropdown keyboard handling.

`engine_rebuild` exercises incremental builds, CMake clean-first, explicit target
selection, standalone cleanup without configuration/rebuild, repeated cleanup,
recovery after cleanup/cancellation, and rejection of missing or foreign caches.
Configuration counters verify explicit configure without compilation, reuse across
Engine restarts, targets and clean-first, recovery from missing generated files,
and retry after failed/cancelled configuration. Ninja Multi-Config switches between
Debug and Release without another explicit configure. Both build-option suites
also verify unbuilt executable discovery and automatic configuration after changing
the build type or test switches.
The fixtures verify preserved source, cache and unrelated build files. `run_ui`
exercises the actual Собрать/Собрать и запустить/Очистить/Пересобрать/CMake dropdown and verifies that the
one-shot full rebuild does not overwrite the saved target or test settings.

```powershell
cmake --build build/engine-tests --config Release --target engine_build_tests engine_rebuild_tests journal_ui run_ui
ctest --test-dir build/engine-tests -C Release -R '^(engine_build_tests_msvc|engine_rebuild|journal_ui|run_ui)$' --parallel 2 --output-on-failure
```

Build the complete UI implementation once, then run both suites:

```powershell
cmake --build build/engine-tests --config Release --target journal_ui run_ui
ctest --test-dir build/engine-tests -C Release -R '^(journal_ui|run_ui)$' --output-on-failure
```

`native_dpi_ui` creates real native HWNDs for the panel and Settings dialog,
blocks activation with a thread-local Windows hook, and immediately applies
native `SW_HIDE`. The HWNDs remain alive for FLTK's Windows message handling;
the test never moves the cursor or injects keyboard/mouse input. Its INI files
are isolated under `%TEMP%`; Windows display settings are never changed.

The suite enumerates actual Windows monitors, measures their DPI using hidden
`GetDpiForWindow` probes, and moves compact/expanded panels and edited Settings
dialogs between the first monitor and each other monitor in both directions three times.
It checks preserved logical sizes and widget geometry, native client dimensions
against FLTK's screen scale, unclipped header/footer/panel controls, delivery of
`WM_DPICHANGED` when native DPI changes, and saved-position restoration on a
secondary monitor. Continuing a native drag after the transition must retain the
corrected rectangle and apply the next message-relative delta to that rectangle.

On two or more monitors it additionally changes only the test process's FLTK
scale on the destination screen, then delivers `WM_DPICHANGED` to the real native
WndProc after `WM_MOVE` has updated the screen number. This reproduces upstream
issue #1439 even when Windows uses equal scales on both displays. The FLTK scale
is restored after all affected test HWNDs are destroyed, including on failure.
Missing additional monitors or different native DPI are reported explicitly as
skipped hardware cases; they are never reported as verified mixed-DPI coverage.

```powershell
cmake --build build/engine-tests --config Release --target native_dpi_ui
ctest --test-dir build/engine-tests -C Release -R '^native_dpi_ui$' --output-on-failure
```

These checks verify actual native monitor transitions on available hardware.
Visual raster quality on other 100/125/150/200% displays remains a separate
hardware check; the offscreen suites verify rendering at those scales.

For an offline inspection of the actual FLTK drawing, the journal harness can
render compact light/dark panels and light/dark Settings windows to P6 RGB PPM
files without showing or activating any native window:

```powershell
.\build\engine-tests\Release\journal_ui.exe --render-preview .\build\engine-tests\ui-preview
```

The renderer uses `Fl_Image_Surface` and the existing widgets' `draw()` methods;
its sample values and settings remain isolated in a disposable temporary folder.

The preview set contains 68 PPM files: light/dark compact minimum-width panels,
button states, expanded journals, and Settings at 100/125/150/200%, plus the four
original unscaled filenames, plus 24 measured and animated progress frames. The
eight `panel-{light,dark}-build-and-run-{100,125,150,200}.ppm` frames additionally
show the persistent combined caption and green mode. Raster checks verify both
split-button halves use `#bcc5ab` for Build and `#c4ffd2` for Build-and-run, dark
readable caption text in both themes, unclipped captions and unchanged geometry.
The progress frames check a half-filled bar, the complete percentage caption and
movement during a real hidden slow CMake configuration. Scaling changes only a scoped graphics-driver value
while creating a high-resolution image surface; OS and monitor settings are untouched.
The journal suite checks the gear's reflected raster symmetry and open center,
compares complete action captions against wider actual-widget reference drawings,
and checks pressed/disabled feedback at each scale. The canvas is initialized to
its real parent background before isolated child drawing, including fractional
origin clipping. Failure diagnostics save a gear PPM under
`build/engine-tests/ui-preview-debug` and print its palette, scale and sampled RGB.

The run suite additionally checks safely elided target names with 5,000 Cyrillic
characters, literal `/`, `\`, `_`, `&`, `@`, full tooltips, correct selection indexes
after expired EXE filtering, and immediate disabling when the final EXE disappears.
Settings tests exercise Enter/KP Enter on Cancel/Close, choices, checkbox and inputs,
Alt+F4 cancellation and trimming of the saved configuration. Nested scroll containers
participate in the actual child-damage regression.
