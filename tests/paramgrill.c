/*
 * Copyright (c) 2015-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/*-************************************
*  Dependencies
**************************************/
#include "util.h"      /* Compiler options, UTIL_GetFileSize */
#include <stdlib.h>    /* malloc */
#include <stdio.h>     /* fprintf, fopen, ftello64 */
#include <string.h>    /* strcmp */
#include <math.h>      /* log */
#include <time.h>
#include <assert.h>

#include "mem.h"
#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_parameters, ZSTD_estimateCCtxSize */
#include "zstd.h"
#include "datagen.h"
#include "xxhash.h"
#include "util.h"
#include "bench.h"
#include "zstd_errors.h"
#include "zstd_internal.h"

/*-************************************
*  Constants
**************************************/
#define PROGRAM_DESCRIPTION "ZSTD parameters tester"
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s %s %i-bits, by %s ***\n", PROGRAM_DESCRIPTION, ZSTD_VERSION_STRING, (int)(sizeof(void*)*8), AUTHOR

#define TIMELOOP_NANOSEC      (1*1000000000ULL) /* 1 second */

#define NBLOOPS    2
#define TIMELOOP  (2 * SEC_TO_MICRO)
#define NB_LEVELS_TRACKED 22   /* ensured being >= ZSTD_maxCLevel() in BMK_init_level_constraints() */

static const size_t maxMemory = (sizeof(size_t)==4)  ?  (2 GB - 64 MB) : (size_t)(1ULL << ((sizeof(size_t)*8)-31));

#define COMPRESSIBILITY_DEFAULT 0.50

static const U64 g_maxVariationTime = 60 * SEC_TO_MICRO;
static const int g_maxNbVariations = 64;

/*-************************************
*  Macros
**************************************/
#define DISPLAY(...)  fprintf(stderr, __VA_ARGS__)
#define TIMED 0
#ifndef DEBUG
#  define DEBUG 0
#endif
#define DEBUGOUTPUT(...) { if (DEBUG) DISPLAY(__VA_ARGS__); }

#undef MIN
#undef MAX
#define MIN(a,b)   ( (a) < (b) ? (a) : (b) )
#define MAX(a,b)   ( (a) > (b) ? (a) : (b) )
#define CUSTOM_LEVEL 99

/* indices for each of the variables */
typedef enum {
    wlog_ind = 0,
    clog_ind = 1,
    hlog_ind = 2,
    slog_ind = 3,
    slen_ind = 4,
    tlen_ind = 5,
    strt_ind = 6
} varInds_t;

#define NUM_PARAMS 7
/* just don't use strategy as a param. */ 

#undef ZSTD_WINDOWLOG_MAX
#define ZSTD_WINDOWLOG_MAX 27 //no long range stuff for now. 

#define ZSTD_TARGETLENGTH_MIN 0 
#define ZSTD_TARGETLENGTH_MAX 999

#define WLOG_RANGE (ZSTD_WINDOWLOG_MAX - ZSTD_WINDOWLOG_MIN + 1)
#define CLOG_RANGE (ZSTD_CHAINLOG_MAX - ZSTD_CHAINLOG_MIN + 1)
#define HLOG_RANGE (ZSTD_HASHLOG_MAX - ZSTD_HASHLOG_MIN + 1)
#define SLOG_RANGE (ZSTD_SEARCHLOG_MAX - ZSTD_SEARCHLOG_MIN + 1)
#define SLEN_RANGE (ZSTD_SEARCHLENGTH_MAX - ZSTD_SEARCHLENGTH_MIN + 1)
#define TLEN_RANGE 17
#define STRT_RANGE (ZSTD_btultra - ZSTD_fast + 1)
/* TLEN_RANGE picked manually */

static const int rangetable[NUM_PARAMS] = { WLOG_RANGE, CLOG_RANGE, HLOG_RANGE, SLOG_RANGE, SLEN_RANGE, TLEN_RANGE, STRT_RANGE };
static const U32 tlen_table[TLEN_RANGE] = { 0, 1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 256, 512, 999 };
/*-************************************
*  Benchmark Parameters/Global Variables
**************************************/

typedef BYTE U8;

static double g_grillDuration_s = 99999;   /* about 27 hours */
static U32 g_nbIterations = NBLOOPS;
static double g_compressibility = COMPRESSIBILITY_DEFAULT;
static U32 g_blockSize = 0;
static U32 g_rand = 1;
static U32 g_singleRun = 0;
static U32 g_optimizer = 0;
static U32 g_target = 0;
static U32 g_noSeed = 0;
static ZSTD_compressionParameters g_params = { 0, 0, 0, 0, 0, 0, ZSTD_greedy };
static UTIL_time_t g_time; /* to be used to compare solution finding speeds to compare to original */


typedef struct {
    BMK_result_t result;
    ZSTD_compressionParameters params;
} winnerInfo_t;

typedef struct {
    U32 cSpeed;  /* bytes / sec */
    U32 dSpeed;
    U32 cMem;    /* bytes */    
} constraint_t;

typedef struct winner_ll_node winner_ll_node;
struct winner_ll_node {
    winnerInfo_t res;
    winner_ll_node* next;
};

static winner_ll_node* g_winners; /* linked list sorted ascending by cSize & cSpeed */
static BMK_result_t g_lvltarget;
static int g_optmode = 0;

static U32 g_speedMultiplier = 1;
static U32 g_ratioMultiplier = 5;

/* g_mode? */

/* range 0 - 99, measure of how strict  */
#define DEFAULT_STRICTNESS 99999
static U32 g_strictness = DEFAULT_STRICTNESS;

void BMK_SetNbIterations(int nbLoops)
{
    g_nbIterations = nbLoops;
    DISPLAY("- %u iterations -\n", g_nbIterations);
}

/*
 * Additional Global Variables (Defined Above Use)
 * g_stratName
 * g_level_constraint
 * g_alreadyTested
 * g_maxTries
 */

/*-*******************************************************
*  Private functions
*********************************************************/

/* accuracy in seconds only, span can be multiple years */
static double BMK_timeSpan(time_t tStart) { return difftime(time(NULL), tStart); }

static size_t BMK_findMaxMem(U64 requiredMem)
{
    size_t const step = 64 MB;
    void* testmem = NULL;

    requiredMem = (((requiredMem >> 26) + 1) << 26);
    if (requiredMem > maxMemory) requiredMem = maxMemory;

    requiredMem += 2*step;
    while (!testmem) {
        requiredMem -= step;
        testmem = malloc ((size_t)requiredMem);
    }

    free (testmem);
    return (size_t) (requiredMem - step);
}


static U32 FUZ_rotl32(U32 x, U32 r)
{
    return ((x << r) | (x >> (32 - r)));
}

U32 FUZ_rand(U32* src)
{
    const U32 prime1 = 2654435761U;
    const U32 prime2 = 2246822519U;
    U32 rand32 = *src;
    rand32 *= prime1;
    rand32 += prime2;
    rand32  = FUZ_rotl32(rand32, 13);
    *src = rand32;
    return rand32 >> 5;
}

/** longCommandWArg() :
 *  check if *stringPtr is the same as longCommand.
 *  If yes, @return 1 and advances *stringPtr to the position which immediately follows longCommand.
 * @return 0 and doesn't modify *stringPtr otherwise.
 * from zstdcli.c
 */
static unsigned longCommandWArg(const char** stringPtr, const char* longCommand)
{
    size_t const comSize = strlen(longCommand);
    int const result = !strncmp(*stringPtr, longCommand, comSize);
    if (result) *stringPtr += comSize;
    return result;
}

static U64 g_clockGranularity = 100000000ULL;

static void findClockGranularity(void) {
    UTIL_time_t clockStart = UTIL_getTime();
    U64 el1 = 0, el2 = 0;
    int i = 0;
    do {
        el1 = el2;
        el2 = UTIL_clockSpanNano(clockStart);
        if(el1 < el2) {
            U64 iv = el2 - el1;
            if(g_clockGranularity > iv) {
                g_clockGranularity = iv;
                i = 0;
            } else {
                i++;
            }
        }
    } while(i < 10);
    DEBUGOUTPUT("Granularity: %llu\n", (unsigned long long)g_clockGranularity);
}

#define CLAMPCHECK(val,min,max) {                     \
    if (val && (((val)<(min)) | ((val)>(max)))) {     \
        DISPLAY("INVALID PARAMETER CONSTRAINTS\n");   \
        return 0;                                     \
}   }


/* Like ZSTD_checkCParams() but allows 0's */
/* no check on targetLen? */
static int cParamValid(ZSTD_compressionParameters paramTarget) {
    CLAMPCHECK(paramTarget.hashLog, ZSTD_HASHLOG_MIN, ZSTD_HASHLOG_MAX);
    CLAMPCHECK(paramTarget.searchLog, ZSTD_SEARCHLOG_MIN, ZSTD_SEARCHLOG_MAX);
    CLAMPCHECK(paramTarget.searchLength, ZSTD_SEARCHLENGTH_MIN, ZSTD_SEARCHLENGTH_MAX);
    CLAMPCHECK(paramTarget.windowLog, ZSTD_WINDOWLOG_MIN, ZSTD_WINDOWLOG_MAX);
    CLAMPCHECK(paramTarget.chainLog, ZSTD_CHAINLOG_MIN, ZSTD_CHAINLOG_MAX);
    if(paramTarget.targetLength > ZSTD_TARGETLENGTH_MAX) {
        DISPLAY("INVALID PARAMETER CONSTRAINTS\n");
        return 0;
    }
    if(paramTarget.strategy > ZSTD_btultra) {
        DISPLAY("INVALID PARAMETER CONSTRAINTS\n");
        return 0;
    }
    return 1;
}

static void cParamZeroMin(ZSTD_compressionParameters* paramTarget) {
    paramTarget->windowLog = paramTarget->windowLog ? paramTarget->windowLog : ZSTD_WINDOWLOG_MIN;
    paramTarget->searchLog = paramTarget->searchLog ? paramTarget->searchLog : ZSTD_SEARCHLOG_MIN;
    paramTarget->chainLog = paramTarget->chainLog ? paramTarget->chainLog : ZSTD_CHAINLOG_MIN;
    paramTarget->hashLog = paramTarget->hashLog ? paramTarget->hashLog : ZSTD_HASHLOG_MIN;
    paramTarget->searchLength = paramTarget->searchLength ? paramTarget->searchLength : ZSTD_SEARCHLENGTH_MIN;
    paramTarget->targetLength = paramTarget->targetLength ? paramTarget->targetLength : 0;
}

static void BMK_translateAdvancedParams(const ZSTD_compressionParameters params)
{
    DISPLAY("--zstd=windowLog=%u,chainLog=%u,hashLog=%u,searchLog=%u,searchLength=%u,targetLength=%u,strategy=%u \n",
             params.windowLog, params.chainLog, params.hashLog, params.searchLog, params.searchLength, params.targetLength, (U32)(params.strategy));
}

/* checks results are feasible */
static int feasible(const BMK_result_t results, const constraint_t target) {
    return (results.cSpeed >= target.cSpeed) && (results.dSpeed >= target.dSpeed) && (results.cMem <= target.cMem) && (!g_optmode || results.cSize <= g_lvltarget.cSize);
}

/* hill climbing value for part 1 */
/* Scoring here is a linear reward for all set constraints normalized between 0 to 1 
 * (with 0 at 0 and 1 being fully fulfilling the constraint), summed with a logarithmic 
 * bonus to exceeding the constraint value. We also give linear ratio for compression ratio.
 * The constant factors are experimental. 
 */
static double resultScore(const BMK_result_t res, const size_t srcSize, const constraint_t target) {
    double cs = 0., ds = 0., rt, cm = 0.;
    const double r1 = 1, r2 = 0.1, rtr = 0.5;
    double ret;
    if(target.cSpeed) { cs = res.cSpeed / (double)target.cSpeed; }
    if(target.dSpeed) { ds = res.dSpeed / (double)target.dSpeed; }
    if(target.cMem != (U32)-1) { cm = (double)target.cMem / res.cMem; }
    rt = ((double)srcSize / res.cSize);

    ret = (MIN(1, cs) + MIN(1, ds)  + MIN(1, cm))*r1 + rt * rtr + 
         (MAX(0, log(cs))+ MAX(0, log(ds))+ MAX(0, log(cm))) * r2;

    return ret;
}

/* calculates normalized squared euclidean distance of result1 if it is in the first quadrant relative to lvlRes */
static double resultDistLvl(const BMK_result_t result1, const BMK_result_t lvlRes) {
    double normalizedCSpeedGain1 = (result1.cSpeed / lvlRes.cSpeed) - 1;
    double normalizedRatioGain1 = ((double)lvlRes.cSize / result1.cSize) - 1;
    if(normalizedRatioGain1 < 0 || normalizedCSpeedGain1 < 0) {
        return 0.0;
    }
    return normalizedRatioGain1 * g_ratioMultiplier + normalizedCSpeedGain1 * g_speedMultiplier;
}

