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
 *     ambi_bin.c
 * Description:
 *     A binaural Ambisonic decoder for reproducing ambisonic signals over headphones.
 *     Optionally, a SOFA file may be loaded for personalised headphone listening.
 * Dependencies:
 *     saf_utilities, afSTFTlib, saf_hrir, saf_sh
 * Author, date created:
 *     Leo McCormack, 14.04.2018
 */
 
#include "ambi_bin_internal.h"

void ambi_bin_create
(
    void ** const phAmbi
)
{
    ambi_bin_data* pData = (ambi_bin_data*)malloc(sizeof(ambi_bin_data));
    if (pData == NULL) { return;/*error*/ }
    *phAmbi = (void*)pData;
    int t, ch, band;
    
    /* afSTFT stuff */
    pData->hSTFT = NULL;
    pData->STFTOutputFrameTF = (complexVector**)malloc2d(TIME_SLOTS, NUM_EARS, sizeof(complexVector));
    for(t=0; t<TIME_SLOTS; t++) {
        for(ch=0; ch< NUM_EARS; ch++) {
            pData->STFTOutputFrameTF[t][ch].re = (float*)calloc(HYBRID_BANDS, sizeof(float));
            pData->STFTOutputFrameTF[t][ch].im = (float*)calloc(HYBRID_BANDS, sizeof(float));
        }
    }
    pData->STFTInputFrameTF = NULL;
    pData->tempHopFrameTD = NULL;

    /* codec data */
    pData->pars = (codecPars*)malloc(sizeof(codecPars));
    codecPars* pars = pData->pars; 
    pars->sofa_filepath = NULL;
    pars->hrirs = NULL;
    pars->hrir_dirs_deg = NULL;
    pars->itds_s = NULL;
    pars->hrtf_fb = NULL;
    
    /* flags */
    pData->reInitCodec = 1;
    pData->reInitTFT = 1;
    
    /* default user parameters */
    for(band=0; band<HYBRID_BANDS; band++)
        pData->EQ[band] = 1.0f;
    pData->useDefaultHRIRsFLAG = 1; /* pars->sofa_filepath must be valid to set this to 0 */
    pData->chOrdering = CH_ACN;
    pData->norm = NORM_N3D;
    pData->rE_WEIGHT = 0;
    pData->yaw = 0.0f;
    pData->pitch = 0.0f;
    pData->roll = 0.0f;
    pData->bFlipYaw = 0;
    pData->bFlipPitch = 0;
    pData->bFlipRoll = 0;
    ambi_bin_setInputOrderPreset(*phAmbi, INPUT_ORDER_FIRST);
    pData->nSH = pData->new_nSH;
    
}

void ambi_bin_destroy
(
    void ** const phAmbi
)
{
    ambi_bin_data *pData = (ambi_bin_data*)(*phAmbi);
    codecPars *pars = pData->pars;
    int t, ch;
    
    if (pData != NULL) {
        if(pData->hSTFT!=NULL)
            afSTFTfree(pData->hSTFT);
        for (t = 0; t<TIME_SLOTS; t++) {
            if(pData->STFTInputFrameTF!=NULL){
                for (ch = 0; ch< pData->nSH; ch++) {
                    free(pData->STFTInputFrameTF[t][ch].re);
                    free(pData->STFTInputFrameTF[t][ch].im);
                }
            }
            for (ch = 0; ch< NUM_EARS; ch++) {
                free(pData->STFTOutputFrameTF[t][ch].re);
                free(pData->STFTOutputFrameTF[t][ch].im);
            }
        }
        free2d((void**)pData->STFTInputFrameTF, TIME_SLOTS);
        free2d((void**)pData->STFTOutputFrameTF, TIME_SLOTS);
        if(pData->tempHopFrameTD!=NULL)
            free2d((void**)pData->tempHopFrameTD, MAX(NUM_EARS, pData->nSH));
        
        if(pars->hrtf_fb!= NULL)
            free(pars->hrtf_fb);
        if(pars->itds_s!= NULL)
            free(pars->itds_s);
        if(pars->hrirs!= NULL)
            free(pars->hrirs);
        if(pars->hrir_dirs_deg!= NULL)
            free(pars->hrir_dirs_deg);

        free(pData);
        pData = NULL;
    }
}

