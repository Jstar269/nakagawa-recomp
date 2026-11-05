// SPDX-License-Identifier: GPL-3.0-only
// ****************************************************************************
// * This file is part of the xBRZ project. It is distributed under           *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0         *
// * Copyright (C) Zenju (zenju AT gmx DOT de) - All Rights Reserved          *
// *                                                                          *
// * Additionally and as a special exception, the author gives permission     *
// * to link the code of this program with the following libraries            *
// * (or with modified versions that use the same licenses), and distribute   *
// * linked combinations including the two: MAME, FreeFileSync, Snes9x, ePSXe *
// *                                                                          *
// * You must obey the GNU General Public License in all respects for all of  *
// * the code used other than MAME, FreeFileSync, Snes9x, ePSXe.              *
// * If you modify this file, you may extend this exception to your version   *
// * of the file, but you are not obligated to do so. If you do not wish to   *
// * do so, delete this exception statement from your version.                *
// ****************************************************************************
//
// Modified by Nakagawa Recomp contributors, 2026-11-04: translated from C++ to C (xBRZ 1.9,
// xbrz.cpp and the helpers it takes from xbrz_tools.h). The compile-time templates are
// runtime parameters (scale factor, rotation), and only the ColorFormat::argbUnbuffered path
// is kept (ColorDistanceUnbufferedARGB, ColorGradientARGB, OobReaderTransparent). The
// arithmetic, its order and the rules are upstream's; the output is bit-identical to
// xbrz::scale(factor, ..., ColorFormat::argbUnbuffered) (src/rt/gpu_sdl3vk/tex_scale_selftest.c
// checks it against goldens made from the pinned upstream source). Factor 1 clamps the slice
// to the image. See PROVENANCE.md in this directory.

#include "xbrz.h"

#include <math.h>
#include <string.h>

/* The goldens are upstream's results under IEEE double arithmetic with no fused
 * multiply-add: keep the compiler from contracting a*b+c, which would change rounding. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#if defined(__GNUC__)
#define XBRZ_INLINE static inline __attribute__((always_inline))
#else
#define XBRZ_INLINE static inline
#endif

void nk_xbrz_default_cfg(NkXbrzCfg *cfg)
{
    cfg->equalColorTolerance        = 30;
    cfg->centerDirectionBias        = 4;
    cfg->dominantDirectionThreshold = 3.6;
    cfg->steepDirectionThreshold    = 2.4;
    cfg->testAttribute              = 0; //unused; test new parameters
}

XBRZ_INLINE unsigned char getAlpha(uint32_t pix) { return (unsigned char)((pix >> 24) & 0xff); }
XBRZ_INLINE unsigned char getRed  (uint32_t pix) { return (unsigned char)((pix >> 16) & 0xff); }
XBRZ_INLINE unsigned char getGreen(uint32_t pix) { return (unsigned char)((pix >>  8) & 0xff); }
XBRZ_INLINE unsigned char getBlue (uint32_t pix) { return (unsigned char)( pix        & 0xff); }

XBRZ_INLINE uint32_t makePixel(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return (a << 24) | (r << 16) | (g << 8) | b; }

XBRZ_INLINE unsigned int uintDivRound(unsigned int num, unsigned int den)
{
    return (num + den / 2) / den;
}

//find intermediate color between two colors with alpha channels (=> NO alpha blending!!!)
//Limitation: alpha should be applied in gamma-decoded linear RGB space: https://ssp.impulsetrain.com/gamma-premult.html
XBRZ_INLINE uint32_t gradientARGB(unsigned int M, unsigned int N, uint32_t pixFront, uint32_t pixBack)
{
    const unsigned int weightFront = getAlpha(pixFront) * M;
    const unsigned int weightBack  = getAlpha(pixBack) * (N - M);
    const unsigned int weightSum   = weightFront + weightBack;
    if (weightSum == 0)
        return 0;

#define XBRZ_CALC_COLOR(colFront, colBack) \
    ((unsigned char)uintDivRound((colFront) * weightFront + (colBack) * weightBack, weightSum))

    return makePixel((unsigned char)uintDivRound(weightSum, N),
                     XBRZ_CALC_COLOR(getRed  (pixFront), getRed  (pixBack)),
                     XBRZ_CALC_COLOR(getGreen(pixFront), getGreen(pixBack)),
                     XBRZ_CALC_COLOR(getBlue (pixFront), getBlue (pixBack)));
#undef XBRZ_CALC_COLOR
}

XBRZ_INLINE double square(double value) { return value * value; }

XBRZ_INLINE double distYCbCr(uint32_t pix1, uint32_t pix2)
{
    //https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.601_conversion
    //Y'CbCr conversion is a matrix multiplication => take advantage of linearity by subtracting first!
    const int r_diff = (int)getRed  (pix1) - getRed  (pix2); //defer division by 255 to after matrix multiplication
    const int g_diff = (int)getGreen(pix1) - getGreen(pix2); //
    const int b_diff = (int)getBlue (pix1) - getBlue (pix2); //substraction for int is noticeable faster than for double!

    const double k_b = 0.0593; //ITU-R BT.2020 conversion
    const double k_r = 0.2627; //
    const double k_g = 1 - k_b - k_r;

    const double scale_b = 0.5 / (1 - k_b);
    const double scale_r = 0.5 / (1 - k_r);

    const double y   = k_r * r_diff + k_g * g_diff + k_b * b_diff; //[!], analog YCbCr!
    const double c_b = scale_b * (b_diff - y);
    const double c_r = scale_r * (r_diff - y);

    //we skip division by 255 to have similar range like other distance functions
    return sqrt(square(y) + square(c_b) + square(c_r));
}

/* ColorDistanceUnbufferedARGB */
XBRZ_INLINE double dist(uint32_t pix1, uint32_t pix2)
{
    const double a1 = getAlpha(pix1) / 255.0 ;
    const double a2 = getAlpha(pix2) / 255.0 ;

    const double d = distYCbCr(pix1, pix2);
    if (a1 < a2)
        return a1 * d + 255 * (a2 - a1);
    else
        return a2 * d + 255 * (a1 - a2);
}

