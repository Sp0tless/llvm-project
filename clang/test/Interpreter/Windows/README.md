# Windows clang-repl COFF experiments

These tests are a diagnostic harness for the experimental
`clang-repl-windows-coff` branch. They are not intended to be an upstream test
layout or a conformance suite.

The harness starts a fresh `clang-repl` process for each input, enforces a
per-process timeout, checks stable output markers, and keeps complete stdout
and stderr transcripts. It covers:

- MSVC C++ streams, locale data, exceptions, RTTI, templates, and guarded
  static initialization across incremental PTUs.
- Static and dynamically initialized emulated TLS on multiple threads.
- COFF runtime initializer re-entry from a worker thread.
- LIFO `atexit` callback order.
- Kernel32 imports, dynamic User32 loading, and recovery with `%undo` after an
  expected unresolved-symbol failure.
- The known absence of worker-thread TLS object destruction.

Run the suite from the repository root with a matching ORC runtime installed
in clang's resource directory:

```powershell
./clang/test/Interpreter/Windows/run-experiments.ps1 `
  -ClangRepl C:/path/to/build/bin/clang-repl.exe `
  -SourceDirectory C:/path/to/llvm-project `
  -OutputDirectory C:/path/to/results
```

The script writes a Markdown summary and one transcript per process. On GitHub
Actions, the summary is also written to the workflow run's job summary and the
transcripts are uploaded as an artifact.

The fork-only manual workflow runs this harness against both the experimental
branch and an unmodified upstream baseline. Baseline failures remain visible in
its summary, but do not fail the overall comparison workflow.

The workflow also uploads a separate runnable Release bundle for each matrix
entry. It contains `clang-repl.exe`, the matching Clang resource directory and
ORC runtime, these experiments, launch scripts, build metadata, dependency
information, checksums, and licenses. LLVM and Clang are linked into the
executable, so no project-specific DLLs are required. Windows system libraries
and the MSVC/UCRT redistributable are not copied; use a Windows x64 machine with
the Visual Studio 2022 C++ workload and Windows SDK installed.

The TLS destructor case reports `KNOWN LIMITATION` when the destructor does not
run. This does not fail the workflow, but it remains visible in the summary so
that the experiment does not claim support for that behavior.