void ambi_bin_init
(
    void * const hAmbi,
    int          sampleRate
)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    int i, band;
    
    /* define frequency vector */
    pData->fs = sampleRate;
    for(band=0; band <HYBRID_BANDS; band++){
        if(sampleRate == 44100)
            pData->freqVector[band] =  (float)__afCenterFreq44100[band];
        else /* Assume 48kHz */
            pData->freqVector[band] =  (float)__afCenterFreq48e3[band];
    }
    
    /* starting values */
    for(i=1; i<=TIME_SLOTS; i++)
        pData->interpolator[i-1] = (float)i*1.0f/(float)TIME_SLOTS;
    memset(pData->current_M, 0, HYBRID_BANDS*NUM_EARS*MAX_NUM_SH_SIGNALS*sizeof(float_complex));
    memset(pData->prev_M, 0, HYBRID_BANDS*NUM_EARS*MAX_NUM_SH_SIGNALS*sizeof(float_complex));
    memset(pData->prev_SHframeTF, 0, HYBRID_BANDS*MAX_NUM_SH_SIGNALS*TIME_SLOTS*sizeof(float_complex));
}

void ambi_bin_process
(
    void  *  const hAmbi,
    float ** const inputs,
    float ** const outputs,
    int            nInputs,
    int            nOutputs,
    int            nSamples,
    int            isPlaying
)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    codecPars* pars = pData->pars;
    int n, t, sample, ch, i, j, band;
    int o[MAX_SH_ORDER+2];
    const float_complex calpha = cmplxf(1.0f,0.0f), cbeta = cmplxf(0.0f, 0.0f);
    float Rxyz[3][3];
    float_complex M_rot[MAX_NUM_SH_SIGNALS][MAX_NUM_SH_SIGNALS];
    float_complex temp_binframeTF[NUM_EARS][TIME_SLOTS];
    float* M_rot_tmp;
    
#ifdef ENABLE_FADE_IN_OUT
    int applyFadeIn;
    if(pData->reInitTFT || pData->reInitCodec)
        applyFadeIn = 1;
    else
        applyFadeIn = 0;