enum //BlendType
{
    BLEND_NONE = 0,
    BLEND_NORMAL,   //a normal indication to blend
    BLEND_DOMINANT, //a strong indication to blend
    //attention: BlendType must fit into the value range of 2 bit!!!
};

typedef struct
{
    unsigned char
    blend_e, blend_f,
             blend_h, blend_i;
} BlendResult;

/* Kernel_4x4: a..i is Kernel_3x3. Fields in upstream's order. */
typedef struct
{
    uint32_t
    a, b, c,
    d, e, f,
    g, h, i;
    uint32_t j, k, l, m, n, o, p;
} Kernel_4x4;
/* input kernel for preprocessing step:

    -----------------
    | A | B | C | P |
    |---|---|---|---|
    | D | E | F | O |   evaluate the four corners between E, F, H, I
    |---|---|---|---|   input pixel is at position E
    | G | H | I | N |
    |---|---|---|---|
    | J | K | L | M |
    -----------------                                                         */

//detect blend direction
XBRZ_INLINE BlendResult preProcessCorners(const Kernel_4x4 *ker, const NkXbrzCfg *cfg) //result: E, F, H, I corners of "GradientType"
{
    BlendResult result = {0, 0, 0, 0};
    if ((ker->e == ker->f &&
         ker->h == ker->i) ||
        (ker->e == ker->h &&
         ker->f == ker->i))
        return result;

    const double hf = dist(ker->g, ker->e) + dist(ker->e, ker->c) + dist(ker->k, ker->i) + dist(ker->i, ker->o) + cfg->centerDirectionBias * dist(ker->h, ker->f);
    const double ei = dist(ker->d, ker->h) + dist(ker->h, ker->l) + dist(ker->b, ker->f) + dist(ker->f, ker->n) + cfg->centerDirectionBias * dist(ker->e, ker->i);

    if (hf < ei) //test sample: 70% of values max(hf, ei) / min(hf, ei) are between 1.1 and 3.7 with median being 1.8
    {
        const int dominantGradient = cfg->dominantDirectionThreshold * hf < ei;
        if (ker->e != ker->f && ker->e != ker->h)
            result.blend_e = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;

        if (ker->i != ker->h && ker->i != ker->f)
            result.blend_i = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
    }
    else if (ei < hf)
    {
        const int dominantGradient = cfg->dominantDirectionThreshold * ei < hf;
        if (ker->h != ker->e && ker->h != ker->i)
            result.blend_h = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;

        if (ker->f != ker->e && ker->f != ker->i)
            result.blend_f = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
    }
    return result;
}

