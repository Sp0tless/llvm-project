[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$ClangRepl,

  [string]$OutputDirectory = (Join-Path $PWD "clang-repl-windows-results"),

  [string]$SourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")),

  [string]$RunLabel = "Experiment",

  [ValidateRange(1, 600)]
  [int]$TimeoutSeconds = 60,

  [switch]$AllowFailures
)

$ErrorActionPreference = "Stop"

$ClangRepl = (Resolve-Path -LiteralPath $ClangRepl).Path
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$TestDirectory = $PSScriptRoot
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$CommonForbiddenPatterns = @(
  "JIT session error:",
  "Failed to materialize symbols:",
  "error: dlupdate failed",
  "PLEASE submit a bug report"
)

$Tests = @(
  [pscustomobject]@{
    Name = "MSVC C++ runtime and cross-PTU behavior"
    File = "coff-msvc-runtime.repl"
    Expected = @(
      "streams=37 tied=1",
      "locale=C",
      "exception=cross-ptu",
      "rtti=42",
      "guarded=77,77 count=1",
      "template=42,20"
    )
    Ordered = $null
    AllowJitErrors = $false
    Kind = "Required"
  },
  [pscustomobject]@{
    Name = "Static and dynamic emulated TLS"
    File = "coff-tls.repl"
    Expected = @(
      "tls-static=2 main=1,2",
      "tls-dynamic=1 count=3",
      "tls-dependency=1101 source=101 count=3"
    )
    Ordered = $null
    AllowJitErrors = $false
    Kind = "Required"
  },
  [pscustomobject]@{
    Name = "Initializer re-entry from a worker thread"
    File = "coff-initializer-reentry.repl"
    Expected = @(
      "initializer-reentry=1",
      "initializer-after=1"
    )
    Ordered = "(?s)initializer-reentry=1.*initializer-after=1"
    AllowJitErrors = $false
    Kind = "Required"
  },
  [pscustomobject]@{
    Name = "COFF atexit LIFO order"
    File = "coff-atexit.repl"
    Expected = @(
      "atexit-registered",
      "atexit-second",
      "atexit-first"
    )
    Ordered = "(?s)atexit-registered.*atexit-second.*atexit-first"
    AllowJitErrors = $false
    Kind = "Required"
  },
  [pscustomobject]@{
    Name = "Win32 imports, dynamic DLL, and undo recovery"
    File = "coff-win32.repl"
    Expected = @(
      "win32-kernel32=1",
      "Symbols not found: [ ClangReplDefinitelyMissingFunction ]",
      "undo-recovered=1",
      "win32-user32=1"
    )
    Ordered = $null
    AllowJitErrors = $true
    Kind = "Required"
  },
  [pscustomobject]@{
    Name = "TLS destructor at worker-thread exit"
    File = "coff-tls-dtor-known-limitation.repl"
    Expected = @("tls-object-value=7")
    Ordered = $null
    AllowJitErrors = $false
    Kind = "KnownTlsDestructorLimitation"
  }
)