/* return true if r2 strictly better than r1 */ 
static int compareResultLT(const BMK_result_t result1, const BMK_result_t result2, const constraint_t target, size_t srcSize) {
    if(feasible(result1, target) && feasible(result2, target)) {
        if(g_optmode) {
            return resultDistLvl(result1, g_lvltarget) < resultDistLvl(result2, g_lvltarget);
        } else {
            return (result1.cSize > result2.cSize) || (result1.cSize == result2.cSize && result2.cSpeed > result1.cSpeed)
            || (result1.cSize == result2.cSize && result2.cSpeed == result1.cSpeed && result2.dSpeed > result1.dSpeed);
        }
    }
    return feasible(result2, target) || (!feasible(result1, target) && (resultScore(result1, srcSize, target) < resultScore(result2, srcSize, target)));
}

static constraint_t relaxTarget(constraint_t target) {
    target.cMem = (U32)-1;
    target.cSpeed *= ((double)g_strictness) / 100; 
    target.dSpeed *= ((double)g_strictness) / 100;
    return target;
}

/*-*******************************************************
*  Bench functions
*********************************************************/

const char* g_stratName[ZSTD_btultra+1] = {
                "(none)       ", "ZSTD_fast    ", "ZSTD_dfast   ",
                "ZSTD_greedy  ", "ZSTD_lazy    ", "ZSTD_lazy2   ",
                "ZSTD_btlazy2 ", "ZSTD_btopt   ", "ZSTD_btultra "};

static ZSTD_compressionParameters emptyParams(void) {
    ZSTD_compressionParameters p = { 0, 0, 0, 0, 0, 0, (ZSTD_strategy)0 };
    return p;
}

static winnerInfo_t initWinnerInfo(ZSTD_compressionParameters p) {
    winnerInfo_t w1;
    w1.result.cSpeed = 0.;
    w1.result.dSpeed = 0.;
    w1.result.cMem = (size_t)-1;
    w1.result.cSize = (size_t)-1;
    w1.params = p;
    return w1;
}

typedef struct {
    void* srcBuffer;
    size_t srcSize;
    const void** srcPtrs;
    size_t* srcSizes;
    void** dstPtrs;
    size_t* dstCapacities;
    size_t* dstSizes;
    void** resPtrs;
    size_t* resSizes;
    size_t nbBlocks;
} buffers_t;

typedef struct {
    size_t dictSize;
    void* dictBuffer;
    ZSTD_CCtx* cctx;
    ZSTD_DCtx* dctx;
} contexts_t;

/*-*******************************************************
*  From bench.c
*********************************************************/

static void BMK_initCCtx(ZSTD_CCtx* ctx, 
    const void* dictBuffer, const size_t dictBufferSize, const int cLevel, 
    const ZSTD_compressionParameters* comprParams, const BMK_advancedParams_t* adv) {
    ZSTD_CCtx_reset(ctx);
    ZSTD_CCtx_resetParameters(ctx);
    if (adv->nbWorkers==1) {
        ZSTD_CCtx_setParameter(ctx, ZSTD_p_nbWorkers, 0);
    } else {
        ZSTD_CCtx_setParameter(ctx, ZSTD_p_nbWorkers, adv->nbWorkers);
    }
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_compressionLevel, cLevel);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_enableLongDistanceMatching, adv->ldmFlag);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_ldmMinMatch, adv->ldmMinMatch);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_ldmHashLog, adv->ldmHashLog);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_ldmBucketSizeLog, adv->ldmBucketSizeLog);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_ldmHashEveryLog, adv->ldmHashEveryLog);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_windowLog, comprParams->windowLog);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_hashLog, comprParams->hashLog);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_chainLog, comprParams->chainLog);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_searchLog, comprParams->searchLog);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_minMatch, comprParams->searchLength);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_targetLength, comprParams->targetLength);
    ZSTD_CCtx_setParameter(ctx, ZSTD_p_compressionStrategy, comprParams->strategy);
    ZSTD_CCtx_loadDictionary(ctx, dictBuffer, dictBufferSize);
}

static void BMK_initDCtx(ZSTD_DCtx* dctx, 
    const void* dictBuffer, const size_t dictBufferSize) {
    ZSTD_DCtx_reset(dctx);
    ZSTD_DCtx_loadDictionary(dctx, dictBuffer, dictBufferSize);
}

typedef struct {
    ZSTD_CCtx* ctx;
    const void* dictBuffer;
    size_t dictBufferSize;
    int cLevel;
    const ZSTD_compressionParameters* comprParams;
    const BMK_advancedParams_t* adv;
} BMK_initCCtxArgs;

static size_t local_initCCtx(void* payload) {
    const BMK_initCCtxArgs* ag = (const BMK_initCCtxArgs*)payload;
    BMK_initCCtx(ag->ctx, ag->dictBuffer, ag->dictBufferSize, ag->cLevel, ag->comprParams, ag->adv);
    return 0;
}

typedef struct {
    ZSTD_DCtx* dctx;
    const void* dictBuffer;
    size_t dictBufferSize;
} BMK_initDCtxArgs;

static size_t local_initDCtx(void* payload) {
    const BMK_initDCtxArgs* ag = (const BMK_initDCtxArgs*)payload;
    BMK_initDCtx(ag->dctx, ag->dictBuffer, ag->dictBufferSize);
    return 0;
}

/* additional argument is just the context */
static size_t local_defaultCompress(
    const void* srcBuffer, size_t srcSize, 
    void* dstBuffer, size_t dstSize, 
    void* addArgs) {
    size_t moreToFlush = 1;
    ZSTD_CCtx* ctx = (ZSTD_CCtx*)addArgs;
    ZSTD_inBuffer in;
    ZSTD_outBuffer out;
    in.src = srcBuffer;
    in.size = srcSize;
    in.pos = 0;
    out.dst = dstBuffer;
    out.size = dstSize;
    out.pos = 0;
    assert(dstSize == ZSTD_compressBound(srcSize)); /* specific to this version, which is only used in paramgrill */
    while (moreToFlush) {
        if(out.pos == out.size) {
            return (size_t)-ZSTD_error_dstSize_tooSmall;
        }
        moreToFlush = ZSTD_compress_generic(ctx, &out, &in, ZSTD_e_end);
        if (ZSTD_isError(moreToFlush)) {
            return moreToFlush;
        }
    }
    return out.pos;
}

/* additional argument is just the context */
static size_t local_defaultDecompress(
    const void* srcBuffer, size_t srcSize, 
    void* dstBuffer, size_t dstSize, 
    void* addArgs) {
    size_t moreToFlush = 1;
    ZSTD_DCtx* dctx = (ZSTD_DCtx*)addArgs;
    ZSTD_inBuffer in;
    ZSTD_outBuffer out;
    in.src = srcBuffer;
    in.size = srcSize;
    in.pos = 0;
    out.dst = dstBuffer;
    out.size = dstSize;
    out.pos = 0;
    while (moreToFlush) {
        if(out.pos == out.size) {
            return (size_t)-ZSTD_error_dstSize_tooSmall;
        }
        moreToFlush = ZSTD_decompress_generic(dctx,
                            &out, &in);
        if (ZSTD_isError(moreToFlush)) {
            return moreToFlush;
        }
    }
    return out.pos;

}

/*-*******************************************************
*  From bench.c End
*********************************************************/

static void freeBuffers(buffers_t b) {
    if(b.srcPtrs != NULL) {
        free(b.srcBuffer);
    }
    free(b.srcPtrs);
    free(b.srcSizes);

    if(b.dstPtrs != NULL) {
        free(b.dstPtrs[0]);
    }
    free(b.dstPtrs);
    free(b.dstCapacities);
    free(b.dstSizes);

    if(b.resPtrs != NULL) {
        free(b.resPtrs[0]);
    }
    free(b.resPtrs);
}

/* srcBuffer will be freed by freeBuffers now */
static int createBuffersFromMemory(buffers_t* buff, void * srcBuffer, size_t nbFiles,
    const size_t* fileSizes)
{
    size_t pos = 0, n, blockSize;
    U32 maxNbBlocks, blockNb = 0;
    buff->srcSize = 0;
    for(n = 0; n < nbFiles; n++) {
        buff->srcSize += fileSizes[n];
    }

    if(buff->srcSize == 0) {
        DISPLAY("No data to bench\n");
        return 1;
    }

    blockSize = g_blockSize ? g_blockSize : buff->srcSize;
    maxNbBlocks = (U32) ((buff->srcSize + (blockSize-1)) / blockSize) + (U32)nbFiles;

    buff->srcPtrs = (const void**)calloc(maxNbBlocks, sizeof(void*));
    buff->srcSizes = (size_t*)malloc(maxNbBlocks * sizeof(size_t));

    buff->dstPtrs = (void**)calloc(maxNbBlocks, sizeof(void*));
    buff->dstCapacities = (size_t*)malloc(maxNbBlocks * sizeof(size_t));
    buff->dstSizes = (size_t*)malloc(maxNbBlocks * sizeof(size_t));

    buff->resPtrs = (void**)calloc(maxNbBlocks, sizeof(void*));
    buff->resSizes = (size_t*)malloc(maxNbBlocks * sizeof(size_t)); 

    if(!buff->srcPtrs || !buff->srcSizes || !buff->dstPtrs || !buff->dstCapacities || !buff->dstSizes || !buff->resPtrs || !buff->resSizes) {
        DISPLAY("alloc error\n");
        freeBuffers(*buff);
        return 1;
    }

    buff->srcBuffer = srcBuffer;
    buff->srcPtrs[0] = (const void*)buff->srcBuffer;
    buff->dstPtrs[0] = malloc(ZSTD_compressBound(buff->srcSize) + (maxNbBlocks * 1024));
    buff->resPtrs[0] = malloc(buff->srcSize);

    if(!buff->dstPtrs[0] || !buff->resPtrs[0]) {
        DISPLAY("alloc error\n");
        freeBuffers(*buff);
        return 1;
    }

    for(n = 0; n < nbFiles; n++) {
        size_t pos_end = pos + fileSizes[n];
        for(; pos < pos_end; blockNb++) {
            buff->srcPtrs[blockNb] = (const void*)((char*)srcBuffer + pos);
            buff->srcSizes[blockNb] = blockSize;
            pos += blockSize;
        }

        if(fileSizes[n] > 0) { buff->srcSizes[blockNb - 1] = ((fileSizes[n] - 1) % blockSize) + 1; }
        pos = pos_end;
    }

    buff->dstCapacities[0] = ZSTD_compressBound(buff->srcSizes[0]);
    buff->dstSizes[0] = buff->dstCapacities[0];
    buff->resSizes[0] = buff->srcSizes[0];

    for(n = 1; n < blockNb; n++) {
        buff->dstPtrs[n] = ((char*)buff->dstPtrs[n-1]) + buff->dstCapacities[n-1];
        buff->resPtrs[n] = ((char*)buff->resPtrs[n-1]) + buff->resSizes[n-1];
        buff->dstCapacities[n] = ZSTD_compressBound(buff->srcSizes[n]);
        buff->dstSizes[n] = buff->dstCapacities[n];
        buff->resSizes[n] = buff->srcSizes[n];
    }

    buff->nbBlocks = blockNb;

    return 0;
} 

/* allocates buffer's arguments. returns success / failuere */
static int createBuffers(buffers_t* buff, const char* const * const fileNamesTable, 
                          size_t nbFiles) {
    size_t pos = 0;
    size_t n;
    size_t totalSizeToLoad = UTIL_getTotalFileSize(fileNamesTable, (U32)nbFiles);
    size_t benchedSize = MIN(BMK_findMaxMem(totalSizeToLoad * 3) / 3, totalSizeToLoad);
    size_t* fileSizes = calloc(sizeof(size_t), nbFiles);
    void* srcBuffer = malloc(benchedSize);
    int ret = 0;  


    if(!fileSizes || !srcBuffer) {
        return 1;
    }

    for(n = 0; n < nbFiles; n++) {
        FILE* f;
        U64 fileSize = UTIL_getFileSize(fileNamesTable[n]);
        if (UTIL_isDirectory(fileNamesTable[n])) {
            DISPLAY("Ignoring %s directory...       \n", fileNamesTable[n]);
            continue;
        }
        if (fileSize == UTIL_FILESIZE_UNKNOWN) {
            DISPLAY("Cannot evaluate size of %s, ignoring ... \n", fileNamesTable[n]);
            continue;
        }
        f = fopen(fileNamesTable[n], "rb");
        if (f==NULL) {
            DISPLAY("impossible to open file %s\n", fileNamesTable[n]);
            free(fileSizes);
            free(srcBuffer);
            fclose(f);
            return 10;
        }

        DISPLAY("Loading %s...       \r", fileNamesTable[n]);

        if (fileSize + pos > benchedSize) fileSize = benchedSize - pos, nbFiles=n;   /* buffer too small - stop after this file */ 

        {
            char* buffer = (char*)(srcBuffer); 
            size_t const readSize = fread((buffer)+pos, 1, (size_t)fileSize, f);

            if (readSize != (size_t)fileSize) { /* should we accept partial read? */
                DISPLAY("could not read %s", fileNamesTable[n]);
                free(fileSizes);
                free(srcBuffer);
                return 1;
            }

            fileSizes[n] = readSize;
            pos += readSize;
        }
        fclose(f);
    }

    ret = createBuffersFromMemory(buff, srcBuffer, nbFiles, fileSizes);
    free(fileSizes);
    return ret;

}