//compress four blend types into a single byte
XBRZ_INLINE unsigned char getTopR   (unsigned char b) { return (unsigned char)(0x3 & (b >> 2)); }
XBRZ_INLINE unsigned char getBottomR(unsigned char b) { return (unsigned char)(0x3 & (b >> 4)); }
XBRZ_INLINE unsigned char getBottomL(unsigned char b) { return (unsigned char)(0x3 & (b >> 6)); }

XBRZ_INLINE void clearAddTopL(unsigned char *b, unsigned char bt) { *b = bt; }
XBRZ_INLINE void addTopR     (unsigned char *b, unsigned char bt) { *b = (unsigned char)(*b | (bt << 2)); } //buffer is assumed to be initialized before preprocessing!
XBRZ_INLINE void addBottomR  (unsigned char *b, unsigned char bt) { *b = (unsigned char)(*b | (bt << 4)); } //e.g. via clearAddTopL()
XBRZ_INLINE void addBottomL  (unsigned char *b, unsigned char bt) { *b = (unsigned char)(*b | (bt << 6)); } //

/* RotationDegree, clock-wise: 0 = ROT_0, 1 = ROT_90, 2 = ROT_180, 3 = ROT_270 */
XBRZ_INLINE unsigned char rotateBlendInfo(int rotDeg, unsigned char b)
{
    switch (rotDeg)
    {
        case 1: return (unsigned char)(((b << 2) | (b >> 6)) & 0xff);
        case 2: return (unsigned char)(((b << 4) | (b >> 4)) & 0xff);
        case 3: return (unsigned char)(((b << 6) | (b >> 2)) & 0xff);
        default: return b;
    }
}

/* get_x<rotDeg>(ker): the kernel position each of a..i reads after rotation, as indices into
 * a, b, c, d, e, f, g, h, i (upstream's DEF_GETTER tables). */
static const unsigned char k_rot_getter[4][9] =
{
    { 0, 1, 2, 3, 4, 5, 6, 7, 8 }, /* ROT_0 */
    { 6, 3, 0, 7, 4, 1, 8, 5, 2 }, /* ROT_90:  a=g b=d c=a d=h e=e f=b g=i h=f i=c */
    { 8, 7, 6, 5, 4, 3, 2, 1, 0 }, /* ROT_180: a=i b=h c=g d=f e=e f=d g=c h=b i=a */
    { 2, 5, 8, 1, 4, 7, 0, 3, 6 }, /* ROT_270: a=c b=f c=i d=b e=e f=h g=a h=d i=g */
};

/* OutputMatrix<N, rotDeg>: access matrix area, top-left at position "out" for image with given
 * width. ref(I, J) is MatrixRotation's (I_old, J_old) for (I, J). */
typedef struct
{
    uint32_t *out_;
    int outWidth_;
    int n_;
    int rotDeg_;
} OutputMatrix;

XBRZ_INLINE uint32_t *ref(const OutputMatrix *o, int I, int J)
{
    const int N = o->n_;
    int I_old, J_old;
    switch (o->rotDeg_)
    {
        case 1:  I_old = N - 1 - J; J_old = I;         break;
        case 2:  I_old = N - 1 - I; J_old = N - 1 - J; break;
        case 3:  I_old = J;         J_old = N - 1 - I; break;
        default: I_old = I;         J_old = J;         break;
    }
    return o->out_ + J_old + I_old * o->outWidth_;
}

/* ColorGradientARGB::alphaGrad<M, N>(pixBack, pixFront) */
XBRZ_INLINE void alphaGrad(unsigned int M, unsigned int N, uint32_t *pixBack, uint32_t pixFront)
{
    *pixBack = gradientARGB(M, N, pixFront, *pixBack);
}

#define AG(M, N, I, J) alphaGrad((M), (N), ref(out, (I), (J)), col)
#define SET(I, J) (*ref(out, (I), (J)) = col)

/* Scaler2x .. Scaler6x: kind 0 blendLineShallow, 1 blendLineSteep, 2 blendLineSteepAndShallow,
 * 3 blendLineDiagonal, 4 blendCorner. */
enum { LINE_SHALLOW, LINE_STEEP, LINE_STEEP_AND_SHALLOW, LINE_DIAGONAL, CORNER };

