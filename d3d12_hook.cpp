// d3d12_hook.cpp - D3D12 Proxy DLL for Arma Reforger on CrossOver/macOS
//
// Problem:  D3DMetal does not support reserved/tiled resources.
//           "TiledResourceBarrier" in D3DMetal logs = D3D12_RESOURCE_BARRIER_TYPE_ALIASING
//           being silently dropped. Reserved resources have broken tile mappings/copies.
//           165+ environment textures use reserved resources → broken rendering.
//           Characters use committed resources → render fine.
//
// Fix:
//   1. CreateReservedResource  → CreateCommittedResource (pre-allocate all memory)
//   2. CreateReservedResource1 → CreateCommittedResource (same)
//   3. UpdateTileMappings      → no-op  (committed resources need no tile mapping)
//   4. CopyTileMappings        → no-op
//   5. ResourceBarrier         → filter D3D12_RESOURCE_BARRIER_TYPE_ALIASING entries
//   6. CopyTiles               → translate to CopyTextureRegion / CopyBufferRegion
//   7. CreateCommandList/1     → return wrapped command list
//   8. CreateCommandQueue      → return wrapped command queue (for UpdateTileMappings)
//
// Build on macOS with mingw-w64:
//   brew install mingw-w64
//   git clone --depth=1 https://github.com/microsoft/DirectX-Headers
//
//   x86_64-w64-mingw32-g++          \
//     -shared -O2 -std=c++17        \
//     -fms-extensions               \
//     -I DirectX-Headers/include/directx \
//     -I DirectX-Headers/include    \
//     -o d3d12.dll                  \
//     d3d12_hook.cpp d3d12.def      \
//     -lkernel32 -lole32 -luuid     \
//     -Wl,--kill-at                 \
//     -Wl,--enable-stdcall-fixup
//
// Deploy: place d3d12.dll next to ArmaReforger.exe (same dir as dxgi.dll)

#define WIN32_LEAN_AND_MEAN
#define INITGUID

#include <windows.h>
#include <d3d12.h>

// mingw-w64 defines __uuidof(T) as __mingw_uuidof<T>() but never specialises that
// template for D3D12 interfaces (DirectX-Headers uses MIDL_INTERFACE which expands
// to plain `struct` under mingw, so no __declspec(uuid(...)) is emitted).
// Provide the missing specialisations manually using the GUIDs from d3d12.idl.
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D12Object,               0xc4fec28f,0x7966,0x4e95,0x9f,0x94,0xf4,0x31,0xcb,0x56,0xc3,0xb8)
__CRT_UUID_DECL(ID3D12DeviceChild,          0x905db94b,0xa00c,0x4140,0x9d,0xf5,0x2b,0x64,0xca,0x9e,0xa3,0x57)
__CRT_UUID_DECL(ID3D12Pageable,             0x63ee58fb,0x1268,0x4835,0x86,0xda,0xf0,0x08,0xce,0x62,0xf0,0xd6)
__CRT_UUID_DECL(ID3D12CommandList,          0x7116d91c,0xe7e4,0x47ce,0xb8,0xc6,0xec,0x81,0x68,0xf4,0x37,0xe5)
__CRT_UUID_DECL(ID3D12CommandQueue,         0x0ec870a6,0x5d7e,0x4c22,0x8c,0xfc,0x5b,0xaa,0xe0,0x76,0x16,0xed)
__CRT_UUID_DECL(ID3D12GraphicsCommandList,  0x5b160d0f,0xac1b,0x4185,0x8b,0xa8,0xb3,0xae,0x42,0xa5,0xa4,0x55)
__CRT_UUID_DECL(ID3D12GraphicsCommandList1, 0x553103fb,0x1fe7,0x4557,0xbb,0x38,0x94,0x6d,0x7d,0x0e,0x7c,0xa7)
__CRT_UUID_DECL(ID3D12GraphicsCommandList2, 0x38C3E585,0xFF17,0x412C,0x91,0x50,0x4F,0xC6,0xF9,0xD7,0x2A,0x28)
__CRT_UUID_DECL(ID3D12GraphicsCommandList3, 0x6FDA83A7,0xB84C,0x4E38,0x9A,0xC8,0xC7,0xBD,0x22,0x01,0x6B,0x3D)
__CRT_UUID_DECL(ID3D12GraphicsCommandList4, 0x8754318e,0xd3a9,0x4541,0x98,0xcf,0x64,0x5b,0x50,0xdc,0x48,0x74)
__CRT_UUID_DECL(ID3D12Device,               0x189819f1,0x1db6,0x4b57,0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7)
__CRT_UUID_DECL(ID3D12Device1,              0x77acce80,0x638e,0x4e65,0x88,0x95,0xc1,0xf2,0x33,0x86,0x86,0x3e)
__CRT_UUID_DECL(ID3D12Device2,              0x30baa41e,0xb15b,0x475c,0xa0,0xbb,0x1a,0xf5,0xc5,0xb6,0x43,0x28)
__CRT_UUID_DECL(ID3D12Device3,              0x81dadc15,0x2bad,0x4392,0x93,0xc5,0x10,0x13,0x45,0xc4,0xaa,0x98)
__CRT_UUID_DECL(ID3D12Device4,              0xe865df17,0xa9ee,0x46f9,0xa4,0x63,0x30,0x98,0x31,0x5a,0xa2,0xe5)
__CRT_UUID_DECL(ID3D12Device5,              0x8b4f173b,0x2fea,0x4b80,0x8f,0x58,0x43,0x07,0x19,0x1a,0xb9,0x5d)
#endif
#include <stdio.h>
#include <stdarg.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <atomic>

// IID_PPV_ARGS - defined in objbase.h on MSVC, may be missing on mingw
#ifndef IID_PPV_ARGS
#define IID_PPV_ARGS(ppType) __uuidof(**(ppType)), reinterpret_cast<void**>(ppType)
#endif

// ============================================================================
// Logging
// ============================================================================

static FILE*            g_log          = nullptr;
static CRITICAL_SECTION g_logCs;
static bool             g_logCsInited  = false;
static DWORD            g_logStartTick = 0;

static void LogOpen() {
    g_log = fopen("d3d12_hook.log", "w");
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log,
            "=== d3d12_hook log opened %04d-%02d-%02d %02d:%02d:%02d ===\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
        fflush(g_log);
    }
}

// D3DLog(level, fmt, ...) — level: "INFO", "WARN", "ERROR"
static void D3DLogV(const char* level, const char* fmt, va_list args) {
    if (!g_log) return;
    DWORD elapsed = GetTickCount() - g_logStartTick;
    DWORD tid     = GetCurrentThreadId();
    if (g_logCsInited) EnterCriticalSection(&g_logCs);
    fprintf(g_log, "[%4u.%03u][t%04X][%s] ",
            elapsed / 1000, elapsed % 1000,
            (UINT)(tid & 0xFFFF), level);
    vfprintf(g_log, fmt, args);
    fputc('\n', g_log);
    fflush(g_log);
    if (g_logCsInited) LeaveCriticalSection(&g_logCs);
}

static void D3DLog(const char* fmt, ...) {
    va_list a; va_start(a, fmt); D3DLogV("INFO ", fmt, a); va_end(a);
}
static void D3DLogW(const char* fmt, ...) {
    va_list a; va_start(a, fmt); D3DLogV("WARN ", fmt, a); va_end(a);
}
static void D3DLogE(const char* fmt, ...) {
    va_list a; va_start(a, fmt); D3DLogV("ERROR", fmt, a); va_end(a);
}

// Log an HRESULT failure; returns the same hr for use in return statements
static HRESULT D3DLogHR(const char* context, HRESULT hr) {
    if (FAILED(hr))
        D3DLogE("%s failed: HRESULT 0x%08X", context, (UINT)hr);
    return hr;
}

// ============================================================================
// Real D3D12 function pointers
// ============================================================================

static HMODULE g_realD3D12 = nullptr;

typedef HRESULT (WINAPI* PFN_D3D12CreateDevice_Real)(
    IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI* PFN_D3D12GetDebugInterface_Real)(
    REFIID, void**);