#endif
    /* local copies of user parameters */
    int order, nSH, rE_WEIGHT;
    NORM_TYPES norm;
    
    /* reinitialise if needed */
    if(pData->reInitTFT==1){
        pData->reInitTFT = 2;
        ambi_bin_initTFT(hAmbi); /* always init before codec or hrtfs (will do this better in future release) */
        pData->reInitTFT = 0;
    }
    if(pData->reInitCodec==1){
        pData->reInitCodec = 2;
        ambi_bin_initCodec(hAmbi);
        pData->reInitCodec = 0;
    }
    
    /* decode audio to loudspeakers or headphones */
    if ( (nSamples == FRAME_SIZE) && (isPlaying) && (pData->reInitCodec==0) && (pData->reInitTFT==0) ) {
        /* copy user parameters to local variables */
        for(n=0; n<MAX_SH_ORDER+2; n++){  o[n] = n*n;  }
        norm = pData->norm;
        rE_WEIGHT = pData->rE_WEIGHT;
        order = pData->order;
        nSH = (order+1)*(order+1);
        
        /* Load time-domain data */
        for(i=0; i < MIN(MAX_NUM_SH_SIGNALS, nInputs); i++)
            memcpy(pData->SHFrameTD[i], inputs[i], FRAME_SIZE * sizeof(float));
        for(; i<MAX_NUM_SH_SIGNALS; i++)
            memset(pData->SHFrameTD[i], 0, FRAME_SIZE * sizeof(float)); /* fill remaining channels with zeros, to avoid funky behaviour */
#ifdef ENABLE_FADE_IN_OUT
        if(applyFadeIn)
            for(ch=0; ch < MAX_NUM_SH_SIGNALS;ch++)
                for(i=0; i<FRAME_SIZE; i++)
                    pData->SHFrameTD[ch][i] *= (float)i/(float)FRAME_SIZE;
#endif
        
        /* account for input normalisation scheme */
        switch(norm){
            case NORM_N3D:  /* already in N3D, do nothing */
                break;
            case NORM_SN3D: /* convert to N3D */
                for (n = 0; n<order+1; n++)
                    for (ch = o[n]; ch<o[n+1]; ch++)
                        for(i = 0; i<FRAME_SIZE; i++)
                            pData->SHFrameTD[ch][i] *= sqrtf(2.0f*(float)n+1.0f);
                break;
        }
        
        /* Apply time-frequency transform (TFT) */
        for ( t=0; t< TIME_SLOTS; t++) {
            for( ch=0; ch < nSH; ch++)
                for ( sample=0; sample < HOP_SIZE; sample++)
                    pData->tempHopFrameTD[ch][sample] = pData->SHFrameTD[ch][sample + t*HOP_SIZE];
            afSTFTforward(pData->hSTFT, (float**)pData->tempHopFrameTD, (complexVector*)pData->STFTInputFrameTF[t]);
        }
        for(band=0; band<HYBRID_BANDS; band++)
            for( ch=0; ch < nSH; ch++)
                for ( t=0; t<TIME_SLOTS; t++)
                    pData->SHframeTF[band][ch][t] = cmplxf(pData->STFTInputFrameTF[t][ch].re[band], pData->STFTInputFrameTF[t][ch].im[band]);
    
        /* Specify rotation matrix */
		if (order > 0) {
			M_rot_tmp = malloc(nSH*nSH * sizeof(float));
			yawPitchRoll2Rzyx(pData->yaw, pData->pitch, pData->roll, Rxyz);
			getSHrotMtxReal(Rxyz, M_rot_tmp, order);
			for (i = 0; i < nSH; i++)
				for (j = 0; j < nSH; j++)
					M_rot[i][j] = cmplxf(M_rot_tmp[i*nSH + j], 0.0f);
			free(M_rot_tmp);
		}
        
        /* Define mixing matrix per band */
        for (band = 0; band < HYBRID_BANDS; band++) {
			if (order > 0) { 
				cblas_cgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, NUM_EARS, nSH, nSH, &calpha,
					pars->M_dec[band], MAX_NUM_SH_SIGNALS,
					M_rot, MAX_NUM_SH_SIGNALS, &cbeta,
					pData->current_M[band], MAX_NUM_SH_SIGNALS);
			}
			else
				for(i=0; i<NUM_EARS; i++)
					memcpy(pData->current_M[band][i], pars->M_dec[band][i], nSH * sizeof(float_complex));
        }
        
        /* mix to headphones */
        for (band = 0; band < HYBRID_BANDS; band++) {
            cblas_cgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, NUM_EARS, TIME_SLOTS, nSH, &calpha,
                        pData->prev_M[band], MAX_NUM_SH_SIGNALS,
                        pData->prev_SHframeTF[band], TIME_SLOTS, &cbeta,
                        temp_binframeTF, TIME_SLOTS);
            cblas_cgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, NUM_EARS, TIME_SLOTS, nSH, &calpha,
                        pData->current_M[band], MAX_NUM_SH_SIGNALS,
                        pData->prev_SHframeTF[band], TIME_SLOTS, &cbeta,
                        pData->binframeTF[band], TIME_SLOTS);
            for (i=0; i < NUM_EARS; i++)
                for(j=0; j<TIME_SLOTS; j++)
                    pData->binframeTF[band][i][j] = ccaddf(crmulf(pData->binframeTF[band][i][j], pData->interpolator[j]),
                                                           crmulf(temp_binframeTF[i][j], (1.0f-pData->interpolator[j])));
        }
        
        /* TODO: Apply order-dependent EQ curve */
        
        
        /* for next frame */
        for (band = 0; band < HYBRID_BANDS; band++){
            for (i = 0; i < nSH; i++)
                memcpy(pData->prev_SHframeTF[band][i], pData->SHframeTF[band][i], TIME_SLOTS*sizeof(float_complex));
            for (i = 0; i < NUM_EARS; i++)
                memcpy(pData->prev_M[band][i], pData->current_M[band][i], nSH*sizeof(float_complex));
        }
        
        /* inverse-TFT */
        for (band = 0; band < HYBRID_BANDS; band++) {
            for (ch = 0; ch < NUM_EARS; ch++) {
                for (t = 0; t < TIME_SLOTS; t++) {
                    pData->STFTOutputFrameTF[t][ch].re[band] = crealf(pData->binframeTF[band][ch][t]);
                    pData->STFTOutputFrameTF[t][ch].im[band] = cimagf(pData->binframeTF[band][ch][t]);
                }
            }
        }
        for (t = 0; t < TIME_SLOTS; t++) {
            afSTFTinverse(pData->hSTFT, pData->STFTOutputFrameTF[t], pData->tempHopFrameTD);
            for (ch = 0; ch < MIN(NUM_EARS, nOutputs); ch++)
                for (sample = 0; sample < HOP_SIZE; sample++)
                    outputs[ch][sample + t* HOP_SIZE] = pData->tempHopFrameTD[ch][sample];
            for (; ch < nOutputs; ch++) /* fill remaining channels with zeros */
                for (sample = 0; sample < HOP_SIZE; sample++)
                    outputs[ch][sample + t* HOP_SIZE] = 0.0f;
        }
