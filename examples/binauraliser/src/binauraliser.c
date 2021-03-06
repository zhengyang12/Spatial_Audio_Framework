    /*
 Copyright 2017-2018 Leo McCormack
 
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
 *     binauraliser.c
 * Description:
 *     Convolves input audio (up to 64 channels) with interpolated HRTFs in the time-frequency
 *     domain. The HRTFs are interpolated by applying amplitude-preserving VBAP gains to the
 *     HRTF magnitude responses and inter-aural time differences (ITDs) individually, before
 *     being re-combined. The example allows the user to specify an external SOFA file for the
 *     convolution.
 * Dependencies:
 *     saf_utilities, saf_hrir, saf_vbap, afSTFTlib
 * Author, date created:
 *     Leo McCormack, 25.09.2017
 */

#include "binauraliser_internal.h"

void binauraliser_create
(
    void ** const phBin
)
{
    binauraliser_data* pData = (binauraliser_data*)malloc(sizeof(binauraliser_data));
    if (pData == NULL) { return;/*error*/ }
    *phBin = (void*)pData;
    int ch;
    
    /* time-frequency transform + buffers */
    pData->hSTFT = NULL;
    pData->STFTInputFrameTF = malloc(MAX_NUM_INPUTS * sizeof(complexVector));
    for(ch=0; ch< MAX_NUM_INPUTS; ch++) {
        pData->STFTInputFrameTF[ch].re = (float*)calloc(HYBRID_BANDS, sizeof(float));
        pData->STFTInputFrameTF[ch].im = (float*)calloc(HYBRID_BANDS, sizeof(float));
    }
    pData->tempHopFrameTD = (float**)malloc2d( MAX(MAX_NUM_INPUTS, NUM_EARS), HOP_SIZE, sizeof(float));
    pData->STFTOutputFrameTF = malloc(NUM_EARS*sizeof(complexVector));
    for(ch=0; ch< NUM_EARS; ch++) {
        pData->STFTOutputFrameTF[ch].re = (float*)calloc(HYBRID_BANDS, sizeof(float));
        pData->STFTOutputFrameTF[ch].im = (float*)calloc(HYBRID_BANDS, sizeof(float));
    }
    
    /* hrir data */
    pData->useDefaultHRIRsFLAG=1;
    pData->hrirs = NULL;
    pData->hrir_dirs_deg = NULL;
    pData->sofa_filepath = NULL;
    
    /* vbap (amplitude normalised) */
    pData->hrtf_vbap_gtableIdx = NULL;
    pData->hrtf_vbap_gtableComp = NULL;
    
    /* HRTF filterbank coefficients */
    pData->itds_s = NULL;
    pData->hrtf_fb = NULL;
    pData->hrtf_fb_mag = NULL;
    
    /* flags */
    pData->reInitHRTFsAndGainTables = 1;
    for(ch=0; ch<MAX_NUM_INPUTS; ch++)
        pData->recalc_hrtf_interpFLAG[ch] = 1;
    pData->reInitTFT = 1;
    pData->recalc_M_rotFLAG = 1;
    
    /* user parameters */
    binauraliser_loadPreset(PRESET_DEFAULT, pData->src_dirs_deg, &(pData->new_nSources), &(pData->input_nDims)); /*check setStateInformation if you change default preset*/
    pData->nSources = pData->new_nSources;
    pData->interpMode = INTERP_TRI;
    pData->yaw = 0.0f;
    pData->pitch = 0.0f;
    pData->roll = 0.0f;
    pData->bFlipYaw = 0;
    pData->bFlipPitch = 0;
    pData->bFlipRoll = 0;
    pData->useRollPitchYawFlag = 0;
    pData->enableRotation = 0;
}