typedef HRESULT (WINAPI* PFN_D3D12SerializeRootSignature_Real)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI* PFN_D3D12SerializeVersionedRootSignature_Real)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI* PFN_D3D12CreateRootSignatureDeserializer_Real)(
    LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT (WINAPI* PFN_D3D12CreateVersionedRootSignatureDeserializer_Real)(
    LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT (WINAPI* PFN_D3D12GetInterface_Real)(
    REFCLSID, REFIID, void**);
typedef HRESULT (WINAPI* PFN_D3D12EnableExperimentalFeatures_Real)(
    UINT, const IID*, void*, UINT*);
typedef HRESULT (WINAPI* PFN_GetBehaviorValue_Real)(
    LPCSTR, UINT64*);
typedef HRESULT (WINAPI* PFN_D3D12CoreCreateLayeredDevice_Real)(
    const void*, UINT, const void*, REFIID, void**);
typedef SIZE_T  (WINAPI* PFN_D3D12CoreGetLayeredDeviceSize_Real)(
    const void*, UINT);
typedef HRESULT (WINAPI* PFN_D3D12CoreRegisterLayers_Real)(
    const void*, UINT);

static PFN_D3D12CreateDevice_Real                           g_realCreateDevice              = nullptr;
static PFN_D3D12GetDebugInterface_Real                      g_realGetDebugInterface         = nullptr;
static PFN_D3D12SerializeRootSignature_Real                 g_realSerializeRootSig          = nullptr;
static PFN_D3D12SerializeVersionedRootSignature_Real        g_realSerializeVersionedRootSig = nullptr;
static PFN_D3D12CreateRootSignatureDeserializer_Real        g_realCreateRootSigDes          = nullptr;
static PFN_D3D12CreateVersionedRootSignatureDeserializer_Real g_realCreateVersionedRootSigDes = nullptr;
static PFN_D3D12GetInterface_Real                           g_realGetInterface              = nullptr;
static PFN_D3D12EnableExperimentalFeatures_Real             g_realEnableExperimental        = nullptr;
static PFN_GetBehaviorValue_Real                            g_realGetBehaviorValue          = nullptr;
static PFN_D3D12CoreCreateLayeredDevice_Real                g_realCoreCreateLayeredDevice   = nullptr;
static PFN_D3D12CoreGetLayeredDeviceSize_Real               g_realCoreGetLayeredDeviceSize  = nullptr;
static PFN_D3D12CoreRegisterLayers_Real                     g_realCoreRegisterLayers        = nullptr;

static bool LoadRealD3D12() {
    if (g_realD3D12) return true;

    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);

    char path[MAX_PATH];
    sprintf(path, "%s\\d3d12.dll", sysDir);

    g_realD3D12 = LoadLibraryA(path);
    if (!g_realD3D12) {
        D3DLogE("Cannot load real d3d12.dll from %s (LastError=%u)",
                path, (UINT)GetLastError());
        return false;
    }

    D3DLog("Real d3d12.dll loaded from %s", path);

#define LOAD_PROC(var, name) \
    var = (decltype(var))GetProcAddress(g_realD3D12, name); \
    if (!var) D3DLogW("  GetProcAddress(" name ") returned null");

    LOAD_PROC(g_realCreateDevice,              "D3D12CreateDevice")
    LOAD_PROC(g_realGetDebugInterface,         "D3D12GetDebugInterface")
    LOAD_PROC(g_realSerializeRootSig,          "D3D12SerializeRootSignature")
    LOAD_PROC(g_realSerializeVersionedRootSig, "D3D12SerializeVersionedRootSignature")
    LOAD_PROC(g_realCreateRootSigDes,          "D3D12CreateRootSignatureDeserializer")
    LOAD_PROC(g_realCreateVersionedRootSigDes, "D3D12CreateVersionedRootSignatureDeserializer")
    LOAD_PROC(g_realGetInterface,              "D3D12GetInterface")
    LOAD_PROC(g_realEnableExperimental,        "D3D12EnableExperimentalFeatures")
    LOAD_PROC(g_realGetBehaviorValue,          "GetBehaviorValue")
    LOAD_PROC(g_realCoreCreateLayeredDevice,   "D3D12CoreCreateLayeredDevice")
    LOAD_PROC(g_realCoreGetLayeredDeviceSize,  "D3D12CoreGetLayeredDeviceSize")
    LOAD_PROC(g_realCoreRegisterLayers,        "D3D12CoreRegisterLayers")
#undef LOAD_PROC

    return true;
}

// ============================================================================
// Reserved-resource tracking
// Resources originally requested as reserved are converted to committed.
// We track them so CopyTiles / UpdateTileMappings can be handled correctly.
// ============================================================================

static std::unordered_map<ID3D12Resource*, bool> g_reservedSet;
static CRITICAL_SECTION                          g_reservedCs;
static bool                                      g_reservedCsInited = false;

static void TrackReserved(ID3D12Resource* r) {
    if (g_reservedCsInited) EnterCriticalSection(&g_reservedCs);
    g_reservedSet[r] = true;
    size_t n = g_reservedSet.size();
    if (g_reservedCsInited) LeaveCriticalSection(&g_reservedCs);
    D3DLog("TrackReserved: %p (total tracked: %zu)", r, n);
}

static void UntrackReserved(ID3D12Resource* r) {
    if (g_reservedCsInited) EnterCriticalSection(&g_reservedCs);
    bool erased = g_reservedSet.erase(r) != 0;
    size_t n = g_reservedSet.size();
    if (g_reservedCsInited) LeaveCriticalSection(&g_reservedCs);
    if (erased)
        D3DLog("UntrackReserved: %p (remaining: %zu)", r, n);
}

static bool IsReserved(ID3D12Resource* r) {
    if (g_reservedCsInited) EnterCriticalSection(&g_reservedCs);
    bool found = g_reservedSet.count(r) > 0;
    if (g_reservedCsInited) LeaveCriticalSection(&g_reservedCs);
    return found;
}

// ============================================================================
// Bytes-per-block helper (needed for row pitch in CopyTiles translation)
// ============================================================================

static UINT GetFormatBytesPerBlock(DXGI_FORMAT fmt) {
    switch (fmt) {
        // 1 byte per texel
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_A8_UNORM:
            return 1;
        // 2 bytes per texel
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_D16_UNORM:
            return 2;
        // 4 bytes per texel
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return 4;
        // 8 bytes per texel
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
            return 8;
        // 16 bytes per texel
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
            return 16;
        // BC compressed formats: 8 bytes per 4x4 block
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8;
        // BC compressed formats: 16 bytes per 4x4 block
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
        default:
            return 4; // safe fallback
    }
}

static bool IsBlockCompressed(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// CopyTiles translation
// Converts a tile-based copy into CopyBufferRegion / CopyTextureRegion calls
// on a committed resource (which was originally requested as reserved).
// ============================================================================

static void TranslateCopyTiles(
    ID3D12GraphicsCommandList*             cmdList,
    ID3D12Device*                          device,
    ID3D12Resource*                        pResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pCoord,
    const D3D12_TILE_REGION_SIZE*          pSize,
    ID3D12Resource*                        pBuffer,
    UINT64                                 bufferOffset,
    D3D12_TILE_COPY_FLAGS                  flags)
{
    const bool toResource =
        (flags & D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) != 0;

    D3D12_RESOURCE_DESC desc; pResource->GetDesc(&desc);

    // ---- Buffer: tiles are linear 64 KB chunks --------------------------------
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        UINT64 resOffset = (UINT64)pCoord->X * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
        UINT64 copySize  = (UINT64)pSize->NumTiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
        copySize = (std::min)(copySize, desc.Width - resOffset);
        D3DLog("TranslateCopyTiles: buffer path res=%p buf=%p "
               "resOffset=%llu copySize=%llu toResource=%d",
               pResource, pBuffer, (unsigned long long)resOffset,
               (unsigned long long)copySize, (int)toResource);
        if (copySize == 0) { D3DLogW("TranslateCopyTiles: buffer copySize==0, skipping"); return; }

        if (toResource)
            cmdList->CopyBufferRegion(pResource, resOffset, pBuffer, bufferOffset, copySize);
        else
            cmdList->CopyBufferRegion(pBuffer, bufferOffset, pResource, resOffset, copySize);
        return;
    }

    // ---- Texture: query tile shapes via GetResourceTiling ---------------------
    if (!device) {
        D3DLogE("TranslateCopyTiles: no device ptr, cannot translate texture tiles "
                "res=%p fmt=%u", pResource, (UINT)desc.Format);
        return;
    }
    D3DLog("TranslateCopyTiles: texture path res=%p fmt=%u %ux%u mips=%u "
           "useBox=%d numTiles=%u toResource=%d",
           pResource, (UINT)desc.Format,
           (UINT)desc.Width, (UINT)desc.Height,
           (UINT)desc.MipLevels,
           (int)(pSize->UseBox), pSize->NumTiles, (int)toResource);

    UINT mipLevels    = desc.MipLevels;
    UINT arraySlices  = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                        ? 1
                        : (UINT)desc.DepthOrArraySize;
    UINT numSubRes    = mipLevels * arraySlices;

    UINT               tilesTotal = 0;
    D3D12_PACKED_MIP_INFO packedMip = {};
    D3D12_TILE_SHAPE   tileShape   = {};
    UINT               numTiling   = numSubRes;

    std::vector<D3D12_SUBRESOURCE_TILING> subTiling(numSubRes);
    device->GetResourceTiling(pResource, &tilesTotal, &packedMip,
                              &tileShape, &numTiling, 0, subTiling.data());

    const UINT tileW = tileShape.WidthInTexels;
    const UINT tileH = tileShape.HeightInTexels;
    const UINT tileD = tileShape.DepthInTexels;

    const bool bc = IsBlockCompressed(desc.Format);
    // For BC formats texel dimensions reported per 4x4 block, row pitch is in blocks
    const UINT blockW = bc ? 4 : 1;
    const UINT blockH = bc ? 4 : 1;

    // UseBox = tile region described by (X,Y,Z,W,H,D) within one subresource
    if (pSize->UseBox) {
        UINT sub = pCoord->Subresource;
        if (sub >= numSubRes) {
            D3DLogE("TranslateCopyTiles UseBox: subresource %u out of range %u", sub, numSubRes);
            return;
        }
        D3DLog("TranslateCopyTiles: UseBox sub=%u region=%ux%ux%u at (%u,%u,%u)",
               sub, pSize->Width, pSize->Height, pSize->Depth,
               pCoord->X, pCoord->Y, pCoord->Z);
        const D3D12_SUBRESOURCE_TILING& st = subTiling[sub];

        // Mip dimensions
        UINT mip  = sub % mipLevels;
        UINT mipW = (UINT)(std::max)((UINT64)1, desc.Width  >> mip);
        UINT mipH = (UINT)(std::max)(1u,        (UINT)desc.Height >> mip);
        UINT mipD = (UINT)(std::max)(1u,
                    (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                    ? (UINT)desc.DepthOrArraySize >> mip : 1u);

        UINT tileIndex = 0;
        for (UINT tz = 0; tz < pSize->Depth;  tz++)
        for (UINT ty = 0; ty < pSize->Height; ty++)
        for (UINT tx = 0; tx < pSize->Width;  tx++, tileIndex++) {
            UINT texX = (pCoord->X + tx) * tileW;
            UINT texY = (pCoord->Y + ty) * tileH;
            UINT texZ = (pCoord->Z + tz) * tileD;

            if (texX >= mipW || texY >= mipH || texZ >= mipD) continue;

            UINT copyW = (std::min)(tileW, mipW - texX);
            UINT copyH = (std::min)(tileH, mipH - texY);
            UINT copyD = (std::min)(tileD, mipD - texZ);

            // Row pitch (256-byte aligned, in bytes)
            UINT blocksX   = (copyW + blockW - 1) / blockW;
            UINT bytesPerBlock = GetFormatBytesPerBlock(desc.Format);
            UINT rowPitch  = ((blocksX * bytesPerBlock) + 255) & ~255u;

            UINT64 tileBufferOffset = bufferOffset
                + (UINT64)tileIndex * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            fp.Offset             = tileBufferOffset;
            fp.Footprint.Format   = desc.Format;
            fp.Footprint.Width    = copyW;
            fp.Footprint.Height   = copyH;
            fp.Footprint.Depth    = copyD;
            fp.Footprint.RowPitch = rowPitch;

            D3D12_TEXTURE_COPY_LOCATION bufLoc = {};
            bufLoc.pResource       = pBuffer;
            bufLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            bufLoc.PlacedFootprint = fp;

            D3D12_TEXTURE_COPY_LOCATION texLoc = {};
            texLoc.pResource        = pResource;
            texLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            texLoc.SubresourceIndex = sub;

            D3D12_BOX box = { texX, texY, texZ,
                              texX + copyW, texY + copyH, texZ + copyD };

            if (toResource)
                cmdList->CopyTextureRegion(&texLoc, texX, texY, texZ, &bufLoc, nullptr);
            else
                cmdList->CopyTextureRegion(&bufLoc, 0, 0, 0, &texLoc, &box);
        }
        return;
    }

    // Linear tile ordering (no UseBox): enumerate tiles across all subresources
    D3DLog("TranslateCopyTiles: linear tile walk startIdx=%u numTiles=%u",
           pCoord->X, pSize->NumTiles);
    UINT64 curBufOffset  = bufferOffset;
    UINT   tilesLeft     = pSize->NumTiles;
    UINT   linearTileIdx = pCoord->X; // starting linear tile index

    for (UINT sub = 0; sub < numSubRes && tilesLeft > 0; sub++) {
        const D3D12_SUBRESOURCE_TILING& st = subTiling[sub];
        if (st.StartTileIndexInOverallResource == D3D12_PACKED_TILE) {
            continue; // packed mips, skip
        }

        UINT tilesInSub = st.WidthInTiles * st.HeightInTiles * st.DepthInTiles;
        if (linearTileIdx >= tilesInSub) {
            linearTileIdx -= tilesInSub;
            continue;
        }

        UINT mip  = sub % mipLevels;
        UINT mipW = (UINT)(std::max)((UINT64)1, desc.Width  >> mip);
        UINT mipH = (UINT)(std::max)(1u,        (UINT)desc.Height >> mip);
        UINT mipD = (UINT)(std::max)(1u,
                    (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                    ? (UINT)desc.DepthOrArraySize >> mip : 1u);

        while (linearTileIdx < tilesInSub && tilesLeft > 0) {
            UINT tz = linearTileIdx / (st.WidthInTiles * st.HeightInTiles);
            UINT ty = (linearTileIdx / st.WidthInTiles) % st.HeightInTiles;
            UINT tx = linearTileIdx % st.WidthInTiles;

            UINT texX = tx * tileW;
            UINT texY = ty * tileH;
            UINT texZ = tz * tileD;

            if (texX < mipW && texY < mipH && texZ < mipD) {
                UINT copyW = (std::min)(tileW, mipW - texX);
                UINT copyH = (std::min)(tileH, mipH - texY);
                UINT copyD = (std::min)(tileD, mipD - texZ);

                UINT blocksX       = (copyW + blockW - 1) / blockW;
                UINT bytesPerBlock = GetFormatBytesPerBlock(desc.Format);
                UINT rowPitch      = ((blocksX * bytesPerBlock) + 255) & ~255u;

                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
                fp.Offset             = curBufOffset;
                fp.Footprint.Format   = desc.Format;
                fp.Footprint.Width    = copyW;
                fp.Footprint.Height   = copyH;
                fp.Footprint.Depth    = copyD;
                fp.Footprint.RowPitch = rowPitch;

                D3D12_TEXTURE_COPY_LOCATION bufLoc = {};
                bufLoc.pResource       = pBuffer;
                bufLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                bufLoc.PlacedFootprint = fp;

                D3D12_TEXTURE_COPY_LOCATION texLoc = {};
                texLoc.pResource        = pResource;
                texLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                texLoc.SubresourceIndex = sub;

                D3D12_BOX box = { texX, texY, texZ,
                                  texX + copyW, texY + copyH, texZ + copyD };

                if (toResource)
                    cmdList->CopyTextureRegion(&texLoc, texX, texY, texZ, &bufLoc, nullptr);
                else
                    cmdList->CopyTextureRegion(&bufLoc, 0, 0, 0, &texLoc, &box);
            }

            curBufOffset += D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
            linearTileIdx++;
            tilesLeft--;
        }
        linearTileIdx = 0; // reset for next subresource
    }
}

// ============================================================================
// CommandListWrapper
// Private IID used to detect and unwrap CommandListWrapper objects.
// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
static const GUID IID_CommandListWrapperMarker = {
    0xa1b2c3d4, 0xe5f6, 0x7890,
    { 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90 }
};

// Wraps ID3D12GraphicsCommandList through ID3D12GraphicsCommandList4
// ============================================================================

class CommandListWrapper : public ID3D12GraphicsCommandList4 {
public:
    ID3D12GraphicsCommandList4* m_real;
    ID3D12Device*               m_device; // weak ref, for CopyTiles translation
    LONG                        m_refs;

    CommandListWrapper(ID3D12GraphicsCommandList4* real, ID3D12Device* dev)
        : m_real(real), m_device(dev), m_refs(1)
    {
        // real already has a ref (caller transferred ownership)
        D3DLog("CommandListWrapper created %p", real);
    }

    ~CommandListWrapper() {
        D3DLog("CommandListWrapper destroyed %p", m_real);
        m_real->Release();
    }

    // ------- IUnknown -------------------------------------------------------

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)                    ||
            riid == __uuidof(ID3D12Object)                ||
            riid == __uuidof(ID3D12DeviceChild)           ||
            riid == __uuidof(ID3D12CommandList)           ||
            riid == __uuidof(ID3D12GraphicsCommandList)   ||
            riid == __uuidof(ID3D12GraphicsCommandList1)  ||
            riid == __uuidof(ID3D12GraphicsCommandList2)  ||
            riid == __uuidof(ID3D12GraphicsCommandList3)  ||
            riid == __uuidof(ID3D12GraphicsCommandList4)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        if (riid == IID_CommandListWrapperMarker) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refs);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }

    // ------- ID3D12Object ---------------------------------------------------

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_real->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_real->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_real->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_real->SetName(Name);
    }

    // ------- ID3D12DeviceChild ----------------------------------------------

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        return m_real->GetDevice(riid, ppvDevice);
    }

    // ------- ID3D12CommandList ----------------------------------------------

    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
        return m_real->GetType();
    }

    // ------- ID3D12GraphicsCommandList --------------------------------------

    HRESULT STDMETHODCALLTYPE Close() override {
        return m_real->Close();
    }

    HRESULT STDMETHODCALLTYPE Reset(
        ID3D12CommandAllocator* pAllocator,
        ID3D12PipelineState*    pInitialState) override
    {
        return m_real->Reset(pAllocator, pInitialState);
    }

    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pPipelineState) override {
        m_real->ClearState(pPipelineState);
    }

    void STDMETHODCALLTYPE DrawInstanced(
        UINT VertexCountPerInstance, UINT InstanceCount,
        UINT StartVertexLocation,   UINT StartInstanceLocation) override
    {
        static std::atomic<uint64_t> s_draws{0};
        uint64_t n = s_draws.fetch_add(1);
        if (n == 0) D3DLog("First DrawInstanced: verts=%u instances=%u", VertexCountPerInstance, InstanceCount);
        if (n == 1000) D3DLog("1000 DrawInstanced calls reached");
        m_real->DrawInstanced(VertexCountPerInstance, InstanceCount,
                              StartVertexLocation, StartInstanceLocation);
    }

    void STDMETHODCALLTYPE DrawIndexedInstanced(
        UINT IndexCountPerInstance, UINT InstanceCount,
        UINT StartIndexLocation,    INT  BaseVertexLocation,
        UINT StartInstanceLocation) override
    {
        static std::atomic<uint64_t> s_draws{0};
        uint64_t n = s_draws.fetch_add(1);
        if (n == 0) D3DLog("First DrawIndexedInstanced: idx=%u instances=%u", IndexCountPerInstance, InstanceCount);
        if (n == 1000) D3DLog("1000 DrawIndexedInstanced calls reached");
        m_real->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount,
                                     StartIndexLocation, BaseVertexLocation,
                                     StartInstanceLocation);
    }

    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX,
                                    UINT ThreadGroupCountY,
                                    UINT ThreadGroupCountZ) override
    {
        m_real->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }

    void STDMETHODCALLTYPE CopyBufferRegion(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset,
        UINT64 NumBytes) override
    {
        m_real->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
    }

    void STDMETHODCALLTYPE CopyTextureRegion(
        const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc,
        const D3D12_BOX*                   pSrcBox) override
    {
        m_real->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
    }

    void STDMETHODCALLTYPE CopyResource(
        ID3D12Resource* pDstResource,
        ID3D12Resource* pSrcResource) override
    {
        m_real->CopyResource(pDstResource, pSrcResource);
    }

    // *** KEY OVERRIDE #1: CopyTiles → translate for committed resources ***
    void STDMETHODCALLTYPE CopyTiles(
        ID3D12Resource*                        pTiledResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE*          pTileRegionSize,
        ID3D12Resource*                        pBuffer,
        UINT64                                 BufferStartOffsetInBytes,
        D3D12_TILE_COPY_FLAGS                  Flags) override
    {
        if (IsReserved(pTiledResource)) {
            D3DLog("CopyTiles: translating for committed res=%p buf=%p "
                   "numTiles=%u flags=0x%X",
                   pTiledResource, pBuffer,
                   pTileRegionSize ? pTileRegionSize->NumTiles : 0u,
                   (UINT)Flags);
            TranslateCopyTiles(m_real, m_device,
                               pTiledResource,
                               pTileRegionStartCoordinate,
                               pTileRegionSize,
                               pBuffer,
                               BufferStartOffsetInBytes,
                               Flags);
        } else {
            D3DLog("CopyTiles: pass-through res=%p buf=%p numTiles=%u",
                   pTiledResource, pBuffer,
                   pTileRegionSize ? pTileRegionSize->NumTiles : 0u);
            m_real->CopyTiles(pTiledResource, pTileRegionStartCoordinate,
                              pTileRegionSize, pBuffer,
                              BufferStartOffsetInBytes, Flags);
        }
    }

    void STDMETHODCALLTYPE ResolveSubresource(
        ID3D12Resource* pDstResource, UINT DstSubresource,
        ID3D12Resource* pSrcResource, UINT SrcSubresource,
        DXGI_FORMAT Format) override
    {
        m_real->ResolveSubresource(pDstResource, DstSubresource,
                                   pSrcResource, SrcSubresource, Format);
    }

    void STDMETHODCALLTYPE IASetPrimitiveTopology(
        D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override
    {
        m_real->IASetPrimitiveTopology(PrimitiveTopology);
    }

    void STDMETHODCALLTYPE RSSetViewports(
        UINT NumViewports, const D3D12_VIEWPORT* pViewports) override
    {
        m_real->RSSetViewports(NumViewports, pViewports);
    }

    void STDMETHODCALLTYPE RSSetScissorRects(
        UINT NumRects, const D3D12_RECT* pRects) override
    {
        m_real->RSSetScissorRects(NumRects, pRects);
    }

    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[4]) override {
        m_real->OMSetBlendFactor(BlendFactor);
    }

    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override {
        m_real->OMSetStencilRef(StencilRef);
    }

    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) override {
        m_real->SetPipelineState(pPipelineState);
    }

    // *** KEY OVERRIDE #2: ResourceBarrier — handle ALIASING barriers ***
    void STDMETHODCALLTYPE ResourceBarrier(
        UINT                          NumBarriers,
        const D3D12_RESOURCE_BARRIER* pBarriers) override
    {
        // DXMT no-ops D3D12_RESOURCE_BARRIER_TYPE_ALIASING. Replace each aliasing
        // barrier with a UAV barrier (pResource=null = global UAV flush) so DXMT
        // serializes all pending GPU writes before the caller reuses the memory.
        // The aliasing barrier itself is dropped since DXMT ignores it anyway.
        std::vector<D3D12_RESOURCE_BARRIER> out;
        out.reserve(NumBarriers);

        for (UINT i = 0; i < NumBarriers; i++) {
            if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                ID3D12Resource* before = pBarriers[i].Aliasing.pResourceBefore;
                ID3D12Resource* after  = pBarriers[i].Aliasing.pResourceAfter;
                D3D12_RESOURCE_DESC db = {}, da = {};
                if (before) before->GetDesc(&db);
                if (after)  after->GetDesc(&da);
                static std::atomic<int> aliasCount{0};
                int n = aliasCount.fetch_add(1);
                if (n < 500 && (db.Dimension == 1 || da.Dimension == 1 || (n % 50) == 0)) {
                    D3DLog("Aliasing[%d]: before=%p dim=%u fmt=%u w=%llu h=%u  after=%p dim=%u fmt=%u w=%llu h=%u",
                        n,
                        (void*)before, (unsigned)db.Dimension, (unsigned)db.Format,
                        (unsigned long long)db.Width, (unsigned)db.Height,
                        (void*)after,  (unsigned)da.Dimension, (unsigned)da.Format,
                        (unsigned long long)da.Width, (unsigned)da.Height);
                }
                // Replace with UAV null-barrier to serialize GPU writes
                D3D12_RESOURCE_BARRIER uav = {};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uav.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                uav.UAV.pResource = nullptr;
                out.push_back(uav);
            } else {
                out.push_back(pBarriers[i]);
            }
        }

        if (!out.empty())
            m_real->ResourceBarrier((UINT)out.size(), out.data());
    }

    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) override {
        m_real->ExecuteBundle(pCommandList);
    }

    void STDMETHODCALLTYPE SetDescriptorHeaps(
        UINT                         NumDescriptorHeaps,
        ID3D12DescriptorHeap* const* ppDescriptorHeaps) override
    {
        m_real->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
    }

    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* pRootSignature) override {
        m_real->SetComputeRootSignature(pRootSignature);
    }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* pRootSignature) override {
        m_real->SetGraphicsRootSignature(pRootSignature);
    }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override
    {
        m_real->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override
    {
        m_real->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override
    {
        m_real->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override
    {
        m_real->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet,
        const void* pSrcData, UINT DestOffsetIn32BitValues) override
    {
        m_real->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet,
                                             pSrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet,
        const void* pSrcData, UINT DestOffsetIn32BitValues) override
    {
        m_real->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet,
                                              pSrcData, DestOffsetIn32BitValues);
    }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    {
        m_real->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    {
        m_real->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    {
        m_real->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    {
        m_real->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    {
        m_real->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override
    {
        m_real->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(
        const D3D12_INDEX_BUFFER_VIEW* pView) override
    {
        m_real->IASetIndexBuffer(pView);
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(
        UINT StartSlot, UINT NumViews,
        const D3D12_VERTEX_BUFFER_VIEW* pViews) override
    {
        m_real->IASetVertexBuffers(StartSlot, NumViews, pViews);
    }
    void STDMETHODCALLTYPE SOSetTargets(
        UINT StartSlot, UINT NumViews,
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) override
    {
        m_real->SOSetTargets(StartSlot, NumViews, pViews);
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(
        UINT NumRenderTargetDescriptors,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
        BOOL RTsSingleHandleToDescriptorRange,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) override
    {
        m_real->OMSetRenderTargets(NumRenderTargetDescriptors,
                                   pRenderTargetDescriptors,
                                   RTsSingleHandleToDescriptorRange,
                                   pDepthStencilDescriptor);
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(
        D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
        D3D12_CLEAR_FLAGS           ClearFlags,
        FLOAT DepthValue, UINT8 StencilValue,
        UINT NumRects, const D3D12_RECT* pRects) override
    {
        m_real->ClearDepthStencilView(DepthStencilView, ClearFlags,
                                      DepthValue, StencilValue,
                                      NumRects, pRects);
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(
        D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView,
        const FLOAT ColorRGBA[4],
        UINT NumRects, const D3D12_RECT* pRects) override
    {
        m_real->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects);
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
        ID3D12Resource* pResource,
        const UINT Values[4],
        UINT NumRects, const D3D12_RECT* pRects) override
    {
        m_real->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle,
                                             pResource, Values, NumRects, pRects);
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
        ID3D12Resource* pResource,
        const FLOAT Values[4],
        UINT NumRects, const D3D12_RECT* pRects) override
    {
        m_real->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle,
                                              pResource, Values, NumRects, pRects);
    }
    void STDMETHODCALLTYPE DiscardResource(
        ID3D12Resource* pResource,
        const D3D12_DISCARD_REGION* pRegion) override
    {
        m_real->DiscardResource(pResource, pRegion);
    }
    void STDMETHODCALLTYPE BeginQuery(
        ID3D12QueryHeap* pQueryHeap,
        D3D12_QUERY_TYPE Type, UINT Index) override
    {
        m_real->BeginQuery(pQueryHeap, Type, Index);
    }
    void STDMETHODCALLTYPE EndQuery(
        ID3D12QueryHeap* pQueryHeap,
        D3D12_QUERY_TYPE Type, UINT Index) override
    {
        m_real->EndQuery(pQueryHeap, Type, Index);
    }
    void STDMETHODCALLTYPE ResolveQueryData(
        ID3D12QueryHeap* pQueryHeap,
        D3D12_QUERY_TYPE Type,
        UINT StartIndex, UINT NumQueries,
        ID3D12Resource* pDestinationBuffer,
        UINT64 AlignedDestinationBufferOffset) override
    {
        m_real->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries,
                                 pDestinationBuffer, AlignedDestinationBufferOffset);
    }
    void STDMETHODCALLTYPE SetPredication(
        ID3D12Resource* pBuffer,
        UINT64 AlignedBufferOffset,
        D3D12_PREDICATION_OP Operation) override
    {
        m_real->SetPredication(pBuffer, AlignedBufferOffset, Operation);
    }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override {
        m_real->SetMarker(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override {
        m_real->BeginEvent(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE EndEvent() override {
        m_real->EndEvent();
    }
    void STDMETHODCALLTYPE ExecuteIndirect(
        ID3D12CommandSignature* pCommandSignature,
        UINT MaxCommandCount,
        ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
        ID3D12Resource* pCountBuffer,    UINT64 CountBufferOffset) override
    {
        static std::atomic<uint64_t> s_indirect{0};
        uint64_t n = s_indirect.fetch_add(1);
        if (n == 0) D3DLog("First ExecuteIndirect: maxCount=%u argBuf=%p argOff=%llu",
            MaxCommandCount, (void*)pArgumentBuffer, (unsigned long long)ArgumentBufferOffset);
        if (n == 1000) D3DLog("1000 ExecuteIndirect calls reached");
        m_real->ExecuteIndirect(pCommandSignature, MaxCommandCount,
                                pArgumentBuffer, ArgumentBufferOffset,
                                pCountBuffer, CountBufferOffset);
    }

    // ------- ID3D12GraphicsCommandList1 -------------------------------------

    void STDMETHODCALLTYPE AtomicCopyBufferUINT(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset,
        UINT Dependencies,
        ID3D12Resource* const* ppDependencies,
        const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override
    {
        m_real->AtomicCopyBufferUINT(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset,
                                     Dependencies, ppDependencies,
                                     pDependentSubresourceRanges);
    }
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset,
        UINT Dependencies,
        ID3D12Resource* const* ppDependencies,
        const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override
    {
        m_real->AtomicCopyBufferUINT64(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset,
                                       Dependencies, ppDependencies,
                                       pDependentSubresourceRanges);
    }
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT Min, FLOAT Max) override {
        m_real->OMSetDepthBounds(Min, Max);
    }
    void STDMETHODCALLTYPE SetSamplePositions(
        UINT NumSamplesPerPixel, UINT NumPixels,
        D3D12_SAMPLE_POSITION* pSamplePositions) override
    {
        m_real->SetSamplePositions(NumSamplesPerPixel, NumPixels, pSamplePositions);
    }
    void STDMETHODCALLTYPE ResolveSubresourceRegion(
        ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
        ID3D12Resource* pSrcResource, UINT SrcSubresource,
        D3D12_RECT* pSrcRect, DXGI_FORMAT Format,
        D3D12_RESOLVE_MODE ResolveMode) override
    {
        m_real->ResolveSubresourceRegion(pDstResource, DstSubresource, DstX, DstY,
                                         pSrcResource, SrcSubresource,
                                         pSrcRect, Format, ResolveMode);
    }
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT Mask) override {
        m_real->SetViewInstanceMask(Mask);
    }

    // ------- ID3D12GraphicsCommandList2 -------------------------------------

    void STDMETHODCALLTYPE WriteBufferImmediate(
        UINT Count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE*      pModes) override
    {
        m_real->WriteBufferImmediate(Count, pParams, pModes);
    }

    // ------- ID3D12GraphicsCommandList3 -------------------------------------

    void STDMETHODCALLTYPE SetProtectedResourceSession(
        ID3D12ProtectedResourceSession* pProtectedResourceSession) override
    {
        m_real->SetProtectedResourceSession(pProtectedResourceSession);
    }

    // ------- ID3D12GraphicsCommandList4 (DXR) --------------------------------

    void STDMETHODCALLTYPE BeginRenderPass(
        UINT NumRenderTargets,
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC*   pRenderTargets,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*   pDepthStencil,
        D3D12_RENDER_PASS_FLAGS                       Flags) override
    {
        m_real->BeginRenderPass(NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
    }
    void STDMETHODCALLTYPE EndRenderPass() override {
        m_real->EndRenderPass();
    }
    void STDMETHODCALLTYPE InitializeMetaCommand(
        ID3D12MetaCommand* pMetaCommand,
        const void* pInitializationParametersData,
        SIZE_T InitializationParametersDataSizeInBytes) override
    {
        m_real->InitializeMetaCommand(pMetaCommand, pInitializationParametersData,
                                      InitializationParametersDataSizeInBytes);
    }
    void STDMETHODCALLTYPE ExecuteMetaCommand(
        ID3D12MetaCommand* pMetaCommand,
        const void* pExecutionParametersData,
        SIZE_T ExecutionParametersDataSizeInBytes) override
    {
        m_real->ExecuteMetaCommand(pMetaCommand, pExecutionParametersData,
                                   ExecutionParametersDataSizeInBytes);
    }
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
        UINT NumPostbuildInfoDescs,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) override
    {
        m_real->BuildRaytracingAccelerationStructure(pDesc, NumPostbuildInfoDescs,
                                                     pPostbuildInfoDescs);
    }
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc,
        UINT NumSourceAccelerationStructures,
        const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData) override
    {
        m_real->EmitRaytracingAccelerationStructurePostbuildInfo(
            pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
    }
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
        D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
        D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) override
    {
        m_real->CopyRaytracingAccelerationStructure(DestAccelerationStructureData,
                                                    SourceAccelerationStructureData, Mode);
    }
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject* pStateObject) override {
        m_real->SetPipelineState1(pStateObject);
    }
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc) override {
        m_real->DispatchRays(pDesc);
    }
};


// ============================================================================
// CommandQueueWrapper
// Wraps ID3D12CommandQueue to intercept UpdateTileMappings / CopyTileMappings
// ============================================================================

class CommandQueueWrapper : public ID3D12CommandQueue {
public:
    ID3D12CommandQueue* m_real;
    LONG                m_refs;

    explicit CommandQueueWrapper(ID3D12CommandQueue* real)
        : m_real(real), m_refs(1)
    {
        D3DLog("CommandQueueWrapper created %p", real);
    }

    ~CommandQueueWrapper() {
        D3DLog("CommandQueueWrapper destroyed");
        m_real->Release();
    }

    // ------- IUnknown -------------------------------------------------------

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)          ||
            riid == __uuidof(ID3D12Object)       ||
            riid == __uuidof(ID3D12DeviceChild)  ||
            riid == __uuidof(ID3D12Pageable)     ||
            riid == __uuidof(ID3D12CommandQueue)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refs);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }

    // ------- ID3D12Object ---------------------------------------------------

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_real->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_real->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_real->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_real->SetName(Name);
    }

    // ------- ID3D12DeviceChild ----------------------------------------------

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        return m_real->GetDevice(riid, ppvDevice);
    }

    // ------- ID3D12CommandQueue ---------------------------------------------

    // *** KEY OVERRIDE #1: UpdateTileMappings → no-op for committed resources ***
    void STDMETHODCALLTYPE UpdateTileMappings(
        ID3D12Resource*                        pResource,
        UINT                                   NumResourceRegions,
        const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
        const D3D12_TILE_REGION_SIZE*          pResourceRegionSizes,
        ID3D12Heap*                            pHeap,
        UINT                                   NumRanges,
        const D3D12_TILE_RANGE_FLAGS*          pRangeFlags,
        const UINT*                            pHeapRangeStartOffsets,
        const UINT*                            pRangeTileCounts,
        D3D12_TILE_MAPPING_FLAGS               Flags) override
    {
        if (IsReserved(pResource)) {
            D3DLog("UpdateTileMappings: no-op for committed res=%p regions=%u",
                   pResource, NumResourceRegions);
            return;
        }
        D3DLog("UpdateTileMappings: pass-through res=%p regions=%u",
               pResource, NumResourceRegions);
        m_real->UpdateTileMappings(pResource, NumResourceRegions,
                                   pResourceRegionStartCoordinates, pResourceRegionSizes,
                                   pHeap, NumRanges, pRangeFlags,
                                   pHeapRangeStartOffsets, pRangeTileCounts, Flags);
    }

    // *** KEY OVERRIDE #2: CopyTileMappings → no-op for committed resources ***
    void STDMETHODCALLTYPE CopyTileMappings(
        ID3D12Resource*                        pDstResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
        ID3D12Resource*                        pSrcResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE*          pRegionSize,
        D3D12_TILE_MAPPING_FLAGS               Flags) override
    {
        if (IsReserved(pDstResource) || IsReserved(pSrcResource)) {
            D3DLog("CopyTileMappings: no-op for committed dst=%p src=%p",
                   pDstResource, pSrcResource);
            return;
        }
        D3DLog("CopyTileMappings: pass-through dst=%p src=%p", pDstResource, pSrcResource);
        m_real->CopyTileMappings(pDstResource, pDstRegionStartCoordinate,
                                 pSrcResource, pSrcRegionStartCoordinate,
                                 pRegionSize, Flags);
    }

    void STDMETHODCALLTYPE ExecuteCommandLists(
        UINT NumCommandLists,
        ID3D12CommandList* const* ppCommandLists) override
    {
        std::vector<ID3D12CommandList*> unwrapped(NumCommandLists);
        for (UINT i = 0; i < NumCommandLists; i++) {
            void* p = nullptr;
            if (ppCommandLists[i] &&
                SUCCEEDED(ppCommandLists[i]->QueryInterface(
                    IID_CommandListWrapperMarker, &p))) {
                unwrapped[i] = static_cast<CommandListWrapper*>(p)->m_real;
                static_cast<CommandListWrapper*>(p)->Release();
            } else {
                unwrapped[i] = ppCommandLists[i];
            }
        }
        m_real->ExecuteCommandLists(NumCommandLists, unwrapped.data());
    }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override {
        m_real->SetMarker(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override {
        m_real->BeginEvent(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE EndEvent() override {
        m_real->EndEvent();
    }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override {
        return m_real->Signal(pFence, Value);
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) override {
        return m_real->Wait(pFence, Value);
    }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) override {
        return m_real->GetTimestampFrequency(pFrequency);
    }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(
        UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) override
    {
        return m_real->GetClockCalibration(pGpuTimestamp, pCpuTimestamp);
    }
    D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC* pDesc) override {
        return m_real->GetDesc(pDesc);
    }
};

// ============================================================================
// DeviceWrapper
// Wraps ID3D12Device through ID3D12Device5
// ============================================================================

class DeviceWrapper : public ID3D12Device5 {
public:
    ID3D12Device5* m_real;
    LONG           m_refs;

    explicit DeviceWrapper(ID3D12Device5* real)
        : m_real(real), m_refs(1)
    {
        D3DLog("DeviceWrapper created %p", real);
    }

    ~DeviceWrapper() {
        D3DLog("DeviceWrapper destroyed");
        m_real->Release();
    }

    // Helper: QI a raw command list up to CL4 and wrap it
    CommandListWrapper* WrapCommandList(ID3D12GraphicsCommandList* rawList) {
        ID3D12GraphicsCommandList4* cl4 = nullptr;
        HRESULT hr = rawList->QueryInterface(
            __uuidof(ID3D12GraphicsCommandList4), (void**)&cl4);
        if (SUCCEEDED(hr) && cl4) {
            D3DLog("WrapCommandList: QI to CL4 succeeded raw=%p cl4=%p", rawList, cl4);
            return new CommandListWrapper(cl4, m_real);
        }
        D3DLogW("WrapCommandList: QI to CL4 failed (hr=0x%08X), using base list raw=%p",
                (UINT)hr, rawList);
        rawList->AddRef();
        return new CommandListWrapper(
            reinterpret_cast<ID3D12GraphicsCommandList4*>(rawList), m_real);
    }

    // ------- IUnknown -------------------------------------------------------

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)      ||
            riid == __uuidof(ID3D12Object)  ||
            riid == __uuidof(ID3D12Device)  ||
            riid == __uuidof(ID3D12Device1) ||
            riid == __uuidof(ID3D12Device2) ||
            riid == __uuidof(ID3D12Device3) ||
            riid == __uuidof(ID3D12Device4) ||
            riid == __uuidof(ID3D12Device5)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refs);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }

    // ------- ID3D12Object ---------------------------------------------------

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return m_real->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return m_real->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return m_real->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return m_real->SetName(Name);
    }

    // ------- ID3D12Device ---------------------------------------------------

    UINT STDMETHODCALLTYPE GetNodeCount() override {
        return m_real->GetNodeCount();
    }

    // *** KEY OVERRIDE: CreateCommandQueue → return wrapped queue ***
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(
        const D3D12_COMMAND_QUEUE_DESC* pDesc,
        REFIID riid, void** ppCommandQueue) override
    {
        D3DLog("CreateCommandQueue type=%d priority=%d flags=0x%X nodeMask=0x%X",
               pDesc ? (int)pDesc->Type : -1,
               pDesc ? (int)pDesc->Priority : 0,
               pDesc ? (UINT)pDesc->Flags : 0u,
               pDesc ? pDesc->NodeMask : 0u);

        ID3D12CommandQueue* realQueue = nullptr;
        HRESULT hr = m_real->CreateCommandQueue(
            pDesc, __uuidof(ID3D12CommandQueue), (void**)&realQueue);
        if (FAILED(hr) || !realQueue) {
            if (ppCommandQueue) *ppCommandQueue = nullptr;
            D3DLogHR("CreateCommandQueue (real)", hr);
            return hr;
        }
        D3DLog("CreateCommandQueue: wrapping real=%p", realQueue);
        // realQueue refcount = 1 from CreateCommandQueue; wrapper takes ownership of
        // that reference. Do NOT call realQueue->Release() here — the wrapper's
        // destructor will call it when the wrapper is destroyed.
        CommandQueueWrapper* wrapper = new CommandQueueWrapper(realQueue);
        hr = wrapper->QueryInterface(riid, ppCommandQueue);
        wrapper->Release();
        if (FAILED(hr))
            D3DLogHR("CreateCommandQueue wrapper QI", hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override
    {
        return m_real->CreateCommandAllocator(type, riid, ppCommandAllocator);
    }

    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
        REFIID riid, void** ppPipelineState) override
    {
        return m_real->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
    }

    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
        REFIID riid, void** ppPipelineState) override
    {
        return m_real->CreateComputePipelineState(pDesc, riid, ppPipelineState);
    }

    // *** KEY OVERRIDE: CreateCommandList → return wrapped ***
    HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState*    pInitialState,
        REFIID riid, void**     ppCommandList) override
    {
        D3DLog("CreateCommandList type=%d nodeMask=0x%X alloc=%p",
               (int)type, nodeMask, pCommandAllocator);
        ID3D12GraphicsCommandList* rawList = nullptr;
        HRESULT hr = m_real->CreateCommandList(
            nodeMask, type, pCommandAllocator, pInitialState,
            __uuidof(ID3D12GraphicsCommandList), (void**)&rawList);
        if (FAILED(hr) || !rawList) {
            if (ppCommandList) *ppCommandList = nullptr;
            return D3DLogHR("CreateCommandList (real)", hr);
        }
        D3DLog("CreateCommandList: wrapping raw=%p", rawList);
        CommandListWrapper* wrapper = WrapCommandList(rawList);
        rawList->Release(); // WrapCommandList took its own ref via QI or AddRef
        hr = wrapper->QueryInterface(riid, ppCommandList);
        wrapper->Release();
        if (FAILED(hr)) D3DLogHR("CreateCommandList wrapper QI", hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D12_FEATURE Feature, void* pFeatureSupportData,
        UINT FeatureSupportDataSize) override
    {
        return m_real->CheckFeatureSupport(Feature, pFeatureSupportData,
                                           FeatureSupportDataSize);
    }

    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
        const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
        REFIID riid, void** ppvHeap) override
    {
        return m_real->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap);
    }

    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override
    {
        return m_real->GetDescriptorHandleIncrementSize(DescriptorHeapType);
    }

    HRESULT STDMETHODCALLTYPE CreateRootSignature(
        UINT nodeMask, const void* pBlobWithRootSignature,
        SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature) override
    {
        return m_real->CreateRootSignature(nodeMask, pBlobWithRootSignature,
                                           blobLengthInBytes, riid, ppvRootSignature);
    }

    void STDMETHODCALLTYPE CreateConstantBufferView(
        const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
    {
        m_real->CreateConstantBufferView(pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D12Resource* pResource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
    {
        m_real->CreateShaderResourceView(pResource, pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
    {
        m_real->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource* pResource,
        const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
    {
        m_real->CreateRenderTargetView(pResource, pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D12Resource* pResource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
    {
        m_real->CreateDepthStencilView(pResource, pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateSampler(
        const D3D12_SAMPLER_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override
    {
        m_real->CreateSampler(pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CopyDescriptors(
        UINT NumDestDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
        const UINT* pDestDescriptorRangeSizes,
        UINT NumSrcDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
        const UINT* pSrcDescriptorRangeSizes,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override
    {
        m_real->CopyDescriptors(NumDestDescriptorRanges,
                                pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                                NumSrcDescriptorRanges,
                                pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
                                DescriptorHeapsType);
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(
        UINT NumDescriptors,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
        D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override
    {
        m_real->CopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart,
                                      SrcDescriptorRangeStart, DescriptorHeapsType);
    }

    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo(
        D3D12_RESOURCE_ALLOCATION_INFO* pInfo,
        UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs) override
    {
        return m_real->GetResourceAllocationInfo(pInfo, visibleMask, numResourceDescs, pResourceDescs);
    }

    D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE GetCustomHeapProperties(
        D3D12_HEAP_PROPERTIES* pHeapProperties,
        UINT nodeMask, D3D12_HEAP_TYPE heapType) override
    {
        return m_real->GetCustomHeapProperties(pHeapProperties, nodeMask, heapType);
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES* pHeapProperties,
        D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riidResource, void** ppvResource) override
    {
        HRESULT hr = m_real->CreateCommittedResource(pHeapProperties, HeapFlags, pDesc,
                                                      InitialResourceState, pOptimizedClearValue,
                                                      riidResource, ppvResource);
        if (FAILED(hr) && pDesc)
            D3DLogE("CreateCommittedResource FAILED: hr=0x%08x HeapFlags=0x%x "
                    "Dim=%u W=%llu H=%u Fmt=%u ResFlags=0x%x",
                    (UINT)hr, (UINT)HeapFlags,
                    (UINT)pDesc->Dimension, pDesc->Width, pDesc->Height,
                    (UINT)pDesc->Format, (UINT)pDesc->Flags);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateHeap(
        const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override
    {
        HRESULT hr = m_real->CreateHeap(pDesc, riid, ppvHeap);
        if (pDesc)
            D3DLog("CreateHeap: size=%llu align=%llu flags=0x%x type=%u hr=0x%08x",
                   pDesc->SizeInBytes, pDesc->Alignment,
                   (UINT)pDesc->Flags, (UINT)pDesc->Properties.Type, (UINT)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreatePlacedResource(
        ID3D12Heap* pHeap, UINT64 HeapOffset,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override
    {
        // Convert ALL placed resources to committed to avoid DXMT placed-resource bugs.
        if (pDesc) {
            D3D12_HEAP_DESC heapDesc = {};
            pHeap->GetDesc(&heapDesc);
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type                 = heapDesc.Properties.Type;
            hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

            D3D12_RESOURCE_DESC desc = *pDesc;
            // Textures as placed use ROW_MAJOR or 64KB_UNDEFINED_SWIZZLE;
            // committed textures must use UNKNOWN.
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
                desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            HRESULT hr = m_real->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &desc,
                InitialState, pOptimizedClearValue, riid, ppvResource);
            if (SUCCEEDED(hr)) {
                static std::atomic<int> placedCount{0};
                int n = placedCount.fetch_add(1);
                if (n < 50) {
                    D3DLog("Placed→Committed[%d]: heapType=%u dim=%u fmt=%u size=%llux%u offset=%llu state=0x%x",
                        n, (unsigned)hp.Type, (unsigned)desc.Dimension, (unsigned)desc.Format,
                        (unsigned long long)desc.Width, (unsigned)desc.Height,
                        (unsigned long long)HeapOffset, (unsigned)InitialState);
                }
                return hr;
            }
            D3DLog("Placed→Committed FAILED hr=0x%08x dim=%u, falling back to placed",
                (unsigned)hr, (unsigned)desc.Dimension);
        }
        return m_real->CreatePlacedResource(pHeap, HeapOffset, pDesc,
                                            InitialState, pOptimizedClearValue,
                                            riid, ppvResource);
    }

    // *** KEY OVERRIDE: CreateReservedResource → CreateCommittedResource ***
    HRESULT STDMETHODCALLTYPE CreateReservedResource(
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES      InitialState,
        const D3D12_CLEAR_VALUE*   pOptimizedClearValue,
        REFIID riid, void**        ppvResource) override
    {
        D3DLog("CreateReservedResource: converting to committed. "
               "Dim=%u W=%llu H=%u D=%u Mips=%u Fmt=%u",
               (UINT)pDesc->Dimension, pDesc->Width, pDesc->Height,
               (UINT)pDesc->DepthOrArraySize, (UINT)pDesc->MipLevels,
               (UINT)pDesc->Format);

        D3D12_RESOURCE_DESC desc = *pDesc;
        // Reserved resources use 64KB_UNDEFINED_SWIZZLE layout.
        // Committed textures must use UNKNOWN (driver chooses optimal layout).
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type                 = D3D12_HEAP_TYPE_DEFAULT;
        hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        HRESULT hr = m_real->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc,
            InitialState, pOptimizedClearValue, riid, ppvResource);

        if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
            TrackReserved(reinterpret_cast<ID3D12Resource*>(*ppvResource));
            D3DLog("CreateReservedResource: committed OK ptr=%p", *ppvResource);
        } else {
            D3DLogE("CreateReservedResource: CreateCommittedResource failed 0x%08X, "
                    "falling back to CreateReservedResource", (UINT)hr);
            hr = m_real->CreateReservedResource(pDesc, InitialState,
                                                pOptimizedClearValue, riid, ppvResource);
            if (FAILED(hr))
                D3DLogE("CreateReservedResource: fallback also failed 0x%08X", (UINT)hr);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
        ID3D12DeviceChild* pObject,
        const SECURITY_ATTRIBUTES* pAttributes,
        DWORD Access, LPCWSTR Name, HANDLE* pHandle) override
    {
        return m_real->CreateSharedHandle(pObject, pAttributes, Access, Name, pHandle);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(
        HANDLE NTHandle, REFIID riid, void** ppvObj) override
    {
        return m_real->OpenSharedHandle(NTHandle, riid, ppvObj);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(
        LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) override
    {
        return m_real->OpenSharedHandleByName(Name, Access, pNTHandle);
    }
    HRESULT STDMETHODCALLTYPE MakeResident(
        UINT NumObjects, ID3D12Pageable* const* ppObjects) override
    {
        return m_real->MakeResident(NumObjects, ppObjects);
    }
    HRESULT STDMETHODCALLTYPE Evict(
        UINT NumObjects, ID3D12Pageable* const* ppObjects) override
    {
        return m_real->Evict(NumObjects, ppObjects);
    }
    HRESULT STDMETHODCALLTYPE CreateFence(
        UINT64 InitialValue, D3D12_FENCE_FLAGS Flags,
        REFIID riid, void** ppFence) override
    {
        return m_real->CreateFence(InitialValue, Flags, riid, ppFence);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
        return m_real->GetDeviceRemovedReason();
    }
    void STDMETHODCALLTYPE GetCopyableFootprints(
        const D3D12_RESOURCE_DESC*          pResourceDesc,
        UINT FirstSubresource, UINT NumSubresources,
        UINT64 BaseOffset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
        UINT*   pNumRows,
        UINT64* pRowSizeInBytes,
        UINT64* pTotalBytes) override
    {
        m_real->GetCopyableFootprints(pResourceDesc, FirstSubresource, NumSubresources,
                                      BaseOffset, pLayouts, pNumRows,
                                      pRowSizeInBytes, pTotalBytes);
    }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(
        const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override
    {
        return m_real->CreateQueryHeap(pDesc, riid, ppvHeap);
    }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override {
        return m_real->SetStablePowerState(Enable);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(
        const D3D12_COMMAND_SIGNATURE_DESC* pDesc,
        ID3D12RootSignature* pRootSignature,
        REFIID riid, void** ppvCommandSignature) override
    {
        HRESULT hr = m_real->CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature);
        if (pDesc && SUCCEEDED(hr)) {
            // Log each argument type in the signature
            for (UINT i = 0; i < pDesc->NumArgumentDescs; i++) {
                D3DLog("CreateCommandSignature: stride=%u arg[%u] type=%u ptr=%p",
                    pDesc->ByteStride, i,
                    (unsigned)pDesc->pArgumentDescs[i].Type,
                    ppvCommandSignature ? *ppvCommandSignature : nullptr);
            }
        }
        return hr;
    }
    void STDMETHODCALLTYPE GetResourceTiling(
        ID3D12Resource* pTiledResource,
        UINT* pNumTilesForEntireResource,
        D3D12_PACKED_MIP_INFO* pPackedMipDesc,
        D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
        UINT* pNumSubresourceTilings,
        UINT FirstSubresourceTilingToGet,
        D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override
    {
        m_real->GetResourceTiling(pTiledResource, pNumTilesForEntireResource,
                                  pPackedMipDesc, pStandardTileShapeForNonPackedMips,
                                  pNumSubresourceTilings, FirstSubresourceTilingToGet,
                                  pSubresourceTilingsForNonPackedMips);
    }
    LUID* STDMETHODCALLTYPE GetAdapterLuid(LUID* pLuid) override {
        return m_real->GetAdapterLuid(pLuid);
    }

    // ------- ID3D12Device1 --------------------------------------------------

    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(
        const void* pLibraryBlob, SIZE_T BlobLength,
        REFIID riid, void** ppPipelineLibrary) override
    {
        return m_real->CreatePipelineLibrary(pLibraryBlob, BlobLength, riid, ppPipelineLibrary);
    }
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
        ID3D12Fence* const* ppFences, const UINT64* pFenceValues,
        UINT NumFences, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags,
        HANDLE hEvent) override
    {
        return m_real->SetEventOnMultipleFenceCompletion(ppFences, pFenceValues,
                                                         NumFences, Flags, hEvent);
    }
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(
        UINT NumObjects,
        ID3D12Pageable* const* ppObjects,
        const D3D12_RESIDENCY_PRIORITY* pPriorities) override
    {
        return m_real->SetResidencyPriority(NumObjects, ppObjects, pPriorities);
    }

    // ------- ID3D12Device2 --------------------------------------------------

    HRESULT STDMETHODCALLTYPE CreatePipelineState(
        const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc,
        REFIID riid, void** ppPipelineState) override
    {
        return m_real->CreatePipelineState(pDesc, riid, ppPipelineState);
    }

    // ------- ID3D12Device3 --------------------------------------------------

    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(
        const void* pAddress, REFIID riid, void** ppvHeap) override
    {
        return m_real->OpenExistingHeapFromAddress(pAddress, riid, ppvHeap);
    }
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(
        HANDLE hFileMapping, REFIID riid, void** ppvHeap) override
    {
        return m_real->OpenExistingHeapFromFileMapping(hFileMapping, riid, ppvHeap);
    }
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(
        D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects,
        ID3D12Pageable* const* ppObjects,
        ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) override
    {
        return m_real->EnqueueMakeResident(Flags, NumObjects, ppObjects,
                                           pFenceToSignal, FenceValueToSignal);
    }

    // ------- ID3D12Device4 --------------------------------------------------

    // *** KEY OVERRIDE: CreateCommandList1 → return wrapped ***
    HRESULT STDMETHODCALLTYPE CreateCommandList1(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
        D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid, void** ppCommandList) override
    {
        D3DLog("CreateCommandList1 type=%d nodeMask=0x%X flags=0x%X",
               (int)type, nodeMask, (UINT)flags);
        ID3D12GraphicsCommandList* rawList = nullptr;
        HRESULT hr = m_real->CreateCommandList1(
            nodeMask, type, flags,
            __uuidof(ID3D12GraphicsCommandList), (void**)&rawList);
        if (FAILED(hr) || !rawList) {
            if (ppCommandList) *ppCommandList = nullptr;
            return D3DLogHR("CreateCommandList1 (real)", hr);
        }
        D3DLog("CreateCommandList1: wrapping raw=%p", rawList);
        CommandListWrapper* wrapper = WrapCommandList(rawList);
        rawList->Release();
        hr = wrapper->QueryInterface(riid, ppCommandList);
        wrapper->Release();
        if (FAILED(hr)) D3DLogHR("CreateCommandList1 wrapper QI", hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc,
        REFIID riid, void** ppSession) override
    {
        return m_real->CreateProtectedResourceSession(pDesc, riid, ppSession);
    }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(
        const D3D12_HEAP_PROPERTIES* pHeapProperties,
        D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riidResource, void** ppvResource) override
    {
        return m_real->CreateCommittedResource1(pHeapProperties, HeapFlags, pDesc,
                                                InitialResourceState, pOptimizedClearValue,
                                                pProtectedSession, riidResource, ppvResource);
    }
    HRESULT STDMETHODCALLTYPE CreateHeap1(
        const D3D12_HEAP_DESC* pDesc,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvHeap) override
    {
        HRESULT hr = m_real->CreateHeap1(pDesc, pProtectedSession, riid, ppvHeap);
        if (pDesc)
            D3DLog("CreateHeap1: size=%llu align=%llu flags=0x%x type=%u hr=0x%08x",
                   pDesc->SizeInBytes, pDesc->Alignment,
                   (UINT)pDesc->Flags, (UINT)pDesc->Properties.Type, (UINT)hr);
        return hr;
    }

    // *** KEY OVERRIDE: CreateReservedResource1 → CreateCommittedResource ***
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(
        const D3D12_RESOURCE_DESC*      pDesc,
        D3D12_RESOURCE_STATES           InitialState,
        const D3D12_CLEAR_VALUE*        pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void**             ppvResource) override
    {
        D3DLog("CreateReservedResource1: converting to committed. "
               "Dim=%u W=%llu H=%u Mips=%u Fmt=%u",
               (UINT)pDesc->Dimension, pDesc->Width, pDesc->Height,
               (UINT)pDesc->MipLevels, (UINT)pDesc->Format);

        D3D12_RESOURCE_DESC desc = *pDesc;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        HRESULT hr = m_real->CreateCommittedResource1(
            &hp, D3D12_HEAP_FLAG_NONE, &desc,
            InitialState, pOptimizedClearValue,
            pProtectedSession, riid, ppvResource);

        if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
            TrackReserved(reinterpret_cast<ID3D12Resource*>(*ppvResource));
            D3DLog("CreateReservedResource1: committed OK ptr=%p", *ppvResource);
        } else {
            D3DLogE("CreateReservedResource1: CreateCommittedResource1 failed 0x%08X, "
                    "falling back to CreateReservedResource1", (UINT)hr);
            hr = m_real->CreateReservedResource1(pDesc, InitialState, pOptimizedClearValue,
                                                 pProtectedSession, riid, ppvResource);
            if (FAILED(hr))
                D3DLogE("CreateReservedResource1: fallback also failed 0x%08X", (UINT)hr);
        }
        return hr;
    }

    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo1(
        D3D12_RESOURCE_ALLOCATION_INFO* pInfo,
        UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override
    {
        return m_real->GetResourceAllocationInfo1(pInfo, visibleMask, numResourceDescs,
                                                  pResourceDescs, pResourceAllocationInfo1);
    }

    // ------- ID3D12Device5 --------------------------------------------------

    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(
        ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) override
    {
        return m_real->CreateLifetimeTracker(pOwner, riid, ppvTracker);
    }
    void STDMETHODCALLTYPE RemoveDevice() override {
        m_real->RemoveDevice();
    }
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(
        UINT* pNumMetaCommands,
        D3D12_META_COMMAND_DESC* pDescs) override
    {
        return m_real->EnumerateMetaCommands(pNumMetaCommands, pDescs);
    }
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(
        REFGUID CommandId,
        D3D12_META_COMMAND_PARAMETER_STAGE Stage,
        UINT* pTotalStructureSizeInBytes,
        UINT* pParameterCount,
        D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) override
    {
        return m_real->EnumerateMetaCommandParameters(CommandId, Stage,
                                                      pTotalStructureSizeInBytes,
                                                      pParameterCount, pParameterDescs);
    }
    HRESULT STDMETHODCALLTYPE CreateMetaCommand(
        REFGUID CommandId, UINT NodeMask,
        const void* pCreationParametersData,
        SIZE_T CreationParametersDataSizeInBytes,
        REFIID riid, void** ppMetaCommand) override
    {
        return m_real->CreateMetaCommand(CommandId, NodeMask,
                                         pCreationParametersData,
                                         CreationParametersDataSizeInBytes,
                                         riid, ppMetaCommand);
    }
    HRESULT STDMETHODCALLTYPE CreateStateObject(
        const D3D12_STATE_OBJECT_DESC* pDesc,
        REFIID riid, void** ppStateObject) override
    {
        return m_real->CreateStateObject(pDesc, riid, ppStateObject);
    }
    void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) override
    {
        m_real->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo);
    }
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) override
    {
        return m_real->CheckDriverMatchingIdentifier(SerializedDataType, pIdentifierToCheck);
    }
};

// ============================================================================
// Note on CreateCommandQueue refcount
// m_real->CreateCommandQueue returns a ref-count-1 pointer.
// CommandQueueWrapper stores it (takes ownership). We must NOT call
// realQueue->Release() after constructing the wrapper.
// The Release() call in the CreateCommandQueue override above was a bug —
// it is intentionally absent in the final version below.
// ============================================================================

// ============================================================================
// Exported functions
// ============================================================================

extern "C" {

HRESULT WINAPI D3D12CreateDevice(
    IUnknown*        pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID           riid,
    void**           ppDevice)
{
    if (!LoadRealD3D12() || !g_realCreateDevice) return E_FAIL;

    D3DLog("D3D12CreateDevice pAdapter=%p minFL=0x%X ppDevice=%p",
           pAdapter, MinimumFeatureLevel, ppDevice);

    // Probe call: ppDevice == NULL means the game is just checking whether the
    // requested feature level is supported. Forward directly to the real DLL.
    if (!ppDevice) {
        HRESULT hr = g_realCreateDevice(pAdapter, MinimumFeatureLevel, riid, nullptr);
        D3DLog("D3D12CreateDevice: probe result 0x%08X", (UINT)hr);
        return hr;
    }

    // Create real device at ID3D12Device5 level
    ID3D12Device5* realDevice = nullptr;
    HRESULT hr = g_realCreateDevice(pAdapter, MinimumFeatureLevel,
                                    __uuidof(ID3D12Device5), (void**)&realDevice);
    if (FAILED(hr) || !realDevice) {
        D3DLogW("Real CreateDevice at Device5 level failed (0x%08X), "
                "retrying with requested riid", (UINT)hr);
        hr = g_realCreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
        if (FAILED(hr))
            D3DLogE("D3D12CreateDevice fallback also failed 0x%08X", (UINT)hr);
        return hr;
    }

    D3DLog("D3D12CreateDevice: real device=%p, wrapping", realDevice);
    DeviceWrapper* wrapper = new DeviceWrapper(realDevice);

    hr = wrapper->QueryInterface(riid, ppDevice);
    wrapper->Release();

    if (SUCCEEDED(hr))
        D3DLog("D3D12CreateDevice: DeviceWrapper returned %p", *ppDevice);
    else
        D3DLogE("D3D12CreateDevice: wrapper QI failed 0x%08X", (UINT)hr);

    return hr;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug)
{
    if (!LoadRealD3D12() || !g_realGetDebugInterface) return E_NOTIMPL;
    return g_realGetDebugInterface(riid, ppvDebug);
}

HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION       Version,
    ID3DBlob**                       ppBlob,
    ID3DBlob**                       ppErrorBlob)
{
    if (!LoadRealD3D12() || !g_realSerializeRootSig) return E_NOTIMPL;
    return g_realSerializeRootSig(pRootSignature, Version, ppBlob, ppErrorBlob);
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob**                                 ppBlob,
    ID3DBlob**                                 ppErrorBlob)
{
    if (!LoadRealD3D12() || !g_realSerializeVersionedRootSig) return E_NOTIMPL;
    return g_realSerializeVersionedRootSig(pRootSignature, ppBlob, ppErrorBlob);
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes,
    REFIID  pRootSignatureDeserializerInterface,
    void**  ppRootSignatureDeserializer)
{
    if (!LoadRealD3D12() || !g_realCreateRootSigDes) return E_NOTIMPL;
    return g_realCreateRootSigDes(pSrcData, SrcDataSizeInBytes,
                                  pRootSignatureDeserializerInterface,
                                  ppRootSignatureDeserializer);
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes,
    REFIID  pRootSignatureDeserializerInterface,
    void**  ppRootSignatureDeserializer)
{
    if (!LoadRealD3D12() || !g_realCreateVersionedRootSigDes) return E_NOTIMPL;
    return g_realCreateVersionedRootSigDes(pSrcData, SrcDataSizeInBytes,
                                           pRootSignatureDeserializerInterface,
                                           ppRootSignatureDeserializer);
}

HRESULT WINAPI D3D12GetInterface(REFCLSID rclsid, REFIID riid, void** ppvDebug)
{
    if (!LoadRealD3D12() || !g_realGetInterface) return E_NOTIMPL;
    return g_realGetInterface(rclsid, riid, ppvDebug);
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT       NumFeatures,
    const IID* pIIDs,
    void*      pConfigurationStructs,
    UINT*      pConfigurationStructSizes)
{
    if (!LoadRealD3D12() || !g_realEnableExperimental) return S_OK;
    return g_realEnableExperimental(NumFeatures, pIIDs,
                                   pConfigurationStructs, pConfigurationStructSizes);
}

// Pass-throughs for functions exported by CrossOver's d3d12.dll that the game
// may call by ordinal. We have no reason to intercept them.

HRESULT WINAPI GetBehaviorValue(LPCSTR pName, UINT64* pValue)
{
    D3DLog("GetBehaviorValue: %s", pName ? pName : "(null)");
    if (!LoadRealD3D12() || !g_realGetBehaviorValue) {
        if (pValue) *pValue = 0;
        return E_NOTIMPL;
    }
    return g_realGetBehaviorValue(pName, pValue);
}

HRESULT WINAPI D3D12CoreCreateLayeredDevice(
    const void* pBlob, UINT blobSize, const void* pOuter, REFIID riid, void** ppOut)
{
    D3DLog("D3D12CoreCreateLayeredDevice blobSize=%u", blobSize);
    if (!LoadRealD3D12() || !g_realCoreCreateLayeredDevice) return E_NOTIMPL;
    return g_realCoreCreateLayeredDevice(pBlob, blobSize, pOuter, riid, ppOut);
}

SIZE_T WINAPI D3D12CoreGetLayeredDeviceSize(const void* pBlob, UINT blobSize)
{
    D3DLog("D3D12CoreGetLayeredDeviceSize blobSize=%u", blobSize);
    if (!LoadRealD3D12() || !g_realCoreGetLayeredDeviceSize) return 0;
    return g_realCoreGetLayeredDeviceSize(pBlob, blobSize);
}

HRESULT WINAPI D3D12CoreRegisterLayers(const void* pBlob, UINT blobSize)
{
    D3DLog("D3D12CoreRegisterLayers blobSize=%u", blobSize);
    if (!LoadRealD3D12() || !g_realCoreRegisterLayers) return E_NOTIMPL;
    return g_realCoreRegisterLayers(pBlob, blobSize);
}

} // extern "C"

// ============================================================================
// DllMain
// ============================================================================

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        g_logStartTick = GetTickCount();
        InitializeCriticalSection(&g_logCs);
        g_logCsInited = true;
        InitializeCriticalSection(&g_reservedCs);
        g_reservedCsInited = true;
        LogOpen();
        D3DLog("=== d3d12_hook.dll loaded - tiled resource fix for CrossOver/macOS ===");
        LoadRealD3D12();
    } else if (reason == DLL_PROCESS_DETACH) {
        D3DLog("=== d3d12_hook.dll unloaded ===");
        if (g_log) {
            fclose(g_log);
            g_log = nullptr;
        }
        if (g_reservedCsInited) {
            DeleteCriticalSection(&g_reservedCs);
            g_reservedCsInited = false;
        }
        if (g_logCsInited) {
            DeleteCriticalSection(&g_logCs);
            g_logCsInited = false;
        }
    }
    return TRUE;
}
