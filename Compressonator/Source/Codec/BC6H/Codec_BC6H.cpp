//===============================================================================
// Copyright (c) 2014-2016  Advanced Micro Devices, Inc. All rights reserved.
//===============================================================================
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
//
//  File Name:   Codec_BC6H.cpp
//  Description: implementation of the CCodec_BC6H class
//
//////////////////////////////////////////////////////////////////////////////

#pragma warning(disable:4100)    // Ignore warnings of unreferenced formal parameters
#include "Common.h"
#include "Codec_BC6H.h"
#include "BC7_Definitions.h"
#include "BC6H_library.h"
#include "BC6H_Definitions.h"
#include "process.h"
#include "HDR_Encode.h"

using namespace HDR_Encode;

#ifdef BC6H_COMPDEBUGGER
#include "CompClient.h"
extern     CompViewerClient g_CompClient;
#endif

//======================================================================================
#define USE_MULTITHREADING  1


struct BC6HEncodeThreadParam
{
    BC6HBlockEncoder    *encoder;
    float    in[MAX_SUBSET_SIZE][MAX_DIMENSION_BIG];
    BYTE    *out;
    volatile BOOL    run;
    volatile BOOL    exit;
};

//
// Thread procedure for encoding a block
//
// The thread stays alive, and expects blocks to be pushed to it by a producer
// process that signals it when new work is available. When the producer is finished
// it should set the exit flag in the parameters to allow the tread to quit
//

unsigned int    _stdcall BC6HThreadProcEncode(void* param)
{
    BC6HEncodeThreadParam *tp = (BC6HEncodeThreadParam*)param;

    while(tp->exit == FALSE)
    {
        if(tp->run == TRUE)
        {
            tp->encoder->CompressBlock(tp->in, tp->out);
            tp->run = FALSE;
        }
        Sleep(0);
    }

    return 0;
}


static BC6HEncodeThreadParam *g_BC6EncodeParameterStorage = NULL;
int    g_block= 0; // Keep track of current encoder block!



//////////////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////////////

CCodec_BC6H::CCodec_BC6H(CodecType codecType) : CCodec_DXTC(codecType)
{

    // user definable setting
    m_Exposure              = 1.0;
    m_ModeMask              = 0xFFFF;
    m_Quality               = AMD_CODEC_QUALITY_DEFAULT;
    m_Use_MultiThreading    = true;
    m_NumThreads            = 8;
    if(codecType == CT_BC6H)
        m_bIsSigned = false;
    else
        m_bIsSigned = true;
    m_UsePatternRec         = false;

    // Internal setting
    m_LibraryInitialized    = false;
    m_NumEncodingThreads    = 1;
    m_EncodingThreadHandle  = NULL;
    m_LiveThreads           = 0;
    m_LastThread            = 0;
    m_CodecType             = codecType;
}


bool CCodec_BC6H::SetParameter(const CMP_CHAR* pszParamName, CMP_CHAR* sValue)
{
    if (sValue == NULL) return false;

    if(strcmp(pszParamName, "ModeMask") == 0)
    {
        m_ModeMask = std::stoi(sValue) & 0xFFFF;
    }
    else
    if(strcmp(pszParamName, "Signed") == 0)
    {
        m_bIsSigned            = (bool) (std::stoi(sValue) > 0);
    }
    else
    if(strcmp(pszParamName, "PatternRec") == 0)
    {
        m_UsePatternRec        = (bool) (std::stoi(sValue) > 0);
    }
    else
    if(strcmp(pszParamName, "NumThreads") == 0)
    {
        m_NumThreads = (CMP_BYTE) std::stoi(sValue);
        m_Use_MultiThreading = m_NumThreads > 1;
    }
    else
    if(strcmp(pszParamName, "Quality") == 0)
    {
        m_Quality = std::stof(sValue);
        if ((m_Quality < 0) || (m_Quality > 1.0))
        {
            return false;
        }
    }
    else
        if (strcmp(pszParamName, "Exposure") == 0)
        {
            m_Exposure = std::stof(sValue);
        }
    else
    return CCodec_DXTC::SetParameter(pszParamName, sValue);

    return true;
}