function Invoke-ReplTest {
  param(
    [Parameter(Mandatory = $true)]
    [pscustomobject]$Test
  )

  $InputPath = Join-Path $TestDirectory $Test.File
  $TranscriptPath = Join-Path $OutputDirectory ($Test.File + ".log")
  $InputText = [IO.File]::ReadAllText($InputPath)

  $StartInfo = [Diagnostics.ProcessStartInfo]::new()
  $StartInfo.FileName = $ClangRepl
  $StartInfo.Arguments = "-Xcc -fno-delayed-template-parsing"
  $StartInfo.WorkingDirectory = $OutputDirectory
  $StartInfo.UseShellExecute = $false
  $StartInfo.CreateNoWindow = $true
  $StartInfo.RedirectStandardInput = $true
  $StartInfo.RedirectStandardOutput = $true
  $StartInfo.RedirectStandardError = $true
  $StartInfo.Environment["TERM"] = "dumb"

  $Process = [Diagnostics.Process]::new()
  $Process.StartInfo = $StartInfo
  [void]$Process.Start()

  $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
  $StderrTask = $Process.StandardError.ReadToEndAsync()
  $Process.StandardInput.Write($InputText)
  if (-not $InputText.EndsWith("`n")) {
    $Process.StandardInput.WriteLine()
  }
  $Process.StandardInput.Close()

  $TimedOut = -not $Process.WaitForExit($TimeoutSeconds * 1000)
  if ($TimedOut) {
    try {
      $Process.Kill($true)
    } catch {
      $Process.Kill()
    }
    $Process.WaitForExit()
  }

  $Stdout = $StdoutTask.GetAwaiter().GetResult()
  $Stderr = $StderrTask.GetAwaiter().GetResult()
  $ExitCode = $Process.ExitCode
  $Combined = $Stdout + "`n" + $Stderr

  $Transcript = @"
command: $ClangRepl -Xcc -fno-delayed-template-parsing
input: $InputPath
timeout: $TimedOut
exit-code: $ExitCode

=== input ===
$InputText
=== stdout ===
$Stdout
=== stderr ===
$Stderr
"@
  [IO.File]::WriteAllText(
    $TranscriptPath,
    $Transcript,
    [Text.UTF8Encoding]::new($false))

  $Problems = [Collections.Generic.List[string]]::new()
  if ($TimedOut) {
    $Problems.Add("timed out after $TimeoutSeconds seconds")
  } elseif ($ExitCode -ne 0) {
    $Problems.Add("clang-repl exited with code $ExitCode")
  }

  foreach ($Pattern in $Test.Expected) {
    if ($Combined -notmatch [regex]::Escape($Pattern)) {
      $Problems.Add("missing expected output: $Pattern")
    }
  }

  if ($Test.Ordered -and $Combined -notmatch $Test.Ordered) {
    $Problems.Add("expected output appeared in the wrong order")
  }

  if (-not $Test.AllowJitErrors) {
    foreach ($Pattern in $CommonForbiddenPatterns) {
      if ($Combined -match [regex]::Escape($Pattern)) {
        $Problems.Add("unexpected diagnostic: $Pattern")
      }
    }
  }

  $Status = "PASS"
  $Detail = "All required markers observed"

  if ($Problems.Count -eq 0 -and
      $Test.Kind -eq "KnownTlsDestructorLimitation") {
    if ($Combined -match "tls-dtor-count=1") {
      $Detail = "Worker-thread TLS destructor ran"
    } elseif ($Combined -match "tls-dtor-count=0") {
      $Status = "KNOWN LIMITATION"
      $Detail = "Worker-thread TLS destructor did not run"
    } else {
      $Problems.Add("missing TLS destructor count")
    }
  }

  if ($Problems.Count -ne 0) {
    $Status = "FAIL"
    $Detail = $Problems -join "; "
  }

  return [pscustomobject]@{
    Name = $Test.Name
    Status = $Status
    Detail = $Detail
    Transcript = $TranscriptPath
  }
}

$Results = foreach ($Test in $Tests) {
  Write-Host "Running: $($Test.Name)"
  try {
    Invoke-ReplTest -Test $Test
  } catch {
    $HarnessErrorPath = Join-Path $OutputDirectory ($Test.File + ".harness-error.log")
    $HarnessError = $_ | Out-String
    [IO.File]::WriteAllText(
      $HarnessErrorPath,
      $HarnessError,
      [Text.UTF8Encoding]::new($false))
    [pscustomobject]@{
      Name = $Test.Name
      Status = "FAIL"
      Detail = $_.Exception.Message
      Transcript = $HarnessErrorPath
    }
  }
}

$GitMarker = Join-Path $SourceDirectory ".git"
if (Test-Path -LiteralPath $GitMarker) {
  $SourceCommit = git -C $SourceDirectory rev-parse HEAD
  if ($LASTEXITCODE -ne 0) {
    throw "Could not read the source commit from $SourceDirectory"
  }
  $SourceState = if (git -C $SourceDirectory status --porcelain) {
    "$SourceCommit (dirty)"
  } else {
    $SourceCommit
  }
} else {
  $SourceState = "not a source checkout"
}

$Summary = [Collections.Generic.List[string]]::new()
$Summary.Add("## Windows clang-repl COFF: $RunLabel")
$Summary.Add("")
$Summary.Add("- clang-repl: ``$ClangRepl``")
$Summary.Add("- source commit: ``$SourceState``")
$Summary.Add("- timeout per test: $TimeoutSeconds seconds")
$Summary.Add("")
$Summary.Add("| Test | Status | Detail |")
$Summary.Add("|---|---|---|")
foreach ($Result in $Results) {
  $SafeDetail = $Result.Detail.Replace("|", "\|").Replace("`r", " ").Replace("`n", " ")
  $Summary.Add("| $($Result.Name) | $($Result.Status) | $SafeDetail |")
}

$SummaryText = $Summary -join "`n"
Write-Host ""
Write-Host $SummaryText
[IO.File]::WriteAllText(
  (Join-Path $OutputDirectory "summary.md"),
  $SummaryText + "`n",
  [Text.UTF8Encoding]::new($false))

if ($env:GITHUB_STEP_SUMMARY) {
  [IO.File]::AppendAllText(
    $env:GITHUB_STEP_SUMMARY,
    $SummaryText + "`n",
    [Text.UTF8Encoding]::new($false))
}

if (($Results.Status -contains "FAIL") -and -not $AllowFailures) {
  exit 1
}