static void scaler2x(int kind, uint32_t col, const OutputMatrix *out)
{
    const int scale = 2;
    switch (kind)
    {
        case LINE_SHALLOW:
            AG(1, 4, scale - 1, 0);
            AG(3, 4, scale - 1, 1);
            break;
        case LINE_STEEP:
            AG(1, 4, 0, scale - 1);
            AG(3, 4, 1, scale - 1);
            break;
        case LINE_STEEP_AND_SHALLOW:
            AG(1, 4, 1, 0);
            AG(1, 4, 0, 1);
            AG(5, 6, 1, 1); //[!] fixes 7/8 used in xBR
            break;
        case LINE_DIAGONAL:
            AG(1, 2, 1, 1);
            break;
        default:
            //model a round corner
            AG(21, 100, 1, 1); //exact: 1 - pi/4 = 0.2146018366
            break;
    }
}

static void scaler3x(int kind, uint32_t col, const OutputMatrix *out)
{
    const int scale = 3;
    switch (kind)
    {
        case LINE_SHALLOW:
            AG(1, 4, scale - 1, 0);
            AG(1, 4, scale - 2, 2);

            AG(3, 4, scale - 1, 1);
            SET(scale - 1, 2);
            break;
        case LINE_STEEP:
            AG(1, 4, 0, scale - 1);
            AG(1, 4, 2, scale - 2);

            AG(3, 4, 1, scale - 1);
            SET(2, scale - 1);
            break;
        case LINE_STEEP_AND_SHALLOW:
            AG(1, 4, 2, 0);
            AG(1, 4, 0, 2);
            AG(3, 4, 2, 1);
            AG(3, 4, 1, 2);
            SET(2, 2);
            break;
        case LINE_DIAGONAL:
            AG(1, 8, 1, 2); //conflict with other rotations for this odd scale
            AG(1, 8, 2, 1);
            AG(7, 8, 2, 2); //
            break;
        default:
            //model a round corner
            AG(45, 100, 2, 2); //exact: 0.4545939598
            break;
    }
}

static void scaler4x(int kind, uint32_t col, const OutputMatrix *out)
{
    const int scale = 4;
    switch (kind)
    {
        case LINE_SHALLOW:
            AG(1, 4, scale - 1, 0);
            AG(1, 4, scale - 2, 2);

            AG(3, 4, scale - 1, 1);
            AG(3, 4, scale - 2, 3);

            SET(scale - 1, 2);
            SET(scale - 1, 3);
            break;
        case LINE_STEEP:
            AG(1, 4, 0, scale - 1);
            AG(1, 4, 2, scale - 2);

            AG(3, 4, 1, scale - 1);
            AG(3, 4, 3, scale - 2);

            SET(2, scale - 1);
            SET(3, scale - 1);
            break;
        case LINE_STEEP_AND_SHALLOW:
            AG(3, 4, 3, 1);
            AG(3, 4, 1, 3);
            AG(1, 4, 3, 0);
            AG(1, 4, 0, 3);

            AG(1, 3, 2, 2); //[!] fixes 1/4 used in xBR

            SET(3, 3);
            SET(3, 2);
            SET(2, 3);
            break;
        case LINE_DIAGONAL:
            AG(1, 2, scale - 1, scale / 2    );
            AG(1, 2, scale - 2, scale / 2 + 1);
            SET(scale - 1, scale - 1);
            break;
        default:
            //model a round corner
            AG(68, 100, 3, 3); //exact: 0.6848532563
            AG( 9, 100, 3, 2); //0.08677704501
            AG( 9, 100, 2, 3); //0.08677704501
            break;
    }
}