bool CCodec_BC6H::SetParameter(const CMP_CHAR* pszParamName, CMP_DWORD dwValue)
{
    if(strcmp(pszParamName, "ModeMask") == 0)
        m_ModeMask            = (CMP_BYTE) dwValue & 0xFF;
    else
    if(strcmp(pszParamName, "Signed") == 0)
        m_bIsSigned            = (bool) (dwValue > 0);
        else
    if(strcmp(pszParamName, "PatternRec") == 0)
        m_UsePatternRec        = (bool) (dwValue > 0);
    else
    if(strcmp(pszParamName, "NumThreads") == 0)
    {
        m_NumThreads = (CMP_BYTE) dwValue;
        m_Use_MultiThreading = m_NumThreads > 1;
    }
    else
        return CCodec_DXTC::SetParameter(pszParamName, dwValue);
    return true;
}

bool CCodec_BC6H::SetParameter(const CMP_CHAR* pszParamName, CODECFLOAT fValue)
{
    if(strcmp(pszParamName, "Quality") == 0)
        m_Quality = fValue;
    else
    if (strcmp(pszParamName, "Exposure") == 0)
         m_Exposure = fValue;
    else
        return CCodec_DXTC::SetParameter(pszParamName, fValue);
    return true;
}


CCodec_BC6H::~CCodec_BC6H()
{
    if (m_LibraryInitialized)
    {

        if (m_Use_MultiThreading)
        {
            // Tell all the live threads that they can exit when they have finished any current work
            for(int i=0; i < m_LiveThreads; i++)
            {
                // If a thread is in the running state then we need to wait for it to finish
                // any queued work from the producer before we can tell it to exit.
                //
                // If we don't wait then there is a race condition here where we have
                // told the thread to run but it hasn't yet been scheduled - if we set
                // the exit flag before it runs then its block will not be processed.
#pragma warning(push)
#pragma warning(disable:4127) //warning C4127: conditional expression is constant
                while(1)
                {
                    if (g_BC6EncodeParameterStorage == NULL) break;
                    if(g_BC6EncodeParameterStorage[i].run != TRUE)
                    {
                        break;
                    }
                }
#pragma warning(pop)
                // Signal to the thread that it can exit
                g_BC6EncodeParameterStorage[i].exit = TRUE;
            }

            // Now wait for all threads to have exited
            if(m_LiveThreads > 0)
            {
                WaitForMultipleObjects(m_LiveThreads,
                                       m_EncodingThreadHandle,
                                       true,
                                       INFINITE);
            }

        } // MultiThreading

        for(int i=0; i < m_LiveThreads; i++)
        {
            if(m_EncodingThreadHandle[i])
            {
                CloseHandle(m_EncodingThreadHandle[i]);
            }
            m_EncodingThreadHandle[i] = 0;
        }

        delete[] m_EncodingThreadHandle;
        m_EncodingThreadHandle = NULL;

        delete[] g_BC6EncodeParameterStorage;
        g_BC6EncodeParameterStorage = NULL;

        
        for(int i=0; i < m_NumEncodingThreads; i++)
        {
            if (m_encoder[i])
            {
                delete m_encoder[i];
                m_encoder[i] = NULL;
            }
        }

        if (m_decoder)
        {
            delete m_decoder;
            m_decoder = NULL;
        }

        m_LibraryInitialized = false;
    }
}