void binauraliser_destroy
(
    void ** const phBin
)
{
    binauraliser_data *pData = (binauraliser_data*)(*phBin);
    int ch;

    if (pData != NULL) {
        if(pData->hSTFT !=NULL)
            afSTFTfree(pData->hSTFT);
        for(ch=0; ch< MAX_NUM_INPUTS; ch++) {
            free(pData->STFTInputFrameTF[ch].re);
            free(pData->STFTInputFrameTF[ch].im);
        }
        for (ch = 0; ch< NUM_EARS; ch++) {
            free(pData->STFTOutputFrameTF[ch].re);
            free(pData->STFTOutputFrameTF[ch].im);
        }
        free(pData->STFTInputFrameTF);
        free(pData->STFTOutputFrameTF);
        free2d((void**)pData->tempHopFrameTD, MAX(MAX_NUM_INPUTS, NUM_EARS));
        
        if(pData->hrtf_vbap_gtableComp!= NULL)
            free(pData->hrtf_vbap_gtableComp);
        if(pData->hrtf_vbap_gtableIdx!= NULL)
            free(pData->hrtf_vbap_gtableIdx);
        if(pData->hrtf_fb!= NULL)
            free(pData->hrtf_fb);
        if(pData->hrtf_fb_mag!= NULL)
            free(pData->hrtf_fb_mag);
        if(pData->itds_s!= NULL)
            free(pData->itds_s);
        if(pData->hrirs!= NULL)
            free(pData->hrirs);
        if(pData->hrir_dirs_deg!= NULL)
            free(pData->hrir_dirs_deg);
         
        free(pData);
        pData = NULL;
    }
}

void binauraliser_init
(
    void * const hBin,
    int          sampleRate
)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    int band;
    
    /* define frequency vector */
    pData->fs = sampleRate;
    for(band=0; band <HYBRID_BANDS; band++){
        if(sampleRate == 44100)
            pData->freqVector[band] =  (float)__afCenterFreq44100[band];
        else
            pData->freqVector[band] =  (float)__afCenterFreq48e3[band];
    }

    /* reinitialise if needed */
    binauraliser_checkReInit(hBin);
    
    /* defaults */
    pData->recalc_M_rotFLAG = 1;
}

void binauraliser_process
(
    void  *  const hBin,
    float ** const inputs,
    float ** const outputs,
    int            nInputs,
    int            nOutputs,
    int            nSamples,
    int            isPlaying
)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    int t, sample, ch, ear, i, band, nSources; 
    float src_dirs[MAX_NUM_INPUTS][2], Rxyz[3][3], hypotxy;
    int enableRotation;
    
    /* reinitialise if needed */
#ifdef __APPLE__
    binauraliser_checkReInit(hBin);
#else
    if(pData->reInitTFT==1){
        pData->reInitTFT = 2;
        binauraliser_initTFT(hBin);
        pData->reInitTFT = 0; 
    }