static void scaler5x(int kind, uint32_t col, const OutputMatrix *out)
{
    const int scale = 5;
    switch (kind)
    {
        case LINE_SHALLOW:
            AG(1, 4, scale - 1, 0);
            AG(1, 4, scale - 2, 2);
            AG(1, 4, scale - 3, 4);

            AG(3, 4, scale - 1, 1);
            AG(3, 4, scale - 2, 3);

            SET(scale - 1, 2);
            SET(scale - 1, 3);
            SET(scale - 1, 4);
            SET(scale - 2, 4);
            break;
        case LINE_STEEP:
            AG(1, 4, 0, scale - 1);
            AG(1, 4, 2, scale - 2);
            AG(1, 4, 4, scale - 3);

            AG(3, 4, 1, scale - 1);
            AG(3, 4, 3, scale - 2);

            SET(2, scale - 1);
            SET(3, scale - 1);
            SET(4, scale - 1);
            SET(4, scale - 2);
            break;
        case LINE_STEEP_AND_SHALLOW:
            AG(1, 4, 0, scale - 1);
            AG(1, 4, 2, scale - 2);
            AG(3, 4, 1, scale - 1);

            AG(1, 4, scale - 1, 0);
            AG(1, 4, scale - 2, 2);
            AG(3, 4, scale - 1, 1);

            AG(2, 3, 3, 3);

            SET(2, scale - 1);
            SET(3, scale - 1);
            SET(4, scale - 1);

            SET(scale - 1, 2);
            SET(scale - 1, 3);
            break;
        case LINE_DIAGONAL:
            AG(1, 8, scale - 1, scale / 2    ); //conflict with other rotations for this odd scale
            AG(1, 8, scale - 2, scale / 2 + 1);
            AG(1, 8, scale - 3, scale / 2 + 2); //

            AG(7, 8, 4, 3);
            AG(7, 8, 3, 4);

            SET(4, 4);
            break;
        default:
            //model a round corner
            AG(86, 100, 4, 4); //exact: 0.8631434088
            AG(23, 100, 4, 3); //0.2306749731
            AG(23, 100, 3, 4); //0.2306749731
            break;
    }
}

static void scaler6x(int kind, uint32_t col, const OutputMatrix *out)
{
    const int scale = 6;
    switch (kind)
    {
        case LINE_SHALLOW:
            AG(1, 4, scale - 1, 0);
            AG(1, 4, scale - 2, 2);
            AG(1, 4, scale - 3, 4);

            AG(3, 4, scale - 1, 1);
            AG(3, 4, scale - 2, 3);
            AG(3, 4, scale - 3, 5);

            SET(scale - 1, 2);
            SET(scale - 1, 3);
            SET(scale - 1, 4);
            SET(scale - 1, 5);

            SET(scale - 2, 4);
            SET(scale - 2, 5);
            break;
        case LINE_STEEP:
            AG(1, 4, 0, scale - 1);
            AG(1, 4, 2, scale - 2);
            AG(1, 4, 4, scale - 3);

            AG(3, 4, 1, scale - 1);
            AG(3, 4, 3, scale - 2);
            AG(3, 4, 5, scale - 3);

            SET(2, scale - 1);
            SET(3, scale - 1);
            SET(4, scale - 1);
            SET(5, scale - 1);

            SET(4, scale - 2);
            SET(5, scale - 2);
            break;
        case LINE_STEEP_AND_SHALLOW:
            AG(1, 4, 0, scale - 1);
            AG(1, 4, 2, scale - 2);
            AG(3, 4, 1, scale - 1);
            AG(3, 4, 3, scale - 2);

            AG(1, 4, scale - 1, 0);
            AG(1, 4, scale - 2, 2);
            AG(3, 4, scale - 1, 1);
            AG(3, 4, scale - 2, 3);

            SET(2, scale - 1);
            SET(3, scale - 1);
            SET(4, scale - 1);
            SET(5, scale - 1);

            SET(4, scale - 2);
            SET(5, scale - 2);

            SET(scale - 1, 2);
            SET(scale - 1, 3);
            break;
        case LINE_DIAGONAL:
            AG(1, 2, scale - 1, scale / 2    );
            AG(1, 2, scale - 2, scale / 2 + 1);
            AG(1, 2, scale - 3, scale / 2 + 2);

            SET(scale - 2, scale - 1);
            SET(scale - 1, scale - 1);
            SET(scale - 1, scale - 2);
            break;
        default:
            //model a round corner
            AG(97, 100, 5, 5); //exact: 0.9711013910
            AG(42, 100, 4, 5); //0.4236372243
            AG(42, 100, 5, 4); //0.4236372243
            AG( 6, 100, 5, 3); //0.05652034508
            AG( 6, 100, 3, 5); //0.05652034508
            break;
    }
}

#undef AG
#undef SET

XBRZ_INLINE void scalerBlend(int scale, int kind, uint32_t col, const OutputMatrix *out)
{
    switch (scale)
    {
        case 2: scaler2x(kind, col, out); break;
        case 3: scaler3x(kind, col, out); break;
        case 4: scaler4x(kind, col, out); break;
        case 5: scaler5x(kind, col, out); break;
        default: scaler6x(kind, col, out); break;
    }
}