CodecError CCodec_BC6H::CInitializeBC6HLibrary()
{
    if (!m_LibraryInitialized)
    {
        for(DWORD i=0; i < BC6H_MAX_THREADS; i++)
        {
            m_encoder[i] = NULL;
        }

        // Create threaded encoder instances
        m_LiveThreads = 0;
        m_LastThread  = 0;
        m_NumEncodingThreads = min(m_NumThreads, BC6H_MAX_THREADS);
        if (m_NumEncodingThreads == 0) m_NumEncodingThreads = 1; 
        m_Use_MultiThreading = m_NumEncodingThreads > 1;

        g_BC6EncodeParameterStorage = new BC6HEncodeThreadParam[m_NumEncodingThreads];
        if(!g_BC6EncodeParameterStorage)
        {
            return CE_Unknown;
        }

        for (int i=0; i<m_NumEncodingThreads; i++)
            g_BC6EncodeParameterStorage[i].run = false;

        m_EncodingThreadHandle = new HANDLE[m_NumEncodingThreads];
        if(!m_EncodingThreadHandle)
        {
            delete[] g_BC6EncodeParameterStorage;
            g_BC6EncodeParameterStorage = NULL;

            return CE_Unknown;
        }

        for(int i=0; i < m_NumEncodingThreads; i++)
        {
            // Create single encoder instance
            CMP_BC6H_BLOCK_PARAMETERS user_options;

            user_options.bIsSigned      = m_bIsSigned;
            user_options.fQuality       = m_Quality;
            user_options.dwMask         = m_ModeMask;
            user_options.fExposure      = m_Exposure;
            user_options.bUsePatternRec = m_UsePatternRec;

            m_encoder[i] = new BC6HBlockEncoder(user_options);
                        
            // Cleanup if problem!
            if(!m_encoder[i])
            {

                delete[] g_BC6EncodeParameterStorage;
                g_BC6EncodeParameterStorage = NULL;

                delete[] m_EncodingThreadHandle;
                m_EncodingThreadHandle = NULL;

                for(int j=0; j<i; j++)
                {
                    delete m_encoder[j];
                    m_encoder[j] = NULL;
                }

                return CE_Unknown;
            }
            
            #ifdef USE_DBGTRACE
                DbgTrace(("Encoder[%d]:ModeMask %X, Quality %f\n",i,m_ModeMask,m_Quality));
            #endif
        }

        // Create the encoding threads in the suspended state
        for(int i=0; i<m_NumEncodingThreads; i++)
        {
            m_EncodingThreadHandle[i] = (HANDLE)_beginthreadex(NULL,
                                               0,
                                               BC6HThreadProcEncode,
                                               (void*)&g_BC6EncodeParameterStorage[i],
                                               CREATE_SUSPENDED,
                                               NULL);
            if(m_EncodingThreadHandle[i])
            {
                g_BC6EncodeParameterStorage[i].encoder = m_encoder[i];
                // Inform the thread that at the moment it doesn't have any work to do
                // but that it should wait for some and not exit
                g_BC6EncodeParameterStorage[i].run = FALSE;
                g_BC6EncodeParameterStorage[i].exit = FALSE;
                // Start the thread and have it wait for work
                ResumeThread(m_EncodingThreadHandle[i]);
                m_LiveThreads++;
            }
         }


        // Create single decoder instance
        m_decoder = new BC6HBlockDecoder();
        if(!m_decoder)
        {
            for(DWORD j=0; j<m_NumEncodingThreads; j++)
            {
                delete m_encoder[j];
                m_encoder[j] = NULL;
            }
            return CE_Unknown;
        }

        m_LibraryInitialized = true;
    }
    return CE_OK;
}


