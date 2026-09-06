# Building Quanta

Quanta uses the XLang3 public C++ SDK (`xlang3/sdk`) and links to
`xlang3_runtime`. It exports `Load(void*, X3Value)` and registers the `quanta`
module. No legacy XLang host or `Api/value.cpp` is needed.

Use the existing CantorAI workspace build scripts. The root contains sibling
`CantorAIWorkspace`, `xlang3`, `Cantor`, and `Quanta` repositories.

From that root, with Visual Studio 2026 installed:

```powershell
.\CantorAIWorkspace\dev\tools\Build\build_project.ps1 -Root $PWD -BuildType Release -CantorOnly -WithQuanta -Target QuantaSdkTests
```

Add `-WithGalaxy -WithVega` when retaining those components in the focused
workspace configuration. `-Target Quanta` builds only the library and dependencies.

The Windows output remains `out/build/x64-Release/bin/Release/Quanta.dll`.
Deploy it with the matching `xlang3_runtime.dll` and XLang3 SQLite package.

```powershell
ctest --test-dir out/build/x64-Release/components/Quanta -C Release --output-on-failure
```

[Test details and import examples](../test/native/README.md).
Linux/macOS validation is deferred; standalone legacy build scripts are not the
supported entry point for this migration.
