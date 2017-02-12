/* ******************************************************************
   FSE : Finite State Entropy encoder
   Copyright (C) 2013-2015, Yann Collet.

   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    You can contact the author at :
    - FSE source repository : https://github.com/Cyan4973/FiniteStateEntropy
    - Public forum : https://groups.google.com/forum/#!forum/lz4c
****************************************************************** */

/* **************************************************************
*  Compiler specifics
****************************************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define FORCE_INLINE static __forceinline
#  include <intrin.h>                    /* For Visual 2005 */
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4214)        /* disable: C4214: non-int bitfields */
#else
#  if defined (__cplusplus) || defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   /* C99 */
#    ifdef __GNUC__
#      define FORCE_INLINE static inline __attribute__((always_inline))
#    else
#      define FORCE_INLINE static inline
#    endif
#  else
#    define FORCE_INLINE static
#  endif /* __STDC_VERSION__ */
#endif


/* **************************************************************
*  Includes
****************************************************************/
#include <stdlib.h>     /* malloc, free, qsort */
#include <string.h>     /* memcpy, memset */
#include <stdio.h>      /* printf (debug) */
#include "bitstream.h"
#define FSE_STATIC_LINKING_ONLY
#include "fse.h"

#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
#include "zstd_internal.h"  /* defaultCustomMem */
static ZSTD_customMem customMalloc = { ZSTD_defaultAllocFunction, ZSTD_defaultFreeFunction, "zstd-fse-c" };
#endif

/* **************************************************************
*  Error Management
****************************************************************/
#define FSE_STATIC_ASSERT(c) { enum { FSE_static_assert = 1/(int)(!!(c)) }; }   /* use only *after* variable declarations */


/* **************************************************************
*  Complex types
****************************************************************/
typedef U32 CTable_max_t[FSE_CTABLE_SIZE_U32(FSE_MAX_TABLELOG, FSE_MAX_SYMBOL_VALUE)];


/* **************************************************************
*  Templates
****************************************************************/
/*
  designed to be included
  for type-specific functions (template emulation in C)
  Objective is to write these functions only once, for improved maintenance
*/

/* safety checks */
#ifndef FSE_FUNCTION_EXTENSION
#  error "FSE_FUNCTION_EXTENSION must be defined"
#endif
#ifndef FSE_FUNCTION_TYPE
#  error "FSE_FUNCTION_TYPE must be defined"
#endif

/* Function names */
#define FSE_CAT(X,Y) X##Y
#define FSE_FUNCTION_NAME(X,Y) FSE_CAT(X,Y)
#define FSE_TYPE_NAME(X,Y) FSE_CAT(X,Y)


