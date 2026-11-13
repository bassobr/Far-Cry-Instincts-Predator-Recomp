#!/usr/bin/env python3
"""Generates config/predator/d3d_symbols.toml: names for the D3D9 library in default.xex (XDK 2.0.2417).

    names.py            writes the TOML file and reports the number of names per confidence level

Sources of the names:
- Setters/getters of the render and sampler states: automatically from the table in the image (0x828A0208,
  97 entries, and 0x828A0698, 20 entries, each {getter, setter, default value}); the order of the states comes
  from King Kong Recompiled, native-renderer/d3d_api.json (MIT, TekRantGaming and contributors): the same
  117 slots there.
- All other functions: hand-written table MANUAL (address, name, confidence, evidence). The evidence was
  measured with tools/d3d/sig.py, q.py, args.py, libfuncs.py.

Confidence: `certain` = unambiguous evidence (import, packet, table slot, object identifier);
`likely` = the evidence fits, the boundary to a neighboring function is not proven;
`assumed` = inferred from location and call graph.
"""
import os
from collections import Counter

import d3dlib

RS_TABLE, RS_COUNT = 0x828A0208, 97
SS_TABLE, SS_COUNT = 0x828A0698, 20

RS_NAMES = {10: "ZENABLE", 11: "ZFUNC", 12: "ZWRITEENABLE", 13: "FILLMODE", 14: "CULLMODE", 15: "ALPHABLENDENABLE",
            16: "SEPARATEALPHABLENDENABLE", 17: "BLENDFACTOR", 18: "SRCBLEND", 19: "DESTBLEND", 20: "BLENDOP",
            21: "SRCBLENDALPHA", 22: "DESTBLENDALPHA", 23: "BLENDOPALPHA", 24: "ALPHATESTENABLE", 25: "ALPHAREF",
            26: "ALPHAFUNC", 27: "STENCILENABLE", 28: "TWOSIDEDSTENCILMODE", 29: "STENCILFAIL", 30: "STENCILZFAIL",
            31: "STENCILPASS", 32: "STENCILFUNC", 33: "STENCILREF", 34: "STENCILMASK", 35: "STENCILWRITEMASK",
            36: "CCW_STENCILFAIL", 37: "CCW_STENCILZFAIL", 38: "CCW_STENCILPASS", 39: "CCW_STENCILFUNC",
            40: "CCW_STENCILREF", 41: "CCW_STENCILMASK", 42: "CCW_STENCILWRITEMASK", 43: "CLIPPLANEENABLE",
            44: "POINTSIZE", 45: "POINTSIZE_MIN", 46: "POINTSPRITEENABLE", 47: "POINTSIZE_MAX",
            48: "MULTISAMPLEANTIALIAS", 49: "MULTISAMPLEMASK", 50: "SCISSORTESTENABLE", 51: "SLOPESCALEDEPTHBIAS",
            52: "DEPTHBIAS", 53: "COLORWRITEENABLE", 54: "COLORWRITEENABLE1", 55: "COLORWRITEENABLE2",
            56: "COLORWRITEENABLE3", 57: "UNKNOWN_228", 58: "TESSELLATIONMODE", 59: "MINTESSELLATIONLEVEL",
            60: "MAXTESSELLATIONLEVEL", 77: "VIEWPORTENABLE", 78: "HIGHPRECISIONBLENDENABLE",
            79: "HIGHPRECISIONBLENDENABLE1", 80: "HIGHPRECISIONBLENDENABLE2", 81: "HIGHPRECISIONBLENDENABLE3",
            82: "HALFPIXELOFFSET", 83: "PRIMITIVERESETENABLE", 84: "PRIMITIVERESETINDEX", 85: "ALPHATOMASKENABLE",
            86: "ALPHATOMASKOFFSETS", 87: "GUARDBAND_X", 88: "GUARDBAND_Y", 89: "DISCARDBAND_X", 90: "DISCARDBAND_Y",
            91: "HISTENCILENABLE", 92: "HISTENCILWRITEENABLE", 93: "HISTENCILFUNC", 94: "HISTENCILREF",
            95: "PRESENTINTERVAL", 96: "PRESENTIMMEDIATETHRESHOLD"}