#ifdef ENABLE_FADE_IN_OUT
        if(pData->reInitTFT || pData->reInitCodec)
            for(ch=0; ch < nOutputs; ch++)
                for(i=0; i<FRAME_SIZE; i++)
                    outputs[ch][i] *= (1.0f - (float)(i+1)/(float)FRAME_SIZE);
#endif
    }
    else
        for (ch=0; ch < nOutputs; ch++)
            memset(outputs[ch],0, FRAME_SIZE*sizeof(float));
}


/* Set Functions */

void ambi_bin_refreshSettings(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    pData->reInitCodec = 1;
    pData->reInitTFT = 1;
}

void ambi_bin_setUseDefaultHRIRsflag(void* const hAmbi, int newState)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    
    if((!pData->useDefaultHRIRsFLAG) && (newState)){
        pData->useDefaultHRIRsFLAG = newState;
        pData->reInitCodec = 1;
    }
}

void ambi_bin_setSofaFilePath(void* const hAmbi, const char* path)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    codecPars* pars = pData->pars;
    
    pars->sofa_filepath = malloc(strlen(path) + 1);
    strcpy(pars->sofa_filepath, path);
    pData->useDefaultHRIRsFLAG = 0;
    pData->reInitCodec = 1;
}

void ambi_bin_setInputOrderPreset(void* const hAmbi, INPUT_ORDERS newPreset)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    if(pData->orderSelected != (INPUT_ORDERS)newPreset ){
        pData->orderSelected = (INPUT_ORDERS)newPreset;
        switch(pData->orderSelected){
            case INPUT_OMNI:          pData->order = 0; break;
            default:
            case INPUT_ORDER_FIRST:   pData->order = 1; break;
            case INPUT_ORDER_SECOND:  pData->order = 2; break;
            case INPUT_ORDER_THIRD:   pData->order = 3; break;
            case INPUT_ORDER_FOURTH:  pData->order = 4; break;
            case INPUT_ORDER_FIFTH:   pData->order = 5; break;
            case INPUT_ORDER_SIXTH:   pData->order = 6; break;
            case INPUT_ORDER_SEVENTH: pData->order = 7; break;
        }
        pData->new_nSH = (pData->order+1)*(pData->order+1);
        if(pData->new_nSH!=pData->nSH)
            pData->reInitTFT = 1;
        pData->reInitCodec = 1;
    }
}

