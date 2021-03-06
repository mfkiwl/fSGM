#include "mex.h"
#include <nmmintrin.h>
#include "common.h"
/*
 * calc_cost_pyd_sgm.c 
 * Perfrom cost volume construct and sgm for pyramidal sgm OF method. 
 * 
 * Description: 
 * Construct a cost volume for image 1/image 2 with fixed search region. (search center could be initialized by a given 
 * mv map, and then perform sgm on the generated Cost volume
 * the output of this program is a 2D index map, each element is the corresponding best cost index for each pixel 
 * 
 *
 * The calling syntax is:
 *
 *      [C, minIdx, minC, mvSub] = calc_cost_sgm(I1, I2, preMv, halfSearchWinSize, aggHalfWinSize, subPixelRefine)
 *     
 * Input:
 * I1/I2 are input images
 * preMv is mv map from previous pyramidal level, must be have same or large size with I1/I2
 * halfSearchWinSize is the half search windows size in vertical direction. it is doubled in horizontal direction
 * aggHalfWinSize is the half aggregation window size. typically 5x5 is good
 * subPixelRefine: enable sub-pixel position calculation. if set mvSub contains the subpixel location of current level.  
 *
 * Output:
 * C: generated Cost volume
 * minIdx: output index (disparity) by sgm
 * minC: the corresponding sum of the path cost w.r.t. minIdx
 * mvSub: subpixel localtion
*/

#define USE_CONST_COST 
//perform a single step to calculate path cost for current pixel position
inline void sgm_step(PathCost* L, //current path cost
    PathCost* Lpre, //previous path cost
    CostType* C, //cost map
    double dx, double dy, int searchWinX, int searchWinY, 
    int P1, int P2)
{
    PathCost minPathCost = MAX_PATH_COST;
    int dMax = searchWinX * searchWinY;
    PathCost LpreMin = Lpre[dMax]; //get minimum value of pre path cost
    for (int sx = 0; sx < searchWinX; sx ++) {
        for (int sy = 0; sy < searchWinY; sy ++) {

            int ypre = sy + dy  + 0.5;
            int xpre = sx + dx  + 0.5;

            //mxAssert((int)LpreMin + P2 < 256);
            PathCost min1 = LpreMin + P2;
            PathCost min2 = LpreMin + P2;
            PathCost min3 = LpreMin + P2;
            PathCost bestCost = min3;

            // ||d-d'|| = 0
            if(xpre >= 0 && xpre < searchWinX && ypre>=0 && ypre <searchWinY) {
                int dtemp = xpre * searchWinY + ypre;
                min1 = Lpre[dtemp];
            }
            // ||d-d'|| < r
            const int r = 2;
            for(int k = -r; k<= r; k++) {
                for(int m = -r; m<= r; m++) {
                    if(m == 0 && k == 0)
                        continue;

                    int ty = ypre + k;
                    int tx = xpre + m;

                    if(tx >= 0 && tx < searchWinX && ty >= 0 && ty < searchWinY)
                    {
                        int dtemp = tx * searchWinY + ty;
						min2 = std::min<PathCost>(min2, Lpre[dtemp] + P1);
                    }
                }
            }

			bestCost = std::min<PathCost>(bestCost, min1);
			bestCost = std::min<PathCost>(bestCost, min2);

            int d = sx * searchWinY + sy;
            mxAssert(C[d] + bestCost >= LpreMin, "bestCost Must > LpreMin\n");
            L[d] = (C[d] + bestCost) - LpreMin;
            minPathCost = std::min<PathCost>(L[d], minPathCost);
        }
    }

    L[dMax] = minPathCost; //set minimum value of current path cost
}

inline int adaptive_P2(int P2, int pixCur, int pixPre) {
    const int threshold = 50;
    
    return (abs(pixCur - pixPre) > threshold ? P2 / 8 : P2);
}

/* sgm on 3-D cost volume
 * Output:
 * bestD is the output best index along the third dimension
 * minC is the corresponding cost along with best index
 * mvSub is the output subpixel position for mvx/mvy
 *
 * Input:
 * C: 3-d cost volume
 * width/height/dMax: width/height/dMax(third dimension) of C
 * mvPre: previous level's the mv map
 * mvWidth/mvHeight: width/height of mvPre
 * searchWinX: search window size at x-direction
 * searchWinY: search window size at y-direction
 * P1/P2: small/large penalty
 * subpixelRefine: enable/disable subpixel position estimation
 *
 */