for _i in range(16):
    RS_NAMES[61 + _i] = "WRAP%d" % _i
SS_NAMES = ["ADDRESSU", "ADDRESSV", "ADDRESSW", "BORDERCOLOR", "MAGFILTER", "MINFILTER", "MIPFILTER", "MIPMAPLODBIAS",
            "MAXMIPLEVEL", "MAXANISOTROPY", "MAGFILTERZ", "MINFILTERZ", "SEPARATEZFILTERENABLE", "MINMIPLEVEL",
            "TRILINEARTHRESHOLD", "ANISOTROPYBIAS", "HGRADIENTEXPBIAS", "VGRADIENTEXPBIAS", "WHITEBORDERCOLORW",
            "POINTBORDERENABLE"]

S, W, V = "certain", "likely", "assumed"

MANUAL = [
    # --- Device: creation, teardown, ownership ---------------------------------------------------------------
    (0x82740698, "Direct3D_CreateDevice", S, "allocates 0x5080 = 20,608 bytes, calls init 0x82744470; 1 caller (engine 0x823AF430)"),
    (0x82744470, "D3D::InitializeDevice", S, "calls VdInitializeEDRAM, VdGetGraphicsAsicID, VdIsHSIOTrainingSucceeded, ring buffer and state init"),
    (0x82743F98, "D3D::InitializeDeviceState", S, "copies the setter/getter table to device+96/+564 and calls every setter with its default value"),
    (0x82743ED0, "D3D::InitializeEngines", S, "calls VdInitializeEngines, VdSetGraphicsInterruptCallback"),
    (0x82743EA8, "D3D::TitleTerminateNotification", S, "calls VdShutdownEngines; registered as a pointer"),
    (0x82743F48, "D3D::InitializeThreadOwnership", V, "sets +0x2888 (owner thread), +12"),
    (0x82738968, "D3D::InitializeRingBuffer", S, "calls VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack, VdSetSystemCommandBufferGpuIdentifierAddress; PM4 ME_INIT (0xC0114800)"),
    (0x827373F8, "D3D::GraphicsInterruptCallback", S, "string 'Unanticipated CPU_INTERRUPT', reads 0x7FC86544; registered in InitializeEngines"),
    (0x82744688, "D3D::UninitializeDevice", S, "calls VdShutdownEngines, VdSetGraphicsInterruptCallback, frees shaders and buffers"),
    (0x827440E8, "D3D::CreateDefaultSurfaces", W, "calls VdQueryVideoMode, CreateTexture, CreateRenderTarget, SetRenderTarget, SetDepthStencilSurface"),
    (0x827442A0, "D3D::ReleaseDefaultSurfaces", V, "sets RT and depth to 0, BlockUntilIdle, Release"),
    (0x8274B820, "D3D::EmitDefaultRegisters", V, "writes the base state (SQ_VS_CONST 0xFF000, SQ_PS_CONST 0xFF100) into the command buffer"),
    (0x827404D0, "D3DDevice_Release", W, "decrements +12, at 0 UninitializeDevice and free"),
    (0x827404B8, "D3DDevice_AddRef", W, "increments +12"),
    (0x82740528, "D3DDevice_AcquireThreadOwnership", S, "writes the thread id to +0x2888"),
    (0x82740568, "D3DDevice_ReleaseThreadOwnership", S, "clears +0x2888; 27 engine callers"),
    (0x82740578, "D3DDevice_Suspend", V, "critical section, flag in +0x28C2, BlockUntilIdle"),
    (0x82740610, "D3DDevice_Resume", V, "counterpart of Suspend"),
    (0x82740720, "D3DDevice_PersistDisplay", W, "calls VdPersistDisplay, MmAllocatePhysicalMemoryEx, Swap"),
    (0x827408F0, "D3DDevice_GetDeviceCaps", W, "memcpy of a static block (0x82026A68)"),
    # --- Command buffer ------------------------------------------------------------------------------------
    (0x82738618, "D3D::AdvanceCommandBufferSegment", S, "segment switch: 75 callers after `cmplw [dev+0],[dev+8]`; calls Kick, returns the new write pointer"),
    (0x82737A30, "D3D::KickSegment", S, "PM4 type-3 INDIRECT_BUFFER (0xC0013F00), writes CP_RB_WPTR (0x7FC80714)"),
    (0x82737C10, "D3D::EmitSegmentSync", V, "PM4 WAIT_REG_MEM (0xC0043C00), register 0xA2F/0xA31"),
    (0x82737DA8, "D3D::EmitFence", W, "PM4 EVENT_WRITE_SHD (0xC0025800), increments fence +0x289C"),
    (0x82737E80, "D3D::SubmitSegment", V, "spinlock, calls KickSegment"),
    (0x82737968, "D3D::ReclaimSegments", V, "reads the fence write-back address +0x2890, 9 callers"),
    (0x82737FC8, "D3D::BlockOnFence", W, "compares with *(+0x2890), calls the segment switch; 12 callers"),
    (0x827380B0, "D3D::EmitCallback", S, "writes registers 0x57C/0x57D (CALLBACK_ADDRESS/CONTEXT) and PM4 INTERRUPT (0xC0005401)"),
    (0x82738290, "D3D::WaitForCommandBufferSpace", V, "8 callers, calls ReclaimSegments"),
    (0x827384D8, "D3D::StartNewSegment", V, "calls EmitFence, SubmitSegment"),
    (0x82738700, "D3DDevice_InsertFence", W, "returns +0x289C, records it in +0x28BC, kicks; 2 engine callers"),
    (0x82738738, "D3DDevice_KickOff", V, "calls the segment switch"),
    (0x82738830, "D3D::ReserveCommandBuffer", W, "8 callers, returns a pointer for n words"),
    (0x82738898, "D3DDevice_BlockUntilIdle", W, "writes WAIT_UNTIL (0x5C8 = 0x20000), blocks on the current fence; 14 internal callers"),
    (0x82738230, "D3DResource_BlockUntilNotBusy", W, "blocks on the resource's fence (+4); 2 engine callers"),
    (0x827381D0, "D3D::BlockOnFenceGlobal", V, "like BlockOnFence via VdGlobalDevice"),
    (0x82738910, "D3DDevice_InsertCallback", S, "(dev, type, callback, context) -> EmitCallback; 2 engine callers"),
    (0x827470B8, "D3D::EmitSwapFence", V, "PM4 EVENT_WRITE_SHD with 0xDEADBEEF"),
    (0x82748570, "D3D::CopyToSecondaryBuffer", V, "tiling: switch to the secondary buffer"),
    (0x82748660, "D3D::ReplaySecondaryBuffer", V, "tiling: replay per tile"),
    # --- Shaders, constants, declaration -------------------------------------------------------------------
    (0x827310A8, "D3DDevice_SetVertexShaderConstantF", S, "target dev+1920+16*reg ((reg+0x78)<<4), VMX copy, mask +16"),
    (0x827311F0, "D3DDevice_SetPixelShaderConstantF", W, "twin directly behind it; engine wrapper 0x823B2880 jumps here"),
    (0x82731338, "D3DDevice_SetVertexShaderConstantB", S, "bits in dev+10112 ((reg>>5)+0x9E0)*4, mask +32 bit 31"),
    (0x82731390, "D3DDevice_SetPixelShaderConstantB", W, "twin"),
    (0x827313E8, "D3DDevice_SetVertexShaderConstantI", W, "packs 3 bytes per register, mask +32"),
    (0x82731438, "D3DDevice_SetPixelShaderConstantI", W, "twin"),
    (0x82731488, "D3DPixelShader_Release", W, "object with a 52-byte header: count +4, microcode +12, flags +28"),
    (0x82731540, "D3DVertexShader_Release", W, "object with a 592-byte header: count +4, microcode +40, flags +588"),
    (0x827315F8, "XGGetShaderSizes", V, "reads 3 header words, writes 4 outputs"),
    (0x82731648, "D3DDevice_SetPixelShader", W, "binds to dev+0x3290 (12944), 52-byte header, literal mask to +24; 5 engine callers"),
    (0x82731790, "D3D::PrepareVertexShader", V, "fills the 592-byte header"),
    (0x82731860, "D3D::BindVertexShader", W, "from SetVertexShader"),
    (0x827319B8, "D3DDevice_SetVertexShader", W, "records it in dev+0x4FFC (20476), frees the old one via VertexShader_Release; 7 engine callers"),
    (0x82731A20, "D3DDevice_CreateVertexDeclaration", W, "allocation + memcpy of the elements; 1 engine caller"),
    (0x82731AE0, "D3DVertexDeclaration_Release", W, "count +4"),
    (0x82731B20, "D3DDevice_SetShaderGPRAllocation", W, "writes SQ_GPR_MANAGEMENT (0xD00), records +0x28A4"),
    (0x82732CA8, "D3DDevice_CreatePixelShader", W, "small header, copies container and microcode; 6 engine callers"),
    (0x82732D98, "D3DDevice_CreateVertexShader", W, "large header (memset), calls PrepareVertexShader; 3 engine callers"),
    (0x82732E70, "D3DDevice_SetVertexDeclaration", S, "dev+0x2C90 (11408), mask +48; 37 engine callers"),
    (0x8272FA00, "D3D::BuildDeclarationFromFVF", S, "D3DDECLTYPE constants 0x2A23B9, 0x182886, 0x2C23A5, 0x1A23A6"),
    (0x82733598, "D3DDevice_SetFVF", W, "records the FVF in +0x30F0, builds the declaration in +0x3010, SetVertexDeclaration"),
    (0x82730E70, "XGMicrocodeWalk", V, "microcode walk with callback; callers D3DX and XGRAPHC"),
    (0x82733F60, "ShaderAssembler_Create", V, "allocates 0x2640 bytes"),
    (0x827336A8, "ShaderAssembler_Link", V, "2232 bytes, error 0x88760B81"),
    (0x82745C00, "D3D::BindShaders", W, "PM4 INVALIDATE_STATE (0x3B), IM_LOAD (0x27), shader linkage"),
    (0x82745798, "D3D::LoadPatchedVertexShader", W, "PM4 IM_LOAD_IMMEDIATE (0xC0002B00), copies the microcode into the buffer"),
    (0x827451B8, "D3D::PatchVertexFetches", V, "1500 bytes, rewrites vfetch instructions according to the declaration"),
    (0x827448F8, "D3D::LinkInterpolators", V, "676 bytes, from LoadPatchedVertexShader"),
    # --- Resources -----------------------------------------------------------------------------------------
    (0x8273C928, "D3DResource_AddRef", S, "common +0 up, surface also parent +44 (recursive)"),
    (0x8273CEA8, "D3DResource_Release", S, "common +0 down, parent +44, calls FreeResource; 53 engine and 27 D3DX callers"),
    (0x8273C990, "D3DResource_GetType", W, "reads common bits 16-18"),
    (0x8273CA40, "D3D::FreeResource", W, "free after fence, EDRAM return; 12 internal callers"),
    (0x8273CBA0, "D3D::LockResource", W, "waits for the GPU (WAIT_REG_MEM), lock count"),
    (0x8273CDC0, "D3D::UnlockResource", W, "cache flush of the locked range"),
    (0x8273CF28, "D3DResource_GetDevice", W, "returns the global device, AddRef"),
    (0x8273CF90, "D3DDevice_CreateVertexBuffer", S, "common 0x01010001 / 0x41010001; 10 engine callers"),
    (0x8273D040, "D3DVertexBuffer_Lock", W, "LockResource; 11 engine callers"),
    (0x8273D0B0, "D3DVertexBuffer_Unlock", W, "UnlockResource; 12 engine callers"),
    (0x8273D0C0, "D3DDevice_CreateIndexBuffer", S, "common 0x01020001 / 0x41020001; 7 engine callers"),
    (0x8273D170, "D3DIndexBuffer_Lock", W, "LockResource"),
    (0x8273D1D8, "D3DIndexBuffer_Unlock", W, "UnlockResource"),
    (0x82734F18, "XGGetBaseTextureDimensions", W, "3 output pointers from fetch word 2"),
    (0x82735050, "D3DTexture_UnlockRect", W, "jump to UnlockResource; 8 engine and 8 D3DX callers"),
    (0x82735068, "D3DCubeTexture_UnlockRect", V, "twin"),
    (0x82735078, "D3DSurface_UnlockRect", V, "via parent +44"),
    (0x82735090, "XGGetBlockDimensions", W, "2 output pointers"),
    (0x82735128, "D3DBaseTexture_GetFormat", W, "builds the D3DFORMAT from the fetch header"),
    (0x82735238, "XGGetTextureLevelSize", W, "7 internal callers"),
    (0x827365A0, "XGSetTextureHeader", S, "writes common 0x01030001 / 0x41030001"),
    (0x82735D98, "D3D::GetLevelDesc", W, "fills D3DSURFACE_DESC (8 fields)"),
    (0x82735ED0, "D3D::LockTextureLevel", W, "1184 bytes, from LockRect/GetSurfaceLevel"),
    (0x82736870, "D3DBaseTexture_GetLevelCount", W, "reads +0x20"),
    (0x82736880, "D3DTexture_GetSurfaceLevel", W, "allocates a surface, AddRef parent; 5 engine callers"),
    (0x82736928, "D3DTexture_LockRect", W, "jump into the lock core"),
    (0x82736940, "D3DCubeTexture_GetCubeMapSurface", W, "twin of GetSurfaceLevel"),
    (0x827369E8, "D3DTexture_GetLevelDesc", W, "jump to GetLevelDesc; 8 engine callers"),
    (0x827369F0, "D3DCubeTexture_LockRect", V, "jump into the lock core"),
    (0x827369F8, "D3DVolumeTexture_GetLevelDesc", V, "jump"),
    (0x82736A00, "D3DVolumeTexture_GetVolumeLevel", V, "twin"),
    (0x82736AA8, "D3DVolumeTexture_LockBox", V, "jump"),
    (0x82736AB0, "D3DDevice_CreateTexture", S, "(width, height, depth, levels, usage, format, pool, type) -> XGSetTextureHeader; 11 engine callers"),
    (0x82736BD0, "D3DDevice_CreateRenderTarget", S, "common 0x01040001, EDRAM allocator 0x827437C0; 1 engine caller (0x823ADC98)"),
    (0x82736ED8, "D3DSurface_GetDesc", W, "via parent +44"),
    (0x82736F30, "D3DSurface_LockRect", W, "via parent +44"),
    (0x82736F58, "D3DVolume_GetDesc", V, ""),
    (0x82736FA0, "D3DVolume_LockBox", V, ""),
    (0x82736FC0, "D3DDevice_SetTexture", S, "fetch constant dev+1152+24*s, pointer dev+(s+0xC68)*4 = 12704+4s; called for 26 samplers during init"),
    (0x827437C0, "D3D::AllocateEdram", W, "critical section 0x828BDA28, from CreateRenderTarget"),
    (0x82743770, "D3D::FreeEdram", W, "critical section 0x828BDA28"),
    # --- State -----------------------------------------------------------------------------------------------
    (0x8273BBE0, "D3DDevice_SetScissorRect", W, "RECT to dev+0x3220 (12832), calls the window scissor writer"),
    (0x8273BCA8, "D3DDevice_SetViewport", W, "584 bytes, viewport dev+0x3208 (12808), calls the SetScissorRect core; 1 engine caller"),
    (0x8273BEF0, "D3DDevice_SetStreamSource", S, "dev+0x310C+8*s {offset, buffer}, stride/4 to dev+0x3190+s, mask +48 bit 10"),
    (0x8273BF90, "D3DDevice_SetIndices", S, "dev+0x30F4 (12532)"),
    (0x8273C010, "D3DDevice_SetGammaRamp", W, "copy dev+0x36FC (14076), calls the gamma computation"),
    (0x8273C0A0, "D3DDevice_SetPWLGamma", V, "twin with a second table format"),
    (0x8273C130, "D3DDevice_SetClipPlane", W, "writes directly into the command buffer (WAIT_UNTIL 0x5C8 + PA_CL_UCP); 3 engine callers"),
    (0x8273C200, "D3D::EmitBinMaskSelect", V, "PM4 SET_BIN_MASK (0x50), SET_BIN_SELECT (0x51)"),
    (0x8273C310, "D3D::ResetViewportAndScissor", W, "after an RT change: viewport and scissor to the whole surface"),
    (0x8273C3E8, "D3D::SetRenderTargetInternal", W, "872 bytes, dev+0x30F8+4*i, register 0x2001.."),
    (0x8273C920, "D3DDevice_SetRenderTarget", W, "jump into the core; 1 engine caller (0x823AE110)"),
    (0x8273C750, "D3DDevice_SetDepthStencilSurface", S, "dev+0x3108 (12552), calls the ZENABLE and STENCILENABLE setters again"),
    (0x82739598, "D3D::EmitWindowScissor", W, "register 0x2080 (3 words) directly into the buffer; 5 internal callers"),
    # --- Draw ------------------------------------------------------------------------------------------------
    (0x827461D0, "D3D::FlushPendingState", S, "walks the 5 dirty masks, register blocks 0x2000/0x2100/0x2180/0x2200/0x2280/0x2300/0x2380/0x4000/0x4400/0x4800/0x4900; called by all 3 draws"),
    (0x82745A40, "D3D::FlushRegisterBlocks", W, "part of the flush for 0x2000..0x2380"),
    (0x82744CC0, "D3D::EmitRegisterRun", W, "type-0 packet of variable length"),
    (0x82744D78, "D3D::EmitRegisterRunMasked", W, "15 callers"),
    (0x82744E38, "D3D::EmitConstantRun", V, "0x00025000"),
    (0x82744F70, "D3D::EmitFetchConstants", V, ""),
    (0x82738D90, "D3D::BeginVerticesInternal", W, "fetch constant 0x48BA (6 words), PM4 DRAW_INDX (0xC0012201), returns a pointer into the command buffer"),
    (0x82739030, "D3DDevice_DrawVerticesUP", S, "(dev, type, count, data, stride): BeginVertices + memcpy(count*stride); 25 engine callers"),
    (0x827390B8, "D3DDevice_DrawVertices", W, "PM4 DRAW_INDX with 2 words (auto index); 2 engine callers"),
    (0x827392F8, "D3DDevice_DrawIndexedVertices", W, "PM4 DRAW_INDX with 4 words (index buffer); 12 engine callers, types 4 and 6"),
    (0x8273DAC8, "D3D::DrawInternalRect", V, "PM4 IM_LOAD_IMMEDIATE + DRAW_INDX_2 with a built-in shader"),
    (0x8273F5D8, "D3D::EmitRectState", V, "register 0x2100, 0x2204, 0x2312"),
    # --- Clear, resolve, tiling ----------------------------------------------------------------------------
    (0x8273DEE8, "D3DDevice_ClearF", W, "1836 bytes, built-in shaders (IM_LOAD_IMMEDIATE), DRAW_INDX_2; callers engine 0x823AE6E8, BeginTiling, Resolve"),
    (0x8273F6E8, "D3DDevice_Resolve", S, "3532 bytes, RB_COPY registers 0x2318..0x231B, DRAW_INDX_2; 8 engine callers with the target texture in r6"),
    (0x82740978, "D3D::EmitDepthClear", V, "PM4 EVENT_WRITE_ZPD (0x5B), register 0x2000, 0x2208, 0x2325"),
    (0x8273D1E8, "D3DDevice_SetPredication", W, "records the mask in dev+0x32A0, PM4 SET_BIN_MASK; 6 engine callers"),
    (0x8273D320, "D3DDevice_BeginTiling", W, "1076 bytes, calls SetViewport, ClearF, secondary buffer; 1 engine caller (0x823AE6E8)"),
    (0x8273D758, "D3DDevice_EndTiling", W, "880 bytes, calls Resolve, SetPredication, replay per tile; 1 engine caller (0x823AE988)"),
    # --- Present, display, gamma ---------------------------------------------------------------------------
    (0x8273E7C8, "D3DDevice_SynchronizeToPresentationInterval", W, "reads dev+0x35DC, WAIT_REG_MEM; contains the canary patch site 0x8273E89C"),
    (0x8273E938, "D3DDevice_Swap", S, "calls VdSwap, VdGetSystemCommandBuffer, VdPersistDisplay; 1 engine caller (0x823AEBD0)"),
    (0x8273E618, "D3D::EmitFrontBufferFormat", V, "PM4 REG_RMW (0x21), formats 0x28280136"),
    (0x8273E6F8, "D3D::VBlankCallback", V, "reads 0x7FC86530; as a pointer in SynchronizeToPresentationInterval"),
    (0x82744390, "D3D::SetDisplayMode", W, "calls VdSetDisplayMode, VdGetCurrentDisplayInformation"),
    (0x8274A630, "D3D::InitializeScaler", S, "calls VdInitializeScalerCommandBuffer"),
    (0x8274A0B0, "D3D::ComputeScalerParameters", V, "calls VdQueryVideoMode, reads VdGpuClockInMHz"),
    (0x82748A60, "D3D::QueryVideoFlags", S, "calls VdQueryVideoFlags"),
    (0x82748C10, "D3D::NotifyDisplayChange", S, "calls VdCallGraphicsNotificationRoutines"),
    (0x82748978, "D3D::RetrainEdram", S, "calls VdRetrainEDRAM, VdRetrainEDRAMWorker"),
    (0x82741600, "D3D::SetClockGating", S, "calls VdEnableDisableClockGating"),
    (0x827475A0, "D3D::BuildGammaTable", W, "calls VdGetCurrentDisplayGamma"),
    (0x82747708, "D3D::BuildPWLGammaTable", W, "calls VdGetCurrentDisplayGamma"),
    (0x82747850, "D3D::EmitGammaRamp", W, "PM4 COND_WRITE (0xC0054500), register 0x1921..0x1927"),
    (0x82747930, "D3D::EmitPWLGammaRamp", W, "PM4 COND_WRITE, register 0x1921..0x1927"),
    (0x827483A8, "D3D::SwapWorkerThread", V, "thread entry, calls SynchronizeToPresentationInterval and Swap"),
    (0x827487D0, "D3D::CreateWorkerThread", W, "calls ExCreateThread, KeSetBasePriorityThread"),
    # --- Occlusion ---------------------------------------------------------------------------------------------
    (0x82740B28, "D3DDevice_BeginConditionalSurvey", W, "PM4 VIZ_QUERY (0xC0002300), register 0x2293; 1 engine caller (0x8250C6D0)"),
    (0x82740BE0, "D3DDevice_EndConditionalSurvey", W, "PM4 VIZ_QUERY, mask dev+0x2F80 (12160)"),
    (0x82740CC0, "D3DDevice_BeginConditionalRendering", W, "counter dev+0x300B (12299)"),
    (0x82740CE0, "D3DDevice_EndConditionalRendering", W, "counter dev+0x300B"),
    (0x82740E58, "D3DDevice_CreateQuery", W, "the engine calls it with type 9 (D3DQUERYTYPE_OCCLUSION) from 0x823B3398"),
    (0x82740F48, "D3DQuery_Release", V, "free, identifier 0x78787878"),
    (0x82741090, "D3DQuery_Issue", W, "PM4 EVENT_WRITE (0x46) and EVENT_WRITE_EXT (0x5A); the engine calls it with flags 2 (BEGIN) and 1 (END)"),
    (0x82741238, "D3DQuery_GetData", W, "840 bytes; 1 engine caller (0x823B3800)"),
    # --- Diagnostics -----------------------------------------------------------------------------------------
    (0x82743938, "D3D::DebugPrint", S, "calls _vsnprintf, DbgPrint"),
    (0x82743A40, "D3D::ReportGpuHang", S, "string 'The GPU appears to be hung'"),
    (0x82743D88, "D3D::BreakOnGpuHang", S, "string 'Breaking into the debugger'"),
    (0x82741790, "D3D::DumpGpuRegisters", V, "1544 bytes, register 0xC48..0xE4B"),
    (0x82741DB0, "D3D::DumpGpuState", V, "5684 bytes, PM4 WAIT_FOR_IDLE (0x26)"),
    (0x82746BC0, "D3D::PixCrashDump", S, "string 'crashdump.pix2'"),
    (0x82747148, "D3D::PixEndFrame", V, "string 'e:\\\\xbperfview.cap'; from Swap"),
]