/* input kernel area naming convention:
-------------
| A | B | C |
|---|---|---|
| D | E | F | input pixel is at position E
|---|---|---|
| G | H | I |
-------------                                  */

XBRZ_INLINE int eq(uint32_t pix1, uint32_t pix2, const NkXbrzCfg *cfg)
{
    return dist(pix1, pix2) < cfg->equalColorTolerance;
}

//perf: quite worth it!
XBRZ_INLINE void blendPixel(int scale, int rotDeg, const Kernel_4x4 *ker3,
                            uint32_t *target, int trgWidth,
                            unsigned char blendInfo, //result of preprocessing all four corners of pixel "E"
                            const NkXbrzCfg *cfg)
{
    const uint32_t k9[9] = { ker3->a, ker3->b, ker3->c, ker3->d, ker3->e, ker3->f, ker3->g, ker3->h, ker3->i };
    const unsigned char *rot = k_rot_getter[rotDeg];
    const uint32_t b = k9[rot[1]];
    const uint32_t c = k9[rot[2]];
    const uint32_t d = k9[rot[3]];
    const uint32_t e = k9[rot[4]];
    const uint32_t f = k9[rot[5]];
    const uint32_t g = k9[rot[6]];
    const uint32_t h = k9[rot[7]];
    const uint32_t i = k9[rot[8]];

    const unsigned char blend = rotateBlendInfo(rotDeg, blendInfo);

    if (getBottomR(blend) >= BLEND_NORMAL)
    {
        int doLineBlend;
        if (getBottomR(blend) >= BLEND_DOMINANT)
            doLineBlend = 1;
        //make sure there is no second blending in an adjacent rotation for this pixel: handles insular pixels, mario eyes
        else if (getTopR(blend) != BLEND_NONE && !eq(e, g, cfg)) //but support double-blending for 90° corners
            doLineBlend = 0;
        else if (getBottomL(blend) != BLEND_NONE && !eq(e, c, cfg))
            doLineBlend = 0;
        //no full blending for L-shapes; blend corner only (handles "mario mushroom eyes")
        else if (!eq(e, i, cfg) && eq(g, h, cfg) && eq(h, i, cfg) && eq(i, f, cfg) && eq(f, c, cfg))
            doLineBlend = 0;
        else
            doLineBlend = 1;

        const uint32_t px = dist(e, f) <= dist(e, h) ? f : h; //choose most similar color

        const OutputMatrix out = { target, trgWidth, scale, rotDeg };

        if (doLineBlend)
        {
            const double fg = dist(f, g); //test sample: 70% of values max(fg, hc) / min(fg, hc) are between 1.1 and 3.7 with median being 1.9
            const double hc = dist(h, c); //

            const int haveShallowLine = cfg->steepDirectionThreshold * fg <= hc && e != g && d != g;
            const int haveSteepLine   = cfg->steepDirectionThreshold * hc <= fg && e != c && b != c;

            if (haveShallowLine)
            {
                if (haveSteepLine)
                    scalerBlend(scale, LINE_STEEP_AND_SHALLOW, px, &out);
                else
                    scalerBlend(scale, LINE_SHALLOW, px, &out);
            }
            else
            {
                if (haveSteepLine)
                    scalerBlend(scale, LINE_STEEP, px, &out);
                else
                    scalerBlend(scale, LINE_DIAGONAL, px, &out);
            }
        }
        else
            scalerBlend(scale, CORNER, px, &out);
    }
}

/* OobReaderTransparent */
typedef struct
{
    const uint32_t *s_m1;
    const uint32_t *s_0;
    const uint32_t *s_p1;
    const uint32_t *s_p2;
    int srcWidth_;
} OobReader;

XBRZ_INLINE void oobReaderInit(OobReader *r, const uint32_t *src, int srcWidth, int srcHeight, int y)
{
    r->s_m1 = 0 <= y - 1 && y - 1 < srcHeight ? src + srcWidth * (y - 1) : NULL;
    r->s_0  = 0 <= y     && y     < srcHeight ? src + srcWidth *  y      : NULL;
    r->s_p1 = 0 <= y + 1 && y + 1 < srcHeight ? src + srcWidth * (y + 1) : NULL;
    r->s_p2 = 0 <= y + 2 && y + 2 < srcHeight ? src + srcWidth * (y + 2) : NULL;
    r->srcWidth_ = srcWidth;
}