static void freeContexts(contexts_t ctx) {
    free(ctx.dictBuffer);
    ZSTD_freeCCtx(ctx.cctx);
    ZSTD_freeDCtx(ctx.dctx);
}

static int createContexts(contexts_t* ctx, const char* dictFileName) {
    FILE* f;
    size_t readSize;
    ctx->cctx = ZSTD_createCCtx();
    ctx->dctx = ZSTD_createDCtx();
    if(dictFileName == NULL) {
        ctx->dictSize = 0;
        ctx->dictBuffer = NULL;
        return 0;
    }
    ctx->dictSize = UTIL_getFileSize(dictFileName);
    ctx->dictBuffer = malloc(ctx->dictSize);

    f = fopen(dictFileName, "rb");
    
    if(!f) {
        DISPLAY("unable to open file\n");
        fclose(f);
        freeContexts(*ctx);
        return 1;
    }
    
    if(ctx->dictSize > 64 MB || !(ctx->dictBuffer)) {
        DISPLAY("dictionary too large\n");
        fclose(f);
        freeContexts(*ctx);
        return 1;
    }
    readSize = fread(ctx->dictBuffer, 1, ctx->dictSize, f);
    if(readSize != ctx->dictSize) {
        DISPLAY("unable to read file\n");
        fclose(f);
        freeContexts(*ctx);
        return 1;
    }
    return 0;
}

/* Replicate function of benchMemAdvanced, but with pre-split src / dst buffers, with relevant info to invert it (compressedSizes) passed out. */
/* BMK_benchMemAdvanced(srcBuffer,srcSize, dstBuffer, dstSize, fileSizes, nbFiles, 0, &cParams, dictBuffer, dictSize, ctx, dctx, 0, "File", &adv); */
/* nbSeconds used in same way as in BMK_advancedParams_t, as nbIters when in iterMode */

/* if in decodeOnly, then srcPtr's will be compressed blocks, and uncompressedBlocks will be written to dstPtrs? */
/* dictionary nullable, nothing else though. */
static BMK_return_t BMK_benchMemInvertible(buffers_t buf, contexts_t ctx, 
                        const int cLevel, const ZSTD_compressionParameters* comprParams,
                        const BMK_mode_t mode, const BMK_loopMode_t loopMode, const unsigned nbSeconds) {

    U32 i;
    BMK_return_t results = { { 0, 0., 0., 0 }, 0 } ;
    const void *const *const srcPtrs = (const void *const *const)buf.srcPtrs;
    size_t const *const srcSizes = buf.srcSizes;
    void** dstPtrs = buf.dstPtrs;
    size_t* dstCapacities = buf.dstCapacities;
    size_t* dstSizes = buf.dstSizes;
    void** resPtrs = buf.resPtrs;
    size_t* resSizes = buf.resSizes;
    const void* dictBuffer = ctx.dictBuffer;
    const size_t dictBufferSize = ctx.dictSize;
    const size_t nbBlocks = buf.nbBlocks;
    const size_t srcSize = buf.srcSize;
    ZSTD_CCtx* cctx = ctx.cctx;
    ZSTD_DCtx* dctx = ctx.dctx;

    BMK_advancedParams_t adv = BMK_initAdvancedParams();
    adv.mode = mode;
    adv.loopMode = loopMode;
    adv.nbSeconds = nbSeconds;

    /* warmimg up memory */
    /* can't do this if decode only */
    for(i = 0; i < buf.nbBlocks; i++) {
        if(mode != BMK_decodeOnly) {
            RDG_genBuffer(dstPtrs[i], dstCapacities[i], 0.10, 0.50, 1);
        } else {
            RDG_genBuffer(resPtrs[i], resSizes[i], 0.10, 0.50, 1);
        }
    }

    /* Bench */
       
    {
        /* init args */
        BMK_initCCtxArgs cctxprep;
        BMK_initDCtxArgs dctxprep;
        cctxprep.ctx = cctx;
        cctxprep.dictBuffer = dictBuffer;
        cctxprep.dictBufferSize = dictBufferSize;
        cctxprep.cLevel = cLevel;
        cctxprep.comprParams = comprParams;
        cctxprep.adv = &adv;
        dctxprep.dctx = dctx;
        dctxprep.dictBuffer = dictBuffer;
        dctxprep.dictBufferSize = dictBufferSize;

        if(loopMode == BMK_timeMode) {
            BMK_customTimedReturn_t intermediateResultCompress;
            BMK_customTimedReturn_t intermediateResultDecompress;  
            BMK_timedFnState_t* timeStateCompress = BMK_createTimeState(nbSeconds);
            BMK_timedFnState_t* timeStateDecompress = BMK_createTimeState(nbSeconds);
            if(mode == BMK_compressOnly) {
                intermediateResultCompress.completed = 0;
                intermediateResultDecompress.completed = 1;
            } else if (mode == BMK_decodeOnly) {
                intermediateResultCompress.completed = 1;
                intermediateResultDecompress.completed = 0;
            } else { /* both */
                intermediateResultCompress.completed = 0;
                intermediateResultDecompress.completed = 0;
            }

            while(!intermediateResultCompress.completed) {
                intermediateResultCompress = BMK_benchFunctionTimed(timeStateCompress, &local_defaultCompress, (void*)cctx, &local_initCCtx, (void*)&cctxprep,
                    nbBlocks, srcPtrs, srcSizes, dstPtrs, dstCapacities, dstSizes);

                if(intermediateResultCompress.result.error) {
                    results.error = intermediateResultCompress.result.error;
                    BMK_freeTimeState(timeStateCompress);
                    BMK_freeTimeState(timeStateDecompress);
                    return results;
                }
                results.result.cSpeed = (srcSize * TIMELOOP_NANOSEC) / intermediateResultCompress.result.result.nanoSecPerRun;
                results.result.cSize = intermediateResultCompress.result.result.sumOfReturn;
            }

            while(!intermediateResultDecompress.completed) {
                intermediateResultDecompress = BMK_benchFunctionTimed(timeStateDecompress, &local_defaultDecompress, (void*)(dctx), &local_initDCtx, (void*)&dctxprep,
                    nbBlocks, (const void* const*)dstPtrs, dstSizes, resPtrs, resSizes, NULL);

                if(intermediateResultDecompress.result.error) {
                    results.error = intermediateResultDecompress.result.error;
                    BMK_freeTimeState(timeStateCompress);
                    BMK_freeTimeState(timeStateDecompress);
                    return results;
                }
                results.result.dSpeed = (srcSize * TIMELOOP_NANOSEC) / intermediateResultDecompress.result.result.nanoSecPerRun;
            }

            BMK_freeTimeState(timeStateCompress);
            BMK_freeTimeState(timeStateDecompress);

        } else { /* iterMode; */
            if(mode != BMK_decodeOnly) {

                BMK_customReturn_t compressionResults = BMK_benchFunction(&local_defaultCompress, (void*)cctx, &local_initCCtx, (void*)&cctxprep,
                    nbBlocks, srcPtrs, srcSizes, dstPtrs, dstCapacities, dstSizes, nbSeconds); 
                if(compressionResults.error) {
                    results.error = compressionResults.error;
                    return results;
                }
                if(compressionResults.result.nanoSecPerRun == 0) {
                    results.result.cSpeed = 0;
                } else {
                    results.result.cSpeed = srcSize * TIMELOOP_NANOSEC / compressionResults.result.nanoSecPerRun;
                }
                results.result.cSize = compressionResults.result.sumOfReturn;
            }

            if(mode != BMK_compressOnly) {
                BMK_customReturn_t decompressionResults;
                decompressionResults = BMK_benchFunction(
                    &local_defaultDecompress, (void*)(dctx),
                    &local_initDCtx, (void*)&dctxprep, nbBlocks,
                    (const void* const*)dstPtrs, dstSizes, resPtrs, resSizes, NULL,
                    nbSeconds);

                if(decompressionResults.error) {
                    results.error = decompressionResults.error;
                    return results;
                }

                if(decompressionResults.result.nanoSecPerRun == 0) {
                    results.result.dSpeed = 0;
                } else {
                    results.result.dSpeed = srcSize * TIMELOOP_NANOSEC / decompressionResults.result.nanoSecPerRun;
                }
            }
        }
    }
   /* Bench */
    results.result.cMem = (1 << (comprParams->windowLog)) + ZSTD_sizeof_CCtx(cctx);
    return results;
}

static int BMK_benchParam(BMK_result_t* resultPtr,
                buffers_t buf, contexts_t ctx,
                const ZSTD_compressionParameters cParams) {
    BMK_return_t res = BMK_benchMemInvertible(buf, ctx, !cParams.targetLength /* cLevel, need lvl 1 so tlen = 0 is possible */, &cParams, BMK_both, BMK_timeMode, 3);
    *resultPtr = res.result;
    return res.error;
}

/* comparison function: */
/* strictly better, strictly worse, equal, speed-side adv, size-side adv */
//Maybe use compress_only for benchmark first run?
#define WORSE_RESULT 0
#define BETTER_RESULT 1
#define ERROR_RESULT 2

#define SPEED_RESULT 4
#define SIZE_RESULT 5
/* maybe have epsilon-eq to limit table size? */
static int speedSizeCompare(BMK_result_t r1, BMK_result_t r2) {
    if(r1.cSpeed > r2.cSpeed) {
        if(r1.cSize <= r2.cSize) {
            return WORSE_RESULT;
        }
        return SIZE_RESULT; /* r2 is smaller but not faster. */
    } else {
        if(r1.cSize >= r2.cSize) {
            return BETTER_RESULT;
        }
        return SPEED_RESULT; /* r2 is faster but not smaller */
    }
}

/* 0 for insertion, 1 for no insert */
/* maintain invariant speedSizeCompare(n, n->next) = SPEED_RESULT */
static int insertWinner(winnerInfo_t w, constraint_t targetConstraints) {
    BMK_result_t r = w.result;
    winner_ll_node* cur_node = g_winners;
    /* first node to insert */
    if(!feasible(r, targetConstraints)) {
        return 1;
    }

    if(g_winners == NULL) {
        winner_ll_node* first_node = malloc(sizeof(winner_ll_node));
        if(first_node == NULL) {
            return 1;
        }
        first_node->next = NULL;
        first_node->res = w;
        g_winners = first_node;
        return 0;
    }

    while(cur_node->next != NULL) {
        switch(speedSizeCompare(r, cur_node->res.result)) {
            case BETTER_RESULT:
            {
                return 1; /* never insert if better */
            }
            case WORSE_RESULT:
            {
                winner_ll_node* tmp;
                cur_node->res = cur_node->next->res;
                tmp = cur_node->next;
                cur_node->next = cur_node->next->next;
                free(tmp);
                break; 
            }
            case SPEED_RESULT:
            {
                cur_node = cur_node->next;
                break;
            }
            case SIZE_RESULT: /* insert after first size result, then return */
            {
                winner_ll_node* newnode = malloc(sizeof(winner_ll_node));
                if(newnode == NULL) {
                    return 1;
                }
                newnode->res = cur_node->res;
                cur_node->res = w;
                newnode->next = cur_node->next;
                cur_node->next = newnode;
                return 0;
            }
        } 

    }

    assert(cur_node->next == NULL);
    switch(speedSizeCompare(r, cur_node->res.result)) {
        case BETTER_RESULT:
        {
            return 1; /* never insert if better */
        }
        case WORSE_RESULT:
        {
            cur_node->res = w;
            return 0;
        }
        case SPEED_RESULT:
        {
            winner_ll_node* newnode = malloc(sizeof(winner_ll_node));
            if(newnode == NULL) {
                return 1;
            }
            newnode->res = w;
            newnode->next = NULL;
            cur_node->next = newnode;
            return 0;
        }
        case SIZE_RESULT: /* insert before first size result, then return */
        {
            winner_ll_node* newnode = malloc(sizeof(winner_ll_node));
            if(newnode == NULL) {
                return 1;
            }
            newnode->res = cur_node->res;
            cur_node->res = w;
            newnode->next = cur_node->next;
            cur_node->next = newnode;
            return 0;
        }
        default: 
            return 1;
    } 
}

/* Writes to f the results of a parameter benchmark */
/* when used with --optimize, will only print results better than previously discovered */
static void BMK_printWinner(FILE* f, const U32 cLevel, const BMK_result_t result, const ZSTD_compressionParameters params, const size_t srcSize)
{
    char lvlstr[15] = "Custom Level";
    const U64 time = UTIL_clockSpanNano(g_time);
    const U64 minutes = time / (60ULL * TIMELOOP_NANOSEC);

    fprintf(f, "\r%79s\r", "");

    fprintf(f,"    {%3u,%3u,%3u,%3u,%3u,%3u, %s },  ",
        params.windowLog, params.chainLog, params.hashLog, params.searchLog, params.searchLength,
        params.targetLength, g_stratName[(U32)(params.strategy)]);

    if(cLevel != CUSTOM_LEVEL) {
        snprintf(lvlstr, 15, "  Level %2u  ", cLevel);
    }

    fprintf(f,
        "/* %s */   /* R:%5.3f at %5.1f MB/s - %5.1f MB/s */",
        lvlstr, (double)srcSize / result.cSize, (double)result.cSpeed / (1 MB), (double)result.dSpeed / (1 MB));

    if(TIMED) { fprintf(f, " - %1lu:%2lu:%05.2f", (unsigned long) minutes / 60,(unsigned long) minutes % 60, (double)(time - minutes * TIMELOOP_NANOSEC * 60ULL)/TIMELOOP_NANOSEC); }
    fprintf(f, "\n"); 
}