void sgm2d(unsigned* bestD, unsigned* minC, double* mvSub, 
        PixelType* I1, CostType* C, int width, int height, int dMax,
        double* mvPre, int mvWidth, int mvHeight, 
        int searchWinX, int searchWinY, int P1, int P2, int subpixelRefine, bool  enableDiagnalPath = true, int totalPass = 2, bool adpativeP2 = false)
{
    mxAssert(dMax == searchWinX*searchWinY, "dMax should equal to searchWinX*searchWinY");
    //allocate path cost buffers. dMax cost entries + 1 minimun cost entry
    PathCost* L1 = (PathCost*) mxMalloc (sizeof(PathCost) * 2 * (dMax + 1));            //Left -> Right direction
    PathCost* L2 = (PathCost*) mxMalloc (sizeof(PathCost) * 2 * width * (dMax + 1));  //top-left -> bottom right direction
    PathCost* L3 = (PathCost*) mxMalloc (sizeof(PathCost) * 2 * width * (dMax + 1));    //up -> bottom direction
    PathCost* L4 = (PathCost*) mxMalloc (sizeof(PathCost) * 2 * width * (dMax + 1));  //top-right->bottom left direction
    unsigned * Sp = (unsigned *) mxMalloc (sizeof(unsigned ) * width * height * dMax); //sum of path cost from all directions
    memset(Sp, 0, sizeof(unsigned)*width*height*dMax);

    const double* pMvx = mvPre;
    const double* pMvy = mvPre + mvWidth * mvHeight;
    const int pathCostEntryPerPixel = (dMax + 1); //dMax + 1 minimun
    const int pathCostEntryPerRow = width * pathCostEntryPerPixel;
    const int costPerRowEntry = width*dMax;
    
    int ystart = 0;
    int yend = height ;
    int ystep = 1;

    int xstart = 0;
    int xend = width ;
    int xstep = 1;

    for (int pass = 0; pass < totalPass; pass++) {
        if (pass == 1) {
            ystart = height - 1;
            yend = -1;
            ystep = -1;

            xstart = width - 1;
            xend = -1;
            xstep = -1;
        }

        PathCost* ptrL1Pre = L1;
        PathCost* ptrL1Cur = L1 + dMax + 1;
        PathCost* ptrL3PreRow = L3;
        PathCost* ptrL3CurRow = L3 + pathCostEntryPerRow;
        PathCost* ptrL2PreRow = L2;
        PathCost* ptrL2CurRow = L2 + pathCostEntryPerRow;
        PathCost* ptrL4PreRow = L4;
        PathCost* ptrL4CurRow = L4 + pathCostEntryPerRow;

        for (int y = ystart; y != yend; y += ystep) {

            unsigned* ptrSp = Sp + y*costPerRowEntry;
            CostType* ptrC = C + y*costPerRowEntry;

            for (int x = xstart; x != xend; x += xstep) {

                PathCost* ptrL3Cur = ptrL3CurRow + x*(dMax + 1);
                PathCost* ptrL3Pre = ptrL3PreRow + x*(dMax + 1);

                PathCost* ptrL2Cur = ptrL2CurRow + x*(dMax + 1);
                PathCost* ptrL2Pre = ptrL2PreRow + (x - xstep)*(dMax + 1);

                PathCost* ptrL4Cur = ptrL4CurRow + x*(dMax + 1);
                PathCost* ptrL4Pre = ptrL4PreRow + (x + xstep)*(dMax + 1);

                CostType* ptrCCur = ptrC + x*dMax;

                if (x == xstart) {
                    memcpy(ptrL1Cur, ptrCCur, sizeof(PathCost)*dMax);
                    ptrL1Cur[dMax] = 0;

                    if (enableDiagnalPath) {
                        memcpy(ptrL2Cur, ptrCCur, sizeof(PathCost)*dMax);
                        ptrL2Cur[dMax] = 0;
                    }
                }

                if (y == ystart) {
                    memcpy(ptrL3Cur, ptrCCur, sizeof(PathCost)*dMax);
                    ptrL3Cur[dMax] = 0;

                    if (enableDiagnalPath) {
                        memcpy(ptrL2Cur, ptrCCur, sizeof(PathCost)*dMax);
                        ptrL2Cur[dMax] = 0;

                        memcpy(ptrL4Cur, ptrCCur, sizeof(PathCost)*dMax);
                        ptrL4Cur[dMax] = 0;
                    }
                }

                if (x == xend - xstep) {
                    if (enableDiagnalPath) {
                        memcpy(ptrL4Cur, ptrCCur, sizeof(PathCost)*dMax);
                        ptrL4Cur[dMax] = 0;
                    }
                }

                if (x != xstart) {
                    //hint map may have different size with image, must set width to mvWidth, otherwise will have 45degree error propagation issue 
                    //when 2nd pyd processing
                    double dx = pMvx[y*mvWidth + x] - pMvx[y*mvWidth + x - xstep];
                    double dy = pMvy[y*mvWidth + x] - pMvy[y*mvWidth + x - xstep];
                    PixelType pixCur = I1[width*y + x];
                    PixelType pixPre = I1[width*y + x - xstep];
                    
                    sgm_step(ptrL1Cur,              //current path cost
                        ptrL1Pre,                   //previous path cost
                        ptrCCur,                    //cost map
                        dx, dy, searchWinX, searchWinY, P1, adpativeP2 ? adaptive_P2(P2, pixCur, pixPre) : P2);
                }


                if (y != ystart) {
                    double dx = pMvx[y*mvWidth + x] - pMvx[(y - ystep)*mvWidth + x];
                    double dy = pMvy[y*mvWidth + x] - pMvy[(y - ystep)*mvWidth + x];

                    PixelType pixCur = I1[width*y + x];
                    PixelType pixPre = I1[width*(y-ystep) + x];

                    sgm_step(ptrL3Cur,              //current path cost
                        ptrL3Pre,                   //previous path cost
                        ptrCCur,                    //cost map
                        dx, dy, searchWinX, searchWinY, P1, adpativeP2 ? adaptive_P2(P2, pixCur, pixPre) : P2);
                }

                if (enableDiagnalPath) {
                    if (x != xstart && y != ystart) {
                        double dx = pMvx[y*mvWidth + x] - pMvx[(y - ystep)*mvWidth + x - xstep];
                        double dy = pMvy[y*mvWidth + x] - pMvy[(y - ystep)*mvWidth + x - xstep];

                        PixelType pixCur = I1[width*y + x];
                        PixelType pixPre = I1[width*(y - ystep) + x - xstep];

                        sgm_step(ptrL2Cur,          //current path cost
                            ptrL2Pre,               //previous path cost
                            ptrCCur,                //cost map
                            dx, dy, searchWinX, searchWinY, P1, adpativeP2 ? adaptive_P2(P2, pixCur, pixPre) : P2);
                    }

                    if (x != xend - xstep && y != ystart) {
                        double dx = pMvx[y*mvWidth + x] - pMvx[(y - ystep)*mvWidth + x + xstep];
                        double dy = pMvy[y*mvWidth + x] - pMvy[(y - ystep)*mvWidth + x + xstep];

                        PixelType pixCur = I1[width*y + x];
                        PixelType pixPre = I1[width*(y - ystep) + x + xstep];

                        sgm_step(ptrL4Cur,          //current path cost
                            ptrL4Pre,               //previous path cost
                            ptrCCur,                //cost map
                            dx, dy, searchWinX, searchWinY, P1, adpativeP2 ? adaptive_P2(P2, pixCur, pixPre) : P2);

                    }
                }
                for (int d = 0; d < dMax; d++) {
                    ptrSp[x*dMax + d] += ptrL1Cur[d] + ptrL3Cur[d];
                    if (enableDiagnalPath) {
                        ptrSp[x*dMax + d] += ptrL2Cur[d] + ptrL4Cur[d];
                    }
                }

                //swap buffer pointer for left->right direction
                PathCost* tmp = ptrL1Pre;
                ptrL1Pre = ptrL1Cur;
                ptrL1Cur = tmp;
            }

            //swap buffer pointer for top->bottom direction
            PathCost* tmp = ptrL3PreRow;
            ptrL3PreRow = ptrL3CurRow;
            ptrL3CurRow = tmp;

            if (enableDiagnalPath) {
                //swap buffer pointer for top left->bottom rightdirection
                tmp = ptrL2PreRow;
                ptrL2PreRow = ptrL2CurRow;
                ptrL2CurRow = tmp;

                //swap buffer pointer for top right->bottom leftdirection
                tmp = ptrL4PreRow;
                ptrL4PreRow = ptrL4CurRow;
                ptrL4CurRow = tmp;
            }
        }
    }
    
    for(int y = 0; y< height; y++) {
        unsigned* SpPtr = Sp + y*costPerRowEntry;
        for (int x = 0; x <width; x++) {
            
            unsigned minCost = SpPtr[x*dMax];
            unsigned minIdx = 0;
            for (int d = 1; d<dMax; d++) {
                
                if(SpPtr[x*dMax + d] < minCost) {
                    minCost = SpPtr[x*dMax + d];
                    minIdx = d;
                }
            }
            minC[y*width +x] = minCost;
            bestD[y*width +x] = minIdx;
        }
    }
    
    double * ptrMvSubMvx = mvSub;
    double * ptrMvSubMvy = mvSub + width*height;
    
    if(subpixelRefine) {
        /*
         * do subpixel quadratic interpolation:
         *fit parabola into (x1=d-1, y1=C[d-1]), (x2=d, y2=C[d]), (x3=d+1, y3=C[d+1])
         *then find minimum of the parabola
         */
        for(int y = 0; y< height; y++) {
            unsigned* SpPtr = Sp + y*costPerRowEntry;
            
            for (int x = 0; x <width; x++) {
                unsigned bestIdx = bestD[y*width + x];
                
                double c0 = (double) (SpPtr[x*dMax + bestIdx]);
                
                int dx = bestIdx / searchWinY;
                int dy = bestIdx % searchWinY;
                
                if(dy > 0 && dy < searchWinY - 1) {
                    double cLeft = (double)(SpPtr[x*dMax + bestIdx - 1]);
                    double cRight  = (double)(SpPtr[x*dMax + bestIdx + 1]);
                    
                    if (cRight < cLeft)
                        ptrMvSubMvy[y*width + x] = (cRight-cLeft)/(c0 - cLeft)/2.0;
                    else
                        ptrMvSubMvy[y*width + x] = (cRight-cLeft)/(c0 - cRight)/2.0;
                    
                } else {
                    ptrMvSubMvy[y*width + x] = 0;
                }
                
                if(dx >0 && dx < searchWinX-1) {
                    double cLeft = (double)(SpPtr[x*dMax + bestIdx - searchWinY]);
                    double cRight  = (double)(SpPtr[x*dMax + bestIdx + searchWinY]);
                    
                    if (cRight < cLeft)
                        ptrMvSubMvx[y*width + x] = (cRight-cLeft)/(c0 - cLeft)/2.0;
                    else
                        ptrMvSubMvx[y*width + x] = (cRight-cLeft)/(c0 - cRight)/2.0;
                    
                } else {
                    ptrMvSubMvx[y*width + x] = 0;
                }
            }
        }
        
    }
       
       
    mxFree(L1);
    mxFree(L2);
    mxFree(L3);
    mxFree(L4);
    mxFree(Sp);
}
        