CodecError CCodec_BC6H::CEncodeBC6HBlock(float  in[MAX_SUBSET_SIZE][MAX_DIMENSION_BIG],
                                         BYTE  *out)
{
if (m_Use_MultiThreading)
{
    WORD   threadIndex;

    if((!m_LibraryInitialized) ||
        (!in) ||
        (!out))
    {
        return CE_Unknown;
    }

    // Loop and look for an available thread
    BOOL found = FALSE;
    threadIndex = m_LastThread;
    while (found == FALSE)
    {
        if(g_BC6EncodeParameterStorage[threadIndex].run == FALSE)
        {
            found = TRUE;
            break;
        }

        // Increment and wrap the thread index
        threadIndex++;
        if(threadIndex == m_LiveThreads)
        {
            threadIndex = 0;
        }
    }

    m_LastThread = threadIndex;

    // Copy the input data into the thread storage
    memcpy(g_BC6EncodeParameterStorage[threadIndex].in,
           in,
           MAX_SUBSET_SIZE * MAX_DIMENSION_BIG * sizeof(float));

    // Set the output pointer for the thread to the provided location
    g_BC6EncodeParameterStorage[threadIndex].out = out;

    // Tell the thread to start working
    g_BC6EncodeParameterStorage[threadIndex].run = TRUE;
}
else 
{
        // Copy the input data into the thread storage
        memcpy(g_BC6EncodeParameterStorage[0].in, in, BC6H_MAX_SUBSET_SIZE * MAX_DIMENSION_BIG * sizeof(float));
        // Set the output pointer for the thread to write
        g_BC6EncodeParameterStorage[0].out = out;
        m_encoder[0]->CompressBlock(g_BC6EncodeParameterStorage[0].in,g_BC6EncodeParameterStorage[0].out);
}
    return CE_OK;
}

CodecError CCodec_BC6H::CFinishBC6HEncoding(void)
{
    if(!m_LibraryInitialized)
    {
        return CE_Unknown;
    }

if (m_Use_MultiThreading)
{
    // Wait for all the live threads to finish any current work
    for(DWORD i=0; i < m_LiveThreads; i++)
    {
        // If a thread is in the running state then we need to wait for it to finish
        // its work from the producer
        while(g_BC6EncodeParameterStorage[i].run == TRUE)
        {
            Sleep(1);
        }
    }
}
return CE_OK;
}

#ifdef BC6H_DEBUG_TO_RESULTS_TXT
FILE *g_fp = NULL;
int  g_mode = 0;
#endif

