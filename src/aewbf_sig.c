/*
 *  ======== ae_sig.c ========
 *  Sigrand's implementation of the AE algorithm.
 *
 *  This file contains an implementation of the IALG interface
 *  required by xDAIS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aewb_xdm.h>
//#include <osa.h>
//#include <drv_motor.h>
#include "iaewbf_sig.h"
#include "aewbf_sig.h"
#include "alg_aewb.h"

extern IAEWBF_Fxns IAEWBF_SIG_IALG;
extern int gIRCut, gFlicker;
int IRcutClose = 1; //IR-cut 1-open, 0 - close
int FPShigh = 1; //FPS 1-high, 0 - low
int GN[3] = { -8, 0, 8}, wbup = 0;
Uint32 RGN = 1<<30, BGN = 1<<30;
int GNR[3] = { 32, 0, -32};
int GNB[3] = { 32, 0, -32};
int rmin[2] = {0, 0}, bmin[2] = {0, 0};
int RR=0, GG=0, BB=0, RR1, GG1, BB1;
int bw = 0;

extern int gHDR;
extern int DEBUG;
extern ALG_AewbfObj gSIG_Obj;
//#define HISTTH 30

extern int DRV_imgsMotorStep(int type, int direction, int steps);
extern int OSA_fileReadFile(const char *fileName, void *addr, size_t readSize, size_t *actualReadSize);
extern int OSA_fileWriteFile(const char *fileName, const void *addr, size_t size);
extern void OSA_waitMsecs(Uint32 msecs);

Int32  frame_count = 0, leave_frames = 5, exp_on = 1, history = 0;

//For HDR mode
int A = 256 - ZERO, B = 2240 - ZERO, g1 = 2, g2 = 5;
Uint32 lut[ALG_SENSOR_BITS], lut1[ALG_SENSOR_BITS<<3];
extern Uint32 gm003[], gm005[];



#define __DEBUG
#ifdef __DEBUG
#define AE_DEBUG_PRINTS
#define dprintf printf
#else
#define dprintf
#endif

#define IALGFXNS  \
    &IAEWBF_SIG_IALG,        /* module ID */                         \
    NULL,                /* activate */                          \
    IAEWBF_SIG_alloc,        /* alloc */                             \
    NULL,                /* control (NULL => no control ops) */  \
    NULL,                /* deactivate */                        \
    IAEWBF_SIG_free,         /* free */                              \
    IAEWBF_SIG_init,         /* init */                              \
    NULL,                /* moved */                             \
    NULL                 /* numAlloc (NULL => IALG_MAXMEMRECS) */

/*
 *  ======== AE_SIG_IAE ========
 *  This structure defines Sigrand's implementation of the IAE interface
 *  for the AE_SIG module.
 */
IAEWBF_Fxns IAEWBF_SIG = {    /* module_vendor_interface */
                          {IALGFXNS},
                          IAEWBF_SIG_process,
                          IAEWBF_SIG_control,
                     };

/*
 *  ======== AE_SIG_IALG ========
 *  This structure defines Sigrand's implementation of the IALG interface
 *  for the AE_SIG module.
 */
IAEWBF_Fxns IAEWBF_SIG_IALG = {      /* module_vendor_interface */
                               IALGFXNS
                        };

/*
 *  ======== AE_SIG_alloc ========
 */
Int IAEWBF_SIG_alloc(const IALG_Params *algParams,
                 IALG_Fxns **pf, IALG_MemRec memTab[])
{
    IAEWBF_Params *params = (IAEWBF_Params *)algParams;
    int numTabs = 1;

    /* Request memory for my object */
    memTab[0].size = sizeof(IAEWBF_SIG_Obj);
    memTab[0].alignment = 0;
    memTab[0].space = IALG_EXTERNAL;
    memTab[0].attrs = IALG_PERSIST;
    return (numTabs);
}

/*
 *  ======== AE_SIG_free ========
 */
Int IAEWBF_SIG_free(IALG_Handle handle, IALG_MemRec memTab[])
{
    IAEWBF_SIG_Obj *h = (IAEWBF_SIG_Obj *)handle;
    int numTabs = 1;
    /* Request memory for my object */
    memTab[0].size = sizeof(IAEWBF_SIG_Obj);
    memTab[0].alignment = 0;
    memTab[0].space = IALG_EXTERNAL;
    memTab[0].attrs = IALG_PERSIST;
    return (numTabs);
}

