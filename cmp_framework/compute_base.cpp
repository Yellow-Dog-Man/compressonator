//=====================================================================
// Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
/// \file Compute_Base.cpp
//
//=====================================================================
#include <cstdlib>

#include "compute_base.h"
#include "plugininterface.h"
#include "cmp_plugininterface.h"
#include "texture.h"
#include "texture_utils.h"
#include "cmp_core.h"
#include "atiformats.h"
#include "bcn_common_kernel.h"

#ifndef _WIN32
#include <unistd.h> /* For open(), creat() */
#endif

#define STB_IMAGE_STATIC
#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include "stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#include <string>
#include <mutex>

CMP_CHAR*                GetFormatDesc(CMP_FORMAT nFormat);
PluginManager            g_pluginManager;
PluginInterface_Encoder* plugin_encoder_codec  = NULL;
static CMP_BOOL          HostPluginsRegistered = FALSE;
static CMP_FORMAT        cmp_format_hold       = CMP_FORMAT_Unknown;

PluginInterface_Pipeline* g_ComputeBase = NULL;
static CMP_Compute_type   ComputeType   = CMP_CPU;
static int                exitreg       = false;
static int                startupreg    = false;

extern void* make_Plugin_DDS();
extern void* make_Plugin_HPC();

// CMP_Core Compression Codecs
extern void* make_Plugin_BC1();
extern void* make_Plugin_BC2();
extern void* make_Plugin_BC3();
extern void* make_Plugin_BC4();
extern void* make_Plugin_BC4_S();
extern void* make_Plugin_BC5();
extern void* make_Plugin_BC5_S();
extern void* make_Plugin_BC6H();
extern void* make_Plugin_BC6H_SF();
extern void* make_Plugin_BC7();

#ifdef USE_GTC
extern void* make_Plugin_GTC();
#endif
#ifdef USE_APC
extern void* make_Plugin_APC();
#endif
#ifdef USE_LOSSLESS_COMPRESSION
extern void* make_Codec_Plugin_BRLG();
#endif

void CMP_RegisterHostPlugins()
{
    if (HostPluginsRegistered == FALSE)
    {
        // Hosts
        g_pluginManager.registerStaticPlugin("IMAGE", "DDS", (void*)make_Plugin_DDS);
        g_pluginManager.registerStaticPlugin("PIPELINE", "HPC", (void*)make_Plugin_HPC);
        // Encoders
        g_pluginManager.registerStaticPlugin("ENCODER", "BC1", (void*)make_Plugin_BC1);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC2", (void*)make_Plugin_BC2);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC3", (void*)make_Plugin_BC3);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC4", (void*)make_Plugin_BC4);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC4_S", (void*)make_Plugin_BC4_S);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC5", (void*)make_Plugin_BC5);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC5_S", (void*)make_Plugin_BC5_S);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC6H", (void*)make_Plugin_BC6H);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC6H_SF", (void*)make_Plugin_BC6H_SF);
        g_pluginManager.registerStaticPlugin("ENCODER", "BC7", (void*)make_Plugin_BC7);
#ifdef USE_GTC
        g_pluginManager.registerStaticPlugin("ENCODER", "GTC", (void*)make_Plugin_GTC);
#endif
#ifdef USE_APC
        g_pluginManager.registerStaticPlugin("ENCODER", "APC", (void*)make_Plugin_APC);
#endif
#ifdef USE_LOSSLESS_COMPRESSION
        g_pluginManager.registerStaticPlugin("ENCODER", "BRLG", (void*)make_Codec_Plugin_BRLG);
#endif
        g_pluginManager.getPluginList(".", TRUE);
        HostPluginsRegistered = TRUE;
    }
}

//
// Closes the Compute library allocation
//
CMP_ERROR CMP_API CMP_DestroyComputeLibrary(bool forceclose = false)
{
    if (g_ComputeBase && forceclose)
    {
        if (plugin_encoder_codec)
        {
            delete plugin_encoder_codec;
            plugin_encoder_codec = NULL;
        }

        g_ComputeBase->TC_Close();
        //free(g_ComputeBase);
        delete g_ComputeBase;
        g_ComputeBase = NULL;
    }

    return CMP_OK;
}