/* Function templates */
size_t FSE_buildCTable(FSE_CTable* ct, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog)
{
    U32 const tableSize = 1 << tableLog;
    U32 const tableMask = tableSize - 1;
    void* const ptr = ct;
    U16* const tableU16 = ( (U16*) ptr) + 2;
    void* const FSCT = ((U32*)ptr) + 1 /* header */ + (tableLog ? tableSize>>1 : 1) ;
    FSE_symbolCompressionTransform* const symbolTT = (FSE_symbolCompressionTransform*) (FSCT);
    U32 const step = FSE_TABLESTEP(tableSize);

#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
    U32* cumul = ZSTD_malloc(sizeof(U32) * (FSE_MAX_SYMBOL_VALUE+2), customMalloc);
    FSE_FUNCTION_TYPE* tableSymbol = ZSTD_malloc(sizeof(FSE_FUNCTION_TYPE) * FSE_MAX_TABLESIZE, customMalloc);
    /* checks */
    //if (!cumul) return ERROR(memory_allocation);
#else
    U32 cumul[FSE_MAX_SYMBOL_VALUE+2];
    FSE_FUNCTION_TYPE tableSymbol[FSE_MAX_TABLESIZE]; /* memset() is not necessary, even if static analyzer complain about it */
#endif
    U32 highThreshold = tableSize-1;

    /* CTable header */
    tableU16[-2] = (U16) tableLog;
    tableU16[-1] = (U16) maxSymbolValue;

    /* For explanations on how to distribute symbol values over the table :
    *  http://fastcompression.blogspot.fr/2014/02/fse-distributing-symbol-values.html */

    /* symbol start positions */
    {   U32 u;
        cumul[0] = 0;
        for (u=1; u<=maxSymbolValue+1; u++) {
            if (normalizedCounter[u-1]==-1) {  /* Low proba symbol */
                cumul[u] = cumul[u-1] + 1;
                tableSymbol[highThreshold--] = (FSE_FUNCTION_TYPE)(u-1);
            } else {
                cumul[u] = cumul[u-1] + normalizedCounter[u-1];
        }   }
        cumul[maxSymbolValue+1] = tableSize+1;
    }

    /* Spread symbols */
    {   U32 position = 0;
        U32 symbol;
        for (symbol=0; symbol<=maxSymbolValue; symbol++) {
            int nbOccurences;
            for (nbOccurences=0; nbOccurences<normalizedCounter[symbol]; nbOccurences++) {
                tableSymbol[position] = (FSE_FUNCTION_TYPE)symbol;
                position = (position + step) & tableMask;
                while (position > highThreshold) position = (position + step) & tableMask;   /* Low proba area */
        }   }

        if (position!=0) {
#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
            ZSTD_free(cumul, customMalloc);
            ZSTD_free(tableSymbol, customMalloc);
#endif
            return ERROR(GENERIC);   /* Must have gone through all positions */
        }
    }

    /* Build table */
    {   U32 u; for (u=0; u<tableSize; u++) {
        FSE_FUNCTION_TYPE s = tableSymbol[u];   /* note : static analyzer may not understand tableSymbol is properly initialized */
        tableU16[cumul[s]++] = (U16) (tableSize+u);   /* TableU16 : sorted by symbol order; gives next state value */
    }   }

    /* Build Symbol Transformation Table */
    {   unsigned total = 0;
        unsigned s;
        for (s=0; s<=maxSymbolValue; s++) {
            switch (normalizedCounter[s])
            {
            case  0: break;

            case -1:
            case  1:
                symbolTT[s].deltaNbBits = (tableLog << 16) - (1<<tableLog);
                symbolTT[s].deltaFindState = total - 1;
                total ++;
                break;
            default :
                {
                    U32 const maxBitsOut = tableLog - BIT_highbit32 (normalizedCounter[s]-1);
                    U32 const minStatePlus = normalizedCounter[s] << maxBitsOut;
                    symbolTT[s].deltaNbBits = (maxBitsOut << 16) - minStatePlus;
                    symbolTT[s].deltaFindState = total - normalizedCounter[s];
                    total +=  normalizedCounter[s];
    }   }   }   }

#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
    ZSTD_free(cumul, customMalloc);
    ZSTD_free(tableSymbol, customMalloc);
#endif

    return 0;
}



#ifndef FSE_COMMONDEFS_ONLY

/*-**************************************************************
*  FSE NCount encoding-decoding
****************************************************************/
size_t FSE_NCountWriteBound(unsigned maxSymbolValue, unsigned tableLog)
{
    size_t maxHeaderSize = (((maxSymbolValue+1) * tableLog) >> 3) + 3;
    return maxSymbolValue ? maxHeaderSize : FSE_NCOUNTBOUND;  /* maxSymbolValue==0 ? use default */
}

static short FSE_abs(short a) { return (short)(a<0 ? -a : a); }