void calc_cost(unsigned char* C, 
    const unsigned* cen1, const unsigned* cen2, int width, int height,
    const double* preMv, int mvWidth, int mvHeight, 
    int winRadiusAgg, int winRadiusX, int winRadiusY)
{
    const double* pMvx = preMv;
    const double* pMvy = preMv + mvWidth*mvHeight;
    int winPixels = (2 * winRadiusAgg + 1)*(2 * winRadiusAgg + 1);
    const CostType defaultCost = 5;
    int dMax = (2 * winRadiusX + 1) * (2 * winRadiusY + 1);

    for (int y = 0; y< height; y++) {
        for (int x = 0; x< width; x++) {
            CostType* ptrC = C + y*dMax*width + dMax*x;
            double mvx = pMvx[mvWidth*y + x];
            double mvy = pMvy[mvWidth*y + x];
            
            int d = 0;
            for (int offx = -winRadiusX; offx <= winRadiusX; offx++) {
                for (int offy = -winRadiusY; offy <= winRadiusY; offy++) {
                
                    //int d = (offx + winRadiusX)* (2 * winRadiusY + 1) + offy + winRadiusY;
                    //mexPrintf("d: %d\n", d);
                  
                    unsigned costSum = 0;
                    for (int aggy = -winRadiusAgg; aggy <= winRadiusAgg; aggy++) {
                        for (int aggx = -winRadiusAgg; aggx <= winRadiusAgg; aggx++) {

                            int y1 = y + aggy;
                            int x1 = x + aggx;
#ifdef USE_CONST_COST
                            if(y1 < 0 || y1 > height-1 || x1 <0 || x1 > width-1) {
                                costSum += defaultCost;  //add constant cost if not valid current pixel position
                                continue;
                            }
#else         
                            y1 = clamp(y1, 0, height - 1);
                            x1 = clamp(x1, 0, width - 1);
#endif
                            unsigned cenCode1 = cen1[width*y1 + x1];

                            int y2 = 1.0*(offy + y1) + mvy + 0.5;
                            int x2 = 1.0*(offx + x1) + mvx + 0.5;
#ifdef USE_CONST_COST                  
                            if(y2 < 0 || y2 > height-1 || x2 <0 || x2 > width-1) {
                                costSum += defaultCost; //add constant cost if not valid reference pixel position
                                continue;
                            }
#else
                            y2 = clamp(y2, 0, height - 1);
                            x2 = clamp(x2, 0, width - 1);
#endif
                            unsigned cenCode2 = cen2[width*y2 + x2];
                            int censusCost = _mm_popcnt_u32((cenCode1^cenCode2));
                            costSum += censusCost;
                        }
                    }
                    ptrC[d] = (1.0 * costSum / winPixels) + 0.5;
                    d++;
                }
            }
        }
    }
}
/* The gateway function */
void mexFunction(int nlhs, mxArray *plhs[],
                 int nrhs, const mxArray *prhs[])
{
    PixelType *I1;                  /* pointer to Input image I1 */
    PixelType *I2;                  /* pointer to Input image I2 */
    double *preMv;                  /* pointer to initial search position */
    
    mwSize width;                   /* rows (width) assume I1/I2 are permuted before passing in */
    mwSize height;                  /* cols (height)*/
    
    I1 = (PixelType*)mxGetData(prhs[0]);
    I2 = (PixelType*)mxGetData(prhs[1]);
    
    preMv = mxGetPr(prhs[2]);       /* previous level's mv result, already upscaled to current level's size*/
    
    width = mxGetM(prhs[0]);
    height = mxGetN(prhs[0]);
    
    double halfSearchWinSizeX = mxGetScalar(prhs[3]);
    double halfSearchWinSizeY = mxGetScalar(prhs[4]);
    double aggHalfWinSize= mxGetScalar(prhs[5]);
    int dMax = (2 * halfSearchWinSizeX + 1)*(2 * halfSearchWinSizeY + 1);
    int subPixelRefine = mxGetScalar(prhs[6]);
    int P1 = mxGetScalar(prhs[7]);
    int P2 = mxGetScalar(prhs[8]);
    bool enableDiagnalPath = mxGetScalar(prhs[9]);
    int totalPass = mxGetScalar(prhs[10]);
    bool adpativeP2 = mxGetScalar(prhs[11]);
    
    /* create the output matrix */
    //const mwSize dims[]={width, height, dMax};      //output: costvolume
    const mwSize dims2[] = { width, height };       //output: best index map
    const mwSize dims3[] = { width, height, 2 };    //output: subpixel position

    //plhs[0] = mxCreateNumericArray(3, dims, mxUINT8_CLASS, mxREAL);
    plhs[0] = mxCreateNumericArray(2, dims2, mxUINT32_CLASS, mxREAL);
    plhs[1] = mxCreateNumericArray(2, dims2, mxUINT32_CLASS, mxREAL);
    plhs[2] = mxCreateNumericArray(3, dims3, mxDOUBLE_CLASS, mxREAL);
    
    unsigned * bestD = (unsigned*) mxGetData(plhs[0]);
    unsigned* minC = (unsigned*)mxGetData(plhs[1]);
    double* mvSub = mxGetPr(plhs[2]);

    unsigned* cen1 = (unsigned*)mxMalloc(width * height * sizeof(unsigned));
    unsigned *cen2 = (unsigned*)mxMalloc(width * height * sizeof(unsigned));

    census(I1, cen1, width, height, 2);
    census(I2, cen2, width, height, 2);
    
    int winRadiusY = halfSearchWinSizeY;
    int winRadiusX = halfSearchWinSizeX;
    int winRadiusAgg = aggHalfWinSize;
    mexPrintf("width: %d, height: %d, dMax: %d, winRadiusAgg: %d\n", width, height, dMax, winRadiusAgg);
    
    int mvWidth = mxGetM(prhs[2]);
    int mvHeight = mxGetN(prhs[2])/2;
    
    CostType* C = (CostType*)mxMalloc(width*height*dMax * sizeof(CostType));
    //construct cost volume
    calc_cost(C, cen1, cen2, width, height, preMv, mvWidth, mvHeight,
        winRadiusAgg, winRadiusX, winRadiusY);

    //perform sgm
    sgm2d(bestD, minC, mvSub, 
        I1, C, width, height, dMax, 
        preMv, mvWidth, mvHeight, 
        winRadiusX*2 +1, winRadiusY*2+1, P1,  P2, subPixelRefine, enableDiagnalPath, totalPass, adpativeP2);
    
    mxFree(cen1);
    mxFree(cen2);
    mxFree(C);
}