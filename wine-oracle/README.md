# Wine oracle

Small Windows programs that check the behaviour of the Windows functions we implement in our
Wine/Proton build for FiveM (branch `fivem` of ValveSoftware/wine `experimental_11.0`).

Each test prints what it got next to what it expects (`attendu`) and ends with `RESULT: PASS` or
`RESULT: FAIL`. Under Wine they are built with `winegcc`; the
[`wine-oracle` workflow](../.github/workflows/wine-oracle.yml) builds them with MSVC and runs them
on a real Windows (GitHub's `windows-2022` image), to confirm that the expected values are the ones
Windows gives.

| Test | Checks |
|---|---|
| `cycletime.c` | `QueryThreadCycleTime` / `QueryProcessCycleTime` follow the TSC for a busy thread |
| `bgmode.c` | `SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN/END)` results and errors, `SetThreadInformation(ThreadMemoryPriority)` |
| `sdrwhite.c` | `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)` |
| `spcdecode.c` | `CryptDecodeObjectEx(SPC_INDIRECT_DATA_OBJID)` through wintrust |