static size_t FSE_writeNCount_generic (void* header, size_t headerBufferSize,
                                       const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog,
                                       unsigned writeIsSafe)
{
    BYTE* const ostart = (BYTE*) header;
    BYTE* out = ostart;
    BYTE* const oend = ostart + headerBufferSize;
    int nbBits;
    const int tableSize = 1 << tableLog;
    int remaining;
    int threshold;
    U32 bitStream;
    int bitCount;
    unsigned charnum = 0;
    int previous0 = 0;

    bitStream = 0;
    bitCount  = 0;
    /* Table Size */
    bitStream += (tableLog-FSE_MIN_TABLELOG) << bitCount;
    bitCount  += 4;

    /* Init */
    remaining = tableSize+1;   /* +1 for extra accuracy */
    threshold = tableSize;
    nbBits = tableLog+1;

    while (remaining>1) {  /* stops at 1 */
        if (previous0) {
            unsigned start = charnum;
            while (!normalizedCounter[charnum]) charnum++;
            while (charnum >= start+24) {
                start+=24;
                bitStream += 0xFFFFU << bitCount;
                if ((!writeIsSafe) && (out > oend-2)) return ERROR(dstSize_tooSmall);   /* Buffer overflow */
                out[0] = (BYTE) bitStream;
                out[1] = (BYTE)(bitStream>>8);
                out+=2;
                bitStream>>=16;
            }
            while (charnum >= start+3) {
                start+=3;
                bitStream += 3 << bitCount;
                bitCount += 2;
            }
            bitStream += (charnum-start) << bitCount;
            bitCount += 2;
            if (bitCount>16) {
                if ((!writeIsSafe) && (out > oend - 2)) return ERROR(dstSize_tooSmall);   /* Buffer overflow */
                out[0] = (BYTE)bitStream;
                out[1] = (BYTE)(bitStream>>8);
                out += 2;
                bitStream >>= 16;
                bitCount -= 16;
        }   }
        {   short count = normalizedCounter[charnum++];
            const short max = (short)((2*threshold-1)-remaining);
            remaining -= FSE_abs(count);
            if (remaining<1) return ERROR(GENERIC);
            count++;   /* +1 for extra accuracy */
            if (count>=threshold) count += max;   /* [0..max[ [max..threshold[ (...) [threshold+max 2*threshold[ */
            bitStream += count << bitCount;
            bitCount  += nbBits;
            bitCount  -= (count<max);
            previous0  = (count==1);
            while (remaining<threshold) nbBits--, threshold>>=1;
        }
        if (bitCount>16) {
            if ((!writeIsSafe) && (out > oend - 2)) return ERROR(dstSize_tooSmall);   /* Buffer overflow */
            out[0] = (BYTE)bitStream;
            out[1] = (BYTE)(bitStream>>8);
            out += 2;
            bitStream >>= 16;
            bitCount -= 16;
    }   }

    /* flush remaining bitStream */
    if ((!writeIsSafe) && (out > oend - 2)) return ERROR(dstSize_tooSmall);   /* Buffer overflow */
    out[0] = (BYTE)bitStream;
    out[1] = (BYTE)(bitStream>>8);
    out+= (bitCount+7) /8;

    if (charnum > maxSymbolValue + 1) return ERROR(GENERIC);

    return (out-ostart);
}


size_t FSE_writeNCount (void* buffer, size_t bufferSize, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog)
{
    if (tableLog > FSE_MAX_TABLELOG) return ERROR(GENERIC);   /* Unsupported */
    if (tableLog < FSE_MIN_TABLELOG) return ERROR(GENERIC);   /* Unsupported */

    if (bufferSize < FSE_NCountWriteBound(maxSymbolValue, tableLog))
        return FSE_writeNCount_generic(buffer, bufferSize, normalizedCounter, maxSymbolValue, tableLog, 0);

    return FSE_writeNCount_generic(buffer, bufferSize, normalizedCounter, maxSymbolValue, tableLog, 1);
}



/*-**************************************************************
*  Counting histogram
****************************************************************/
/*! FSE_count_simple
    This function just counts byte values within `src`,
    and store the histogram into table `count`.
    This function is unsafe : it doesn't check that all values within `src` can fit into `count`.
    For this reason, prefer using a table `count` with 256 elements.
    @return : count of most numerous element
*/
static size_t FSE_count_simple(unsigned* count, unsigned* maxSymbolValuePtr,
                               const void* src, size_t srcSize)
{
    const BYTE* ip = (const BYTE*)src;
    const BYTE* const end = ip + srcSize;
    unsigned maxSymbolValue = *maxSymbolValuePtr;
    unsigned max=0;


    memset(count, 0, (maxSymbolValue+1)*sizeof(*count));
    if (srcSize==0) { *maxSymbolValuePtr = 0; return 0; }

    while (ip<end) count[*ip++]++;

    while (!count[maxSymbolValue]) maxSymbolValue--;
    *maxSymbolValuePtr = maxSymbolValue;

    { U32 s; for (s=0; s<=maxSymbolValue; s++) if (count[s] > max) max = count[s]; }

    return (size_t)max;
}