const CMP_CHAR* GetEncodeWithDesc(CMP_Compute_type nFormat)
{
    switch (nFormat)
    {
    case CMP_HPC:  // Use CPU High Performance Compute to compress textures, full support
        return "HPC";
        break;
    case CMP_GPU_OCL:
        return "OCL";
        break;
    case CMP_GPU_HW:
        return "GPU";
        break;
    case CMP_GPU_VLK:
        return "VLK";
        break;
    case CMP_GPU_DXC:
        return "DXC";
        break;
    }
    return "CPU";
}

#ifdef _WIN32
bool cmp_recompile_shader(std::string m_sourceShaderFile)
{
    bool    rebuild    = false;
    FILE*   p_file_bin = NULL;
    errno_t fopen_result;
    // Check build configuration of the shader, has it been modified since last use
    fopen_result = fopen_s(&p_file_bin, m_sourceShaderFile.c_str(), "r+");
    if (fopen_result == 0)
    {
        time_t attr;
        fseek(p_file_bin, 3, SEEK_CUR);
        fscanf_s(p_file_bin, "%llx", &attr);
        struct stat attrib;
        stat(m_sourceShaderFile.c_str(), &attrib);
        if (attr != attrib.st_ctime)
        {
            char buff[128];
            sprintf_s(buff, " %llx ", attrib.st_ctime);
            // snprintf(buff, 128, " %llx ", attrib.st_ctime);
            fseek(p_file_bin, 2, SEEK_SET);
            fwrite(buff, 1, 9, p_file_bin);
            rebuild = true;
        }
        fseek(p_file_bin, 0, SEEK_SET);
        fclose(p_file_bin);
    }
    return rebuild;
}
#endif

//-------------------------
// Application is "Exiting"
//-------------------------
void exiting()
{
    exitreg = false;
    CMP_DestroyComputeLibrary(true);
}

