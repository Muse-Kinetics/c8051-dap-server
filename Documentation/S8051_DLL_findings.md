# S8051.DLL Findings

## Summary

`S8051.DLL` is the Keil/uVision-facing debugger entry point, but it does not appear to be the low-level Silicon Labs debug engine itself.

## What We Verified

- `S8051.DLL` exports only one public symbol: `BootDll`.
- Keil/uVision loads `S8051.DLL` successfully through our proxy.
- The proxy can forward `BootDll` to the original DLL without breaking simulator stepping or breakpoints.
- `S8051.DLL` imports `LoadLibraryA`, `GetProcAddress`, and `FreeLibrary`, which strongly suggests runtime loading of additional components.

## BootDll Behavior

Static disassembly shows that `BootDll` is a selector-based dispatcher.

Observed selectors during simulator startup:

- `1` -> returns `1`
- `2` -> returns `1`
- `3` -> returns `0`
- `4` -> returns `0`
- `411` -> returns `0`
- `41` -> returns `0`

This suggests:

- selectors `1` and `2` are handshake or capability-style calls
- selectors `3`, `4`, `41`, and `411` are successful command/setup calls
- `BootDll` is part of startup/configuration, not the live step/run/breakpoint path

## Selector 2

Selector `2` writes a pointer through its second argument and returns `1`.

Observed output:

- output pointer: `00E50840`
- returned pointer: `1847BC40`
- first 8 dwords at that pointer:

```text
00000B30,00000000,00000000,00000002,00000000,0003F801,00000000,00000000
```

This does not look like a direct function-pointer table. It looks more like a descriptor or metadata block.

## Selector 3 Context

For selector `3`, we inspected selected offsets before and after the call.

Important result:

- `context + 0x410` already contains the string:

```text
C:\Keil_v5\C51\BIN\SiC8051F.dll
```

This indicates the context passed into `BootDll` already tells `S8051.DLL` which backend DLL to use.

Offsets checked:

- `+0x208` remained unchanged
- `+0x410` contained the backend DLL path before and after the call
- `+0x19C3` remained unchanged

## Practical Interpretation

- `S8051.DLL` is a front-end dispatcher used by Keil/uVision.
- `SiC8051F.dll` is likely the actual Silicon Labs backend implementing debug operations.
- The best next reverse-engineering target is `SiC8051F.dll`, not deeper instrumentation of `BootDll`.

## Stable Proxy Result

The current stable `S8051.DLL` proxy can:

- log selector usage
- log return values
- log selector `2` descriptor output
- run the Keil simulator without breaking normal stepping and breakpoints

## Next Step

Build a conservative proxy for `SiC8051F.dll` and observe which `AG_*` exports are used during simulator startup and debugging.