static size_t FSE_count_parallel(unsigned* count, unsigned* maxSymbolValuePtr,
                                const void* source, size_t sourceSize,
                                unsigned checkMax)
{
    const BYTE* ip = (const BYTE*)source;
    const BYTE* const iend = ip+sourceSize;
    unsigned maxSymbolValue = *maxSymbolValuePtr;
    unsigned max=0;

#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
    U32* Counting1 = ZSTD_malloc(256 * sizeof(U32), customMalloc);
    U32* Counting2 = ZSTD_malloc(256 * sizeof(U32), customMalloc);
    U32* Counting3 = ZSTD_malloc(256 * sizeof(U32), customMalloc);
    U32* Counting4 = ZSTD_malloc(256 * sizeof(U32), customMalloc);
    memset(Counting1, 0, 256 * sizeof(U32));
    memset(Counting2, 0, 256 * sizeof(U32));
    memset(Counting3, 0, 256 * sizeof(U32));
    memset(Counting4, 0, 256 * sizeof(U32));
#else
    U32 Counting1[256] = { 0 };
    U32 Counting2[256] = { 0 };
    U32 Counting3[256] = { 0 };
    U32 Counting4[256] = { 0 };
#endif

    /* safety checks */
    if (!sourceSize) {
        memset(count, 0, maxSymbolValue + 1);
        *maxSymbolValuePtr = 0;
#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
        ZSTD_free(Counting1, customMalloc);
        ZSTD_free(Counting2, customMalloc);
        ZSTD_free(Counting3, customMalloc);
        ZSTD_free(Counting4, customMalloc);
#endif
        return 0;
    }
    if (!maxSymbolValue) maxSymbolValue = 255;            /* 0 == default */

    /* by stripes of 16 bytes */
    {   U32 cached = MEM_read32(ip); ip += 4;
        while (ip < iend-15) {
            U32 c = cached; cached = MEM_read32(ip); ip += 4;
            Counting1[(BYTE) c     ]++;
            Counting2[(BYTE)(c>>8) ]++;
            Counting3[(BYTE)(c>>16)]++;
            Counting4[       c>>24 ]++;
            c = cached; cached = MEM_read32(ip); ip += 4;
            Counting1[(BYTE) c     ]++;
            Counting2[(BYTE)(c>>8) ]++;
            Counting3[(BYTE)(c>>16)]++;
            Counting4[       c>>24 ]++;
            c = cached; cached = MEM_read32(ip); ip += 4;
            Counting1[(BYTE) c     ]++;
            Counting2[(BYTE)(c>>8) ]++;
            Counting3[(BYTE)(c>>16)]++;
            Counting4[       c>>24 ]++;
            c = cached; cached = MEM_read32(ip); ip += 4;
            Counting1[(BYTE) c     ]++;
            Counting2[(BYTE)(c>>8) ]++;
            Counting3[(BYTE)(c>>16)]++;
            Counting4[       c>>24 ]++;
        }
        ip-=4;
    }

    /* finish last symbols */
    while (ip<iend) Counting1[*ip++]++;

    if (checkMax) {   /* verify stats will fit into destination table */
        U32 s; for (s=255; s>maxSymbolValue; s--) {
            Counting1[s] += Counting2[s] + Counting3[s] + Counting4[s];
            if (Counting1[s]) {
#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
                ZSTD_free(Counting1, customMalloc);
                ZSTD_free(Counting2, customMalloc);
                ZSTD_free(Counting3, customMalloc);
                ZSTD_free(Counting4, customMalloc);
#endif
                return ERROR(maxSymbolValue_tooSmall);
            }
    }   }

    { U32 s; for (s=0; s<=maxSymbolValue; s++) {
        count[s] = Counting1[s] + Counting2[s] + Counting3[s] + Counting4[s];
        if (count[s] > max) max = count[s];
    }}

    while (!count[maxSymbolValue]) maxSymbolValue--;
    *maxSymbolValuePtr = maxSymbolValue;
#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
        ZSTD_free(Counting1, customMalloc);
        ZSTD_free(Counting2, customMalloc);
        ZSTD_free(Counting3, customMalloc);
        ZSTD_free(Counting4, customMalloc);
#endif
    return (size_t)max;
}

/* fast variant (unsafe : won't check if src contains values beyond count[] limit) */
size_t FSE_countFast(unsigned* count, unsigned* maxSymbolValuePtr,
                     const void* source, size_t sourceSize)
{
    if (sourceSize < 1500) return FSE_count_simple(count, maxSymbolValuePtr, source, sourceSize);
    return FSE_count_parallel(count, maxSymbolValuePtr, source, sourceSize, 0);
}

size_t FSE_count(unsigned* count, unsigned* maxSymbolValuePtr,
                 const void* source, size_t sourceSize)
{
    if (*maxSymbolValuePtr <255)
        return FSE_count_parallel(count, maxSymbolValuePtr, source, sourceSize, 1);
    *maxSymbolValuePtr = 255;
    return FSE_countFast(count, maxSymbolValuePtr, source, sourceSize);
}