#endif
    
    /* apply binaural panner */
    if ((nSamples == FRAME_SIZE) && (pData->hrtf_fb!=NULL) && (pData->reInitTFT == 0) &&
        (pData->reInitHRTFsAndGainTables == 0)) {
        nSources = pData->nSources;
        enableRotation = pData->enableRotation;
        memcpy(src_dirs, pData->src_dirs_deg, MAX_NUM_INPUTS*2*sizeof(float));
        
        /* Load time-domain data */
        for(i=0; i < MIN(nSources,nInputs); i++)
            utility_svvcopy(inputs[i], FRAME_SIZE, pData->inputFrameTD[i]);
        for(; i<MAX_NUM_INPUTS; i++)
            memset(pData->inputFrameTD[i], 0, FRAME_SIZE * sizeof(float));
#ifdef ENABLE_FADE_IN_OUT
        if(applyFadeIn)
            for(ch=0; ch < nSources;ch++)
                for(i=0; i<FRAME_SIZE; i++)
                    pData->inputFrameTD[ch][i] *= (float)i/(float)FRAME_SIZE;
#endif
        
        /* Apply time-frequency transform (TFT) */
        for(t=0; t< TIME_SLOTS; t++) {
            for(ch = 0; ch < nSources; ch++)
                utility_svvcopy(&(pData->inputFrameTD[ch][t*HOP_SIZE]), HOP_SIZE, pData->tempHopFrameTD[ch]);
            afSTFTforward(pData->hSTFT, (float**)pData->tempHopFrameTD, (complexVector*)pData->STFTInputFrameTF);
            for(band=0; band<HYBRID_BANDS; band++)
                for(ch=0; ch < nSources; ch++)
                    pData->inputframeTF[band][ch][t] = cmplxf(pData->STFTInputFrameTF[ch].re[band], pData->STFTInputFrameTF[ch].im[band]);
        }
        
        /* Main processing: */
        if(isPlaying){
            /* Rotate source directions */
            if(enableRotation && pData->recalc_M_rotFLAG){
                yawPitchRoll2Rzyx (pData->yaw, pData->pitch, pData->roll, pData->useRollPitchYawFlag, Rxyz);
                for(i=0; i<nSources; i++){
                    pData->src_dirs_xyz[i][0] = cosf(DEG2RAD(pData->src_dirs_deg[i][1])) * cosf(DEG2RAD(pData->src_dirs_deg[i][0]));
                    pData->src_dirs_xyz[i][1] = cosf(DEG2RAD(pData->src_dirs_deg[i][1])) * sinf(DEG2RAD(pData->src_dirs_deg[i][0]));
                    pData->src_dirs_xyz[i][2] = sinf(DEG2RAD(pData->src_dirs_deg[i][1]));
                    pData->recalc_hrtf_interpFLAG[i] = 1;
                }
                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, nSources, 3, 3, 1.0f,
                            (float*)(pData->src_dirs_xyz), 3,
                            (float*)Rxyz, 3, 0.0f,
                            (float*)(pData->src_dirs_rot_xyz), 3);
                for(i=0; i<nSources; i++){
                    hypotxy = sqrtf(powf(pData->src_dirs_rot_xyz[i][0], 2.0f) + powf(pData->src_dirs_rot_xyz[i][1], 2.0f));
                    pData->src_dirs_rot_deg[i][0] = RAD2DEG(atan2f(pData->src_dirs_rot_xyz[i][1], pData->src_dirs_rot_xyz[i][0]));
                    pData->src_dirs_rot_deg[i][1] = RAD2DEG(atan2f(pData->src_dirs_rot_xyz[i][2], hypotxy));
                }
                pData->recalc_M_rotFLAG = 0;
            }
         
            /* interpolate hrtfs and apply to each source */
            memset(pData->outputframeTF, 0, HYBRID_BANDS*NUM_EARS*TIME_SLOTS * sizeof(float_complex));
            for (ch = 0; ch < nSources; ch++) {
                if(pData->recalc_hrtf_interpFLAG[ch]){
                    if(enableRotation)
                        binauraliser_interpHRTFs(hBin, pData->src_dirs_rot_deg[ch][0], pData->src_dirs_rot_deg[ch][1], pData->hrtf_interp[ch]);
                    else
                        binauraliser_interpHRTFs(hBin, pData->src_dirs_deg[ch][0], pData->src_dirs_deg[ch][1], pData->hrtf_interp[ch]);
                    pData->recalc_hrtf_interpFLAG[ch] = 0;
                }
                for (band = 0; band < HYBRID_BANDS; band++)
                    for (ear = 0; ear < NUM_EARS; ear++)
                        for (t = 0; t < TIME_SLOTS; t++)
                            pData->outputframeTF[band][ear][t] = ccaddf(pData->outputframeTF[band][ear][t], ccmulf(pData->inputframeTF[band][ch][t], pData->hrtf_interp[ch][band][ear]));
            }
            
            /* scale by number of sources */
            for (band = 0; band < HYBRID_BANDS; band++)
                for (ear = 0; ear < NUM_EARS; ear++)
                    for (t = 0; t < TIME_SLOTS; t++)
                        pData->outputframeTF[band][ear][t] = crmulf(pData->outputframeTF[band][ear][t], 1.0f/sqrtf((float)nSources));
        }
        else
            memset(pData->outputframeTF, 0, HYBRID_BANDS*NUM_EARS*TIME_SLOTS*sizeof(float_complex));
        
        /* inverse-TFT */
        for (t = 0; t < TIME_SLOTS; t++) {
            for (band = 0; band < HYBRID_BANDS; band++) {
                for (ch = 0; ch < NUM_EARS; ch++) {
                    pData->STFTOutputFrameTF[ch].re[band] = crealf(pData->outputframeTF[band][ch][t]);
                    pData->STFTOutputFrameTF[ch].im[band] = cimagf(pData->outputframeTF[band][ch][t]);
                }
            }
            afSTFTinverse(pData->hSTFT, pData->STFTOutputFrameTF, pData->tempHopFrameTD);
            for (ch = 0; ch < MIN(NUM_EARS, nOutputs); ch++)
                utility_svvcopy(pData->tempHopFrameTD[ch], HOP_SIZE, &(outputs[ch][t* HOP_SIZE]));
            for (; ch < nOutputs; ch++)
                memset(&(outputs[ch][t* HOP_SIZE]), 0, HOP_SIZE*sizeof(float));
        }
    }
    else{
        for (ch=0; ch < nOutputs; ch++)
            memset(outputs[ch],0, FRAME_SIZE*sizeof(float));
    }
}