XBRZ_INLINE void readPonm(const OobReader *r, Kernel_4x4 *ker, int x) //(x, y) is at kernel position E
{
    const int x_p2 = x + 2;
    if (0 <= x_p2 && x_p2 < r->srcWidth_)
    {
        ker->p = r->s_m1 ? r->s_m1[x_p2] : 0;
        ker->o = r->s_0  ? r->s_0 [x_p2] : 0;
        ker->n = r->s_p1 ? r->s_p1[x_p2] : 0;
        ker->m = r->s_p2 ? r->s_p2[x_p2] : 0;
    }
    else
    {
        ker->p = 0;
        ker->o = 0;
        ker->n = 0;
        ker->m = 0;
    }
}

XBRZ_INLINE void fillBlock(uint32_t *trg, int trgWidth, uint32_t col, int blockSize)
{
    for (int y = 0; y < blockSize; ++y, trg += trgWidth)
        for (int x = 0; x < blockSize; ++x)
            trg[x] = col;
}

/* Shift the kernel one column left and read the new right column (P, O, N, M) at x. */
XBRZ_INLINE void shiftKernel(const OobReader *r, Kernel_4x4 *ker4, int x)
{
    ker4->a = ker4->b;    //shift previous kernel to the left
    ker4->d = ker4->e;    // -----------------
    ker4->g = ker4->h;    // | A | B | C | P |
    ker4->j = ker4->k;    // |---|---|---|---|
    /**/                  // | D | E | F | O | (x, y) is at position E
    ker4->b = ker4->c;    // |---|---|---|---|
    ker4->e = ker4->f;    // | G | H | I | N |
    ker4->h = ker4->i;    // |---|---|---|---|
    ker4->k = ker4->l;    // | J | K | L | M |
    /**/                  // -----------------
    ker4->c = ker4->p;
    ker4->f = ker4->o;
    ker4->i = ker4->n;
    ker4->l = ker4->m;

    readPonm(r, ker4, x);
}

/* Initialize the kernel at position x = -1 of the reader's row. */
XBRZ_INLINE void initKernel(const OobReader *r, Kernel_4x4 *ker4)
{
    memset(ker4, 0, sizeof(*ker4));
    readPonm(r, ker4, -4); //hack: read a, d, g, j at x = -1
    ker4->a = ker4->p;
    ker4->d = ker4->o;
    ker4->g = ker4->n;
    ker4->j = ker4->m;

    readPonm(r, ker4, -3);
    ker4->b = ker4->p;
    ker4->e = ker4->o;
    ker4->h = ker4->n;
    ker4->k = ker4->m;

    readPonm(r, ker4, -2);
    ker4->c = ker4->p;
    ker4->f = ker4->o;
    ker4->i = ker4->n;
    ker4->l = ker4->m;

    readPonm(r, ker4, -1);
}