/*-**************************************************************
*  FSE Compression Code
****************************************************************/
/*! FSE_sizeof_CTable() :
    FSE_CTable is a variable size structure which contains :
    `U16 tableLog;`
    `U16 maxSymbolValue;`
    `U16 nextStateNumber[1 << tableLog];`                         // This size is variable
    `FSE_symbolCompressionTransform symbolTT[maxSymbolValue+1];`  // This size is variable
Allocation is manual (C standard does not support variable-size structures).
*/

size_t FSE_sizeof_CTable (unsigned maxSymbolValue, unsigned tableLog)
{
    size_t size;
    FSE_STATIC_ASSERT((size_t)FSE_CTABLE_SIZE_U32(FSE_MAX_TABLELOG, FSE_MAX_SYMBOL_VALUE)*4 >= sizeof(CTable_max_t));   /* A compilation error here means FSE_CTABLE_SIZE_U32 is not large enough */
    if (tableLog > FSE_MAX_TABLELOG) return ERROR(GENERIC);
    size = FSE_CTABLE_SIZE_U32 (tableLog, maxSymbolValue) * sizeof(U32);
    return size;
}

FSE_CTable* FSE_createCTable (unsigned maxSymbolValue, unsigned tableLog)
{
    size_t size;
    if (tableLog > FSE_TABLELOG_ABSOLUTE_MAX) tableLog = FSE_TABLELOG_ABSOLUTE_MAX;
    size = FSE_CTABLE_SIZE_U32 (tableLog, maxSymbolValue) * sizeof(U32);
#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
    return (FSE_CTable*)ZSTD_malloc(size, customMalloc);
#else
    return (FSE_CTable*)malloc(size);
#endif
}

#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
void FSE_freeCTable (FSE_CTable* ct) { ZSTD_free(ct, customMalloc); }
#else
void FSE_freeCTable (FSE_CTable* ct) { free(ct); }
#endif

/* provides the minimum logSize to safely represent a distribution */
static unsigned FSE_minTableLog(size_t srcSize, unsigned maxSymbolValue)
{
	U32 minBitsSrc = BIT_highbit32((U32)(srcSize - 1)) + 1;
	U32 minBitsSymbols = BIT_highbit32(maxSymbolValue) + 2;
	U32 minBits = minBitsSrc < minBitsSymbols ? minBitsSrc : minBitsSymbols;
	return minBits;
}

unsigned FSE_optimalTableLog_internal(unsigned maxTableLog, size_t srcSize, unsigned maxSymbolValue, unsigned minus)
{
	U32 maxBitsSrc = BIT_highbit32((U32)(srcSize - 1)) - minus;
    U32 tableLog = maxTableLog;
	U32 minBits = FSE_minTableLog(srcSize, maxSymbolValue);
    if (tableLog==0) tableLog = FSE_DEFAULT_TABLELOG;
	if (maxBitsSrc < tableLog) tableLog = maxBitsSrc;   /* Accuracy can be reduced */
	if (minBits > tableLog) tableLog = minBits;   /* Need a minimum to safely represent all symbol values */
    if (tableLog < FSE_MIN_TABLELOG) tableLog = FSE_MIN_TABLELOG;
    if (tableLog > FSE_MAX_TABLELOG) tableLog = FSE_MAX_TABLELOG;
    return tableLog;
}

unsigned FSE_optimalTableLog(unsigned maxTableLog, size_t srcSize, unsigned maxSymbolValue)
{
    return FSE_optimalTableLog_internal(maxTableLog, srcSize, maxSymbolValue, 2);
}


/* Secondary normalization method.
   To be used when primary method fails. */