CodecError CCodec_BC6H::Compress(CCodecBuffer& bufferIn, CCodecBuffer& bufferOut, Codec_Feedback_Proc pFeedbackProc, DWORD_PTR pUser1, DWORD_PTR pUser2)
{
    assert(bufferIn.GetWidth()    == bufferOut.GetWidth());
    assert(bufferIn.GetHeight() == bufferOut.GetHeight());

    if(bufferIn.GetWidth() != bufferOut.GetWidth() || bufferIn.GetHeight() != bufferOut.GetHeight())
        return CE_Unknown;

    CodecError err = CInitializeBC6HLibrary();
    if (err != CE_OK)
        return err;

#ifdef BC6H_COMPDEBUGGER
    CompViewerClient    g_CompClient;
    if (g_CompClient.connect())
    {
    #ifdef USE_DBGTRACE
        DbgTrace(("-------> Remote Server Connected"));
    #endif
    }
#endif

#ifdef BC6H_DEBUG_TO_RESULTS_TXT
    g_fp = fopen("AMD_Results.txt","w");
    g_block = 0;
#endif

    const CMP_DWORD dwBlocksX = ((bufferIn.GetWidth() + 3) >> 2);
    const CMP_DWORD dwBlocksY = ((bufferIn.GetHeight() + 3) >> 2);


#ifdef _REMOTE_DEBUG
    DbgTrace(("IN : BufferType %d ChannelCount %d ChannelDepth %d",bufferIn.GetBufferType(),bufferIn.GetChannelCount(),bufferIn.GetChannelDepth()));
    DbgTrace(("   : Height %d Width %d Pitch %d isFloat %d",bufferIn.GetHeight(),bufferIn.GetWidth(),bufferIn.GetWidth(),bufferIn.IsFloat()));

    DbgTrace(("OUT: BufferType %d ChannelCount %d ChannelDepth %d",bufferOut.GetBufferType(),bufferOut.GetChannelCount(),bufferOut.GetChannelDepth()));
    DbgTrace(("   : Height %d Width %d Pitch %d isFloat %d",bufferOut.GetHeight(),bufferOut.GetWidth(),bufferOut.GetWidth(),bufferOut.IsFloat()));
#endif;

    char            row,col,srcIndex;

    CMP_BYTE    *pOutBuffer;
    pOutBuffer    = bufferOut.GetData();

    CMP_BYTE*    pInBuffer;
    pInBuffer    =  bufferIn.GetData();

    DWORD block = 0;

#ifdef _SAVE_AS_BC6
    FILE *bc6file = fopen("Test.bc6", "wb");
#endif

    for(CMP_DWORD j = 0; j < dwBlocksY; j++)
    {

        for(CMP_DWORD i = 0; i < dwBlocksX; i++)
        {
            float blockToEncode[BLOCK_SIZE_4X4][CHANNEL_SIZE_ARGB];
            CMP_FLOAT srcBlock[BLOCK_SIZE_4X4X4];

            memset(srcBlock,0,sizeof(srcBlock));
            bufferIn.ReadBlockRGBA(i*4, j*4, 4, 4, srcBlock);

            #ifdef _BC6H_COMPDEBUGGER
            g_CompClient.SendData(1,sizeof(srcBlock),srcBlock);
            #endif

            // Create the block for encoding
            srcIndex = 0;
            for(row=0; row < BLOCK_SIZE_4; row++)
            {
                for(col=0; col < BLOCK_SIZE_4; col++)
                {
                    blockToEncode[row*BLOCK_SIZE_4+col][BC6H_COMP_RED]        = (float)srcBlock[srcIndex];
                    blockToEncode[row*BLOCK_SIZE_4+col][BC6H_COMP_GREEN]    = (float)srcBlock[srcIndex+1];
                    blockToEncode[row*BLOCK_SIZE_4+col][BC6H_COMP_BLUE]        = (float)srcBlock[srcIndex+2];
                    blockToEncode[row*BLOCK_SIZE_4+col][BC6H_COMP_ALPHA]    = (float)srcBlock[srcIndex+3];
                    srcIndex+=4;
                }
            }

            union BBLOCKS
            {
                CMP_DWORD    compressedBlock[4];
                BYTE            out[16];
                BYTE            in[16];
            } data;

            memset(data.in,0,sizeof(data));
            CEncodeBC6HBlock(blockToEncode,pOutBuffer+block);
            
#ifdef _SAVE_AS_BC6
            if (fwrite(pOutBuffer+block, sizeof(char), 16, bc6file) != 16)
                throw "File error on write";
#endif

            block += 16;


                #ifdef _BC6H_COMPDEBUGGER // Checks decompression it should match or be close to source
                union DBLOCKS
                {
                    float            blockToSave[16][4];
                    float            block[64];
                } savedata;
        
                CMP_BYTE destBlock[BLOCK_SIZE_4X4X4];
                memset(savedata.block,0,sizeof(savedata));
                m_decoder->DecompressBlock(savedata.blockToSave,data.in);

                for (row=0; row<64; row++)
                {
                    destBlock[row] = (BYTE)savedata.block[row];
                }
                g_CompClient.SendData(3,sizeof(destBlock),destBlock);
                #endif


                if(pFeedbackProc)
                {
                    float fProgress = 100.f * (j * dwBlocksX) / (dwBlocksX * dwBlocksY);
                    if(pFeedbackProc(fProgress, pUser1, pUser2))
                    {
                        #ifdef _BC6H_COMPDEBUGGER
                            g_CompClient.disconnect();
                        #endif
                        CFinishBC6HEncoding();
                        return CE_Aborted;
                    }
                }
        }
    }


    #ifdef _SAVE_AS_BC6
    if (fclose(bc6file)) throw "Close failed on .bc6 file";
    #endif

    #ifdef BC6H_COMPDEBUGGER
    g_CompClient.disconnect();
    #endif

    #ifdef BC6H_DEBUG_TO_RESULTS_TXT
        if (g_fp)
        fclose(g_fp);
    #endif

    if(pFeedbackProc)
    {
        float fProgress = 100.f;
        pFeedbackProc(fProgress, pUser1, pUser2);
    }
        
    return CFinishBC6HEncoding();
}