/* Set Functions */

void binauraliser_refreshSettings(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    pData->reInitHRTFsAndGainTables = 1;
    pData->reInitTFT = 1;
}

void binauraliser_checkReInit(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);

    /* reinitialise if needed */
    if (pData->reInitTFT==1) {
        pData->reInitTFT = 2;
        binauraliser_initTFT(hBin);
        pData->reInitTFT = 0;
    }
    if (pData->reInitHRTFsAndGainTables==1) {
        pData->reInitHRTFsAndGainTables = 2;
        binauraliser_initHRTFsAndGainTables(hBin);
        pData->reInitHRTFsAndGainTables = 0;
    }
}

void binauraliser_setSourceAzi_deg(void* const hBin, int index, float newAzi_deg)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    if(newAzi_deg>180.0f)
        newAzi_deg = -360.0f + newAzi_deg;
    newAzi_deg = MAX(newAzi_deg, -180.0f);
    newAzi_deg = MIN(newAzi_deg, 180.0f);
    pData->recalc_hrtf_interpFLAG[index] = 1;
    pData->src_dirs_deg[index][0] = newAzi_deg;
    pData->recalc_M_rotFLAG = 1;
}

void binauraliser_setSourceElev_deg(void* const hBin, int index, float newElev_deg)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    newElev_deg = MAX(newElev_deg, -90.0f);
    newElev_deg = MIN(newElev_deg, 90.0f);  
    pData->recalc_hrtf_interpFLAG[index] = 1;
    pData->src_dirs_deg[index][1] = newElev_deg;
    pData->recalc_M_rotFLAG = 1;
}

void binauraliser_setNumSources(void* const hBin, int new_nSources)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    pData->new_nSources = new_nSources > MAX_NUM_INPUTS ? MAX_NUM_INPUTS : new_nSources;
    if(pData->nSources != pData->new_nSources)
        pData->reInitTFT = 1;
    pData->recalc_M_rotFLAG = 1;
}

void binauraliser_setUseDefaultHRIRsflag(void* const hBin, int newState)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    if((!pData->useDefaultHRIRsFLAG) && (newState)){
        pData->useDefaultHRIRsFLAG = newState;
        pData->reInitHRTFsAndGainTables = 1;
    }
}

void binauraliser_setSofaFilePath(void* const hBin, const char* path)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    
    pData->sofa_filepath = malloc(strlen(path) + 1);
    strcpy(pData->sofa_filepath, path);
    pData->useDefaultHRIRsFLAG = 0;
    pData->reInitHRTFsAndGainTables = 1; 
}

void binauraliser_setInputConfigPreset(void* const hBin, int newPresetID)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    int ch;
    
    binauraliser_loadPreset(newPresetID, pData->src_dirs_deg, &(pData->new_nSources), &(pData->input_nDims));
    if(pData->nSources != pData->new_nSources)
        pData->reInitTFT = 1;
    for(ch=0; ch<MAX_NUM_INPUTS; ch++)
        pData->recalc_hrtf_interpFLAG[ch] = 1;
}

void binauraliser_setEnableRotation(void* const hBin, int newState)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    int ch;

    pData->enableRotation = newState;
    if(!pData->enableRotation)
        for (ch = 0; ch<MAX_NUM_INPUTS; ch++) 
            pData->recalc_hrtf_interpFLAG[ch] = 1;
}

void binauraliser_setYaw(void  * const hBin, float newYaw)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    pData->yaw = pData->bFlipYaw == 1 ? -DEG2RAD(newYaw) : DEG2RAD(newYaw);
    pData->recalc_M_rotFLAG = 1;
}