void ambi_bin_setChOrder(void* const hAmbi, int newOrder)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    pData->chOrdering = (CH_ORDER)newOrder;
}

void ambi_bin_setNormType(void* const hAmbi, int newType)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    pData->norm = (NORM_TYPES)newType;
}

void ambi_bin_setDecEnableMaxrE(void* const hAmbi, int newState)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    pData->rE_WEIGHT = newState;
}

void ambi_bin_setEnableEQ(void* const hAmbi, int newState)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    pData->enableEQ = newState;
}

void ambi_bin_setYaw(void  * const hAmbi, float newYaw)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    pData->yaw = pData->bFlipYaw == 1 ? -DEG2RAD(newYaw) : DEG2RAD(newYaw);
}

void ambi_bin_setPitch(void* const hAmbi, float newPitch)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    pData->pitch = pData->bFlipPitch == 1 ? -DEG2RAD(newPitch) : DEG2RAD(newPitch);
}

void ambi_bin_setRoll(void* const hAmbi, float newRoll)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    pData->roll = pData->bFlipRoll == 1 ? -DEG2RAD(newRoll) : DEG2RAD(newRoll);
}

void ambi_bin_setFlipYaw(void* const hAmbi, int newState)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    if(newState !=pData->bFlipYaw ){
        pData->bFlipYaw = newState;
        ambi_bin_setYaw(hAmbi, -ambi_bin_getYaw(hAmbi));
    }
}

void ambi_bin_setFlipPitch(void* const hAmbi, int newState)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    if(newState !=pData->bFlipPitch ){
        pData->bFlipPitch = newState;
        ambi_bin_setPitch(hAmbi, -ambi_bin_getPitch(hAmbi));
    }
}

void ambi_bin_setFlipRoll(void* const hAmbi, int newState)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    if(newState !=pData->bFlipRoll ){
        pData->bFlipRoll = newState;
        ambi_bin_setRoll(hAmbi, -ambi_bin_getRoll(hAmbi));
    }
}

/* Get Functions */

int ambi_bin_getUseDefaultHRIRsflag(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->useDefaultHRIRsFLAG;
}

int ambi_bin_getInputOrderPreset(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return (int)pData->orderSelected;
}

char* ambi_bin_getSofaFilePath(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    codecPars* pars = pData->pars;
    if(pars->sofa_filepath!=NULL)
        return pars->sofa_filepath;
    else
        return "no_file";
}

int ambi_bin_getChOrder(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return (int)pData->chOrdering;
}

int ambi_bin_getNormType(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return (int)pData->norm;
}

int ambi_bin_getDecEnableMaxrE(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->rE_WEIGHT;
}

int ambi_bin_getEnableEQ(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->enableEQ;
}

float ambi_bin_getYaw(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->bFlipYaw == 1 ? -RAD2DEG(pData->yaw) : RAD2DEG(pData->yaw);
}

float ambi_bin_getPitch(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->bFlipPitch == 1 ? -RAD2DEG(pData->pitch) : RAD2DEG(pData->pitch);
}

float ambi_bin_getRoll(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->bFlipRoll == 1 ? -RAD2DEG(pData->roll) : RAD2DEG(pData->roll);
}

int ambi_bin_getFlipYaw(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->bFlipYaw;
}

int ambi_bin_getFlipPitch(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->bFlipPitch;
}

int ambi_bin_getFlipRoll(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->bFlipRoll;
}

int ambi_bin_getNDirs(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    codecPars* pars = pData->pars;
    return pars->N_hrir_dirs;
}

int ambi_bin_getHRIRlength(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    codecPars* pars = pData->pars;
    return pars->hrir_len;
}

int ambi_bin_getHRIRsamplerate(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    codecPars* pars = pData->pars;
    return pars->hrir_fs;
}

int ambi_bin_getDAWsamplerate(void* const hAmbi)
{
    ambi_bin_data *pData = (ambi_bin_data*)(hAmbi);
    return pData->fs;
}