CodecError CCodec_BC6H::Decompress(CCodecBuffer& bufferIn, CCodecBuffer& bufferOut, Codec_Feedback_Proc pFeedbackProc, DWORD_PTR pUser1, DWORD_PTR pUser2)
{
    assert(bufferIn.GetWidth() == bufferOut.GetWidth());
    assert(bufferIn.GetHeight() == bufferOut.GetHeight());
    
    CodecError err = CInitializeBC6HLibrary();
    if (err != CE_OK) return err;
    
    if(bufferIn.GetWidth() != bufferOut.GetWidth() || bufferIn.GetHeight() != bufferOut.GetHeight())
        return CE_Unknown;

    const CMP_DWORD dwBlocksX = ((bufferIn.GetWidth() + 3) >> 2);
    const CMP_DWORD dwBlocksY = ((bufferIn.GetHeight() + 3) >> 2);
    const CMP_DWORD dwBlocksXY = dwBlocksX*dwBlocksY;

    for(CMP_DWORD j = 0; j < dwBlocksY; j++)
    {
        for(CMP_DWORD i = 0; i < dwBlocksX; i++)
        {
            union FBLOCKS
            {
                float decodedBlock[16][4];
                float destBlock[BLOCK_SIZE_4X4X4];
            } DecData;

            union BBLOCKS
            {
                CMP_DWORD    compressedBlock[4];
                BYTE            out[16];
                BYTE            in[16];
            } CompData;

            CMP_FLOAT destBlock[BLOCK_SIZE_4X4X4];
            
            bufferIn.ReadBlock(i*4, j*4, CompData.compressedBlock, 4);

            // Encode to the appropriate location in the compressed image
            if (m_CodecType == CT_BC6H_SF)
                m_decoder->bc6signed = true;
            else
                m_decoder->bc6signed = false;

            m_decoder->DecompressBlock(DecData.decodedBlock,CompData.in);

            // Create the block for decoding
            float R,G,B,A;
            int srcIndex = 0;
            for(int row=0; row < BLOCK_SIZE_4; row++)
            {
                for(int col=0; col<BLOCK_SIZE_4; col++)
                {
                    R = (CMP_FLOAT)DecData.decodedBlock[row*BLOCK_SIZE_4+col][BC6H_COMP_RED];         
                    G = (CMP_FLOAT)DecData.decodedBlock[row*BLOCK_SIZE_4+col][BC6H_COMP_GREEN];     
                    B = (CMP_FLOAT)DecData.decodedBlock[row*BLOCK_SIZE_4+col][BC6H_COMP_BLUE];     
                    A = (CMP_FLOAT)DecData.decodedBlock[row*BLOCK_SIZE_4+col][BC6H_COMP_ALPHA];     
                    destBlock[srcIndex]   = R;         
                    destBlock[srcIndex+1] = G;     
                    destBlock[srcIndex+2] = B;     
                    destBlock[srcIndex+3] = A;     
                    srcIndex+=4;
                }
            }

            bufferOut.WriteBlockRGBA(i*4, j*4, 4, 4, (float *) destBlock);

        }

        if (pFeedbackProc)
        {
            float fProgress = 100.f * (j * dwBlocksX) / dwBlocksXY;
            if (pFeedbackProc(fProgress, pUser1, pUser2))
            {
                return CE_Aborted;
            }
        }

    }

    return CE_OK;
}

// Not implemented
CodecError CCodec_BC6H::Compress_Fast(CCodecBuffer& bufferIn, CCodecBuffer& bufferOut, Codec_Feedback_Proc pFeedbackProc, DWORD_PTR pUser1, DWORD_PTR pUser2)
{
    return CE_OK;
}

// Not implemented
CodecError CCodec_BC6H::Compress_SuperFast(CCodecBuffer& bufferIn, CCodecBuffer& bufferOut, Codec_Feedback_Proc pFeedbackProc, DWORD_PTR pUser1, DWORD_PTR pUser2)
{
   return CE_OK;
}