void binauraliser_setPitch(void* const hBin, float newPitch)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    pData->pitch = pData->bFlipPitch == 1 ? -DEG2RAD(newPitch) : DEG2RAD(newPitch);
    pData->recalc_M_rotFLAG = 1;
}

void binauraliser_setRoll(void* const hBin, float newRoll)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    pData->roll = pData->bFlipRoll == 1 ? -DEG2RAD(newRoll) : DEG2RAD(newRoll);
    pData->recalc_M_rotFLAG = 1;
}

void binauraliser_setFlipYaw(void* const hBin, int newState)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    if(newState !=pData->bFlipYaw ){
        pData->bFlipYaw = newState;
        binauraliser_setYaw(hBin, -binauraliser_getYaw(hBin));
    }
}

void binauraliser_setFlipPitch(void* const hBin, int newState)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    if(newState !=pData->bFlipPitch ){
        pData->bFlipPitch = newState;
        binauraliser_setPitch(hBin, -binauraliser_getPitch(hBin));
    }
}

void binauraliser_setFlipRoll(void* const hBin, int newState)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    if(newState !=pData->bFlipRoll ){
        pData->bFlipRoll = newState;
        binauraliser_setRoll(hBin, -binauraliser_getRoll(hBin));
    }
}

void binauraliser_setRPYflag(void* const hBin, int newState)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    pData->useRollPitchYawFlag = newState;
}

void binauraliser_setInterpMode(void* const hBin, int newMode)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    pData->interpMode = newMode;
}

/* Get Functions */

float binauraliser_getSourceAzi_deg(void* const hBin, int index)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->src_dirs_deg[index][0];
}

float binauraliser_getSourceElev_deg(void* const hBin, int index)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->src_dirs_deg[index][1];
}

int binauraliser_getNumSources(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->new_nSources;
}

int binauraliser_getMaxNumSources()
{
    return MAX_NUM_INPUTS;
}

int binauraliser_getNumEars(void)
{
    return NUM_EARS;
}

int binauraliser_getNDirs(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->N_hrir_dirs;
}

int binauraliser_getNTriangles(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->nTriangles;
}

float binauraliser_getHRIRAzi_deg(void* const hBin, int index)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    if(pData->hrir_dirs_deg!=NULL)
        return pData->hrir_dirs_deg[index*2+0];
    else
        return 0.0f;
}

float binauraliser_getHRIRElev_deg(void* const hBin, int index)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    if(pData->hrir_dirs_deg!=NULL)
        return pData->hrir_dirs_deg[index*2+1];
    else
        return 0.0f;
}

int binauraliser_getHRIRlength(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->hrir_len;
}

int binauraliser_getHRIRsamplerate(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->hrir_fs;
}

int binauraliser_getUseDefaultHRIRsflag(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->useDefaultHRIRsFLAG;
}

char* binauraliser_getSofaFilePath(void* const hCmp)
{
    binauraliser_data *pData = (binauraliser_data*)(hCmp);
    if(pData->sofa_filepath!=NULL)
        return pData->sofa_filepath;
    else
        return "no_file";
}

int binauraliser_getDAWsamplerate(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->fs;
}

int binauraliser_getEnableRotation(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->enableRotation;
}

float binauraliser_getYaw(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->bFlipYaw == 1 ? -RAD2DEG(pData->yaw) : RAD2DEG(pData->yaw);
}

float binauraliser_getPitch(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->bFlipPitch == 1 ? -RAD2DEG(pData->pitch) : RAD2DEG(pData->pitch);
}

float binauraliser_getRoll(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->bFlipRoll == 1 ? -RAD2DEG(pData->roll) : RAD2DEG(pData->roll);
}

int binauraliser_getFlipYaw(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->bFlipYaw;
}

int binauraliser_getFlipPitch(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->bFlipPitch;
}

int binauraliser_getFlipRoll(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->bFlipRoll;
}

int binauraliser_getRPYflag(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return pData->useRollPitchYawFlag;
}

int binauraliser_getInterpMode(void* const hBin)
{
    binauraliser_data *pData = (binauraliser_data*)(hBin);
    return (int)pData->interpMode;
}

int binauraliser_getProcessingDelay()
{
    return 12*HOP_SIZE;
}
 
    
    
