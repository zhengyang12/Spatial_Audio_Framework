/*
 Copyright 2018 Leo McCormack
 
 Permission to use, copy, modify, and/or distribute this software for any purpose with or
 without fee is hereby granted, provided that the above copyright notice and this permission
 notice appear in all copies.
 
 THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO
 THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT
 SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR
 ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE
 OR PERFORMANCE OF THIS SOFTWARE.
*/
/*
 * Filename:
 *     ambi_bin_internal.h
 * Description:
 *     A binaural Ambisonic decoder for reproducing ambisonic signals over headphones. 
 * Dependencies:
 *     saf_utilities, afSTFTlib, saf_hrir, saf_sh
 * Author, date created:
 *     Leo McCormack, 14.04.2018
 */

#ifndef __AMBI_BIN_INTERNAL_H_INCLUDED__
#define __AMBI_BIN_INTERNAL_H_INCLUDED__

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ambi_bin.h"
#include "ambi_bin_database.h"
#define SAF_ENABLE_AFSTFT   /* for time-frequency transform */
#define SAF_ENABLE_HOA      /* for ambisonic decoding matrices */
#define SAF_ENABLE_SH       /* for spherical harmonic weights */
#define SAF_ENABLE_HRIR     /* for HRIR->HRTF filterbank coefficients conversion */
#define SAF_ENABLE_VBAP     /* for interpolation table */
#include "saf.h"

#ifdef __cplusplus
extern "C" {
#endif
    
/***************/
/* Definitions */
/***************/
     
#define USE_NEAREST_HRIRS (1)                               /* 1: find nearest HRIRs to t-design dirs, 0: use triangular interpolation */
    
#define HOP_SIZE ( 128 )                                    /* STFT hop size = nBands */
#define HYBRID_BANDS ( HOP_SIZE + 5 )                       /* hybrid mode incurs an additional 5 bands  */
#define TIME_SLOTS ( FRAME_SIZE / HOP_SIZE )                /* 4/8/16 */
#define NUM_EARS ( 2 )                                      /* true for most humans */
#define MAX_SH_ORDER ( 7 )                                  /* 7->64 channels; maximum for most hosts */
#define MAX_NUM_SH_SIGNALS ( (MAX_SH_ORDER+1)*(MAX_SH_ORDER+1) )
#define POST_GAIN ( -9.0f )   /* dB */
    
#ifndef DEG2RAD
  #define DEG2RAD(x) (x * PI / 180.0f)
#endif
#ifndef RAD2DEG
  #define RAD2DEG(x) (x * 180.0f / PI)
#endif
    
/***********/
/* Structs */
/***********/
    
typedef struct _codecPars
{
    /* Decoder */
    float_complex M_dec[HYBRID_BANDS][NUM_EARS][MAX_NUM_SH_SIGNALS];
    
    /* sofa file info */
    char* sofa_filepath;                     /* absolute/relevative file path for a sofa file */
    float* hrirs;                            /* time domain HRIRs; N_hrir_dirs x 2 x hrir_len */
    float* hrir_dirs_deg;                    /* directions of the HRIRs in degrees [azi elev]; N_hrir_dirs x 2 */
    int N_hrir_dirs;                         /* number of HRIR directions in the current sofa file */
    int hrir_len;                            /* length of the HRIRs, this can be truncated, see "saf_sofa_reader.h" */
    int hrir_fs;                             /* sampling rate of the HRIRs, should ideally match the host sampling rate, although not required */
   
    /* hrir filterbank coefficients */
    float* itds_s;                           /* interaural-time differences for each HRIR (in seconds); N_hrirs x 1 */
    float_complex* hrtf_fb;                  /* HRTF filterbank coeffs; nBands x nCH x N_hrirs */
    
}codecPars;

typedef struct _ambi_bin
{
    /* audio buffers + afSTFT time-frequency transform handle */
    float SHFrameTD[MAX_NUM_SH_SIGNALS][FRAME_SIZE]; 
    float_complex SHframeTF[HYBRID_BANDS][MAX_NUM_SH_SIGNALS][TIME_SLOTS];
    float_complex SHframeTF_rot[HYBRID_BANDS][MAX_NUM_SH_SIGNALS][TIME_SLOTS];
    float_complex binframeTF[HYBRID_BANDS][NUM_EARS][TIME_SLOTS];
    complexVector* STFTInputFrameTF;
    complexVector* STFTOutputFrameTF;
    void* hSTFT;                             /* afSTFT handle */
    int afSTFTdelay;                         /* for host delay compensation */
    float** tempHopFrameTD;                  /* temporary multi-channel time-domain buffer of size "HOP_SIZE". */
    int fs;                                  /* host sampling rate */
    float freqVector[HYBRID_BANDS];          /* frequency vector for time-frequency transform, in Hz */
    
    /* our codec configuration */
    codecPars* pars;                         /* codec parameters */
    
    /* internal variables */
    float_complex M_rot[MAX_NUM_SH_SIGNALS][MAX_NUM_SH_SIGNALS];
    int new_order;                           /* new decoding order */
    int new_nSH;                             /* if new_nSH != nSH, afSTFT is reinitialised */
    int nSH;                                 /* number of spherical harmonic signals */
    
    /* flags */
    int reInitCodec;                         /* 0: no init required, 1: init required, 2: init in progress */
    int reInitTFT;                           /* 0: no init required, 1: init required, 2: init in progress */
    int recalc_M_rotFLAG;                    /* 0: no init required, 1: init required */
    
    /* user parameters */
    int order;                               /* current decoding order */
    int enableMaxRE;                         /* 0: disabled, 1: enabled */
    int enableDiffuseMatching;               /* 0: disabled, 1: enabled */
    int enablePhaseWarping;                  /* 0: disabled, 1: enabled */
    DECODING_METHODS method;                 /* current decoding method */
    float EQ[HYBRID_BANDS];                  /* EQ curve */ 
    int useDefaultHRIRsFLAG;                 /* 1: use default HRIRs in database, 0: use those from SOFA file */
    CH_ORDER chOrdering;                     /* only ACN is supported */
    NORM_TYPES norm;                         /* N3D or SN3D */
    int enableRotation;
    float yaw, roll, pitch;                  /* rotation angles in degrees */
    int bFlipYaw, bFlipPitch, bFlipRoll;     /* flag to flip the sign of the individual rotation angles */
    int useRollPitchYawFlag;                 /* rotation order flag, 1: r-p-y, 0: y-p-r */
    
} ambi_bin_data;


/**********************/
/* Internal functions */
/**********************/
    
/* Intialises the codec parameters */
/* Note: take care to initalise time-frequency transform "ambi_bin_initTFT" first */
void ambi_bin_initCodec(void* const hAmbi);                   /* ambi_bin handle */

/* Initialise the filterbank used by ambiDEC */
void ambi_bin_initTFT(void* const hAmbi);                     /* ambi_bin handle */
    

#ifdef __cplusplus
}
#endif


#endif /* __AMBI_BIN_INTERNAL_H_INCLUDED__ */




