static void BMK_printWinnerOpt(FILE* f, const U32 cLevel, const BMK_result_t result, const ZSTD_compressionParameters params, const constraint_t targetConstraints, const size_t srcSize)
{
    /* global winner used for constraints */
    static winnerInfo_t g_winner = { { 0, 0, (size_t)-1, (size_t)-1 } , { 0, 0, 0, 0, 0, 0, ZSTD_fast } }; 

    if(DEBUG || compareResultLT(g_winner.result, result, targetConstraints, srcSize)) {
        if(DEBUG && compareResultLT(g_winner.result, result, targetConstraints, srcSize)) {
            DISPLAY("New Winner: \n");
        }

        BMK_printWinner(f, cLevel, result, params, srcSize);

        if(compareResultLT(g_winner.result, result, targetConstraints, srcSize)) {
            BMK_translateAdvancedParams(params);
            g_winner.result = result;
            g_winner.params = params;
        }
    }  

    //prints out tradeoff table if using lvloptimize
    if(g_optmode && g_optimizer) {
        winnerInfo_t w;
        winner_ll_node* n;
        w.result = result;
        w.params = params;
        insertWinner(w, targetConstraints);

        if(!DEBUG) { fprintf(f, "\033c"); }
        fprintf(f, "\n"); 
        
        /* the table */
        fprintf(f, "================================\n");
        for(n = g_winners; n != NULL; n = n->next) {
            fprintf(f, "\r%79s\r", "");

            fprintf(f,"    {%3u,%3u,%3u,%3u,%3u,%3u, %s },  ",
                n->res.params.windowLog, n->res.params.chainLog, n->res.params.hashLog, n->res.params.searchLog, n->res.params.searchLength,
                n->res.params.targetLength, g_stratName[(U32)(n->res.params.strategy)]);
            fprintf(f,
            "   /* R:%5.3f at %5.1f MB/s - %5.1f MB/s */\n",
            (double)srcSize / n->res.result.cSize, (double)n->res.result.cSpeed / (1 MB), (double)n->res.result.dSpeed / (1 MB));
        }
        fprintf(f, "================================\n");
        fprintf(f, "Level Bounds: R: > %.3f AND C: < %.1f MB/s \n\n",
            (double)srcSize / g_lvltarget.cSize, (double)g_lvltarget.cSpeed / (1 MB));


        fprintf(f, "Overall Winner: \n");
        fprintf(f,"    {%3u,%3u,%3u,%3u,%3u,%3u, %s },  ",
            g_winner.params.windowLog, g_winner.params.chainLog, g_winner.params.hashLog, g_winner.params.searchLog, g_winner.params.searchLength,
            g_winner.params.targetLength, g_stratName[(U32)(g_winner.params.strategy)]);
        fprintf(f,
        "   /* R:%5.3f at %5.1f MB/s - %5.1f MB/s */\n",
        (double)srcSize / g_winner.result.cSize, (double)g_winner.result.cSpeed / (1 MB), (double)g_winner.result.dSpeed / (1 MB));

        BMK_translateAdvancedParams(g_winner.params);

        fprintf(f, "Latest BMK: \n");
        fprintf(f,"    {%3u,%3u,%3u,%3u,%3u,%3u, %s },  ",
            params.windowLog, params.chainLog, params.hashLog, params.searchLog, params.searchLength,
            params.targetLength, g_stratName[(U32)(params.strategy)]);
        fprintf(f,
            "   /* R:%5.3f at %5.1f MB/s - %5.1f MB/s */\n",
            (double)srcSize / result.cSize, (double)result.cSpeed / (1 MB), (double)result.dSpeed / (1 MB));

    }
}

static void BMK_printWinners2(FILE* f, const winnerInfo_t* winners, size_t srcSize)
{
    int cLevel;

    fprintf(f, "\n /* Proposed configurations : */ \n");
    fprintf(f, "    /* W,  C,  H,  S,  L,  T, strat */ \n");

    for (cLevel=0; cLevel <= NB_LEVELS_TRACKED; cLevel++)
        BMK_printWinner(f, cLevel, winners[cLevel].result, winners[cLevel].params, srcSize);
}


static void BMK_printWinners(FILE* f, const winnerInfo_t* winners, size_t srcSize)
{
    fseek(f, 0, SEEK_SET);
    BMK_printWinners2(f, winners, srcSize);
    fflush(f);
    BMK_printWinners2(stdout, winners, srcSize);
}


typedef struct {
    U64 cSpeed_min;
    U64 dSpeed_min;
    U32 windowLog_max;
    ZSTD_strategy strategy_max;
} level_constraints_t;

static level_constraints_t g_level_constraint[NB_LEVELS_TRACKED+1];

static void BMK_init_level_constraints(int bytePerSec_level1)
{
    assert(NB_LEVELS_TRACKED >= ZSTD_maxCLevel());
    memset(g_level_constraint, 0, sizeof(g_level_constraint));
    g_level_constraint[1].cSpeed_min = bytePerSec_level1;
    g_level_constraint[1].dSpeed_min = 0.;
    g_level_constraint[1].windowLog_max = 19;
    g_level_constraint[1].strategy_max = ZSTD_fast;

    /* establish speed objectives (relative to level 1) */
    {   int l;
        for (l=2; l<=NB_LEVELS_TRACKED; l++) {
            g_level_constraint[l].cSpeed_min = (g_level_constraint[l-1].cSpeed_min * 49) / 64;
            g_level_constraint[l].dSpeed_min = 0.;
            g_level_constraint[l].windowLog_max = (l<20) ? 23 : l+5;   /* only --ultra levels >= 20 can use windowlog > 23 */
            g_level_constraint[l].strategy_max = (l<19) ? ZSTD_btopt : ZSTD_btultra;   /* level 19 is allowed to use btultra */
    }   }
}

static int BMK_seed(winnerInfo_t* winners, const ZSTD_compressionParameters params, 
                    buffers_t buf, contexts_t ctx)
{
    BMK_result_t testResult;
    int better = 0;
    int cLevel;

    BMK_benchParam(&testResult, buf, ctx, params);


    for (cLevel = 1; cLevel <= NB_LEVELS_TRACKED; cLevel++) {
        if (testResult.cSpeed < g_level_constraint[cLevel].cSpeed_min)
            continue;   /* not fast enough for this level */
        if (testResult.dSpeed < g_level_constraint[cLevel].dSpeed_min)
            continue;   /* not fast enough for this level */
        if (params.windowLog > g_level_constraint[cLevel].windowLog_max)
            continue;   /* too much memory for this level */
        if (params.strategy > g_level_constraint[cLevel].strategy_max)
            continue;   /* forbidden strategy for this level */
        if (winners[cLevel].result.cSize==0) {
            /* first solution for this cLevel */
            winners[cLevel].result = testResult;
            winners[cLevel].params = params;
            BMK_printWinner(stdout, cLevel, testResult, params, buf.srcSize);
            better = 1;
            continue;
        }

        if ((double)testResult.cSize <= ((double)winners[cLevel].result.cSize * (1. + (0.02 / cLevel))) ) {
            /* Validate solution is "good enough" */
            double W_ratio = (double)buf.srcSize / testResult.cSize;
            double O_ratio = (double)buf.srcSize / winners[cLevel].result.cSize;
            double W_ratioNote = log (W_ratio);
            double O_ratioNote = log (O_ratio);
            size_t W_DMemUsed = (1 << params.windowLog) + (16 KB);
            size_t O_DMemUsed = (1 << winners[cLevel].params.windowLog) + (16 KB);
            double W_DMemUsed_note = W_ratioNote * ( 40 + 9*cLevel) - log((double)W_DMemUsed);
            double O_DMemUsed_note = O_ratioNote * ( 40 + 9*cLevel) - log((double)O_DMemUsed);

            size_t W_CMemUsed = (1 << params.windowLog) + ZSTD_estimateCCtxSize_usingCParams(params);
            size_t O_CMemUsed = (1 << winners[cLevel].params.windowLog) + ZSTD_estimateCCtxSize_usingCParams(winners[cLevel].params);
            double W_CMemUsed_note = W_ratioNote * ( 50 + 13*cLevel) - log((double)W_CMemUsed);
            double O_CMemUsed_note = O_ratioNote * ( 50 + 13*cLevel) - log((double)O_CMemUsed);

            double W_CSpeed_note = W_ratioNote * ( 30 + 10*cLevel) + log(testResult.cSpeed);
            double O_CSpeed_note = O_ratioNote * ( 30 + 10*cLevel) + log(winners[cLevel].result.cSpeed);

            double W_DSpeed_note = W_ratioNote * ( 20 + 2*cLevel) + log(testResult.dSpeed);
            double O_DSpeed_note = O_ratioNote * ( 20 + 2*cLevel) + log(winners[cLevel].result.dSpeed);

            if (W_DMemUsed_note < O_DMemUsed_note) {
                /* uses too much Decompression memory for too little benefit */
                if (W_ratio > O_ratio)
                DISPLAY ("Decompression Memory : %5.3f @ %4.1f MB  vs  %5.3f @ %4.1f MB   : not enough for level %i\n",
                         W_ratio, (double)(W_DMemUsed) / 1024 / 1024,
                         O_ratio, (double)(O_DMemUsed) / 1024 / 1024,   cLevel);
                continue;
            }
            if (W_CMemUsed_note < O_CMemUsed_note) {
                /* uses too much memory for compression for too little benefit */
                if (W_ratio > O_ratio)
                DISPLAY ("Compression Memory : %5.3f @ %4.1f MB  vs  %5.3f @ %4.1f MB   : not enough for level %i\n",
                         W_ratio, (double)(W_CMemUsed) / 1024 / 1024,
                         O_ratio, (double)(O_CMemUsed) / 1024 / 1024,   cLevel);
                continue;
            }
            if (W_CSpeed_note   < O_CSpeed_note  ) {
                /* too large compression speed difference for the compression benefit */
                if (W_ratio > O_ratio)
                DISPLAY ("Compression Speed : %5.3f @ %4.1f MB/s  vs  %5.3f @ %4.1f MB/s   : not enough for level %i\n",
                         W_ratio, (double)testResult.cSpeed / (1 MB),
                         O_ratio, (double)winners[cLevel].result.cSpeed / (1 MB),   cLevel);
                continue;
            }
            if (W_DSpeed_note   < O_DSpeed_note  ) {
                /* too large decompression speed difference for the compression benefit */
                if (W_ratio > O_ratio)
                DISPLAY ("Decompression Speed : %5.3f @ %4.1f MB/s  vs  %5.3f @ %4.1f MB/s   : not enough for level %i\n",
                         W_ratio, (double)testResult.dSpeed / (1 MB),
                         O_ratio, (double)winners[cLevel].result.dSpeed / (1 MB),   cLevel);
                continue;
            }

            if (W_ratio < O_ratio)
                DISPLAY("Solution %4.3f selected over %4.3f at level %i, due to better secondary statistics \n", W_ratio, O_ratio, cLevel);

            winners[cLevel].result = testResult;
            winners[cLevel].params = params;
            BMK_printWinner(stdout, cLevel, testResult, params, buf.srcSize);

            better = 1;
    }   }

    return better;
}

/* bounds check in sanitize too? */
#define CLAMP(var, lo, hi) {                          \
    var = MAX(MIN(var, hi), lo);                      \
}

/* nullified useless params, to ensure count stats */
/* no point in windowLog < chainLog (no point 2x chainLog for bt) */
/* now with built in bounds-checking */
/* no longer does anything with sanitizeVarArray + clampcheck */
static ZSTD_compressionParameters sanitizeParams(ZSTD_compressionParameters params)
{
    if (params.strategy == ZSTD_fast)
        params.chainLog = 0, params.searchLog = 0;
    if (params.strategy == ZSTD_dfast)
        params.searchLog = 0;
    if (params.strategy != ZSTD_btopt && params.strategy != ZSTD_btultra && params.strategy != ZSTD_fast)
        params.targetLength = 0;

    return params;
}

/* new length */
/* keep old array, will need if iter over strategy. */
static int sanitizeVarArray(varInds_t* varNew, const int varLength, const varInds_t* varArray, const ZSTD_strategy strat) {
    int i, j = 0;
    for(i = 0; i < varLength; i++) {
        if( !((varArray[i] == clog_ind && strat == ZSTD_fast)
            || (varArray[i] == slog_ind && strat == ZSTD_fast)
            || (varArray[i] == slog_ind && strat == ZSTD_dfast) 
            || (varArray[i] == tlen_ind && strat != ZSTD_btopt && strat != ZSTD_btultra && strat != ZSTD_fast)
            /* || varArray[i] == strt_ind */ )) {
            varNew[j] = varArray[i];
            j++;
        }
    }
    return j;

}