/*
 *  ======== AE_SIG_initObj ========
 */
Int IAEWBF_SIG_init(IALG_Handle handle,
                const IALG_MemRec memTab[], IALG_Handle p,
                const IALG_Params *algParams)
{
    return (IAES_EOK);
}

int add_history(IAEWBF_Param *p)
{
    int diff = 0;
    p->Avrg += p->New;
    p->Avrg -= p->Hist[p->HistC];
    if(p->New) diff = abs(p->Hist[p->HistC] - p->New)*100/p->New;
    p->Hist[p->HistC] = p->New;
    p->HistC = (p->HistC == (HISTORY - 1)) ? 0 : p->HistC + 1;
    p->NewA = (history < HISTORY) ? p->Avrg/history : p->Avrg/HISTORY;
    return diff;
}

/*
 *  ======== AE_SIG_process ========
 */

XDAS_Int32 IAEWBF_SIG_process(IAEWBF_Handle handle, IAEWBF_InArgs *inArgs, IAEWBF_OutArgs *outArgs)
{
    IAEWBF_SIG_Obj *hn = (IAEWBF_SIG_Obj *)handle;

    int i, j, st, tmp=0;
    Uint32 w = hn->w, h = hn->h, cn = 0;
    int sz = w*h,  sz4 = sz*4, sz3 = sz*3, sz2 = sz3>>1;
    Uint32 r, g, b;
    Uint32  Y=0, ns = 3, GR[ns], GB[ns];
    Uint16 *box = hn->box;
    Uint32 hsz = ALG_SENSOR_BITS;
    Uint32 minr, minb, min, up;
    Uint32 hist[hsz], rgb[3][hsz];
    //Uint32 upth = sz3/7, mid, uphalf, len;
    static int frames = 0;

    //HDR mode
    int ga = (1<<g1)-1, gb = (1<<(g2-g1))-1;
    int Ai = A, Bi = (((B - A)<<g1) + A);
    int A1 = A>>3, B1 = B>>3, A1h = A1>>1, Ah = 0;
    int An, Bn;
    int sum, diff, rgb_min[3], rgb_max[3];

    //if(!(frames%leave_frames)  && frames > 5){

    for(j=0; j < ns; j++) { GR[j] = 0; GB[j] = 0; }

    if(gHDR){
        //Clear histogram
        memset(rgb[0], 0, sizeof(Uint32)*hsz);
        memset(rgb[1], 0, sizeof(Uint32)*hsz);
        memset(rgb[2], 0, sizeof(Uint32)*hsz);

        for(i=0; i < sz4; i+=4) {
            //AE and WB
            r = box[i+2]>>5;
            g = box[i+1]>>5;
            b = box[i  ]>>5;

            rgb[0][r]++;
            rgb[1][g]++;
            rgb[2][b]++;

            r = lut[r];
            g = lut[g];
            b = lut[b];
            for(j=0; j < ns; j++) {
                GB[j] += abs(g - (b*(hn->Bgain.New + GN[j])>>9));
                GR[j] += abs(g - (r*(hn->Rgain.New + GN[j])>>9));
            }
        }

        //White balance algorithm
        min = GR[0]; minr = 0;
        for(j=1; j < ns; j++){
            if(GR[j] < min) { min = GR[j]; minr = j; }
        }
        min = GB[0]; minb = 0;
        for(j=1; j < ns; j++){
            if(GB[j] < min) { min = GB[j]; minb = j; }
        }
        if(minr != 1) hn->Rgain.New = hn->Rgain.New + GN[minr];
        if(minb != 1) hn->Bgain.New = hn->Bgain.New + GN[minb];

        if(GN[0] > 4 && !wbup) { GN[0] /= 2; GN[2] /= 2; }
        else if(GN[0] == 4  ) { wbup = 1; GN[0] *= 2; GN[2] *= 2;}
        else if(GN[0] <  16 && wbup) { GN[0] *= 2; GN[2] *= 2; }
        else if(GN[0] == 16) { wbup = 0; GN[0] /= 2; GN[2] /= 2;}


        //Find min and max of each color
        for(j=0; j < 3; j++){
            sum = 0;
            for(i=0;  sum < hn->SatTh; i++) sum += rgb[j][i];
            rgb_min[j] = i;
            sum = 0;
            for(i=hsz-1;  sum < hn->SatTh; i--) sum += rgb[j][i];
            rgb_max[j] = i;
        }

        //Make gamma table for each color
        int vl0, vl1, st[3], st1, min_g[3], max_g[3];
        int min1, max1, min2, minn, maxn;

        //min1 = (hn->Hmin.New<<6)/hn->Rgain.New;
        //max1 = (hn->Hmax.New<<6)/hn->Rgain.New;
        //min1 = hn->Hmin.New>>3;
        //max1 = hn->Hmax.New>>3;

        //Calculate new min
        for(j=0; j < 3; j++){
            if(j != 1){
                if(j == 0) r = lut[rgb_min[j]]*hn->Rgain.New>>9;
                if(j == 2) r = lut[rgb_min[j]]*hn->Bgain.New>>9;
                if(r > Ai){
                    if(r > Bi) r = ((r-Bi)>>g2) + Bi;
                    else r = ((r-Ai)>>g1) + Ai;
                }
                min_g[j] = r >>3;
            } else {
                min_g[j] = rgb_min[j];
            }
        }
        //Calculate new max
        for(j=0; j < 3; j++){
            if(j != 1){
                if(j == 0) r = lut[rgb_max[j]]*hn->Rgain.New>>9;
                if(j == 2) r = lut[rgb_max[j]]*hn->Bgain.New>>9;
                if(r > Ai){
                    if(r > Bi) r = ((r-Bi)>>g2) + Bi;
                    else r = ((r-Ai)>>g1) + Ai;
                }
                max_g[j] = r >>3;
            } else {
                max_g[j] = rgb_max[j];
            }
        }

        for(j=0; j < 3; j++) {
            if(max_g[j] - min_g[j]) st[j] = (1<<20)/(max_g[j] - min_g[j]);
        }

        min2 = min1<<3;
        minn = lut1[min1<<3];
        maxn = lut1[max1<<3];
        //minn2 = lut1[min1<<3];
        st1 = (1<<23)/(maxn - minn);

        printf("A1 = %d B1 = %d Rgain = %d Bgain  = %d\n", A1, B1, hn->Rgain.New, hn->Bgain.New);
        printf("R rgb_min = %d rgb_max = %d min_g = %d max_g = %d st = %d\n", rgb_min[0], rgb_max[0], min_g[0], max_g[0], st[0]);
        printf("G rgb_min = %d rgb_max = %d min_g = %d max_g = %d st = %d\n", rgb_min[1], rgb_max[1], min_g[1], max_g[1], st[1]);
        printf("B rgb_min = %d rgb_max = %d min_g = %d max_g = %d st = %d\n", rgb_min[2], rgb_max[2], min_g[2], max_g[2], st[2]);
        //Color gamma table
        for(j=0; j < 3; j++){
            vl0 = 0;
            for(i=0; i < hsz; i++){
                if(j != 1){
                    if(j == 0) r = lut[i]*hn->Rgain.New>>9;
                    if(j == 2) r = lut[i]*hn->Bgain.New>>9;
                    if(r > Ai){
                        if(r > Bi) r = ((r-Bi)>>g2) + Bi;
                        else r = ((r-Ai)>>g1) + Ai;
                    }
                    r >>= 3;
                } else {
                    r = i;
                }

                if(i < rgb_min[j]) vl1 = 0;
                else if(i >= rgb_min[j]  && i < rgb_max[j]) {
                    vl1 = gm005[(r - min_g[j])*st[j]>>11];
                    //vl1 = (r - min_g[j])*st[j]>>10;
                    //vl1 = (lut1[r] - minn)*st1>>13;
                }
                else vl1 = 1023;
                hn->RGB[j][i] = (vl0<<10) | (vl1 - vl0);
                vl0 = vl1;
            }
        }

        /*
             for(i=0; i < 512; i++){
                 if(i == min1) printf("min1\n");
                 else if(i == max1) printf("max1\n");
                 else if(i == A1) printf("A1\n");
                 else if(i == B1) printf("B1\n");

                 printf("%3d R %4d  %4d  G %4d  %4d  B %4d  %4d lut = %d lut1 = %d\n",
                        i, hn->RGB[0][i]>>10, hn->RGB[0][i]&1023, hn->RGB[1][i]>>10, hn->RGB[1][i]&1023, hn->RGB[2][i]>>10, hn->RGB[2][i]&1023, lut[i], lut1[i<<3]);
             }
             */
    } else {
        memset(hist, 0, sizeof(Uint32)*hsz);

        for(i=0; i < sz4; i+=4) {
            //AE and WB
            //printf(" %3d %3d r = %d g = %d b = %d\n", (i>>2)/w, (i>>2)%w, box[i+2], box[i+1], box[i  ]);
            r = box[i+2]>>2;
            g = box[i+1]>>2;
            b = box[i  ]>>2;

            RR += r; GG += g; BB += b;
            Y += ((117*b + 601*g + 306*r)>>10);

            hist[r>>3]++;
            hist[g>>3]++;
            hist[b>>3]++;
            /*
            tmp = r*hn->Rgain.Old>>12;
            tmp = tmp > 511 ? 511 : tmp;
            hist[tmp]++;
            hist[g>>3]++;
            tmp = r*hn->Bgain.Old>>12;
            tmp = tmp > 511 ? 511 : tmp;
            hist[tmp]++;
            */
            for(j=0; j < ns; j++) {
                GB[j] += abs(g - (b*(512 + GN[j])>>9));
                GR[j] += abs(g - (r*(512 + GN[j])>>9));
                //GR[j] += abs(g - (r*(hn->Rgain.Old + GN[j])>>9));
                //GB[j] += abs(g - (b*(hn->Bgain.Old + GN[j])>>9));
            }
        }

        Y = Y/sz;
        hn->Y.New = Y;

        RR = RR/sz; GG = GG/sz; BB = BB/sz;
        printf("R = %d G = %d B = %d\n", RR, GG, BB);
        printf("R = %d G = %d B = %d\n", RR1, GG1, BB1);
        if(RR1 && GG1 && BB1) {
            printf("GR = %d GG = %d GB = %d\n", (RR1-RR)*512/RR1, (GG1-GG)*512/GG1, (BB1-BB)*512/BB1);
        }
        RR1 = RR; GG1 = GG; BB1 = BB;


        //Make integral histogram
        for(i=1; i < hsz; i++) hist[i] += hist[i-1];
        //Find min in color histogram
        for(i=0;  hist[i] < hn->SatTh; i++) ;
        hn->Hmin.New = i;
        //Find max in color histogram
        for(i=hsz-1; (sz3 - hist[i]) < hn->SatTh; i--);
        hn->Hmax.New = i+1;

        //Averaging
        history++;
        add_history(&hn->Hmax);
        hn->Hmax.New = hn->Hmax.New<<3;
        hn->Hmax.NewA = hn->Hmax.NewA<<3;

        add_history(&hn->Hmin);
        hn->Hmin.New = hn->Hmin.New<<3;
        hn->Hmin.NewA = hn->Hmin.NewA<<3;

        diff = add_history(&hn->Y);

        //White balance oldalgorithm
        if(frames > 3){
            if(0){
            //if(IRcutClose){
                min = GR[0]; minr = 0;
                for(j=1; j < ns; j++){
                    if(GR[j] < min) { min = GR[j]; minr = j; }
                }
                min = GB[0]; minb = 0;
                for(j=1; j < ns; j++){
                    if(GB[j] < min) { min = GB[j]; minb = j; }
                }

                printf("GR = %d GG = %d GB = %d\n", (GN[minr]*hn->Rgain.Old>>9), 1, (GN[minb]*hn->Bgain.Old>>9));
                printf("Rgain.Old = %d, Bgain.Old = %d\n", hn->Rgain.Old*(512 + GN[minr])>>9, hn->Bgain.Old*(512 + GN[minb])>>9);

                //if(minr != 1) hn->Rgain.New = hn->Rgain.Old + GN[minr];
                //if(minb != 1) hn->Bgain.New = hn->Bgain.Old + GN[minb];
                printf("RGN = %d, BGN = %d\n", RGN, BGN);

                //if(GR[minr] < RGN) {
                    if(minr != 1) hn->Rgain.New = hn->Rgain.Old + (GN[minr]*hn->Rgain.Old>>9);
                   // RGN = GR[minr];
                //}
                //if(GB[minb] < BGN) {
                    if(minb != 1) hn->Bgain.New = hn->Bgain.Old + (GN[minb]*hn->Bgain.Old>>9);
                    //BGN = GB[minb];
                //}

                printf("Rgain.New = %d, Bgain.New = %d\n", hn->Rgain.New, hn->Bgain.New);
                /*
            if(GN[0] > 4 && !wbup) { GN[0] /= 2; GN[2] /= 2; }
            else if(GN[0] == 4  ) { wbup = 1; GN[0] *= 2; GN[2] *= 2;}
            else if(GN[0] <  16 && wbup) { GN[0] *= 2; GN[2] *= 2; }
            else if(GN[0] == 16) { wbup = 0; GN[0] /= 2; GN[2] /= 2;}
            */
            } else {
                //Night AW mode
                if(!(bw%3)){
                    if(RR) hn->Rgain.New = GG*hn->Rgain.Old/RR;
                    if(BB) hn->Bgain.New = GG*hn->Bgain.Old/BB;
                }
                bw++;
            }


            //White balance new algorithm
            /*
        rmin[0] = GR[0]; minr = 0;
        for(j=1; j < ns; j++){
            if(GR[j] < rmin[0]) { rmin[0] = GR[j]; minr = j; }
        }
        rmin[0] = GNR[minr];
        //Change direction
        if(rmin[0] != rmin[1] || (!rmin[0] && !rmin[1])) {
            GNR[0] /= 2; GNR[2] /= 2;
            rmin[0] = GNR[minr];
        }
        rmin[1] = rmin[0];

        bmin[0] = GB[0]; minb = 0;
        for(j=1; j < ns; j++){
            if(GB[j] < bmin[0]) { bmin[0] = GB[j]; minb = j; }
        }
        bmin[0] = GNB[minb];
        //Change direction
        if(bmin[0] != bmin[1] || (!bmin[0] && !bmin[1])) {
            GNB[0] /= 2; GNB[2] /= 2;
            bmin[0] = GNB[minb];
        }
        bmin[1] = bmin[0];

        //if(minr != 1) hn->Rgain.New = hn->Rgain.Old + (GN[minr]<<9)/hn->Rgain.Old;
        //if(minb != 1) hn->Bgain.New = hn->Bgain.Old + (GN[minb]<<9)/hn->Bgain.Old;
        if(minr != 1) hn->Rgain.New = hn->Rgain.Old + (GNR[minr]*hn->Rgain.Old>>9);
        if(minb != 1) hn->Bgain.New = hn->Bgain.Old + (GNB[minb]*hn->Bgain.Old>>9);
        */

            printf("GR[0] = %d GR[1] = %d GR[2] = %d rmin[0] = %d rmin[1] = %d \n",
                   GR[0], GR[1], GR[2], rmin[0], rmin[1]);
            printf("GB[0] = %d GB[1] = %d GB[2] = %d bmin[0] = %d bmin[1] = %d \n",
                   GB[0], GB[1], GB[2], bmin[0], bmin[1]);
            printf("diff = %d\n", diff);


            //Check range
            hn->Rgain.New = hn->Rgain.New > hn->Rgain.Range.max ? hn->Rgain.Range.max : hn->Rgain.New;
            hn->Rgain.New = hn->Rgain.New < hn->Rgain.Range.min ? hn->Rgain.Range.min : hn->Rgain.New;
            hn->Bgain.New = hn->Bgain.New > hn->Bgain.Range.max ? hn->Bgain.Range.max : hn->Bgain.New;
            hn->Bgain.New = hn->Bgain.New < hn->Bgain.Range.min ? hn->Bgain.Range.min : hn->Bgain.New;

            //add_history(&hn->Rgain);
            //add_history(&hn->Bgain);
        }

        st = hn->YAE;
        if(gFlicker == VIDEO_NONE){
            if(hn->Y.New) tmp = hn->Y.New > st ? hn->Y.New*100/st : st*100/hn->Y.New;
            if(tmp > 200){
                //if(hn->Y.New) hn->Exp.New = hn->Exp.Old*(hn->Y.New + st)/(hn->Y.New*2);
                if(hn->Y.New) hn->Exp.New = hn->Exp.Old*(hn->Y.New*2 + st)/(hn->Y.New*3);
            } else if(tmp > 110 && tmp <= 200) {
                if(hn->Y.New > st) hn->Exp.New = hn->Exp.Old*99/100;
                else hn->Exp.New = hn->Exp.Old*100/99;
                //RGN = 1<<30, BGN = 1<<30; //Start WB
            }
            if(hn->Exp.New > hn->Exp.Range.max)  hn->Exp.New = hn->Exp.Range.max;
        } else {
            if(diff > 10){
                //RGN = 1<<30, BGN = 1<<30; //Start WB
            }
        }

        //Change the offset
        hn->Offset.New = hn->Hmin.NewA;

        //Change gain
        if(hn->Y.NewA - hn->Offset.New) hn->GIFIF.New = ((hn->HmaxTh>>2)<<9)/(hn->Y.NewA - hn->Offset.New);
        up = hn->Hmax.NewA*hn->GIFIF.New>>9;
        //printf("up = %d hn->HmaxTh = %d hn->GIFIF.New = %d", up, hn->HmaxTh, hn->GIFIF.New);
        if((up < hn->HmaxTh) && (hn->Y.NewA - hn->Offset.New))
            if(hn->Y.NewA - hn->Offset.New) hn->GIFIF.New = (((hn->HmaxTh*2 - up)>>2)<<9)/(hn->Y.NewA - hn->Offset.New);

        //If not enough IFIF gain add rgb2rgb gain

        if(hn->GIFIF.New > hn->GIFIF.Range.max){
            hn->Grgb2rgb.New = (hn->GIFIF.New*256/hn->GIFIF.Range.max);
        } else hn->Grgb2rgb.New = 256;


        //Check gain range
        hn->GIFIF.New = hn->GIFIF.New > hn->GIFIF.Range.max ? hn->GIFIF.Range.max : hn->GIFIF.New;
        hn->GIFIF.New = hn->GIFIF.New < hn->GIFIF.Range.min ? hn->GIFIF.Range.min : hn->GIFIF.New;
        hn->Grgb2rgb.New = hn->Grgb2rgb.New > hn->Grgb2rgb.Range.max ? hn->Grgb2rgb.Range.max : hn->Grgb2rgb.New;
        hn->Grgb2rgb.New = hn->Grgb2rgb.New < hn->Grgb2rgb.Range.min ? hn->Grgb2rgb.Range.min : hn->Grgb2rgb.New;


        //Check Low light condition
        //First down fps
        if ( FPShigh == 1 && IRcutClose == 1 && Y < 100 ) {
            frame_count += leave_frames;
            if (frame_count > 300) {
                FPShigh = 0;
                frame_count = 0;
                //if(DEBUG) dprintf("FPS DOWN : YN %d YO %d \n", hn->Y.New, hn->Y.Old);
            }
        }
        if ( FPShigh == 0 && IRcutClose == 1 && Y > 220) {
            frame_count += leave_frames;
            if (frame_count > 300) {
                FPShigh = 1;
                frame_count = 0;
                //if(DEBUG) dprintf("FPS UP : YN %d YO %d \n", hn->Y.New, hn->Y.Old);
            }
        }
        hn->Y.Old = hn->Y.New;
        //hn->Rgain.Old = hn->Rgain.New;
    }

    //Second open IR-cut
    if(gIRCut == ALG_IRCUT_AUTO){
        //Got to night mode
        if ( FPShigh == 0 && IRcutClose == 1 && Y < 100) {
            frame_count += leave_frames;
            if (frame_count > 300) {
                IRcutClose = 0;
                frame_count = 0;
                //if(DEBUG) dprintf("IR OPEN : YN %d YO %d \n", hn->Y.New, hn->Y.Old);
            }
        }
        //Come back to day mode
        if ( FPShigh == 0 && IRcutClose == 0 && Y > 240) {
            frame_count += leave_frames;
            if (frame_count > 300) {
                IRcutClose = 1;
                frame_count = 0;
                //if(DEBUG) dprintf("IR CLOSE : YN %d YO %d \n", hn->Y.New, hn->Y.Old);
            }
        }
    }

    frames++;
    return(IAES_EOK);
}