#ifdef _WIN32
bool isDX12Supported()
{
    typedef LONG NTSTATUS, *PNTSTATUS;
    typedef NTSTATUS(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW win10OSver = {0};

    HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
    if (hMod)
    {
        RtlGetVersionPtr fxPtr = (RtlGetVersionPtr)::GetProcAddress(hMod, "RtlGetVersion");
        if (fxPtr != nullptr)
        {
            RTL_OSVERSIONINFOW rovi  = {0};
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (0 == fxPtr(&rovi))
            {
                win10OSver = rovi;
            }
        }
    }

    if (win10OSver.dwMajorVersion < 10)
    {
        return false;
    }

    if ((win10OSver.dwBuildNumber != 0) && (win10OSver.dwBuildNumber < 15063))
    {
        return false;
    }

    return true;
}
#else
bool isDX12Supported()
{
    return false;
}
#endif

//
// Initialize the Compute library based on support types
//
CMP_ERROR CMP_API CMP_CreateComputeLibrary(MipSet* srcTexture, KernelOptions* kernel_options, void* CMips)
{
    CMP_Compute_type CompType   = kernel_options->encodeWith;
    CMP_FORMAT       cmp_format = kernel_options->format;
    if ((CompType != ComputeType) || cmp_format != cmp_format_hold)
    {
        CMP_DestroyComputeLibrary(true);
    }

    if (plugin_encoder_codec == NULL)
    {
        plugin_encoder_codec = reinterpret_cast<PluginInterface_Encoder*>(g_pluginManager.GetPlugin("ENCODER", GetFormatDesc(cmp_format)));
        if (plugin_encoder_codec == NULL)
        {
            PrintInfo("Format [%s] for [%s] is not supported or failed to load\n", GetFormatDesc(cmp_format), GetEncodeWithDesc(CompType));
            return CMP_ERR_UNABLE_TO_INIT_COMPUTELIB;
        }
        cmp_format_hold = cmp_format;
    }

    if (kernel_options->srcfile == NULL)
    {
        kernel_options->srcfile = plugin_encoder_codec->TC_ComputeSourceFile(kernel_options->encodeWith);
        if (kernel_options->srcfile == NULL)
        {
            PrintInfo("Failed to load the encoders shader code\n");
            return CMP_ERR_NOSHADER_CODE_DEFINED;
        }
    }

    // Initialize the compression codec, pass kernel options
    kernel_options->height    = srcTexture->dwHeight;
    kernel_options->width     = srcTexture->dwWidth;
    kernel_options->srcformat = srcTexture->m_format;
    if (plugin_encoder_codec->TC_Init(kernel_options) != 0)
    {
        PrintInfo("Failed to init encoder\n");
        return CMP_ERR_UNABLE_TO_INIT_COMPUTELIB;
    }

    if (plugin_encoder_codec->TC_PluginSetSharedIO(CMips) != 0)
    {
        PrintInfo("Warning unable to set print IO\n");
    }

    // Are we re-running the same codec type
    if (g_ComputeBase && (ComputeType == CompType))
    {
        return CMP_OK;
    }

    switch (CompType)
    {
    default:
    case CMP_HPC:
        g_ComputeBase = reinterpret_cast<PluginInterface_Pipeline*>(g_pluginManager.GetPlugin("PIPELINE", "HPC"));
        break;
    case CMP_GPU_OCL:
        g_ComputeBase = reinterpret_cast<PluginInterface_Pipeline*>(g_pluginManager.GetPlugin("PIPELINE", "GPU_OCL"));
        break;
    case CMP_GPU_HW:
        g_ComputeBase = reinterpret_cast<PluginInterface_Pipeline*>(g_pluginManager.GetPlugin("PIPELINE", "GPU_HW"));
        break;
    case CMP_GPU_DXC:
        if (isDX12Supported())
        {
            g_ComputeBase = reinterpret_cast<PluginInterface_Pipeline*>(g_pluginManager.GetPlugin("PIPELINE", "GPU_DXC"));
        }
        else
        {
            PrintInfo("Format [%s] for [%s] is supported for DX12 only\n", GetFormatDesc(cmp_format), GetEncodeWithDesc(CompType));
            return CMP_ERR_UNABLE_TO_INIT_COMPUTELIB;
        }

        break;
    case CMP_GPU_VLK:
        g_ComputeBase = reinterpret_cast<PluginInterface_Pipeline*>(g_pluginManager.GetPlugin("PIPELINE", "GPU_VLK"));
        break;
    }

    if (g_ComputeBase)
    {
        // some code here if needed
        if (g_ComputeBase->TC_Init(kernel_options) != 0)
            return CMP_ERR_UNABLE_TO_INIT_COMPUTELIB;
        ComputeType = CompType;
        // Init IO
        if (g_ComputeBase->TC_PluginSetSharedIO(CMips) != 0)
            return CMP_ERR_UNABLE_TO_INIT_COMPUTELIB;
    }
    else
        return CMP_ERR_UNABLE_TO_INIT_COMPUTELIB;

    if (!exitreg)
    {
        exitreg = true;
        std::atexit(exiting);
    }

    if (!startupreg)
    {
        startupreg = true;
        // reserved for any one time startup code need
    }

    return CMP_OK;
}

CMP_ERROR CMP_API CMP_GetPerformanceStats(KernelPerformanceStats* pPerfStats)
{
    CMP_ERROR result;
    if (g_ComputeBase)
    {
        result = g_ComputeBase->TC_GetPerformanceStats(pPerfStats);
        if (result != CMP_OK)
            return (result);
    }
    else
        return CMP_ABORTED;
    return CMP_OK;
}

CMP_ERROR CMP_API CMP_GetDeviceInfo(KernelDeviceInfo* pDeviceInfo)
{
    CMP_ERROR result;
    if (g_ComputeBase)
    {
        result = g_ComputeBase->TC_GetDeviceInfo(pDeviceInfo);
        if (result != CMP_OK)
            return (result);
    }
    else
        return CMP_ABORTED;
    return CMP_OK;
}

CMP_ERROR CMP_API CMP_CompressTexture(KernelOptions* options, CMP_MipSet srcMipSet, CMP_MipSet dstMipSet, CMP_Feedback_Proc pFeedback)
{
    CMP_ERROR result;

    if (g_ComputeBase)
    {
        result = g_ComputeBase->TC_Compress(options, srcMipSet, dstMipSet, pFeedback);
        if (result != CMP_OK)
            return (result);
    }
    else
        return CMP_ABORTED;

    return CMP_OK;
}

CMP_ERROR CMP_API CMP_SetComputeOptions(ComputeOptions* options)
{
    options->plugin_compute = plugin_encoder_codec;
    if (g_ComputeBase)
    {
        g_ComputeBase->TC_SetComputeOptions(options);
    }
    else
        return CMP_ERR_UNABLE_TO_INIT_COMPUTELIB;

    return CMP_OK;
}

//===========================================================================================================
// This is a high level API that locks calling thread with mutex, blocking if necessary until completed
//===========================================================================================================
std::mutex cmp_mutex;

CMP_ERROR CMP_API CMP_ProcessTexture(CMP_MipSet* srcMipSet, CMP_MipSet* dstMipSet, KernelOptions kernelOptions, CMP_Feedback_Proc pFeedbackProc)
{
    cmp_mutex.lock();

    CMP_CMIPS CMips;
    assert(srcMipSet);
    assert(dstMipSet);

    // -------------
    // Output
    // -------------
    dstMipSet->m_Flags   = MS_FLAG_Default;
    dstMipSet->m_format  = kernelOptions.format;
    dstMipSet->m_nHeight = srcMipSet->m_nHeight;
    dstMipSet->m_nWidth  = srcMipSet->m_nWidth;

    CMP_Format2FourCC(dstMipSet->m_format, dstMipSet);

    //=====================================================
    // Case Uncompressed Source to Compressed Destination
    //=====================================================
    // Allocate compression data
    dstMipSet->m_ChannelFormat = CF_Compressed;
    dstMipSet->m_nMaxMipLevels = srcMipSet->m_nMaxMipLevels;
    dstMipSet->m_nMipLevels    = 1;  // this is overwriiten depending on input.
    dstMipSet->m_nBlockWidth   = 4;  // Update is required for other sizes.
    dstMipSet->m_nBlockHeight  = 4;  // - need to fix p_MipSetIn m_nBlock settings for this to work
    dstMipSet->m_nBlockDepth   = 1;
    dstMipSet->m_nDepth        = srcMipSet->m_nDepth;
    dstMipSet->m_TextureType   = srcMipSet->m_TextureType;

    if (!CMips.AllocateMipSet(
            dstMipSet, dstMipSet->m_ChannelFormat, TDT_ARGB, dstMipSet->m_TextureType, srcMipSet->m_nWidth, srcMipSet->m_nHeight, dstMipSet->m_nDepth))
    {
        cmp_mutex.unlock();
        return CMP_ERR_MEM_ALLOC_FOR_MIPSET;
    }

    CMP_Texture srcTexture;
    srcTexture.dwSize = sizeof(srcTexture);
    int DestMipLevel  = srcMipSet->m_nMipLevels;

    dstMipSet->m_nMipLevels = DestMipLevel;

    for (int nMipLevel = 0; nMipLevel < DestMipLevel; nMipLevel++)
    {
        for (int nFaceOrSlice = 0; nFaceOrSlice < CMP_MaxFacesOrSlices(srcMipSet, nMipLevel); nFaceOrSlice++)
        {
            //=====================
            // Uncompressed source
            //======================
            MipLevel* pInMipLevel   = CMips.GetMipLevel(srcMipSet, nMipLevel, nFaceOrSlice);
            srcTexture.dwPitch      = 0;
            srcTexture.nBlockWidth  = srcMipSet->m_nBlockWidth;
            srcTexture.nBlockHeight = srcMipSet->m_nBlockHeight;
            srcTexture.nBlockDepth  = srcMipSet->m_nBlockDepth;
            srcTexture.format       = srcMipSet->m_format;
            srcTexture.dwWidth      = pInMipLevel->m_nWidth;
            srcTexture.dwHeight     = pInMipLevel->m_nHeight;
            srcTexture.pData        = pInMipLevel->m_pbData;
            srcTexture.dwDataSize   = CMP_CalculateBufferSize(&srcTexture);

            // Temporary setting!
            srcMipSet->dwWidth    = pInMipLevel->m_nWidth;
            srcMipSet->dwHeight   = pInMipLevel->m_nHeight;
            srcMipSet->pData      = pInMipLevel->m_pbData;
            srcMipSet->dwDataSize = CMP_CalculateBufferSize(&srcTexture);

            //========================
            // Compressed Destination
            //========================
            CMP_Texture destTexture;
            destTexture.dwSize       = sizeof(destTexture);
            destTexture.dwWidth      = pInMipLevel->m_nWidth;
            destTexture.dwHeight     = pInMipLevel->m_nHeight;
            destTexture.dwPitch      = 0;
            destTexture.nBlockWidth  = dstMipSet->m_nBlockWidth;
            destTexture.nBlockHeight = dstMipSet->m_nBlockHeight;
            destTexture.nBlockDepth  = dstMipSet->m_nBlockDepth;
            destTexture.format       = dstMipSet->m_format;
            destTexture.dwDataSize   = CMP_CalculateBufferSize(&destTexture);

            dstMipSet->m_format   = dstMipSet->m_format;
            dstMipSet->dwDataSize = CMP_CalculateBufferSize(&destTexture);
            dstMipSet->dwWidth    = pInMipLevel->m_nWidth;
            dstMipSet->dwHeight   = pInMipLevel->m_nHeight;

            MipLevel* pOutMipLevel = CMips.GetMipLevel(dstMipSet, nMipLevel, nFaceOrSlice);
            if (!CMips.AllocateCompressedMipLevelData(pOutMipLevel, destTexture.dwWidth, destTexture.dwHeight, destTexture.dwDataSize))
            {
                cmp_mutex.unlock();
                return CMP_ERR_MEM_ALLOC_FOR_MIPSET;
            }

            destTexture.pData = pOutMipLevel->m_pbData;
            dstMipSet->pData  = pOutMipLevel->m_pbData;

            //========================
            // Process ConvertTexture
            //========================
            //------------------------------------------------
            // Initializing the Host Framework
            // if it fails revert to CPU version of the codec
            //------------------------------------------------
            ComputeOptions options;
            options.force_rebuild = false;  // set this to true if you want the shader source code  to be allways compiled!

            //===============================================================================
            // Initalize the  Pipeline that will be used for the codec to run on HPC or GPU
            //===============================================================================
            if (CMP_CreateComputeLibrary(srcMipSet, &kernelOptions, &CMips) != CMP_OK)
            {
                PrintInfo("Failed to init HOST Lib. CPU will be used for compression\n");
                cmp_mutex.unlock();
                return CMP_ERR_FAILED_HOST_SETUP;
            }

            // Init Compute Codec info IO
            if ((CMips.PrintLine == NULL) && (PrintStatusLine != NULL))
            {
                CMips.PrintLine = PrintStatusLine;
            }

            // Set any addition feature as needed for the Host
            if (CMP_SetComputeOptions(&options) != CMP_OK)
            {
                CMP_DestroyComputeLibrary(true);
                PrintInfo("Failed to setup SPMD GPU options\n");
                cmp_mutex.unlock();
                return CMP_ERR_FAILED_HOST_SETUP;
            }

            // Do the compression
            if (CMP_CompressTexture(&kernelOptions, *srcMipSet, *dstMipSet, pFeedbackProc) != CMP_OK)
            {
                CMips.FreeMipSet(dstMipSet);
                CMP_DestroyComputeLibrary(true);
                PrintInfo("Failed to run compute plugin: CPU will be used for compression.\n");
                cmp_mutex.unlock();
                return CMP_ERR_FAILED_HOST_SETUP;
            }

            // Get Performance Stats
            if (kernelOptions.getPerfStats)
            {
                if (CMP_GetPerformanceStats(&kernelOptions.perfStats) != CMP_OK)
                    PrintInfo("Warning unable to get compute plugin performance stats\n");
            }

            //===============================================================================
            // Close the Pipeline with option to cache as needed
            //===============================================================================
            CMP_DestroyComputeLibrary(true);
        }
    }

    //if (pFeedbackProc)
    //    pFeedbackProc(100, NULL, NULL);

    cmp_mutex.unlock();
    return CMP_OK;
}

//
// Block Level Encoder Support
//
CMP_ERROR CMP_API CMP_CreateBlockEncoder(void** block_encoder, CMP_EncoderSetting encodeSettings)
{
    CMP_RegisterHostPlugins();  // Keep for legacy, user should now use CMP_InitFramework

    PluginInterface_Encoder* encoder_codec;
    encoder_codec = reinterpret_cast<PluginInterface_Encoder*>(g_pluginManager.GetPlugin("ENCODER", GetFormatDesc((CMP_FORMAT)encodeSettings.format)));
    if (encoder_codec == NULL)
    {
        PrintInfo("Failed to load [%s] encoder\n", GetFormatDesc((CMP_FORMAT)encodeSettings.format));
        return CMP_ERR_UNABLE_TO_LOAD_ENCODER;
    }

    CMP_Encoder* blockEncoder = (CMP_Encoder*)encoder_codec->TC_Create();
    if (blockEncoder == NULL)
    {
        PrintInfo("Failed to create block encoder [%s]\n", GetFormatDesc((CMP_FORMAT)encodeSettings.format));
        return CMP_ERR_UNABLE_TO_CREATE_ENCODER;
    }

    KernelOptions kernelOptions;
    kernelOptions.height   = encodeSettings.height;
    kernelOptions.width    = encodeSettings.width;
    kernelOptions.fquality = encodeSettings.quality;

    encoder_codec->TC_Init(&kernelOptions);

    blockEncoder->m_quality   = encodeSettings.quality;
    blockEncoder->m_srcHeight = encodeSettings.height;
    blockEncoder->m_srcWidth  = encodeSettings.width;

    *block_encoder = blockEncoder;
    delete encoder_codec;

    return CMP_OK;
}

CMP_ERROR CMP_API CMP_CompressBlock(void** block_encoder, void* SourceTexture, unsigned int sourceStride, void* DestTexture, unsigned int DestStride)
{
    CMP_Encoder* encoder = (CMP_Encoder*)*block_encoder;
    encoder->m_srcStride = sourceStride;
    encoder->m_dstStride = DestStride;
    CMP_ERROR res        = (CMP_ERROR)encoder->CompressBlock(0, 0, SourceTexture, DestTexture);
    return (res);
}

CMP_ERROR CMP_API
CMP_CompressBlockXY(void** block_encoder, unsigned int x, unsigned int y, void* in, unsigned int sourceStride, void* out, unsigned int DestStride)
{
    CMP_Encoder* encoder = (CMP_Encoder*)*block_encoder;
    encoder->m_srcStride = sourceStride;
    encoder->m_dstStride = DestStride;
    CMP_ERROR res        = (CMP_ERROR)encoder->CompressBlock(x, y, in, out);
    return (res);
}

void CMP_API CMP_DestroyBlockEncoder(void** block_encoder)
{
    delete (CMP_Encoder*)(*block_encoder);
}

void CMP_API CMP_GetMipLevel(CMP_MipLevel** data, const CMP_MipSet* pMipSet, CMP_INT nMipLevel, CMP_INT nFaceOrSlice)
{
    CMP_CMIPS CMips;
    *data = CMips.GetMipLevel(pMipSet, nMipLevel, nFaceOrSlice);
}

//==============================
// FILE IO static plugin libs
//==============================

CMP_ERROR stb_load(const char* SourceFile, MipSet* MipSetIn)
{
    int            Width, Height, ComponentCount;
    unsigned char* pTempData = stbi_load(SourceFile, &Width, &Height, &ComponentCount, STBI_rgb_alpha);

    if (pTempData == NULL)
    {
        return CMP_ERR_UNSUPPORTED_SOURCE_FORMAT;
    }

    CMP_CMIPS CMips;

    memset(MipSetIn, 0, sizeof(MipSet));
    if (!CMips.AllocateMipSet(MipSetIn, CF_8bit, TDT_ARGB, TT_2D, Width, Height, 1))
    {
        return CMP_ERR_MEM_ALLOC_FOR_MIPSET;
    }

    if (!CMips.AllocateMipLevelData(CMips.GetMipLevel(MipSetIn, 0), Width, Height, CF_8bit, TDT_ARGB))
    {
        return CMP_ERR_MEM_ALLOC_FOR_MIPSET;
    }

    MipSetIn->m_nMipLevels = 1;
    MipSetIn->m_format     = CMP_FORMAT_RGBA_8888;

    CMP_BYTE* pData = CMips.GetMipLevel(MipSetIn, 0)->m_pbData;

    // RGBA : 8888 = 4 bytes
    CMP_DWORD dwPitch = (4 * MipSetIn->m_nWidth);
    CMP_DWORD dwSize  = dwPitch * MipSetIn->m_nHeight;

    memcpy(pData, pTempData, dwSize);

    // Assign miplevel 0 to MipSetin pData ref
    // both miplevel pData and mipset pData will point to the same location
    // Typically mipset pData is assign a pointer to the current miplevel data been processed at run time
    MipSetIn->pData      = pData;
    MipSetIn->dwDataSize = dwSize;

    stbi_image_free(pTempData);
    return CMP_OK;
}

void CMP_API CMP_FreeMipSet(CMP_MipSet* MipSetIn)
{
    if (!MipSetIn)
        return;

    if (MipSetIn->m_pMipLevelTable)
    {
        CMP_CMIPS CMips;
        CMips.FreeMipSet(MipSetIn);
        MipSetIn->m_pMipLevelTable = NULL;
    }
}

char toupperChar(char ch)
{
    return static_cast<char>(::toupper(static_cast<unsigned char>(ch)));
}

static std::string GetFileExtension(const char* filename)
{
    if (!filename)
        return "";
        
    const std::string& fn = filename;
    std::string file_extension = fn.substr(fn.find_last_of('.') + 1);
    std::transform(file_extension.begin(), file_extension.end(), file_extension.begin(), toupperChar);
    return file_extension;
}

void CMP_API CMP_InitFramework()
{
    CMP_RegisterHostPlugins();
}

CMP_ERROR CMP_API CMP_LoadTexture(const char* SourceFile, CMP_MipSet* MipSetIn)
{
    CMP_RegisterHostPlugins();  // Keep for legacy, user should now use CMP_InitFramework

    CMP_CMIPS CMips;
    CMP_ERROR status = CMP_OK;

    std::string file_extension = GetFileExtension(SourceFile);

    PluginInterface_Image* plugin_Image;
    do
    {
        plugin_Image = reinterpret_cast<PluginInterface_Image*>(g_pluginManager.GetPlugin("IMAGE", (char*)file_extension.c_str()));
        // do the load
        if (plugin_Image == NULL)
        {
            status = CMP_ERR_PLUGIN_FILE_NOT_FOUND;
            break;
        }
        else
        {
            plugin_Image->TC_PluginSetSharedIO(&CMips);
            if (plugin_Image->TC_PluginFileLoadTexture(SourceFile, MipSetIn) != 0)
            {
                // Process Error
                delete plugin_Image;
                plugin_Image = NULL;
                status       = CMP_ERR_UNABLE_TO_LOAD_FILE;
                break;
            }

            delete plugin_Image;
            plugin_Image = NULL;
        }
    } while (0);

    // load failed: try stb lib
    if (status != CMP_OK)
    {
        status = stb_load(SourceFile, MipSetIn);
    }
    else
    {
        // Make sure MipSetIn->pData is at top mip level
        if (MipSetIn->pData == NULL)
        {
            CMP_MipLevel* pOutMipLevel = CMips.GetMipLevel(MipSetIn, 0, 0);
            MipSetIn->pData            = pOutMipLevel->m_pbData;
            MipSetIn->dwDataSize       = pOutMipLevel->m_dwLinearSize;
            MipSetIn->dwHeight         = pOutMipLevel->m_nHeight;
            MipSetIn->dwWidth          = pOutMipLevel->m_nWidth;
        }
    }
    return status;
}

CMP_ERROR CMP_API CMP_SaveTexture(const char* DestFile, CMP_MipSet* MipSetIn)
{
    CMP_RegisterHostPlugins();  // Keep for legacy, user should now use CMP_InitFramework

    bool  filesaved = false;
    CMIPS m_CMIPS;

    std::string file_extension = GetFileExtension(DestFile);

    //if (((((file_extension.compare("DDS") == 0)
    //    || file_extension.compare("KTX") == 0)
    //    || file_extension.compare("KTX2") == 0))
    //    != TRUE)
    //    {
    //    return CMP_ERR_INVALID_DEST_TEXTURE;
    //}

    PluginInterface_Image* plugin_Image;
    plugin_Image = reinterpret_cast<PluginInterface_Image*>(g_pluginManager.GetPlugin("IMAGE", (char*)file_extension.c_str()));

    if (plugin_Image)
    {
        bool holdswizzle = MipSetIn->m_swizzle;

        plugin_Image->TC_PluginSetSharedIO(&m_CMIPS);
        if (plugin_Image->TC_PluginFileSaveTexture(DestFile, (MipSet*)MipSetIn) == 0)
        {
            filesaved = true;
        }

        MipSetIn->m_swizzle = holdswizzle;

        delete plugin_Image;
        plugin_Image = NULL;
    }

    if (!filesaved)
    {  // ToDo create a stb_save()
        if (file_extension.compare("PNG") == 0)
            stbi_write_png(DestFile, MipSetIn->m_nWidth, MipSetIn->m_nHeight, 4, MipSetIn->pData, MipSetIn->m_nWidth * 4);
        else if (file_extension.compare("BMP") == 0)
            stbi_write_bmp(DestFile, MipSetIn->m_nWidth, MipSetIn->m_nHeight, 4, MipSetIn->pData);
        else if (file_extension.compare("JPG") == 0)
            stbi_write_jpg(DestFile, MipSetIn->m_nWidth, MipSetIn->m_nHeight, 4, MipSetIn->pData, 100);
        else
            return CMP_ERR_GENERIC;
    }

    return CMP_OK;
}

static CMP_ERROR SaveTextureViaMipSet(const char* destFile, CMP_Texture* pTexture)
{
    CMP_MipSet tempMipSet = {};
    
    ChannelFormat channelFormat;
    TextureDataType textureDataType = TDT_ARGB;
    
    if (CMP_IsCompressedFormat(pTexture->format))
        channelFormat = CF_Compressed;
    else
        channelFormat = GetChannelFormat(pTexture->format);
    
    CMP_ERROR result = CMP_CreateMipSet(&tempMipSet, pTexture->dwWidth, pTexture->dwHeight, 1, channelFormat, TT_2D);
    if (result != CMP_OK)
        return result;
    
    tempMipSet.m_format = pTexture->format;
    tempMipSet.m_transcodeFormat = pTexture->transcodeFormat;
    tempMipSet.m_nBlockWidth = pTexture->nBlockWidth;
    tempMipSet.m_nBlockHeight = pTexture->nBlockHeight;
    tempMipSet.m_nBlockDepth = pTexture->nBlockDepth;
    
    CMP_MipLevel* pMipLevel = NULL;
    CMP_GetMipLevel(&pMipLevel, &tempMipSet, 0, 0);
    
    if (pMipLevel)
    {
        // Store the original allocated pointer to restore it later
        CMP_BYTE* originalData = pMipLevel->m_pbData;
        
        // Point to the texture's data instead of copying
        pMipLevel->m_pbData = pTexture->pData;
        pMipLevel->m_dwLinearSize = pTexture->dwDataSize;
        tempMipSet.pData = pTexture->pData;
        tempMipSet.dwDataSize = pTexture->dwDataSize;
        
        result = CMP_SaveTexture(destFile, &tempMipSet);
        
        // Restore the original pointer before cleanup so FreeMipSet can clean it up properly
        pMipLevel->m_pbData = originalData;
        tempMipSet.pData = originalData;
    }
    else
    {
        result = CMP_ERR_GENERIC;
    }
    
    CMP_FreeMipSet(&tempMipSet);
    
    return result;
}

CMP_ERROR CMP_API CMP_SaveTextureEx(const char* destFile, CMP_Texture* pTexture)
{
    if (!destFile || !pTexture)
        return CMP_ERR_INVALID_DEST_TEXTURE;

    if (!pTexture->pData || pTexture->dwDataSize == 0)
        return CMP_ERR_INVALID_SOURCE_TEXTURE;

    std::string file_extension = GetFileExtension(destFile);

    // For simple formats (PNG, BMP, JPG), save directly using stbi
    if (file_extension.compare("PNG") == 0)
    {
        stbi_write_png(destFile, pTexture->dwWidth, pTexture->dwHeight, 4, pTexture->pData, pTexture->dwWidth * 4);
        return CMP_OK;
    }
    else if (file_extension.compare("BMP") == 0)
    {
        stbi_write_bmp(destFile, pTexture->dwWidth, pTexture->dwHeight, 4, pTexture->pData);
        return CMP_OK;
    }
    else if (file_extension.compare("JPG") == 0 || file_extension.compare("JPEG") == 0)
    {
        stbi_write_jpg(destFile, pTexture->dwWidth, pTexture->dwHeight, 4, pTexture->pData, 100);
        return CMP_OK;
    }
    
    // For other formats (DDS, KTX, etc.)
    return SaveTextureViaMipSet(destFile, pTexture);
}

CMP_INT CMP_API CMP_NumberOfProcessors(void)
{
#ifndef _WIN32
    return sysconf(_SC_NPROCESSORS_ONLN);
#else
    // Figure out how many cores there are on this machine
    //SYSTEM_INFO sysinfo;
    //GetSystemInfo(&sysinfo);
    //return (sysinfo.dwNumberOfProcessors);
    return GetMaximumProcessorCount(0);
#endif
}