def table_entries(im):
    out = []
    for base, n, kind in ((RS_TABLE, RS_COUNT, "rs"), (SS_TABLE, SS_COUNT, "ss")):
        for i in range(n):
            g, s, d = im.words(base + 12 * i, 3)
            if kind == "rs":
                nm = RS_NAMES.get(i)
                slot = i
                st = "RenderState"
            else:
                nm = SS_NAMES[i]
                slot = 97 + i
                st = "SamplerState"
            if nm is None:
                continue
            for fn, role in ((s, "Set"), (g, "Get")):
                if not (d3dlib.D3D_LO <= fn < d3dlib.D3D_HI):
                    continue
                out.append((fn, "D3DDevice_%s%s_%s" % (role, st, nm), S,
                            "%s table slot %d (device+%d), default 0x%X" % (
                                "setter" if role == "Set" else "getter", slot, (96 if role == "Set" else 564) + 4 * slot, d)))
    return out


def main():
    im = d3dlib.Img()
    rows = {}
    for a, n, c, m in table_entries(im):
        rows.setdefault(a, (n, c, m))
    for a, n, c, m in MANUAL:
        if a in rows and rows[a][0] != n:
            raise SystemExit("duplicate: %08X %s / %s" % (a, n, rows[a][0]))
        rows[a] = (n, c, m)
    lib = d3dlib.Lib(im, d3dlib.D3D_LO, d3dlib.D3D_HI)
    starts = set(lib.starts)
    bad = [a for a in rows if a not in starts]
    p = os.path.join(d3dlib.REPO, "config", "predator", "d3d_symbols.toml")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    cnt = Counter(c for _, c, _ in rows.values())
    with open(p, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Names for the statically linked D3D9 library in default.xex (XDK 2.0.2417, disc release without TU).\n")
        fh.write("# Generated by tools/d3d/names.py; do not edit by hand.\n")
        fh.write("# All addresses were determined statically; no game code was executed.\n")
        fh.write("# confidence: certain | likely | assumed. State names follow King Kong Recompiled (MIT).\n\n")
        fh.write("[library]\n")
        fh.write('xdk = "2.0.2417"\n')
        fh.write("d3d9 = [0x%08X, 0x%08X]\n" % (d3dlib.D3D_LO, d3dlib.D3D_HI))
        fh.write("d3dx9 = [0x8264A980, 0x%08X]\n" % d3dlib.D3D_LO)
        fh.write("xgraphc = [0x8277FB40, 0x82866000]\n")
        fh.write("device_size = 20608\n")
        fh.write("device_global = 0x829161B4\n")
        fh.write("render_state_table = 0x%08X\n" % RS_TABLE)
        fh.write("sampler_state_table = 0x%08X\n\n" % SS_TABLE)
        for a in sorted(rows):
            n, c, m = rows[a]
            fh.write("[[function]]\n")
            fh.write("address = 0x%08X\n" % a)
            fh.write('name = "%s"\n' % n)
            fh.write('confidence = "%s"\n' % c)
            fh.write('evidence = "%s"\n\n' % m.replace("\\", "\\\\").replace('"', "'") if "\\\\" not in m else
                     'evidence = "%s"\n\n' % m.replace('"', "'"))
    print("%d names: %s -> %s" % (len(rows), ", ".join("%s %d" % (k, cnt[k]) for k in (S, W, V)), p))
    print("of which setters/getters from the table: %d" % len(table_entries(im)))
    if bad:
        print("WARNING, not a function start according to refine_funcs: " + " ".join("%08X" % a for a in sorted(bad)))


if __name__ == "__main__":
    main()