static size_t FSE_normalizeM2(short* norm, U32 tableLog, const unsigned* count, size_t total, U32 maxSymbolValue)
{
    U32 s;
    U32 distributed = 0;
    U32 ToDistribute;

    /* Init */
    U32 lowThreshold = (U32)(total >> tableLog);
    U32 lowOne = (U32)((total * 3) >> (tableLog + 1));

    for (s=0; s<=maxSymbolValue; s++) {
        if (count[s] == 0) {
            norm[s]=0;
            continue;
        }
        if (count[s] <= lowThreshold) {
            norm[s] = -1;
            distributed++;
            total -= count[s];
            continue;
        }
        if (count[s] <= lowOne) {
            norm[s] = 1;
            distributed++;
            total -= count[s];
            continue;
        }
        norm[s]=-2;
    }
    ToDistribute = (1 << tableLog) - distributed;

    if ((total / ToDistribute) > lowOne) {
        /* risk of rounding to zero */
        lowOne = (U32)((total * 3) / (ToDistribute * 2));
        for (s=0; s<=maxSymbolValue; s++) {
            if ((norm[s] == -2) && (count[s] <= lowOne)) {
                norm[s] = 1;
                distributed++;
                total -= count[s];
                continue;
        }   }
        ToDistribute = (1 << tableLog) - distributed;
    }

    if (distributed == maxSymbolValue+1) {
        /* all values are pretty poor;
           probably incompressible data (should have already been detected);
           find max, then give all remaining points to max */
        U32 maxV = 0, maxC = 0;
        for (s=0; s<=maxSymbolValue; s++)
            if (count[s] > maxC) maxV=s, maxC=count[s];
        norm[maxV] += (short)ToDistribute;
        return 0;
    }

    {
        U64 const vStepLog = 62 - tableLog;
        U64 const mid = (1ULL << (vStepLog-1)) - 1;
        U64 const rStep = ((((U64)1<<vStepLog) * ToDistribute) + mid) / total;   /* scale on remaining */
        U64 tmpTotal = mid;
        for (s=0; s<=maxSymbolValue; s++) {
            if (norm[s]==-2) {
                U64 end = tmpTotal + (count[s] * rStep);
                U32 sStart = (U32)(tmpTotal >> vStepLog);
                U32 sEnd = (U32)(end >> vStepLog);
                U32 weight = sEnd - sStart;
                if (weight < 1)
                    return ERROR(GENERIC);
                norm[s] = (short)weight;
                tmpTotal = end;
    }   }   }

    return 0;
}


size_t FSE_normalizeCount (short* normalizedCounter, unsigned tableLog,
                           const unsigned* count, size_t total,
                           unsigned maxSymbolValue)
{
    /* Sanity checks */
    if (tableLog==0) tableLog = FSE_DEFAULT_TABLELOG;
    if (tableLog < FSE_MIN_TABLELOG) return ERROR(GENERIC);   /* Unsupported size */
    if (tableLog > FSE_MAX_TABLELOG) return ERROR(tableLog_tooLarge);   /* Unsupported size */
    if (tableLog < FSE_minTableLog(total, maxSymbolValue)) return ERROR(GENERIC);   /* Too small tableLog, compression potentially impossible */

    {   U32 const rtbTable[] = {     0, 473195, 504333, 520860, 550000, 700000, 750000, 830000 };

        U64 const scale = 62 - tableLog;
        U64 const step = ((U64)1<<62) / total;   /* <== here, one division ! */
        U64 const vStep = 1ULL<<(scale-20);
        int stillToDistribute = 1<<tableLog;
        unsigned s;
        unsigned largest=0;
        short largestP=0;
        U32 lowThreshold = (U32)(total >> tableLog);

        for (s=0; s<=maxSymbolValue; s++) {
            if (count[s] == total) return 0;   /* rle special case */
            if (count[s] == 0) { normalizedCounter[s]=0; continue; }
            if (count[s] <= lowThreshold) {
                normalizedCounter[s] = -1;
                stillToDistribute--;
            } else {
                short proba = (short)((count[s]*step) >> scale);
                if (proba<8) {
                    U64 restToBeat = vStep * rtbTable[proba];
                    proba += (count[s]*step) - ((U64)proba<<scale) > restToBeat;
                }
                if (proba > largestP) largestP=proba, largest=s;
                normalizedCounter[s] = proba;
                stillToDistribute -= proba;
        }   }
        if (-stillToDistribute >= (normalizedCounter[largest] >> 1)) {
            /* corner case, need another normalization method */
            size_t errorCode = FSE_normalizeM2(normalizedCounter, tableLog, count, total, maxSymbolValue);
            if (FSE_isError(errorCode)) return errorCode;
        }
        else normalizedCounter[largest] += (short)stillToDistribute;
    }

#if 0
    {   /* Print Table (debug) */
        U32 s;
        U32 nTotal = 0;
        for (s=0; s<=maxSymbolValue; s++)
            printf("%3i: %4i \n", s, normalizedCounter[s]);
        for (s=0; s<=maxSymbolValue; s++)
            nTotal += abs(normalizedCounter[s]);
        if (nTotal != (1U<<tableLog))
            printf("Warning !!! Total == %u != %u !!!", nTotal, 1U<<tableLog);
        getchar();
    }
#endif

    return tableLog;
}