/*
 *  ======== AE_SIG_control ========
 */
XDAS_Int32 IAEWBF_SIG_control(IAEWBF_Handle handle, IAEWBF_Cmd id,
                              IAEWBF_DynamicParams *params, IAEWBF_Status *status)
{
    return IAES_EOK;
}

enum choise {autofocus, zoomchange, af_end};

void AF_SIG_process(int *afEnable)
{
    IAEWBF_SIG_Obj *hn = (IAEWBF_SIG_Obj *)(IAEWBF_Handle)gSIG_Obj.handle_aewbf;

    Int32 w = hn->w, h = hn->h;
    Int32 sz = w*h,  sz4 = sz*4;
    Int32 i, focus_val = 0;
    Uint16 *box = hn->box;
    Int32 MAX_STEP = 220;
    int status;
    char zoomvalue[4];
    int readsize, zoom, shift = 0;
    static int frames = 0, numframes = 0;
    static int zoomstep = 0;

    static Bool zoomdir = 0;
    static Bool focusdir = 1;
    static Bool dir = 0;

    static int maxstep, maxN, maxO;
    static int step = autofocus;

    static int firststep = 0;
    static int stepcnt = 0;
    static int AFMax = 0;


    for(i=5; i < sz4; i+=4) {
        focus_val += abs(box[i] - box[i-4]);
    }
    //focus_val = (focus_val>>2)/sz;
    //if(numframes)

    if (!numframes) {
        status = OSA_fileReadFile("/var/run/zoom", zoomvalue, sizeof(zoomvalue), (size_t*)&readsize);

        if(status!=OSA_SOK) {
            OSA_printf("AF: error read from file\n");
            status = 0;
            zoom = 0;
        } else {
            zoom = atoi(zoomvalue);
        }

        if (zoom > 0 && zoom < 2000) {
            if (zoom > 1000) {
                zoomdir = TRUE;
                zoomstep = zoom - 1000;
            } else {
                zoomdir = FALSE;
                zoomstep = zoom;
            }
            dir = zoomdir;
            if (zoomstep) {
                step = zoomchange;
                //if (zoomstep > 10) zoomstep += 10;
                //else zoomstep *= 2;
                DRV_imgsMotorStep(1, dir, zoomstep>>1);
                sprintf(zoomvalue, "%04d", 0);
                status = OSA_fileWriteFile("/var/run/zoom", zoomvalue, sizeof(zoomvalue));
                if(status!=OSA_SOK) {
                    OSA_printf("AF: error write in file\n");
                }
            }
        }
        //numframes = 0;
        if(step != zoomchange) step = autofocus;
        AFMax = 0;
        maxO = 0;
        OSA_printf("step  = %d zoomstep = %d \n",step, zoomstep);
    }
    //step = coarse;
    numframes++;

    switch(step) {
    case zoomchange:
        //if (!numframes) {
        //    DRV_imgsMotorStep(1, dir, zoomstep>>1);
        //    maxO = focus_val;
        //} else {
            stepcnt ++;
            DRV_imgsMotorStep(1, dir, 1); // 2 is optimal step for AF
            if(stepcnt > shift){
                //maxO = 0;
                if(focus_val > AFMax){
                    maxN = focus_val;
                    if(maxN < maxO){
                        DRV_imgsMotorStep(1, !dir, 6);
                        step = af_end;
                        stepcnt = 0;
                    }
                    OSA_printf("zoomchange stepcnt = %d focus_val = %d maxN = %d maxO = %d focusdir = %d step = %d\n",
                               stepcnt, focus_val, maxN, maxO, focusdir, step);
                    maxO = maxN;
                }
            }
            if ((stepcnt - shift) > MAX_STEP) {
                step = af_end;
                stepcnt = 0;
            }
        //}
        break;
    case autofocus:
        if (firststep < 20) { // set start position and stabilize video
            if (firststep == 0) { // set start position
                focusdir = 0;
                DRV_imgsMotorStep(1, focusdir, MAX_STEP);
                //if(!focusdir) {
                //    DRV_imgsMotorStep(1, 1, maxstep-4);
                //}
            }
            stepcnt = 0;
            firststep++;
            AFMax = focus_val;
        } else {
            stepcnt +=2;
            DRV_imgsMotorStep(1, !focusdir, 2); // 2 is optimal step for AF
            if(stepcnt > shift){
                if(focus_val > AFMax) {  AFMax = focus_val; maxstep = stepcnt; }
                if ((stepcnt - shift) > MAX_STEP) {
                    stepcnt = 0;
                    AFMax = AFMax*80/100;
                    maxO = 0;
                    dir = focusdir;
                    step = zoomchange;
                }
                OSA_printf("autofocus stepcnt = %d focus_val = %d AFMax = %d maxstep = %d focusdir = %d step = %d\n",
                           stepcnt, focus_val, AFMax, maxstep, focusdir, step);
            }
        }
        break;
    case af_end:
        *afEnable = 0;
        stepcnt = 0;
        numframes = 0;
        firststep = 0;
        DRV_imgsMotorStep(1, 0, 0); // turn off gpio
        break;
    }

    frames++;
}