/* res should be NUM_PARAMS size */
/* constructs varArray from ZSTD_compressionParameters style parameter */
static int variableParams(const ZSTD_compressionParameters paramConstraints, varInds_t* res) {
    int j = 0;
    if(!paramConstraints.windowLog) {
        res[j] = wlog_ind;
        j++;
    }
    if(!paramConstraints.chainLog) {
        res[j] = clog_ind;
        j++;
    }
    if(!paramConstraints.hashLog) {
        res[j] = hlog_ind;
        j++;
    }
    if(!paramConstraints.searchLog) {
        res[j] = slog_ind;
        j++;
    }
    if(!paramConstraints.searchLength) {
        res[j] = slen_ind;
        j++;
    }
    if(!paramConstraints.targetLength) {
        res[j] = tlen_ind;
        j++;
    }
    if(!paramConstraints.strategy) {
        res[j] = strt_ind;
        j++;
    }
    return j;
}

/* bin-search on tlen_table for correct index. */
static int tlen_inv(U32 x) {
    int lo = 0;
    int hi = TLEN_RANGE;
    while(lo < hi) {
        int mid = (lo + hi) / 2;
        if(tlen_table[mid] < x) {
            lo = mid + 1;
        } if(tlen_table[mid] == x) {
            return mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* amt will probably always be \pm 1? */
/* slight change from old paramVariation, targetLength can only take on powers of 2 now (999 ~= 1024?) */
/* take max/min bounds into account as well? */
static void paramVaryOnce(const varInds_t paramIndex, const int amt, ZSTD_compressionParameters* ptr) {
    switch(paramIndex)
    {
        case wlog_ind: ptr->windowLog    += amt; break;
        case clog_ind: ptr->chainLog     += amt; break;
        case hlog_ind: ptr->hashLog      += amt; break;
        case slog_ind: ptr->searchLog    += amt; break;
        case slen_ind: ptr->searchLength += amt; break;
        case tlen_ind: 
            ptr->targetLength = tlen_table[MAX(0, MIN(TLEN_RANGE - 1, tlen_inv(ptr->targetLength) + amt))];
            break;
        case strt_ind: ptr->strategy     += amt; break;
        default: break;
    }
}

/* varies ptr by nbChanges respecting varyParams*/
static void paramVariation(ZSTD_compressionParameters* ptr, const varInds_t* varyParams, const int varyLen, const U32 nbChanges)
{
    ZSTD_compressionParameters p;
    U32 validated = 0;
    while (!validated) {
        U32 i;
        p = *ptr;
        for (i = 0 ; i < nbChanges ; i++) {
            const U32 changeID = FUZ_rand(&g_rand) % (varyLen << 1);
            paramVaryOnce(varyParams[changeID >> 1], ((changeID & 1) << 1) - 1, &p);
        }
        validated = !ZSTD_isError(ZSTD_checkCParams(p)) && p.strategy > 0;
    }
    *ptr = p;
}

/* length of memo table given free variables */
static size_t memoTableLen(const varInds_t* varyParams, const int varyLen) {
    size_t arrayLen = 1;
    int i;
    for(i = 0; i < varyLen; i++) {
        if(varyParams[i] != strt_ind) {
            arrayLen *= rangetable[varyParams[i]];
        }
    }
    return arrayLen;
}

/* returns unique index in memotable of compression parameters */
static unsigned memoTableInd(const ZSTD_compressionParameters* ptr, const varInds_t* varyParams, const int varyLen) {
    int i;
    unsigned ind = 0;
    for(i = 0; i < varyLen; i++) {
        switch(varyParams[i]) {
            case wlog_ind: ind *= WLOG_RANGE; ind += ptr->windowLog              
                - ZSTD_WINDOWLOG_MIN   ; break;
            case clog_ind: ind *= CLOG_RANGE; ind += ptr->chainLog               
                - ZSTD_CHAINLOG_MIN    ; break;
            case hlog_ind: ind *= HLOG_RANGE; ind += ptr->hashLog                
                - ZSTD_HASHLOG_MIN     ; break;
            case slog_ind: ind *= SLOG_RANGE; ind += ptr->searchLog              
                - ZSTD_SEARCHLOG_MIN   ; break;
            case slen_ind: ind *= SLEN_RANGE; ind += ptr->searchLength           
                - ZSTD_SEARCHLENGTH_MIN; break;
            case tlen_ind: ind *= TLEN_RANGE; ind += tlen_inv(ptr->targetLength) 
                - ZSTD_TARGETLENGTH_MIN; break;
            case strt_ind: break;
        }
    }
    return ind;
}

/* inverse of above function (from index to parameters) */
static void memoTableIndInv(ZSTD_compressionParameters* ptr, const varInds_t* varyParams, const int varyLen, size_t ind) {
    int i;
    for(i = varyLen - 1; i >= 0; i--) {
        switch(varyParams[i]) {
            case wlog_ind: ptr->windowLog    = ind % WLOG_RANGE + ZSTD_WINDOWLOG_MIN;                             ind /= WLOG_RANGE; break;
            case clog_ind: ptr->chainLog     = ind % CLOG_RANGE + ZSTD_CHAINLOG_MIN;                              ind /= CLOG_RANGE; break;
            case hlog_ind: ptr->hashLog      = ind % HLOG_RANGE + ZSTD_HASHLOG_MIN;                               ind /= HLOG_RANGE; break;
            case slog_ind: ptr->searchLog    = ind % SLOG_RANGE + ZSTD_SEARCHLOG_MIN;                             ind /= SLOG_RANGE; break;
            case slen_ind: ptr->searchLength = ind % SLEN_RANGE + ZSTD_SEARCHLENGTH_MIN;                          ind /= SLEN_RANGE; break;
            case tlen_ind: ptr->targetLength = tlen_table[(ind % TLEN_RANGE)];                                    ind /= TLEN_RANGE; break;
            case strt_ind: break; 
        }
    }
}

/* Initialize memoization table, which tracks and prevents repeated benchmarking
 * of the same set of parameters. In addition, it is also used to immediately mark 
 * redundant / obviously non-optimal parameter configurations (e.g. wlog - 1 larger)
 * than srcSize, clog > wlog, ... 
 */
static void initMemoTable(U8* memoTable, ZSTD_compressionParameters paramConstraints, const constraint_t target, const varInds_t* varyParams, const int varyLen, const size_t srcSize) {
    size_t i;
    size_t arrayLen = memoTableLen(varyParams, varyLen);
    int cwFixed = !paramConstraints.chainLog || !paramConstraints.windowLog;
    int scFixed = !paramConstraints.searchLog || !paramConstraints.chainLog;
    int whFixed = !paramConstraints.windowLog || !paramConstraints.hashLog;
    int wFixed = !paramConstraints.windowLog;
    int j = 0;
    assert(memoTable != NULL);
    memset(memoTable, 0, arrayLen);
    cParamZeroMin(&paramConstraints);

    for(i = 0; i < arrayLen; i++) {
        memoTableIndInv(&paramConstraints, varyParams, varyLen, i);
        if(ZSTD_estimateCStreamSize_usingCParams(paramConstraints) > (size_t)target.cMem) {
            memoTable[i] = 255;
            j++;
        }
        if(wFixed && (1ULL << (paramConstraints.windowLog - 1)) > srcSize) {
            memoTable[i] = 255;
        }
        /* nil out parameter sets equivalent to others. */
        if(cwFixed/* at most least 1 param fixed. */) {
            if(paramConstraints.strategy == ZSTD_btlazy2 || paramConstraints.strategy == ZSTD_btopt || paramConstraints.strategy == ZSTD_btultra) {
                if(paramConstraints.chainLog > paramConstraints.windowLog + 1) {
                    if(memoTable[i] != 255) { j++; }
                    memoTable[i] = 255;
                }
            } else {
                if(paramConstraints.chainLog > paramConstraints.windowLog) {
                    if(memoTable[i] != 255) { j++; }
                    memoTable[i] = 255;
                }
            }
        }

        if(scFixed) {
            if(paramConstraints.searchLog > paramConstraints.chainLog) {
                if(memoTable[i] != 255) { j++; }
                memoTable[i] = 255;
            }
        }

        if(whFixed) {
            if(paramConstraints.hashLog > paramConstraints.windowLog + 1) {
                if(memoTable[i] != 255) { j++; }
                memoTable[i] = 255;
            }
        }
    }
    DEBUGOUTPUT("%d / %d Invalid\n", j, (int)i);
    if((int)i == j) {
        DEBUGOUTPUT("!!!Strategy %d totally infeasible\n", (int)paramConstraints.strategy)
    }
}

/* frees all allocated memotables */
static void freeMemoTableArray(U8** mtAll) {
    int i;
    if(mtAll == NULL) { return; }
    for(i = 1; i <= (int)ZSTD_btultra; i++) {
        free(mtAll[i]);
    }
    free(mtAll);
}

/* inits memotables for all (including mallocs), all strategies */
/* takes unsanitized varyParams */
static U8** createMemoTableArray(ZSTD_compressionParameters paramConstraints, constraint_t target, const varInds_t* varyParams, const int varyLen, const size_t srcSize) {
    varInds_t varNew[NUM_PARAMS];
    U8** mtAll = calloc(sizeof(U8*),(ZSTD_btultra + 1));
    int i;
    if(mtAll == NULL) {
        return NULL;
    }

    for(i = 1; i <= (int)ZSTD_btultra; i++) {
        const int varLenNew = sanitizeVarArray(varNew, varyLen, varyParams, i);
        mtAll[i] = malloc(sizeof(U8) * memoTableLen(varNew, varLenNew));
        if(mtAll[i] == NULL) {
            freeMemoTableArray(mtAll);
            return NULL;
        }
        initMemoTable(mtAll[i], paramConstraints, target, varNew, varLenNew, srcSize);
    }
    
    return mtAll;
}

static ZSTD_compressionParameters maskParams(ZSTD_compressionParameters base, ZSTD_compressionParameters mask) {
    base.windowLog = mask.windowLog ? mask.windowLog : base.windowLog;
    base.chainLog = mask.chainLog ? mask.chainLog : base.chainLog;
    base.hashLog = mask.hashLog ? mask.hashLog : base.hashLog;
    base.searchLog = mask.searchLog ? mask.searchLog : base.searchLog;
    base.searchLength = mask.searchLength ? mask.searchLength : base.searchLength;
    base.targetLength = mask.targetLength ? mask.targetLength : base.targetLength;
    base.strategy = mask.strategy ? mask.strategy : base.strategy;
    return base;
}

#define PARAMTABLELOG   25
#define PARAMTABLESIZE (1<<PARAMTABLELOG)
#define PARAMTABLEMASK (PARAMTABLESIZE-1)
static BYTE g_alreadyTested[PARAMTABLESIZE] = {0};   /* init to zero */

/*
#define NB_TESTS_PLAYED(p) \
    g_alreadyTested[(XXH64(((void*)&sanitizeParams(p), sizeof(p), 0) >> 3) & PARAMTABLEMASK] */

static BYTE* NB_TESTS_PLAYED(ZSTD_compressionParameters p) {
    ZSTD_compressionParameters p2 = sanitizeParams(p);
    return &g_alreadyTested[(XXH64((void*)&p2, sizeof(p2), 0) >> 3) & PARAMTABLEMASK];
}

static void playAround(FILE* f, winnerInfo_t* winners,
                       ZSTD_compressionParameters params,
                       buffers_t buf, contexts_t ctx)
{
    int nbVariations = 0;
    UTIL_time_t const clockStart = UTIL_getTime();
    const U32 unconstrained[NUM_PARAMS] = { 0, 1, 2, 3, 4, 5, 6 };


    while (UTIL_clockSpanMicro(clockStart) < g_maxVariationTime) {
        ZSTD_compressionParameters p = params;
        BYTE* b;

        if (nbVariations++ > g_maxNbVariations) break;
        paramVariation(&p, unconstrained, NUM_PARAMS, 4);

        /* exclude faster if already played params */
        if (FUZ_rand(&g_rand) & ((1 << *NB_TESTS_PLAYED(p))-1))
            continue;

        /* test */
        b = NB_TESTS_PLAYED(p);
        (*b)++;
        if (!BMK_seed(winners, p, buf, ctx)) continue;

        /* improvement found => search more */
        BMK_printWinners(f, winners, buf.srcSize);
        playAround(f, winners, p, buf, ctx);
    }

}

/* Completely random parameter selection */
static ZSTD_compressionParameters randomParams(void)
{
    ZSTD_compressionParameters p;
    U32 validated = 0;
    while (!validated) {
        /* totally random entry */
        p.chainLog   = (FUZ_rand(&g_rand) % (ZSTD_CHAINLOG_MAX+1 - ZSTD_CHAINLOG_MIN))         + ZSTD_CHAINLOG_MIN;
        p.hashLog    = (FUZ_rand(&g_rand) % (ZSTD_HASHLOG_MAX+1 - ZSTD_HASHLOG_MIN))           + ZSTD_HASHLOG_MIN;
        p.searchLog  = (FUZ_rand(&g_rand) % (ZSTD_SEARCHLOG_MAX+1 - ZSTD_SEARCHLOG_MIN))       + ZSTD_SEARCHLOG_MIN;
        p.windowLog  = (FUZ_rand(&g_rand) % (ZSTD_WINDOWLOG_MAX+1 - ZSTD_WINDOWLOG_MIN))       + ZSTD_WINDOWLOG_MIN;
        p.searchLength=(FUZ_rand(&g_rand) % (ZSTD_SEARCHLENGTH_MAX+1 - ZSTD_SEARCHLENGTH_MIN)) + ZSTD_SEARCHLENGTH_MIN;
        p.targetLength=(FUZ_rand(&g_rand) % (512));
        p.strategy   = (ZSTD_strategy) (FUZ_rand(&g_rand) % (ZSTD_btultra +1));
        validated = !ZSTD_isError(ZSTD_checkCParams(p));
    }
    return p;
}

/* Sets pc to random unmeasured set of parameters */
static void randomConstrainedParams(ZSTD_compressionParameters* pc, varInds_t* varArray, int varLen, U8* memoTable)
{
    size_t tries = memoTableLen(varArray, varLen); 
    const size_t maxSize = memoTableLen(varArray, varLen);
    size_t ind;
    do {
        ind = (FUZ_rand(&g_rand)) % maxSize;
        tries--;
    } while(memoTable[ind] > 0 && tries > 0); 

    memoTableIndInv(pc, varArray, varLen, (unsigned)ind);
}

static void BMK_selectRandomStart(
                       FILE* f, winnerInfo_t* winners,
                       buffers_t buf, contexts_t ctx)
{
    U32 const id = FUZ_rand(&g_rand) % (NB_LEVELS_TRACKED+1);
    if ((id==0) || (winners[id].params.windowLog==0)) {
        /* use some random entry */
        ZSTD_compressionParameters const p = ZSTD_adjustCParams(randomParams(), buf.srcSize, 0);
        playAround(f, winners, p, buf, ctx);
    } else {
        playAround(f, winners, winners[id].params, buf, ctx);
    }
}

static void BMK_benchFullTable(buffers_t buf, contexts_t ctx, const size_t maxBlockSize) 
{
    ZSTD_compressionParameters params;
    winnerInfo_t winners[NB_LEVELS_TRACKED+1];
    const char* const rfName = "grillResults.txt";
    FILE* const f = fopen(rfName, "w");

    /* init */
    assert(g_singleRun==0);
    memset(winners, 0, sizeof(winners));
    if (f==NULL) { DISPLAY("error opening %s \n", rfName); exit(1); }

    if (g_target) {
        BMK_init_level_constraints(g_target * (1 MB));
    } else {
        /* baseline config for level 1 */
        ZSTD_compressionParameters const l1params = ZSTD_getCParams(1, maxBlockSize, ctx.dictSize);
        BMK_result_t testResult;
        BMK_benchParam(&testResult, buf, ctx, l1params);
        BMK_init_level_constraints((int)((testResult.cSpeed * 31) / 32));
    }

    /* populate initial solution */
    {   const int maxSeeds = g_noSeed ? 1 : ZSTD_maxCLevel();
        int i;
        for (i=0; i<=maxSeeds; i++) {
            params = ZSTD_getCParams(i, maxBlockSize, 0);
            BMK_seed(winners, params, buf, ctx);
    }   }
    BMK_printWinners(f, winners, buf.srcSize);

    /* start tests */
    {   const time_t grillStart = time(NULL);
        do {
            BMK_selectRandomStart(f, winners, buf, ctx);
        } while (BMK_timeSpan(grillStart) < g_grillDuration_s);
    }

    /* end summary */
    BMK_printWinners(f, winners, buf.srcSize);
    DISPLAY("grillParams operations completed \n");

    /* clean up*/
    fclose(f);
}

static int benchOnce(buffers_t buf, contexts_t ctx) {
    BMK_result_t testResult;

    if(BMK_benchParam(&testResult, buf, ctx, g_params)) {
        DISPLAY("Error during benchmarking\n");
        return 1;
    }

    BMK_printWinner(stdout, CUSTOM_LEVEL, testResult, g_params, buf.srcSize);

    return 0;
}

static int benchSample(void)
{
    const char* const name = "Sample 10MB";
    size_t const benchedSize = 10 MB;
    U32 blockSize = g_blockSize ? g_blockSize : benchedSize;
    void* srcBuffer = malloc(benchedSize);
    int ret = 0;

    buffers_t buf;
    contexts_t ctx;
    
    if(srcBuffer == NULL) {
        DISPLAY("Out of Memory\n");
        return 2;
    }

    RDG_genBuffer(srcBuffer, benchedSize, g_compressibility, 0.0, 0);

    if(createBuffersFromMemory(&buf, srcBuffer, 1, &benchedSize)) {
        DISPLAY("Buffer Creation Error\n");
        free(srcBuffer);
        return 3;
    }

    if(createContexts(&ctx, NULL)) {
        DISPLAY("Context Creation Error\n");
        freeBuffers(buf);
        return 1;
    }

    /* bench */
    DISPLAY("\r%79s\r", "");
    DISPLAY("using %s %i%%: \n", name, (int)(g_compressibility*100));

    if(g_singleRun) {
        ret = benchOnce(buf, ctx);
    } else {
        BMK_benchFullTable(buf, ctx, MIN(blockSize, benchedSize));
    }

    freeBuffers(buf);
    freeContexts(ctx);

    return ret;
}

/* benchFiles() :
 * note: while this function takes a table of filenames,
 * in practice, only the first filename will be used */
int benchFiles(const char** fileNamesTable, int nbFiles, const char* dictFileName, int cLevel)
{
    buffers_t buf;
    contexts_t ctx;
    size_t maxBlockSize = 0, i;
    int ret = 0;

    if(createBuffers(&buf, fileNamesTable, nbFiles)) {
        DISPLAY("unable to load files\n");
        return 1;
    }

    if(createContexts(&ctx, dictFileName)) {
        DISPLAY("unable to load dictionary\n");
        freeBuffers(buf);
        return 2;
    }

    for(i = 0; i < buf.nbBlocks; i++) {
        maxBlockSize = MAX(maxBlockSize, buf.srcSizes[i]);
    }

    DISPLAY("\r%79s\r", "");
    if(nbFiles == 1) {
        DISPLAY("using %s : \n", fileNamesTable[0]);
    } else {
        DISPLAY("using %d Files : \n", nbFiles);
    }

    g_params = ZSTD_adjustCParams(maskParams(ZSTD_getCParams(cLevel, maxBlockSize, ctx.dictSize), g_params), maxBlockSize, ctx.dictSize);

    if(g_singleRun) {
        ret = benchOnce(buf, ctx);
    } else {
        BMK_benchFullTable(buf, ctx, maxBlockSize);
    }

    freeBuffers(buf);
    freeContexts(ctx);
    return ret;
}

/* Benchmarking which stops when we are sufficiently sure the solution is infeasible / worse than the winner */
#define VARIANCE 1.2 
#define HIGH_VARIANCE 100.0
static int allBench(BMK_result_t* resultPtr,
                buffers_t buf, contexts_t ctx,
                const ZSTD_compressionParameters cParams,
                const constraint_t target,
                BMK_result_t* winnerResult, int feas) {
    BMK_return_t benchres;
    BMK_result_t resultMax;
    U64 loopDurationC = 0, loopDurationD = 0;
    double uncertaintyConstantC = 3., uncertaintyConstantD = 3.;
    double winnerRS;

    /* initial benchmarking, gives exact ratio and memory, warms up future runs */
    benchres = BMK_benchMemInvertible(buf, ctx, 0, &cParams, BMK_both, BMK_iterMode, 1);

    winnerRS = resultScore(*winnerResult, buf.srcSize, target);
    DEBUGOUTPUT("WinnerScore: %f\n ", winnerRS);

    if(benchres.error) {
        DEBUGOUTPUT("Benchmarking failed\n");
        return ERROR_RESULT;
    } 
    *resultPtr = benchres.result;

    /* calculate uncertainty in compression / decompression runs */
    if(benchres.result.cSpeed) {
        loopDurationC = ((buf.srcSize * TIMELOOP_NANOSEC) / benchres.result.cSpeed); 
        uncertaintyConstantC = ((loopDurationC + (double)(2 * g_clockGranularity))/loopDurationC); 
    }

    if(benchres.result.dSpeed) {
        loopDurationD = ((buf.srcSize * TIMELOOP_NANOSEC) / benchres.result.dSpeed); 
        uncertaintyConstantD = ((loopDurationD + (double)(2 * g_clockGranularity))/loopDurationD);  
    }

    /* anything with worse ratio in feas is definitely worse, discard */
    if(feas && benchres.result.cSize < winnerResult->cSize && !g_optmode) {
        return WORSE_RESULT;
    }

    /* second run, if first run is too short, gives approximate cSpeed + dSpeed */
    if(loopDurationC < TIMELOOP_NANOSEC / 10) {
        BMK_return_t benchres2 = BMK_benchMemInvertible(buf, ctx, 0, &cParams, BMK_compressOnly, BMK_iterMode, 1);
        if(benchres2.error) {
            return ERROR_RESULT;
        }
        benchres = benchres2;
    }
    if(loopDurationD < TIMELOOP_NANOSEC / 10) {
        BMK_return_t benchres2 = BMK_benchMemInvertible(buf, ctx, 0, &cParams, BMK_decodeOnly, BMK_iterMode, 1);
        if(benchres2.error) {
            return ERROR_RESULT;
        }
        benchres.result.dSpeed = benchres2.result.dSpeed;
    }

    *resultPtr = benchres.result;

    /* optimistic assumption of benchres.result */
    resultMax = benchres.result;
    resultMax.cSpeed *= uncertaintyConstantC * VARIANCE;
    resultMax.dSpeed *= uncertaintyConstantD * VARIANCE;

    /* disregard infeasible results in feas mode */
    /* disregard if resultMax < winner in infeas mode */
    if((feas && !feasible(resultMax, target)) ||
      (!feas && (winnerRS > resultScore(resultMax, buf.srcSize, target)))) {
        return WORSE_RESULT;
    }

    /* Final full run if estimates are unclear */
    if(loopDurationC < TIMELOOP_NANOSEC) {
        BMK_return_t benchres2 = BMK_benchMemInvertible(buf, ctx, 0, &cParams, BMK_compressOnly, BMK_timeMode, 1);
        if(benchres2.error) {
            return ERROR_RESULT;
        }
        benchres.result.cSpeed = benchres2.result.cSpeed;
    } 

    if(loopDurationD < TIMELOOP_NANOSEC) {
        BMK_return_t benchres2 = BMK_benchMemInvertible(buf, ctx, 0, &cParams, BMK_decodeOnly, BMK_timeMode, 1);
        if(benchres2.error) {
            return ERROR_RESULT;
        }
        benchres.result.dSpeed = benchres2.result.dSpeed;
    }

    *resultPtr = benchres.result;

    /* compare by resultScore when in infeas */
    /* compare by compareResultLT when in feas */
    if((!feas && (resultScore(benchres.result, buf.srcSize, target) > resultScore(*winnerResult, buf.srcSize, target))) || 
       (feas && (compareResultLT(*winnerResult, benchres.result, target, buf.srcSize))) )  { 
        return BETTER_RESULT; 
    } else { 
        return WORSE_RESULT; 
    }
}

#define INFEASIBLE_THRESHOLD 200

/* Memoized benchmarking, won't benchmark anything which has already been benchmarked before. */
static int benchMemo(BMK_result_t* resultPtr,
                buffers_t buf, contexts_t ctx, 
                const ZSTD_compressionParameters cParams,
                const constraint_t target,
                BMK_result_t* winnerResult, U8* memoTable,
                const varInds_t* varyParams, const int varyLen, int feas) {
    static int bmcount = 0;
    size_t memind = memoTableInd(&cParams, varyParams, varyLen);
    int res;

    if(memoTable[memind] >= INFEASIBLE_THRESHOLD) { return WORSE_RESULT; } 

    res = allBench(resultPtr, buf, ctx, cParams, target, winnerResult, feas);

    if(DEBUG && !(bmcount % 250)) {
        DISPLAY("Count: %d\n", bmcount);
        bmcount++;
    }
    BMK_printWinnerOpt(stdout, CUSTOM_LEVEL, *resultPtr, cParams, target, buf.srcSize);

    if(res == BETTER_RESULT || feas) {
        memoTable[memind] = 255; 
    }
    return res;
}


/* One iteration of hill climbing. Specifically, it first tries all 
 * valid parameter configurations w/ manhattan distance 1 and picks the best one
 * failing that, it progressively tries candidates further and further away (up to #dim + 2)
 * if it finds a candidate exceeding winnerInfo, it will repeat. Otherwise, it will stop the 
 * current stage of hill climbing. 
 * Each iteration of hill climbing proceeds in 2 'phases'. Phase 1 climbs according to 
 * the resultScore function, which is effectively a linear increase in reward until it reaches
 * the constraint-satisfying value, it which point any excess results in only logarithmic reward.
 * This aims to find some constraint-satisfying point.
 * Phase 2 optimizes in accordance with what the original function sets out to maximize, with
 * all feasible solutions valued over all infeasible solutions.
 */

/* sanitize all params here. 
 * all generation after random should be sanitized. (maybe sanitize random)
 */
static winnerInfo_t climbOnce(const constraint_t target, 
                const varInds_t* varArray, const int varLen, ZSTD_strategy strat,
                U8** memoTableArray, 
                buffers_t buf, contexts_t ctx,
                const ZSTD_compressionParameters init) {
    /* 
     * cparam - currently considered 'center'
     * candidate - params to benchmark/results
     * winner - best option found so far.
     */
    ZSTD_compressionParameters cparam = init;
    winnerInfo_t candidateInfo, winnerInfo;
    int better = 1;
    int feas = 0;
    varInds_t varNew[NUM_PARAMS];
    int varLenNew = sanitizeVarArray(varNew, varLen, varArray, strat);

    winnerInfo = initWinnerInfo(init);
    candidateInfo = winnerInfo;

    {
        winnerInfo_t bestFeasible1 = initWinnerInfo(cparam);
        DEBUGOUTPUT("Climb Part 1\n");
        while(better) {

            int i, dist, offset;
            better = 0;
            DEBUGOUTPUT("Start\n");
            cparam = winnerInfo.params;
            BMK_printWinnerOpt(stdout, CUSTOM_LEVEL, winnerInfo.result, winnerInfo.params, target, buf.srcSize);
            candidateInfo.params = cparam;
             /* all dist-1 candidates */
            for(i = 0; i < varLen; i++) {
                for(offset = -1; offset <= 1; offset += 2) {
                    candidateInfo.params = cparam;
                    paramVaryOnce(varArray[i], offset, &candidateInfo.params); 
                    
                    if(!ZSTD_isError(ZSTD_checkCParams(candidateInfo.params)) && candidateInfo.params.strategy > 0) {
                        int res;
                        if(strat != candidateInfo.params.strategy) { /* maybe only try strategy switching after exhausting non-switching solutions? */
                            strat = candidateInfo.params.strategy;
                            varLenNew = sanitizeVarArray(varNew, varLen, varArray, strat);
                        }
                        res = benchMemo(&candidateInfo.result,
                            buf, ctx,
                            sanitizeParams(candidateInfo.params), target, &winnerInfo.result, memoTableArray[strat],
                            varNew, varLenNew, feas);
                        if(res == BETTER_RESULT) { /* synonymous with better when called w/ infeasibleBM */
                            winnerInfo = candidateInfo;
                            BMK_printWinnerOpt(stdout, CUSTOM_LEVEL, winnerInfo.result, sanitizeParams(winnerInfo.params), target, buf.srcSize);
                            better = 1;
                            if(compareResultLT(bestFeasible1.result, winnerInfo.result, target, buf.srcSize)) {
                                bestFeasible1 = winnerInfo;
                            }
                        }
                    }
                }
            }

            if(better) {
                continue;
            }

            for(dist = 2; dist < varLen + 2; dist++) { /* varLen is # dimensions */
                for(i = 0; i < (1 << varLen) / varLen + 2; i++) {
                    int res;
                    candidateInfo.params = cparam;
                    /* param error checking already done here */
                    paramVariation(&candidateInfo.params, varArray, varLen, dist);

                    if(strat != candidateInfo.params.strategy) {
                        strat = candidateInfo.params.strategy;
                        varLenNew = sanitizeVarArray(varNew, varLen, varArray, strat);
                    }

                    res = benchMemo(&candidateInfo.result,
                        buf, ctx, 
                        sanitizeParams(candidateInfo.params), target, &winnerInfo.result, memoTableArray[strat],
                        varNew, varLenNew, feas);
                    if(res == BETTER_RESULT) { /* synonymous with better in this case*/
                        winnerInfo = candidateInfo;
                        BMK_printWinnerOpt(stdout, CUSTOM_LEVEL, winnerInfo.result, sanitizeParams(winnerInfo.params), target, buf.srcSize);
                        better = 1;
                        if(compareResultLT(bestFeasible1.result, winnerInfo.result, target, buf.srcSize)) {
                            bestFeasible1 = winnerInfo;
                        }
                        break;
                    }

                }
                if(better) {
                    break;
                }
            }

            if(!better) { /* infeas -> feas -> stop */ 
                if(feas) { return winnerInfo; } 

                feas = 1;
                better = 1;
                winnerInfo = bestFeasible1; /* note with change, bestFeasible may not necessarily be feasible, but if one has been benchmarked, it will be. */
                DEBUGOUTPUT("Climb Part 2\n");
            }
        }
        winnerInfo = bestFeasible1;
    }

    return winnerInfo;
}

/* Optimizes for a fixed strategy */

/* flexible parameters: iterations of (failed?) climbing (or if we do non-random, maybe this is when everything is close to visitied)
   weight more on visit for bad results, less on good results/more on later results / ones with more failures.
   allocate memoTable here. 
   only real use for paramTarget is to get the fixed values, right? 
   maybe allow giving it a first init? 
 */
static winnerInfo_t optimizeFixedStrategy(
    buffers_t buf, contexts_t ctx, 
    const constraint_t target, ZSTD_compressionParameters paramTarget,
    const ZSTD_strategy strat, 
    const varInds_t* varArray, const int varLen,
    U8** memoTableArray, const int tries) {
    int i = 0;
    varInds_t varNew[NUM_PARAMS];
    int varLenNew = sanitizeVarArray(varNew, varLen, varArray, strat);

    ZSTD_compressionParameters init;
    winnerInfo_t winnerInfo, candidateInfo; 
    winnerInfo = initWinnerInfo(emptyParams());
    /* so climb is given the right fixed strategy */
    paramTarget.strategy = strat;
    /* to pass ZSTD_checkCParams */

    cParamZeroMin(&paramTarget);

    init = paramTarget;

    while(i < tries) {
        DEBUGOUTPUT("Restart\n"); 
        randomConstrainedParams(&init, varNew, varLenNew, memoTableArray[strat]);
        candidateInfo = climbOnce(target, varArray, varLen, strat, memoTableArray, buf, ctx, init);
        if(compareResultLT(winnerInfo.result, candidateInfo.result, target, buf.srcSize)) {
            winnerInfo = candidateInfo;
            BMK_printWinnerOpt(stdout, CUSTOM_LEVEL, winnerInfo.result, winnerInfo.params, target, buf.srcSize);
            i = 0;
        }

        i++;
    }
    return winnerInfo;
}

/* goes best, best-1, best+1, best-2, ... */
/* return 0 if nothing remaining */
static int nextStrategy(const int currentStrategy, const int bestStrategy) {
    if(bestStrategy <= currentStrategy) {
        int candidate = 2 * bestStrategy - currentStrategy - 1;
        if(candidate < 1) {
            candidate = currentStrategy + 1;
            if(candidate > (int)ZSTD_btultra) {
                return 0;
            } else {
                return candidate;
            }
        } else {
            return candidate;
        }
    } else { /* bestStrategy >= currentStrategy */
        int candidate = 2 * bestStrategy - currentStrategy;
        if(candidate > (int)ZSTD_btultra) {
            candidate = currentStrategy - 1;
            if(candidate < 1) {
                return 0;
            } else {
                return candidate;
            }
        } else {
            return candidate;
        }
    }
}

/* experiment with playing with this and decay value */

/* main fn called when using --optimize */
/* Does strategy selection by benchmarking default compression levels
 * then optimizes by strategy, starting with the best one and moving 
 * progressively moving further away by number */

static int g_maxTries = 3;
#define TRY_DECAY 1

static int optimizeForSize(const char* const * const fileNamesTable, const size_t nbFiles, const char* dictFileName, constraint_t target, ZSTD_compressionParameters paramTarget, int cLevel)
{
    varInds_t varArray [NUM_PARAMS];
    int ret = 0;
    const int varLen = variableParams(paramTarget, varArray);
    winnerInfo_t winner = initWinnerInfo(emptyParams());
    U8** allMT = NULL;
    size_t k;
    size_t maxBlockSize = 0;
    contexts_t ctx;
    buffers_t buf;

    /* Init */
    if(!cParamValid(paramTarget)) {
        return 1;
    }

    /* load dictionary*/
    if(createBuffers(&buf, fileNamesTable, nbFiles)) {
        DISPLAY("unable to load files\n");
        return 1;
    }

    if(createContexts(&ctx, dictFileName)) {
        DISPLAY("unable to load dictionary\n");
        freeBuffers(buf);
        return 2;
    }

    if(nbFiles == 1) {
        DISPLAY("Loading %s...       \r", fileNamesTable[0]);
    } else {
        DISPLAY("Loading %lu Files...       \r", (unsigned long)nbFiles); 
    }


    for(k = 0; k < buf.nbBlocks; k++) {
        maxBlockSize = MAX(buf.srcSizes[k], maxBlockSize);
    }

    /* if strategy is fixed, only init that part of memotable */
    if(paramTarget.strategy) {
        varInds_t varNew[NUM_PARAMS];
        int varLenNew = sanitizeVarArray(varNew, varLen, varArray, paramTarget.strategy);
        allMT = calloc(sizeof(U8), (ZSTD_btultra + 1));
        if(allMT == NULL) {
            ret = 57;
            goto _cleanUp;
        }

        allMT[paramTarget.strategy] = malloc(sizeof(U8) * memoTableLen(varNew, varLenNew));
        
        if(allMT[paramTarget.strategy] == NULL) {
            ret = 58;
            goto _cleanUp;
        }
        
        initMemoTable(allMT[paramTarget.strategy], paramTarget, target, varNew, varLenNew, maxBlockSize);
    } else {
         allMT = createMemoTableArray(paramTarget, target, varArray, varLen, maxBlockSize);
    }
   

    if(!allMT) {
        DISPLAY("MemoTable Init Error\n");
        ret = 2;
        goto _cleanUp;
    }
 
    /* default strictness = Maximum for */
    if(g_strictness == DEFAULT_STRICTNESS) {
        if(g_optmode) {
            g_strictness = 99;
        } else {
            g_strictness = 90;
        }
    } else {
        if(0 >= g_strictness || g_strictness > 100) {
            DISPLAY("Strictness Outside of Bounds\n");
            ret = 4;
            goto _cleanUp;
        }
    }

    /* use level'ing mode instead of normal target mode */
    /* Should lvl be parameter-masked here? */
    if(g_optmode) {
        winner.params = ZSTD_getCParams(cLevel, maxBlockSize, ctx.dictSize);
        if(BMK_benchParam(&winner.result, buf, ctx, winner.params)) {
            ret = 3;
            goto _cleanUp;
        }
 
        g_lvltarget = winner.result; 
        g_lvltarget.cSpeed *= ((double)g_strictness) / 100;
        g_lvltarget.dSpeed *= ((double)g_strictness) / 100;
        g_lvltarget.cSize /= ((double)g_strictness) / 100;

        target.cSpeed = (U32)g_lvltarget.cSpeed;  
        target.dSpeed = (U32)g_lvltarget.dSpeed; //See if this is reasonable. 

        BMK_printWinnerOpt(stdout, cLevel, winner.result, winner.params, target, buf.srcSize);
    }

    /* Don't want it to return anything worse than the best known result */
    if(g_singleRun) {
        BMK_result_t res;
        g_params = ZSTD_adjustCParams(maskParams(ZSTD_getCParams(cLevel, maxBlockSize, ctx.dictSize), g_params), maxBlockSize, ctx.dictSize);
        if(BMK_benchParam(&res, buf, ctx, g_params)) {
            ret = 45;
            goto _cleanUp;
        }
        if(compareResultLT(winner.result, res, relaxTarget(target), buf.srcSize)) {
            winner.result = res;
            winner.params = g_params;
        }
    }

    /* bench */
    DISPLAY("\r%79s\r", "");
    if(nbFiles == 1) {
        DISPLAY("optimizing for %s", fileNamesTable[0]);
    } else {
        DISPLAY("optimizing for %lu Files", (unsigned long)nbFiles);
    }

    if(target.cSpeed != 0) { DISPLAY(" - limit compression speed %u MB/s", target.cSpeed >> 20); }
    if(target.dSpeed != 0) { DISPLAY(" - limit decompression speed %u MB/s", target.dSpeed >> 20); }
    if(target.cMem != (U32)-1) { DISPLAY(" - limit memory %u MB", target.cMem >> 20); }

    DISPLAY("\n");
    findClockGranularity();

    {   
        ZSTD_compressionParameters CParams;

        /* find best solution from default params */
        {
            /* strategy selection */
            const int maxSeeds = g_noSeed ? 1 : ZSTD_maxCLevel();
            DEBUGOUTPUT("Strategy Selection\n");
            if(paramTarget.strategy == 0) {
                BMK_result_t candidate;
                int i;
                for (i=1; i<=maxSeeds; i++) {
                    int ec;
                    CParams = maskParams(ZSTD_getCParams(i, maxBlockSize, ctx.dictSize), paramTarget);
                    ec = BMK_benchParam(&candidate, buf, ctx, CParams);
                    BMK_printWinnerOpt(stdout, i, candidate, CParams, target, buf.srcSize);

                    if(!ec && compareResultLT(winner.result, candidate, relaxTarget(target), buf.srcSize)) {
                        winner.result = candidate;
                        winner.params = CParams;
                    }

                    /* if the current params are too slow, just stop. */
                    if(target.cSpeed > candidate.cSpeed * 3 / 2) { break; }
                }
            }
        }

        BMK_printWinnerOpt(stdout, CUSTOM_LEVEL, winner.result, winner.params, target, buf.srcSize);
        BMK_translateAdvancedParams(winner.params);
        DEBUGOUTPUT("Real Opt\n");
        /* start 'real' tests */
        {   
            int bestStrategy = (int)winner.params.strategy;
            if(paramTarget.strategy == 0) {
                int st = (int)winner.params.strategy;
                int tries = g_maxTries;

                { 
                    /* one iterations of hill climbing with the level-defined parameters. */
                    winnerInfo_t w1 = climbOnce(target, varArray, varLen, st, allMT, 
                        buf, ctx, winner.params);
                    if(compareResultLT(winner.result, w1.result, target, buf.srcSize)) {
                        winner = w1;
                    }
                }

                while(st && tries > 0) {
                    winnerInfo_t wc;
                    DEBUGOUTPUT("StrategySwitch: %s\n", g_stratName[st]);
                    
                    wc = optimizeFixedStrategy(buf, ctx, target, paramTarget, 
                        st, varArray, varLen, allMT, tries);

                    if(compareResultLT(winner.result, wc.result, target, buf.srcSize)) {
                        winner = wc;
                        tries = g_maxTries;
                        bestStrategy = st;
                    } else {
                        st = nextStrategy(st, bestStrategy);
                        tries -= TRY_DECAY;
                    }
                }
            } else {
                winner = optimizeFixedStrategy(buf, ctx, target, paramTarget, paramTarget.strategy, 
                    varArray, varLen, allMT, g_maxTries);
            }

        }

        /* no solution found */
        if(winner.result.cSize == (size_t)-1) {
            ret = 1;
            DISPLAY("No feasible solution found\n");
            goto _cleanUp;
        }
        /* end summary */
        BMK_printWinnerOpt(stdout, CUSTOM_LEVEL, winner.result, winner.params, target, buf.srcSize);
        BMK_translateAdvancedParams(winner.params);
        DISPLAY("grillParams size - optimizer completed \n");

    }
_cleanUp: 
    freeContexts(ctx);
    freeBuffers(buf);
    freeMemoTableArray(allMT);
    return ret;
}

static void errorOut(const char* msg)
{
    DISPLAY("%s \n", msg); exit(1);
}

/*! readU32FromChar() :
 * @return : unsigned integer value read from input in `char` format.
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 *  Note : function will exit() program if digit sequence overflows */
static unsigned readU32FromChar(const char** stringPtr)
{
    const char errorMsg[] = "error: numeric value too large";
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        unsigned const max = (((unsigned)(-1)) / 10) - 1;
        if (result > max) errorOut(errorMsg);
        result *= 10, result += **stringPtr - '0', (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        unsigned const maxK = ((unsigned)(-1)) >> 10;
        if (result > maxK) errorOut(errorMsg);
        result <<= 10;
        if (**stringPtr=='M') {
            if (result > maxK) errorOut(errorMsg);
            result <<= 10;
        }
        (*stringPtr)++;  /* skip `K` or `M` */
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}

static int usage(const char* exename)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [arg] file\n", exename);
    DISPLAY( "Arguments :\n");
    DISPLAY( " file : path to the file used as reference (if none, generates a compressible sample)\n");
    DISPLAY( " -H/-h  : Help (this text + advanced options)\n");
    return 0;
}

static int usage_advanced(void)
{
    DISPLAY( "\nAdvanced options :\n");
    DISPLAY( " -T#          : set level 1 speed objective \n");
    DISPLAY( " -B#          : cut input into blocks of size # (default : single block) \n");
    DISPLAY( " -i#          : iteration loops (default : %i) \n", NBLOOPS);
    DISPLAY( " --optimize=  : same as -O with more verbose syntax (see README.md)\n");
    DISPLAY( " -S           : Single run \n");
    DISPLAY( " --zstd       : Single run, parameter selection same as zstdcli \n");
    DISPLAY( " -P#          : generated sample compressibility (default : %.1f%%) \n", COMPRESSIBILITY_DEFAULT * 100);
    DISPLAY( " -t#          : Caps runtime of operation in seconds (default : %u seconds (%.1f hours)) \n", (U32)g_grillDuration_s, g_grillDuration_s / 3600);
    DISPLAY( " -v           : Prints Benchmarking output\n");
    DISPLAY( " -D           : Next argument dictionary file\n");
    DISPLAY( " -s           : Seperate Files\n");
    return 0;
}

static int badusage(const char* exename)
{
    DISPLAY("Wrong parameters\n");
    usage(exename);
    return 1;
}

int main(int argc, const char** argv)
{
    int i,
        filenamesStart=0,
        result;
    const char* exename=argv[0];
    const char* input_filename = NULL;
    const char* dictFileName = NULL;
    U32 main_pause = 0;
    int cLevel = 0;
    int seperateFiles = 0;

    constraint_t target = { 0, 0, (U32)-1 }; 

    ZSTD_compressionParameters paramTarget = emptyParams();

    assert(argc>=1);   /* for exename */

    g_time = UTIL_getTime();

    /* Welcome message */
    DISPLAY(WELCOME_MESSAGE);

    for(i=1; i<argc; i++) {
        const char* argument = argv[i];
        DEBUGOUTPUT("%d: ", i);
        DEBUGOUTPUT("%s\n", argument);

        assert(argument != NULL);

        if(!strcmp(argument,"--no-seed")) { g_noSeed = 1; continue; }

        if (longCommandWArg(&argument, "--optimize=")) {
            g_optimizer = 1;
            for ( ; ;) {
                if (longCommandWArg(&argument, "windowLog=") || longCommandWArg(&argument, "wlog=")) { paramTarget.windowLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "chainLog=") || longCommandWArg(&argument, "clog=")) { paramTarget.chainLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "hashLog=") || longCommandWArg(&argument, "hlog=")) { paramTarget.hashLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "searchLog=") || longCommandWArg(&argument, "slog=")) { paramTarget.searchLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "searchLength=") || longCommandWArg(&argument, "slen=")) { paramTarget.searchLength = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "targetLength=") || longCommandWArg(&argument, "tlen=")) { paramTarget.targetLength = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "strategy=") || longCommandWArg(&argument, "strat=")) { paramTarget.strategy = (ZSTD_strategy)(readU32FromChar(&argument)); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "compressionSpeed=") || longCommandWArg(&argument, "cSpeed=")) { target.cSpeed = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "decompressionSpeed=") || longCommandWArg(&argument, "dSpeed=")) { target.dSpeed = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "compressionMemory=") || longCommandWArg(&argument, "cMem=")) { target.cMem = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "level=") || longCommandWArg(&argument, "lvl=")) { cLevel = readU32FromChar(&argument); g_optmode = 1; if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "strict=") || longCommandWArg(&argument, "stc=")) { g_strictness = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "prefer") || longCommandWArg(&argument, "prf")) {
                        if (longCommandWArg(&argument, "Speed=") || longCommandWArg(&argument, "Spd=")) {
                            g_speedMultiplier = readU32FromChar(&argument);
                        } else if (longCommandWArg(&argument, "Ratio=") || longCommandWArg(&argument, "Rto=")) {
                            g_ratioMultiplier = readU32FromChar(&argument);
                        }
                        if (argument[0]==',') { argument++; continue; } else break; 
                }
                if (longCommandWArg(&argument, "maxTries=") || longCommandWArg(&argument, "tries=")) { g_maxTries = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                DISPLAY("invalid optimization parameter \n");
                return 1;
            }

            if (argument[0] != 0) {
                DISPLAY("invalid --optimize= format\n");
                return 1; /* check the end of string */
            }
            continue;
        } else if (longCommandWArg(&argument, "--zstd=")) {
        /* Decode command (note : aggregated commands are allowed) */
            g_singleRun = 1;
            cLevel = 2;
            for ( ; ;) {
                if (longCommandWArg(&argument, "windowLog=") || longCommandWArg(&argument, "wlog=")) { g_params.windowLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "chainLog=") || longCommandWArg(&argument, "clog=")) { g_params.chainLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "hashLog=") || longCommandWArg(&argument, "hlog=")) { g_params.hashLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "searchLog=") || longCommandWArg(&argument, "slog=")) { g_params.searchLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "searchLength=") || longCommandWArg(&argument, "slen=")) { g_params.searchLength = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "targetLength=") || longCommandWArg(&argument, "tlen=")) { g_params.targetLength = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "strategy=") || longCommandWArg(&argument, "strat=")) { g_params.strategy = (ZSTD_strategy)(readU32FromChar(&argument)); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "level=") || longCommandWArg(&argument, "lvl=")) { cLevel = readU32FromChar(&argument); g_params = emptyParams(); if (argument[0]==',') { argument++; continue; } else break; }
                DISPLAY("invalid compression parameter \n");
                return 1;
            }

            if (argument[0] != 0) {
                DISPLAY("invalid --zstd= format\n");
                return 1; /* check the end of string */
            }
            continue;
            /* if not return, success */
        } else if (argument[0]=='-') {
            argument++;

            while (argument[0]!=0) {

                switch(argument[0])
                {
                    /* Display help on usage */
                case 'h' :
                case 'H': usage(exename); usage_advanced(); return 0;

                    /* Pause at the end (hidden option) */
                case 'p': main_pause = 1; argument++; break;
                    /* Modify Nb Iterations */

                case 'i':
                    argument++;
                    g_nbIterations = readU32FromChar(&argument);
                    break;

                    /* Sample compressibility (when no file provided) */
                case 'P':
                    argument++;
                    {   U32 const proba32 = readU32FromChar(&argument);
                        g_compressibility = (double)proba32 / 100.;
                    }
                    break;

                    /* Run Single conf */
                case 'S':
                    g_singleRun = 1;
                    argument++;
                    g_params = ZSTD_getCParams(2, g_blockSize, 0);
                    for ( ; ; ) {
                        switch(*argument)
                        {
                        case 'w':
                            argument++;
                            g_params.windowLog = readU32FromChar(&argument);
                            continue;
                        case 'c':
                            argument++;
                            g_params.chainLog = readU32FromChar(&argument);
                            continue;
                        case 'h':
                            argument++;
                            g_params.hashLog = readU32FromChar(&argument);
                            continue;
                        case 's':
                            argument++;
                            g_params.searchLog = readU32FromChar(&argument);
                            continue;
                        case 'l':  /* search length */
                            argument++;
                            g_params.searchLength = readU32FromChar(&argument);
                            continue;
                        case 't':  /* target length */
                            argument++;
                            g_params.targetLength = readU32FromChar(&argument);
                            continue;
                        case 'S':  /* strategy */
                            argument++;
                            g_params.strategy = (ZSTD_strategy)readU32FromChar(&argument);
                            continue;
                        case 'L':
                            {   argument++;
                                cLevel = readU32FromChar(&argument);
                                g_params = emptyParams();
                                continue;
                            }
                        default : ;
                        }
                        break;
                    }

                    break;

                    /* target level1 speed objective, in MB/s */
                case 'T':
                    argument++;
                    g_target = readU32FromChar(&argument);
                    break;

                    /* cut input into blocks */
                case 'B':
                    argument++;
                    g_blockSize = readU32FromChar(&argument);
                    DISPLAY("using %u KB block size \n", g_blockSize>>10);
                    break;

                    /* caps runtime (in seconds) */
                case 't':
                    argument++;
                    g_grillDuration_s = (double)readU32FromChar(&argument);
                    break;

                case 's':
                    seperateFiles = 1;
                    break;

                /* load dictionary file (only applicable for optimizer rn) */
                case 'D':
                    if(i == argc - 1) { /* last argument, return error. */ 
                        DISPLAY("Dictionary file expected but not given : %d\n", i);
                        return 1;
                    } else {
                        i++;
                        dictFileName = argv[i];
                        argument += strlen(argument);
                    }
                    break;

                    /* Unknown command */
                default : return badusage(exename);
                }
            }
            continue;
        }   /* if (argument[0]=='-') */

        /* first provided filename is input */
        if (!input_filename) { input_filename=argument; filenamesStart=i; continue; }
    }
    if (filenamesStart==0) {
        if (g_optimizer) {
            DISPLAY("Optimizer Expects File\n");
            return 1;
        } else {
            result = benchSample();
        }
    } else {
        if(seperateFiles) {
            for(i = 0; i < argc - filenamesStart; i++) {
                if (g_optimizer) {
                    result = optimizeForSize(argv+filenamesStart + i, 1, dictFileName, target, paramTarget, cLevel);
                    if(result) { DISPLAY("Error on File %d", i); return result; }
                } else {
                    result = benchFiles(argv+filenamesStart + i, 1, dictFileName, cLevel);
                    if(result) { DISPLAY("Error on File %d", i); return result; }
                }
            }
        } else {
            if (g_optimizer) {
                result = optimizeForSize(argv+filenamesStart, argc-filenamesStart, dictFileName, target, paramTarget, cLevel);
            } else {
                result = benchFiles(argv+filenamesStart, argc-filenamesStart, dictFileName, cLevel);
            }
        }   
    }

    if (main_pause) { int unused; printf("press enter...\n"); unused = getchar(); (void)unused; }

    return result;
}