/* fake FSE_CTable, for raw (uncompressed) input */
size_t FSE_buildCTable_raw (FSE_CTable* ct, unsigned nbBits)
{
    const unsigned tableSize = 1 << nbBits;
    const unsigned tableMask = tableSize - 1;
    const unsigned maxSymbolValue = tableMask;
    void* const ptr = ct;
    U16* const tableU16 = ( (U16*) ptr) + 2;
    void* const FSCT = ((U32*)ptr) + 1 /* header */ + (tableSize>>1);   /* assumption : tableLog >= 1 */
    FSE_symbolCompressionTransform* const symbolTT = (FSE_symbolCompressionTransform*) (FSCT);
    unsigned s;

    /* Sanity checks */
    if (nbBits < 1) return ERROR(GENERIC);             /* min size */

    /* header */
    tableU16[-2] = (U16) nbBits;
    tableU16[-1] = (U16) maxSymbolValue;

    /* Build table */
    for (s=0; s<tableSize; s++)
        tableU16[s] = (U16)(tableSize + s);

    /* Build Symbol Transformation Table */
    {   const U32 deltaNbBits = (nbBits << 16) - (1 << nbBits);

        for (s=0; s<=maxSymbolValue; s++) {
            symbolTT[s].deltaNbBits = deltaNbBits;
            symbolTT[s].deltaFindState = s-1;
    }   }


    return 0;
}

/* fake FSE_CTable, for rle (100% always same symbol) input */
size_t FSE_buildCTable_rle (FSE_CTable* ct, BYTE symbolValue)
{
    void* ptr = ct;
    U16* tableU16 = ( (U16*) ptr) + 2;
    void* FSCTptr = (U32*)ptr + 2;
    FSE_symbolCompressionTransform* symbolTT = (FSE_symbolCompressionTransform*) FSCTptr;

    /* header */
    tableU16[-2] = (U16) 0;
    tableU16[-1] = (U16) symbolValue;

    /* Build table */
    tableU16[0] = 0;
    tableU16[1] = 0;   /* just in case */

    /* Build Symbol Transformation Table */
    symbolTT[symbolValue].deltaNbBits = 0;
    symbolTT[symbolValue].deltaFindState = 0;

    return 0;
}


static size_t FSE_compress_usingCTable_generic (void* dst, size_t dstSize,
                           const void* src, size_t srcSize,
                           const FSE_CTable* ct, const unsigned fast)
{
    const BYTE* const istart = (const BYTE*) src;
    const BYTE* const iend = istart + srcSize;
    const BYTE* ip=iend;


    BIT_CStream_t bitC;
    FSE_CState_t CState1, CState2;

    /* init */
    if (srcSize <= 2) return 0;
    { size_t const errorCode = BIT_initCStream(&bitC, dst, dstSize);
      if (FSE_isError(errorCode)) return 0; }

#define FSE_FLUSHBITS(s)  (fast ? BIT_flushBitsFast(s) : BIT_flushBits(s))

    if (srcSize & 1) {
        FSE_initCState2(&CState1, ct, *--ip);
        FSE_initCState2(&CState2, ct, *--ip);
        FSE_encodeSymbol(&bitC, &CState1, *--ip);
        FSE_FLUSHBITS(&bitC);
    } else {
        FSE_initCState2(&CState2, ct, *--ip);
        FSE_initCState2(&CState1, ct, *--ip);
    }

    /* join to mod 4 */
    srcSize -= 2;
    if ((sizeof(bitC.bitContainer)*8 > FSE_MAX_TABLELOG*4+7 ) && (srcSize & 2)) {  /* test bit 2 */
        FSE_encodeSymbol(&bitC, &CState2, *--ip);
        FSE_encodeSymbol(&bitC, &CState1, *--ip);
        FSE_FLUSHBITS(&bitC);
    }

    /* 2 or 4 encoding per loop */
    for ( ; ip>istart ; ) {

        FSE_encodeSymbol(&bitC, &CState2, *--ip);

        if (sizeof(bitC.bitContainer)*8 < FSE_MAX_TABLELOG*2+7 )   /* this test must be static */
            FSE_FLUSHBITS(&bitC);

        FSE_encodeSymbol(&bitC, &CState1, *--ip);

        if (sizeof(bitC.bitContainer)*8 > FSE_MAX_TABLELOG*4+7 ) {  /* this test must be static */
            FSE_encodeSymbol(&bitC, &CState2, *--ip);
            FSE_encodeSymbol(&bitC, &CState1, *--ip);
        }

        FSE_FLUSHBITS(&bitC);
    }

    FSE_flushCState(&bitC, &CState2);
    FSE_flushCState(&bitC, &CState1);
    return BIT_closeCStream(&bitC);
}