static void scaleImage(int scale, const uint32_t *src, uint32_t *trg, int srcWidth, int srcHeight, const NkXbrzCfg *cfg, int yFirst, int yLast)
{
    yFirst = yFirst > 0 ? yFirst : 0;
    yLast  = yLast < srcHeight ? yLast : srcHeight;
    if (yFirst >= yLast || srcWidth <= 0)
        return;

    const int trgWidth = srcWidth * scale;

    //(ab)use space of "sizeof(uint32_t) * srcWidth * Scaler::scale" at the end of the image as temporary
    //buffer for "on the fly preprocessing" without risk of accidental overwriting before accessing
    unsigned char *const preProcBuf = (unsigned char *)(trg + (size_t)yLast * scale * trgWidth) - srcWidth;

    //initialize preprocessing buffer for first row of current stripe: detect upper left and right corner blending
    //this cannot be optimized for adjacent processing stripes; we must not allow for a memory race condition!
    {
        OobReader oobReader;
        oobReaderInit(&oobReader, src, srcWidth, srcHeight, yFirst - 1);

        //initialize at position x = -1
        Kernel_4x4 ker4;
        initKernel(&oobReader, &ker4);

        {
            const BlendResult res = preProcessCorners(&ker4, cfg);
            clearAddTopL(&preProcBuf[0], res.blend_i); //set 1st known corner for (0, yFirst)
        }

        for (int x = 0; x < srcWidth; ++x)
        {
            shiftKernel(&oobReader, &ker4, x); //(x, yFirst - 1) is at position E

            /*  preprocessing blend result:
                ---------
                | E | F |   evaluate corner between E, F, H, I
                |---+---|   current input pixel is at position E
                | H | I |
                ---------                                        */
            const BlendResult res = preProcessCorners(&ker4, cfg);
            addTopR(&preProcBuf[x], res.blend_h); //set 2nd known corner for (x, yFirst)

            if (x + 1 < srcWidth)
                clearAddTopL(&preProcBuf[x + 1], res.blend_i); //set 1st known corner for (x + 1, yFirst)
        }
    }
    //------------------------------------------------------------------------------------

    for (int y = yFirst; y < yLast; ++y)
    {
        uint32_t *out = trg + (size_t)scale * y * trgWidth; //consider MT "striped" access

        OobReader oobReader;
        oobReaderInit(&oobReader, src, srcWidth, srcHeight, y);

        //initialize at position x = -1
        Kernel_4x4 ker4;
        initKernel(&oobReader, &ker4);

        unsigned char blend_xy1 = 0; //corner blending for current (x, y + 1) position
        {
            const BlendResult res = preProcessCorners(&ker4, cfg);
            clearAddTopL(&blend_xy1, res.blend_i); //set 1st known corner for (0, y + 1) and buffer for use on next column

            addBottomL(&preProcBuf[0], res.blend_f); //set 3rd known corner for (0, y)
        }

        for (int x = 0; x < srcWidth; ++x, out += scale)
        {
            shiftKernel(&oobReader, &ker4, x); //(x, y) is at position E

            //evaluate the four corners on bottom-right of current pixel
            unsigned char blend_xy = preProcBuf[x]; //for current (x, y) position
            {
                /*  preprocessing blend result:
                    ---------
                    | E | F |   evaluate corner between E, F, H, I
                    |---+---|   current input pixel is at position E
                    | H | I |
                    ---------                                        */
                const BlendResult res = preProcessCorners(&ker4, cfg);
                addBottomR(&blend_xy, res.blend_e); //all four corners of (x, y) have been determined at this point due to processing sequence!

                addTopR(&blend_xy1, res.blend_h); //set 2nd known corner for (x, y + 1)
                preProcBuf[x] = blend_xy1; //store on current buffer position for use on next row

                if (x + 1 < srcWidth)
                {
                    //blend_xy1 -> blend_x1y1
                    clearAddTopL(&blend_xy1, res.blend_i); //set 1st known corner for (x + 1, y + 1) and buffer for use on next column

                    addBottomL(&preProcBuf[x + 1], res.blend_f); //set 3rd known corner for (x + 1, y)
                }
            }

            //fill block of size scale * scale with the given color
            fillBlock(out, trgWidth, ker4.e, scale);

            //place *after* preprocessing step, to not overwrite the results while processing the last pixel!

            //blend all four corners of current pixel
            if (blend_xy != 0)
            {
                blendPixel(scale, 0, &ker4, out, trgWidth, blend_xy, cfg);
                blendPixel(scale, 1, &ker4, out, trgWidth, blend_xy, cfg);
                blendPixel(scale, 2, &ker4, out, trgWidth, blend_xy, cfg);
                blendPixel(scale, 3, &ker4, out, trgWidth, blend_xy, cfg);
            }
        }
    }
}

int nk_xbrz_scale(int factor, const uint32_t *src, uint32_t *trg, int srcWidth, int srcHeight,
                  const NkXbrzCfg *cfg, int yFirst, int yLast)
{
    NkXbrzCfg defaults;
    if (!cfg)
    {
        nk_xbrz_default_cfg(&defaults);
        cfg = &defaults;
    }
    if (factor == 1)
    {
        if (yFirst < 0) yFirst = 0;
        if (yLast > srcHeight) yLast = srcHeight;
        if (yFirst < yLast && srcWidth > 0)
            memcpy(trg, src + (size_t)yFirst * srcWidth, (size_t)(yLast - yFirst) * srcWidth * sizeof(uint32_t));
        return 1;
    }
    if (factor < 2 || factor > NK_XBRZ_SCALE_FACTOR_MAX)
        return 0;
    scaleImage(factor, src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
    return 1;
}
