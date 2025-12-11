# DXGI D3DMetal Fix

A DXGI proxy DLL that fixes display mode enumeration issues with Apple's D3DMetal translation layer on CrossOver/Wine for macOS.

## The Problem

When running DirectX 12 games on macOS via CrossOver (using D3DMetal), some games fail during adapter initialization with errors like:

```
RENDER : Adapter 0 (AMD Compatibility Mode) failed to provide some output
RENDER (E): Couldn't build device list for any adapter.
ENGINE (E): Failed to initialize Enfusion engine
```

**Root Cause:** D3DMetal's `IDXGIOutput::GetDisplayModeList()` returns 0 modes for `DXGI_FORMAT_R16G16B16A16_FLOAT` (format 11 / HDR). Games that validate HDR display mode support reject the adapter as broken.

## Affected Games

- **Arma Reforger** (Enfusion Engine)
- **DayZ** (Enfusion Engine) — likely affected, untested
- Potentially other DX12 games that query HDR display modes

## The Fix

This proxy DLL intercepts DXGI calls and:

1. Wraps `IDXGIOutput` objects returned by D3DMetal
2. Intercepts `GetDisplayModeList()` calls
3. Provides valid display modes when D3DMetal returns none for HDR formats
4. Logs all DXGI activity to `dxgi_hook.log` for debugging

## Installation

1. Download `dxgi.dll` from [Releases](../../releases)

2. Copy to your game folder:
   ```
   ~/Library/Application Support/CrossOver/Bottles/<BOTTLE>/drive_c/Program Files (x86)/Steam/steamapps/common/<GAME>/
   ```
   
   For Arma Reforger:
   ```
   ~/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Arma Reforger/
   ```

3. Launch the game normally

4. Check `dxgi_hook.log` in the game folder for debug output

## Building from Source

### Requirements
- Windows 10/11 with Visual Studio 2022
- For ARM64 machines: Use "x64 Native Tools" or "ARM64_x64 Cross Tools" Command Prompt

### Compile
```cmd
cd src
cl /LD /EHsc /O2 /DUNICODE /D_UNICODE dxgi_hook.cpp /link /DEF:dxgi.def /OUT:dxgi.dll
```

## Current Status

| Stage | Status |
|-------|--------|
| DXGI Enumeration | Fixed |
| Adapter Detection | Working |
| D3D12 Device Creation | Working |
| Swap Chain Creation | Working |
| Rendering | !!! Crashes in D3DMetal |

**Note:** This fix resolves the DXGI enumeration bug, but Arma Reforger still crashes during D3DMetal rendering. This is a separate issue inside Apple's D3DMetal that requires a fix from Apple or CodeWeavers.

## Sample Log Output

```
=== DXGI Hook DLL Loaded ===
CreateDXGIFactory2 called with flags 0
FactoryWrapper::EnumAdapters1 index 0
AdapterWrapper::EnumOutputs called with index 0
Real EnumOutputs returned 0x00000000
Wrapping real output 0000600002BB5C80
FakeOutput::GetDesc called
  DeviceName: \\.\DISPLAY1
  DesktopCoordinates: 0,0 - 1920,1080
  AttachedToDesktop: 1
FakeOutput::GetDisplayModeList format=11 flags=3 pNumModes=0
Real output returned no modes for format 11, providing fake ones
  Returning fake mode count: 7
FakeOutput::GetDisplayModeList format=11 flags=3 pNumModes=7
  Returned 7 fake modes for format 11 (buffer was 7)
```

## Technical Details

### What the Hook Intercepts

| Function | Purpose |
|----------|---------|
| `CreateDXGIFactory` | Wrap factory to intercept adapter enumeration |
| `CreateDXGIFactory1` | Same as above |
| `CreateDXGIFactory2` | Same as above (DX12 path) |
| `IDXGIFactory::EnumAdapters` | Wrap returned adapters |
| `IDXGIAdapter::EnumOutputs` | Wrap returned outputs |
| `IDXGIOutput::GetDisplayModeList` | Fix empty HDR mode lists |
| `IDXGIOutput::GetDesc` | Log and validate output descriptions |

### Display Modes Injected

When D3DMetal returns 0 modes for a format, the hook injects these common modes:

- 3440×1440 @ 180Hz / 60Hz (ultrawide)
- 2560×1440 @ 180Hz / 60Hz (1440p)
- 1920×1080 @ 120Hz / 60Hz (1080p)
- 1280×720 @ 60Hz (720p)

## Environment

Tested on:
- **macOS:** 26.1 (Tahoe)
- **Hardware:** Mac Studio M3 Ultra, 96GB RAM
- **CrossOver:** 25.1 (D3DMetal 2.1)
- **Game:** Arma Reforger (Enfusion Engine v187158)

## Related Links

- [CodeWeavers Forum Post](https://www.codeweavers.com/compatibility/crossover/forum/arma-reforger) — Bug report with full details