size_t FSE_compress_usingCTable (void* dst, size_t dstSize,
                           const void* src, size_t srcSize,
                           const FSE_CTable* ct)
{
    const unsigned fast = (dstSize >= FSE_BLOCKBOUND(srcSize));

    if (fast)
        return FSE_compress_usingCTable_generic(dst, dstSize, src, srcSize, ct, 1);
    else
        return FSE_compress_usingCTable_generic(dst, dstSize, src, srcSize, ct, 0);
}


size_t FSE_compressBound(size_t size) { return FSE_COMPRESSBOUND(size); }

size_t FSE_compress2 (void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned maxSymbolValue, unsigned tableLog)
{
    const BYTE* const istart = (const BYTE*) src;
    const BYTE* ip = istart;

    BYTE* const ostart = (BYTE*) dst;
    BYTE* op = ostart;
    BYTE* const oend = ostart + dstSize;
    size_t heap_return;

#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
    U32*   count = ZSTD_malloc(sizeof(U32) * (FSE_MAX_SYMBOL_VALUE+1), customMalloc);
    S16*   norm = ZSTD_malloc(sizeof(S16) * (FSE_MAX_SYMBOL_VALUE+1), customMalloc);
    FSE_CTable* ct = ZSTD_malloc(sizeof(CTable_max_t), customMalloc);
#else
    U32   count[FSE_MAX_SYMBOL_VALUE+1];
    S16   norm[FSE_MAX_SYMBOL_VALUE+1];
    CTable_max_t ct;
#endif
    size_t errorCode;

    /* init conditions */
    if (srcSize <= 1) {
        heap_return = 0;  /* Uncompressible */
        goto fsec2_err;
    }
    if (!maxSymbolValue) maxSymbolValue = FSE_MAX_SYMBOL_VALUE;
    if (!tableLog) tableLog = FSE_DEFAULT_TABLELOG;

    /* Scan input and build symbol stats */
    errorCode = FSE_count (count, &maxSymbolValue, ip, srcSize);
    if (FSE_isError(errorCode)) {
        heap_return = errorCode;
        goto fsec2_err;
    }
    if (errorCode == srcSize) {
        heap_return = 1;
        goto fsec2_err;
    }
    if (errorCode == 1) {
        heap_return = 0; /* each symbol only present once */
        goto fsec2_err;
    }
    if (errorCode < (srcSize >> 7)) {
        heap_return = 0; /* Heuristic : not compressible enough */
        goto fsec2_err;
    }

    tableLog = FSE_optimalTableLog(tableLog, srcSize, maxSymbolValue);
    errorCode = FSE_normalizeCount (norm, tableLog, count, srcSize, maxSymbolValue);
    if (FSE_isError(errorCode)) {
        heap_return = errorCode;
        goto fsec2_err;
    }

    /* Write table description header */
    errorCode = FSE_writeNCount (op, oend-op, norm, maxSymbolValue, tableLog);
    if (FSE_isError(errorCode)) {
        heap_return = errorCode;
        goto fsec2_err;
    }
    op += errorCode;

    /* Compress */
    errorCode = FSE_buildCTable (ct, norm, maxSymbolValue, tableLog);
    if (FSE_isError(errorCode)) {
        heap_return = errorCode;
        goto fsec2_err;
    }
    errorCode = FSE_compress_usingCTable(op, oend - op, ip, srcSize, ct);
    if (errorCode == 0) {
        heap_return = 0; /* not enough space for compressed data */
        goto fsec2_err;
    }
    op += errorCode;

    /* check compressibility */
    if ( (size_t)(op-ostart) >= srcSize-1 ) {
        heap_return = 0;
        goto fsec2_err;
    }

#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
    ZSTD_free(count, customMalloc);
    ZSTD_free(norm, customMalloc);
    ZSTD_free(ct, customMalloc);
#endif
    return op-ostart;

fsec2_err:
#if defined(ZSTD_HEAPMODE) && (ZSTD_HEAPMODE==1)
    ZSTD_free(count, customMalloc);
    ZSTD_free(norm, customMalloc);
    ZSTD_free(ct, customMalloc);
#endif
    return heap_return;
}

size_t FSE_compress (void* dst, size_t dstSize, const void* src, size_t srcSize)
{
    return FSE_compress2(dst, dstSize, src, (U32)srcSize, FSE_MAX_SYMBOL_VALUE, FSE_DEFAULT_TABLELOG);
}


#endif   /* FSE_COMMONDEFS_ONLY */
