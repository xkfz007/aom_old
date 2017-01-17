/*
 * Copyright (c) 2001-2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ratectrl_xiph.h"

/* hack, removeme */
#undef OD_CLAMPI
#define OD_CLAMPI(a, b, c) (OD_MAXI(a, OD_MINI(b, c)))

#define OD_Q57(v) ((int64_t)((uint64_t)(v) << 57))
#define OD_F_Q45(v) ((int64_t)(((v)*((int64_t)1 << 45))))
#define OD_F_Q12(v) ((int32_t)(((v)*((int32_t)1 << 12))))

/*A rough lookup table for tan(x), 0 <= x < pi/2.
  The values are Q12 fixed-point and spaced at 5 degree intervals.
  These decisions are somewhat arbitrary, but sufficient for the 2nd order
   Bessel follower below.
  Values of x larger than 85 degrees are extrapolated from the last interval,
   which is way off, but "good enough".*/
static uint16_t OD_ROUGH_TAN_LOOKUP[18] = {
    0,   358,   722,  1098,  1491,  1910,
 2365,  2868,  3437,  4096,  4881,  5850,
 7094,  8784, 11254, 15286, 23230, 46817
};

static int od_codedquantizer_to_quantizer(int q)
{
    return q;
}

static int od_quantizer_to_codedquantizer(int q)
{
    return q;
}

/*alpha is Q24 in the range [0,0.5).
  The return values is 5.12.*/
static int od_warp_alpha(int alpha) {
  int i;
  int d;
  int t0;
  int t1;
  i = alpha*36 >> 24;
  if (i >= 17)
    i = 16;
  t0 = OD_ROUGH_TAN_LOOKUP[i];
  t1 = OD_ROUGH_TAN_LOOKUP[i + 1];
  d = alpha*36 - (i << 24);
  return (int)((((int64_t)t0 << 32) + ((t1 - t0) << 8)*(int64_t)d) >> 32);
}

static const int64_t OD_ATANH_LOG2[32]={
  0x32B803473F7AD0F4LL,0x2F2A71BD4E25E916LL,0x2E68B244BB93BA06LL,
  0x2E39FB9198CE62E4LL,0x2E2E683F68565C8FLL,0x2E2B850BE2077FC1LL,
  0x2E2ACC58FE7B78DBLL,0x2E2A9E2DE52FD5F2LL,0x2E2A92A338D53EECLL,
  0x2E2A8FC08F5E19B6LL,0x2E2A8F07E51A485ELL,0x2E2A8ED9BA8AF388LL,
  0x2E2A8ECE2FE7384ALL,0x2E2A8ECB4D3E4B1ALL,0x2E2A8ECA94940FE8LL,
  0x2E2A8ECA6669811DLL,0x2E2A8ECA5ADEDD6ALL,0x2E2A8ECA57FC347ELL,
  0x2E2A8ECA57438A43LL,0x2E2A8ECA57155FB4LL,0x2E2A8ECA5709D510LL,
  0x2E2A8ECA5706F267LL,0x2E2A8ECA570639BDLL,0x2E2A8ECA57060B92LL,
  0x2E2A8ECA57060008LL,0x2E2A8ECA5705FD25LL,0x2E2A8ECA5705FC6CLL,
  0x2E2A8ECA5705FC3ELL,0x2E2A8ECA5705FC33LL,0x2E2A8ECA5705FC30LL,
  0x2E2A8ECA5705FC2FLL,0x2E2A8ECA5705FC2FLL
};

static int od_ilog64(int64_t v) {
  static const unsigned char OD_DEBRUIJN_IDX64[64]={
   0, 1, 2, 7, 3,13, 8,19, 4,25,14,28, 9,34,20,40,
   5,17,26,38,15,46,29,48,10,31,35,54,21,50,41,57,
   63, 6,12,18,24,27,33,39,16,37,45,47,30,53,49,56,
   62,11,23,32,36,44,52,55,61,22,43,51,60,42,59,58
  };
  int ret;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v |= v >> 32;
  ret = (int)v & 1;
  v = (v >> 1) + 1;
  ret += OD_DEBRUIJN_IDX64[v*UINT64_C(0x218A392CD3D5DBF) >> 58 & 0x3F];
  return ret;
}

/*Computes the binary exponential of logq57.
  input: a log base 2 in Q57 format
  output: a 64 bit integer in Q0 (no fraction) */
static int64_t od_bexp64(int64_t logq57) {
  int64_t w;
  int64_t z;
  int ipart;
  ipart = (int)(logq57 >> 57);
  if (ipart < 0) return 0;
  if (ipart >= 63) return 0x7FFFFFFFFFFFFFFFLL;
  z = logq57 - OD_Q57(ipart);
  if (z) {
    int64_t mask;
    long wlo;
    int i;
    /*C doesn't give us 64x64->128 muls, so we use CORDIC.
      This is not particularly fast, but it's not being used in time-critical
       code; it is very accurate.*/
    /*z is the fractional part of the log in Q62 format.
      We need 1 bit of headroom since the magnitude can get larger than 1
       during the iteration, and a sign bit.*/
    z <<= 5;
    /*w is the exponential in Q61 format (since it also needs headroom and can
       get as large as 2.0); we could get another bit if we dropped the sign,
       but we'll recover that bit later anyway.
      Ideally this should start out as
        \lim_{n->\infty} 2^{61}/\product_{i=1}^n \sqrt{1-2^{-2i}}
       but in order to guarantee convergence we have to repeat iterations 4,
        13 (=3*4+1), and 40 (=3*13+1, etc.), so it winds up somewhat larger.*/
    w = 0x26A3D0E401DD846DLL;
    for (i = 0;; i++) {
      mask = -(z < 0);
      w += ((w >> (i + 1)) + mask) ^ mask;
      z -= (OD_ATANH_LOG2[i] + mask) ^ mask;
      /*Repeat iteration 4.*/
      if (i >= 3) break;
      z *= 2;
    }
    for (;; i++) {
      mask = -(z < 0);
      w += ((w >> (i + 1)) + mask) ^ mask;
      z -= (OD_ATANH_LOG2[i] + mask) ^ mask;
      /*Repeat iteration 13.*/
      if (i >= 12) break;
      z *= 2;
    }
    for (; i < 32; i++) {
      mask = -(z < 0);
      w += ((w >> (i + 1)) + mask) ^ mask;
      z = (z - ((OD_ATANH_LOG2[i] + mask) ^ mask))*2;
    }
    wlo = 0;
    /*Skip the remaining iterations unless we really require that much
       precision.
      We could have bailed out earlier for smaller iparts, but that would
       require initializing w from a table, as the limit doesn't converge to
       61-bit precision until n=30.*/
    if (ipart > 30) {
      /*For these iterations, we just update the low bits, as the high bits
         can't possibly be affected.
        OD_ATANH_LOG2 has also converged (it actually did so one iteration
         earlier, but that's no reason for an extra special case).*/
      for (;; i++) {
        mask = -(z < 0);
        wlo += ((w >> i) + mask) ^ mask;
        z -= (OD_ATANH_LOG2[31] + mask) ^ mask;
        /*Repeat iteration 40.*/
        if (i >= 39) break;
        z <<= 1;
      }
      for (; i<61; i++) {
        mask = -(z < 0);
        wlo += ((w >> i) + mask) ^ mask;
        z = (z - ((OD_ATANH_LOG2[31] + mask) ^ mask)) << 1;
      }
    }
    w = (w << 1) + wlo;
  }
  else {
    w = (int64_t)1 << 62;
  }
  if (ipart < 62) {
    w = ((w >> (61 - ipart)) + 1) >> 1;
  }
  return w;
}


/*Computes the binary log of w
  input: a 64-bit integer in Q0 (no fraction)
  output: a 64-bit log in Q57 */
static int64_t od_blog64(int64_t w) {
  int64_t z;
  int ipart;
  if (w <= 0) return -1;
  ipart = od_ilog64(w) - 1;
  if (ipart > 61) {
    w >>= ipart - 61;
  }
  else {
    w <<= 61 - ipart;
  }
  z = 0;
  if (w & (w - 1)) {
    int64_t x;
    int64_t y;
    int64_t u;
    int64_t mask;
    int i;
    /*C doesn't give us 64x64->128 muls, so we use CORDIC.
      This is not particularly fast, but it's not being used in time-critical
       code; it is very accurate.*/
    /*z is the fractional part of the log in Q61 format.*/
    /*x and y are the cosh() and sinh(), respectively, in Q61 format.
      We are computing z = 2*atanh(y/x) = 2*atanh((w - 1)/(w + 1)).*/
    x = w + ((int64_t)1 << 61);
    y = w - ((int64_t)1 << 61);
    for (i = 0; i < 4; i++) {
      mask = -(y < 0);
      z += ((OD_ATANH_LOG2[i] >> i) + mask) ^ mask;
      u = x >> (i + 1);
      x -= ((y >> (i + 1)) + mask) ^ mask;
      y -= (u + mask) ^ mask;
    }
    /*Repeat iteration 4.*/
    for (i--; i < 13; i++) {
      mask = -(y < 0);
      z += ((OD_ATANH_LOG2[i] >> i) + mask) ^ mask;
      u = x >> (i + 1);
      x -= ((y >> (i + 1)) + mask) ^ mask;
      y -= (u + mask) ^ mask;
    }
    /*Repeat iteration 13.*/
    for (i--; i < 32; i++) {
      mask = -(y < 0);
      z += ((OD_ATANH_LOG2[i] >> i) + mask) ^ mask;
      u = x >> (i + 1);
      x -= ((y >> (i + 1)) + mask) ^ mask;
      y -= (u + mask) ^ mask;
    }
    /*OD_ATANH_LOG2 has converged.*/
    for (; i < 40; i++) {
      mask = -(y < 0);
      z += ((OD_ATANH_LOG2[31] >> i) + mask) ^ mask;
      u = x >> (i + 1);
      x -= ((y >> (i + 1)) + mask) ^ mask;
      y -= (u + mask) ^ mask;
    }
    /*Repeat iteration 40.*/
    for (i--; i < 62; i++) {
      mask = -(y < 0);
      z += ((OD_ATANH_LOG2[31] >> i) + mask) ^ mask;
      u = x >> (i + 1);
      x -= ((y >> (i + 1)) + mask) ^ mask;
      y -= (u + mask) ^ mask;
    }
    z = (z + 8) >> 4;
  }
  return OD_Q57(ipart) + z;
}

/*Convenience function converts Q57 value to a clamped 32-bit Q24 value
  in: input in Q57 format.
  Return: same number in Q24 */
static int32_t od_q57_to_q24(int64_t in){
  int64_t ret;
  ret = (in + ((int64_t)1 << 32)) >> 33;
  /*0x80000000 is automatically converted to unsigned on 32-bit systems.
    -0x7FFFFFFF-1 is needed to avoid "promoting" the whole expression to
    unsigned.*/
  return (int32_t)OD_CLAMPI(-0x7FFFFFFF-1, ret, 0x7FFFFFFF);
}

/*Binary exponential of log_scale with 24-bit fractional precision and
   saturation.
  log_scale: A binary logarithm in Q57 format.
  Return: The binary exponential in Q24 format, saturated to 2**31-1 if
   log_scale was too large.*/
static int32_t od_bexp64_q24(int64_t log_scale) {
  if (log_scale < OD_Q57(8)) {
    int64_t ret;
    ret = od_bexp64(log_scale + OD_Q57(24));
    return ret < 0x7FFFFFFF ? (int32_t)ret : 0x7FFFFFFF;
  }
  return 0x7FFFFFFF;
}

/*Re-initialize Bessel filter coefficients with the specified delay.
  This does not alter the x/y state, but changes the reaction time of the
   filter.
  Altering the time constant of a reactive filter without alterning internal
   state is something that has to be done carefuly, but our design operates at
   high enough delays and with small enough time constant changes to make it
   safe.*/
static void od_iir_bessel2_reinit(od_iir_bessel2 *f, int delay) {
  int alpha;
  int64_t one48;
  int64_t warp;
  int64_t k1;
  int64_t k2;
  int64_t d;
  int64_t a;
  int64_t ik2;
  int64_t b1;
  int64_t b2;
  /*This borrows some code from an unreleased version of Postfish.
    See the recipe at http://unicorn.us.com/alex/2polefilters.html for details
     on deriving the filter coefficients.*/
  /*alpha is Q24*/
  alpha = (1 << 24)/delay;
  one48 = (int64_t)1 << 48;
  /*warp is 7.12*/
  warp = OD_MAXI(od_warp_alpha(alpha), 1);
  /*k1 is 9.12*/
  k1 = 3*warp;
  /*k2 is 16.24.*/
  k2 = k1*warp;
  /*d is 16.15.*/
  d = ((((1 << 12) + k1) << 12) + k2 + 256) >> 9;
  /*a is 0.32, since d is larger than both 1.0 and k2.*/
  a = (k2 << 23)/d;
  /*ik2 is 25.24.*/
  ik2 = one48 / k2;
  /*b1 is Q56; in practice, the integer ranges between -2 and 2.*/
  b1 = 2*a*(ik2 - (1<<24));
  /*b2 is Q56; in practice, the integer ranges between -2 and 2.*/
  b2 = (one48 << 8) - ((4*a) << 24) - b1;
  /*All of the filter parameters are Q24.*/
  f->c[0] = (int32_t)((b1 + ((int64_t)1 << 31)) >> 32);
  f->c[1] = (int32_t)((b2 + ((int64_t)1 << 31)) >> 32);
  f->g = (int32_t)((a + 128) >> 8);
}

/*Initialize a 2nd order low-pass Bessel filter with the corresponding delay
   and initial value.
  value is Q24.*/
static void od_iir_bessel2_init(od_iir_bessel2 *f, int delay, int32_t value) {
  od_iir_bessel2_reinit(f, delay);
  f->y[1] = f->y[0] = f->x[1] = f->x[0] = value;
}

static int64_t od_iir_bessel2_update(od_iir_bessel2 *f, int32_t x) {
  int64_t c0;
  int64_t c1;
  int64_t g;
  int64_t x0;
  int64_t x1;
  int64_t y0;
  int64_t y1;
  int64_t ya;
  c0 = f->c[0];
  c1 = f->c[1];
  g = f->g;
  x0 = f->x[0];
  x1 = f->x[1];
  y0 = f->y[0];
  y1 = f->y[1];
  ya = ((x + x0*2 + x1)*g + y0*c0 + y1*c1 + (1<<23)) >> 24;
  f->x[1] = (int32_t)x0;
  f->x[0] = x;
  f->y[1] = (int32_t)y0;
  f->y[0] = (int32_t)ya;
  return ya;
}

static void od_enc_rc_reset(od_enc_ctx *enc) {
  int64_t npixels;
  int64_t ibpp;
  enc->rc.bits_per_frame = enc->rc.target_bitrate/enc->state.info.framerate;
  /*Insane framerates or frame sizes mean insane bitrates.
    Let's not get carried away.*/
  if(enc->rc.bits_per_frame > 0x400000000000LL) {
    enc->rc.bits_per_frame = (int64_t)0x400000000000LL;
  }
  else {
    if (enc->rc.bits_per_frame < 32) {
      enc->rc.bits_per_frame = 32;
    }
  }
  enc->rc.reservoir_frame_delay = OD_MAXI(enc->rc.reservoir_frame_delay, 12);
  enc->rc.reservoir_max = enc->rc.bits_per_frame*enc->rc.reservoir_frame_delay;
  /*Start with a buffer fullness and fullness target of 50% */
  enc->rc.reservoir_target = (enc->rc.reservoir_max + 1) >> 1;
  enc->rc.reservoir_fullness = enc->rc.reservoir_target;
  /*Pick exponents and initial scales for quantizer selection.*/
  npixels = enc->state.frame_width *
   (int64_t)enc->state.frame_height;
  enc->rc.log_npixels = od_blog64(npixels);
  ibpp = npixels/enc->rc.bits_per_frame;
  /*All of these initial scale/exp values are from Theora, and have not yet
     been adapted to Daala, so they're certainly wrong.
    The B-frame values especially are simply copies of the P-frame values.*/
  if (ibpp < 1) {
    enc->rc.exp[OD_I_FRAME] = 59;
    enc->rc.log_scale[OD_I_FRAME] = od_blog64(1997) - OD_Q57(OD_COEFF_SHIFT);
  }
  else if (ibpp < 2) {
    enc->rc.exp[OD_I_FRAME] = 55;
    enc->rc.log_scale[OD_I_FRAME] = od_blog64(1604) - OD_Q57(OD_COEFF_SHIFT);
  }
  else {
    enc->rc.exp[OD_I_FRAME] = 48;
    enc->rc.log_scale[OD_I_FRAME] = od_blog64(834) - OD_Q57(OD_COEFF_SHIFT);
  }
  if (ibpp < 4) {
    enc->rc.exp[OD_P_FRAME] = 100;
    enc->rc.log_scale[OD_P_FRAME] = od_blog64(2249) - OD_Q57(OD_COEFF_SHIFT);
  }
  else if (ibpp < 8) {
    enc->rc.exp[OD_P_FRAME] = 95;
    enc->rc.log_scale[OD_P_FRAME] = od_blog64(1751) - OD_Q57(OD_COEFF_SHIFT);
  }
  else {
    enc->rc.exp[OD_P_FRAME] = 73;
    enc->rc.log_scale[OD_P_FRAME] = od_blog64(1260) - OD_Q57(OD_COEFF_SHIFT);
  }
  /*Golden P-frames both use the same log_scale and exp modeling
     values as regular P-frames and the same scale follower.
    For convenience in the rate calculation code, we maintain a copy of
    the scale and exp values in OD_GOLDEN_P_FRAME.*/
  enc->rc.exp[OD_GOLDEN_P_FRAME] = enc->rc.exp[OD_P_FRAME];
  enc->rc.log_scale[OD_GOLDEN_P_FRAME] = enc->rc.log_scale[OD_P_FRAME];
  /*We clamp the actual I and B frame delays to a minimum of 10 to work within
     the range of values where later incrementing the delay works as designed.
    10 is not an exact choice, but rather a good working trade-off.*/
  enc->rc.inter_p_delay = 10;
  enc->rc.inter_b_delay = 10;
  enc->rc.inter_delay_target = enc->rc.reservoir_frame_delay >> 1;
  memset(enc->rc.frame_count, 0, sizeof(enc->rc.frame_count));
  /*Drop-frame tracking is concerned with more than just the basic three frame
     types.
    It needs to track boosted and cut subtypes (of which there is only one
     right now, OD_GOLDEN_P_FRAME). */
  enc->rc.prev_drop_count[OD_I_FRAME] = 0;
  enc->rc.log_drop_scale[OD_I_FRAME] = OD_Q57(0);
  enc->rc.prev_drop_count[OD_P_FRAME] = 0;
  enc->rc.log_drop_scale[OD_P_FRAME] = OD_Q57(0);
  enc->rc.prev_drop_count[OD_GOLDEN_P_FRAME] = 0;
  enc->rc.log_drop_scale[OD_GOLDEN_P_FRAME] = OD_Q57(0);
  enc->rc.prev_drop_count[OD_ALTREF_P_FRAME] = 0;
  enc->rc.log_drop_scale[OD_ALTREF_P_FRAME] = OD_Q57(0);
  /*Set up second order followers, initialized according to corresponding
     time constants.*/
  od_iir_bessel2_init(&enc->rc.scalefilter[OD_I_FRAME], 4,
   od_q57_to_q24(enc->rc.log_scale[OD_I_FRAME]));
  od_iir_bessel2_init(&enc->rc.scalefilter[OD_P_FRAME],enc->rc.inter_p_delay,
   od_q57_to_q24(enc->rc.log_scale[OD_P_FRAME]));
  od_iir_bessel2_init(&enc->rc.vfrfilter[OD_I_FRAME],4,
   od_bexp64_q24(enc->rc.log_drop_scale[OD_I_FRAME]));
  od_iir_bessel2_init(&enc->rc.vfrfilter[OD_P_FRAME],4,
   od_bexp64_q24(enc->rc.log_drop_scale[OD_P_FRAME]));
  od_iir_bessel2_init(&enc->rc.vfrfilter[OD_GOLDEN_P_FRAME],4,
   od_bexp64_q24(enc->rc.log_drop_scale[OD_GOLDEN_P_FRAME]));
  od_iir_bessel2_init(&enc->rc.vfrfilter[OD_ALTREF_P_FRAME],4,
   od_bexp64_q24(enc->rc.log_drop_scale[OD_ALTREF_P_FRAME]));
}

int od_enc_rc_resize(od_enc_ctx *enc) {
  /*If encoding has not yet begun, reset the buffer state.*/
  if (enc->state.cur_time == 0) {
    od_enc_rc_reset(enc);
  }
  else {
    int idt;
    /*Otherwise, update the bounds on the buffer, but not the current
       fullness.*/
    enc->rc.bits_per_frame = enc->rc.target_bitrate/enc->state.info.framerate;
    /*Insane framerates or frame sizes mean insane bitrates.
      Let's not get carried away.*/
    if (enc->rc.bits_per_frame > 0x400000000000LL) {
      enc->rc.bits_per_frame = (int64_t)0x400000000000LL;
    }
    else {
      if (enc->rc.bits_per_frame < 32) {
        enc->rc.bits_per_frame = 32;
      }
    }
    enc->rc.reservoir_frame_delay = OD_MAXI(enc->rc.reservoir_frame_delay,12);
    enc->rc.reservoir_max =
     enc->rc.bits_per_frame*enc->rc.reservoir_frame_delay;
    enc->rc.reservoir_target =
     ((enc->rc.reservoir_max + 1) >> 1) + ((enc->rc.bits_per_frame + 2) >> 2)*
     OD_MINI(enc->state.info.keyframe_rate, enc->rc.reservoir_frame_delay);
    /*Update the INTER-frame scale filter delay.
      We jump to it immediately if we've already seen enough frames; otherwise
       it is simply set as the new target.*/
    enc->rc.inter_delay_target = idt
     = OD_MAXI(enc->rc.reservoir_frame_delay >> 1, 10);
    if (idt < OD_MINI(enc->rc.inter_p_delay,
     enc->rc.frame_count[OD_P_FRAME])) {
      od_iir_bessel2_init(&enc->rc.scalefilter[OD_P_FRAME], idt,
       enc->rc.scalefilter[OD_P_FRAME].y[0]);
      enc->rc.inter_p_delay = idt;
    }
  }
  return OD_SUCCESS;
}

int od_enc_rc_init(od_enc_ctx *enc, long bitrate) {
  od_rc_state *rc;
  if(enc->state.info.framerate <= 0)
    return OD_EINVAL;
  rc = &enc->rc;
  //fprintf(stderr, "RC_INIT   = %li %i %f\n", bitrate, enc->quality, enc->state.info.framerate);
  if (rc->target_bitrate > 0) {
    /*State has already been initialized; rather than reinitialize,
      adjust the buffering for the new target rate. */
    rc->target_bitrate = bitrate;
    return od_enc_rc_resize(enc);
  }
  enc->target_quantizer = 0;
  rc->target_bitrate = bitrate;
  rc->rate_bias = 0;
  if (bitrate > 0) {
    /*The buffer size is set equal to 1.5x the keyframe interval, clamped to the
       range [12,256] frames.
      The interval is short enough to allow reaction, but long enough to allow
       looking into the next GOP (avoiding the case where the last frames
       before an I-frame get starved).
      The 12 frame minimum gives us some chance to distribute bit estimation
       errors in the worst case.
      The 256 frame maximum means we'll require 8-10 seconds of pre-buffering
      at 24-30 fps, which is not unreasonable.*/
    rc->reservoir_frame_delay = enc->state.info.keyframe_rate*1.5 > 256 ? 256 :
     enc->state.info.keyframe_rate*1.5;
    /*By default, enforce hard buffer constraints.*/
    rc->drop_frames=1;
    rc->cap_overflow=1;
    rc->cap_underflow=0;
    rc->twopass_state=0;
    od_enc_rc_reset(enc);
  }
  return OD_SUCCESS;
}

void od_enc_rc_clear(od_enc_ctx *enc) {
  /*No-op until we get to two-pass support.*/
  OD_UNUSED(enc);
}

/*Scale the number of frames by the number of expected drops/duplicates.*/
static int od_rc_scale_drop(od_rc_state *rc, int frame_type, int nframes) {
  if(rc->prev_drop_count[frame_type] > 0 ||
   rc->log_drop_scale[frame_type] > OD_Q57(0)) {
    int64_t dup_scale;
    dup_scale = od_bexp64(((rc->log_drop_scale[frame_type] +
     od_blog64(rc->prev_drop_count[frame_type] + 1)) >> 1) + OD_Q57(8));
    if (dup_scale < nframes << 8) {
      int dup_scalei;
      dup_scalei = (int)dup_scale;
      if (dup_scalei > 0) {
        nframes = ((nframes << 8) + dup_scalei - 1)/dup_scalei;
      }
    }
    else {
      nframes = !!nframes;
    }
  }
  return nframes;
}

/*Closed form version of frame determination code.
  Used by rate control to predict frame types and subtypes into the future.
  No side effects, may be called any number of times.
  Note that it ignores end-of-file conditions; one-pass planning *should*
   ignore end-of-file. */
int od_frame_type(daala_enc_ctx *enc, int64_t coding_frame_count, int *is_golden,
                  int *is_altref, int64_t *ip_count) {
  int frame_type;
  if (coding_frame_count == 0) {
    *is_golden = 1;
    *is_altref = 1;
    *ip_count = 0;
    frame_type = OD_I_FRAME;
  }
  else {
    int keyrate = enc->input_queue.keyframe_rate;
    if (enc->input_queue.closed_gop) {
      int ip_per_gop;
      int gop_n;
      int gop_i;
      ip_per_gop = (keyrate - 1)/2;
      gop_n = coding_frame_count/keyrate;
      gop_i = coding_frame_count - gop_n*keyrate;
      *ip_count = gop_n * ip_per_gop + (gop_i > 0) +
       (gop_i -1);
      frame_type = gop_i == 0 ? OD_I_FRAME : OD_P_FRAME;
    }
    else {
      int ip_per_gop;
      int gop_n;
      int gop_i;
      ip_per_gop = (keyrate);
      gop_n = (coding_frame_count - 1)/keyrate;
      gop_i = coding_frame_count - gop_n*keyrate - 1;
      *ip_count = (coding_frame_count > 0) + gop_n * ip_per_gop +
       (gop_i);
      frame_type =
       gop_i / 1 < ip_per_gop - 1 ? OD_P_FRAME : OD_I_FRAME;
    }
  }
  *is_golden = *ip_count %
   (enc->input_queue.goldenframe_rate/(enc->b_frames + 1)) == 0 || frame_type == OD_I_FRAME;
  *is_altref = *ip_count %
   (enc->input_queue.altref_rate/(enc->b_frames + 1)) == 0 || frame_type == OD_I_FRAME;
  //fprintf(stderr, "Frametype = %i\n", frame_type);
  return frame_type;
}

/*Count frames types forward from the current frame up to but not including
   the last I-frame in reservoir_frame_delay.
  If reservoir_frame_delay contains no I-frames (or the current frame is the
   only I-frame), count all reservoir_frame_delay frames.
  Returns the number of frames counted.
  Right now, this implementation is simple, brute-force, and expensive.
  It is also easy to understand and debug.
  TODO: replace with a virtual FIFO that keeps running totals as
   repeating the counting over-and-over will have a performance impact on
   whole-file 2pass usage.*/
static int frame_type_count(od_enc_ctx *enc, int nframes[OD_FRAME_NSUBTYPES]) {
  int i;
  int j;
  int acc[OD_FRAME_NSUBTYPES];
  int count;
  int reservoir_frames;
  int reservoir_frame_delay;
  memset(nframes, 0, OD_FRAME_NSUBTYPES*sizeof(*nframes));
  memset(acc, 0, sizeof(acc));
  count = 0;
  reservoir_frames = 0;
#if 1
  /*Go ahead and count past end-of-stream.
    We won't nail the exact bitrate on short files that end with a partial
     GOP, but we also won't [potentially] destroy the quality of the last few
     frames in that same case when we suddenly find out the stream is ending
     before the original planning horizon.*/
  reservoir_frame_delay = enc->rc.reservoir_frame_delay;
#else
  /*Don't count past the end of the stream (once we know where end-of-stream
     is).*/
  reservoir_frame_delay = enc->input_queue.end_of_input ?
    enc->input_queue.input_size + 1 : enc->rc.reservoir_frame_delay;
#endif
  for (i = 0; i < reservoir_frame_delay; i++) {
    int frame_type;
    int is_golden;
    int is_altref;
    int64_t dummy;
    frame_type =
     od_frame_type(enc, enc->curr_coding_order + i, &is_golden, &is_altref, &dummy);
    switch (frame_type) {
      case OD_I_FRAME: {
        for (j=0; j<OD_FRAME_NSUBTYPES; j++) nframes[j] += acc[j];
        reservoir_frames += count;
        memset(acc,0,sizeof(acc));
        acc[OD_I_FRAME] = 1;
        count = 1;
        break;
      }
      case OD_P_FRAME: {
        if (is_golden) {
          ++acc[OD_GOLDEN_P_FRAME];
          ++count;
        }
        else {
          ++acc[OD_P_FRAME];
          ++count;
        }
        break;
      }
    }
  }
  /*If there were no I-frames at all, or only the first frame was an I-frame,
     the accumulators never flushed and still contain the counts for the
     entire buffer.
    In both these cases, we return these counts.
    Otherwise, we discard what remains in the accumulators as they contain
     the counts from and past the last I-frame.*/
  if (reservoir_frames == 0) {
    for (i=0; i<OD_FRAME_NSUBTYPES; i++) nframes[i] = acc[i];
    reservoir_frames += count;
  }
  return reservoir_frames;
}

static int quality_to_quantizer(int quality, int bit_depth) {
  if (quality < 96) /* Linear region for low quantizers */
    return (quality << OD_COEFF_SHIFT >> OD_QUALITY_SHIFT) - (quality >> 2);
  else
    return av1_convert_qindex_to_q(quality, bit_depth) + 60;
}

int od_enc_rc_select_quantizers_and_lambdas(od_enc_ctx *enc,
 int is_golden_frame, int is_altref_frame, int frame_type, int *bottom_idx, int *top_idx) {
  int frame_subtype;
  int lossy_quantizer_min;
  int lossy_quantizer_max;
  double mqp_i = OD_MQP_I;
  double mqp_p = OD_MQP_P;
  double mqp_gp = OD_MQP_GP;
  double mqp_ap = OD_MQP_AP;
  int32_t mqp_Q12[OD_FRAME_NSUBTYPES];
  int64_t dqp_Q45[OD_FRAME_NSUBTYPES];
  /*Verify the closed-form frame type determination code matches what the
     input queue set.*/
  /*One pseudo-non-closed-form caveat:
    Once we've seen end-of-input, the batched frame determination code
     suppresses the last open-GOP's I-frame (since it would only be
     useful for the next GOP, which doesn't exist).
     Thus, don't check one the input queue is drained.*/
  if (!enc->input_queue.end_of_input) {
    int closed_form_type;
    int closed_form_golden;
    int closed_form_altref;
    int64_t closed_form_ip_count;
    closed_form_type =
     od_frame_type(enc, enc->curr_coding_order, &closed_form_golden, &closed_form_altref,
     &closed_form_ip_count);
    //fprintf(stderr, "RC_CLOSED = Type: (%i vs %i), Gold: (%i vs %i), Count: (%li vs %li)\n", frame_type, closed_form_type, is_golden_frame, closed_form_golden, enc->ip_frame_count, closed_form_ip_count);
    OD_UNUSED(closed_form_type);
    OD_ASSERT(closed_form_type == frame_type);
    OD_ASSERT(closed_form_ip_count == enc->ip_frame_count);
    OD_ASSERT(closed_form_altref == is_altref_frame);
    OD_ASSERT(closed_form_golden == is_golden_frame);
  }
  /*Quantizer selection sticks to the codable, lossy portion of the quantizer
    range.*/
  lossy_quantizer_min =
    od_codedquantizer_to_quantizer(1);
  lossy_quantizer_max =
    od_codedquantizer_to_quantizer(OD_N_CODED_QUANTIZERS - 1);
  /*P-frames can be golden, and thus boosted.
    Boosted and un-boosted P-frames are treated as different subtypes for
    convenience. */
  frame_subtype = is_golden_frame && frame_type == OD_P_FRAME ?
    OD_GOLDEN_P_FRAME : frame_type;
  /*Stash quantizer modulation by frame type.*/
  mqp_Q12[OD_I_FRAME] = OD_F_Q12(mqp_i);
  mqp_Q12[OD_P_FRAME] = OD_F_Q12(mqp_p);
  mqp_Q12[OD_GOLDEN_P_FRAME] = OD_F_Q12(mqp_gp);
  mqp_Q12[OD_ALTREF_P_FRAME] = OD_F_Q12(mqp_ap);
  dqp_Q45[OD_I_FRAME] = OD_F_Q45(OD_DQP_I);
  dqp_Q45[OD_P_FRAME] = OD_F_Q45(OD_DQP_P);
  dqp_Q45[OD_GOLDEN_P_FRAME] = OD_F_Q45(OD_DQP_GP);
  dqp_Q45[OD_ALTREF_P_FRAME] = OD_F_Q45(OD_DQP_AP);
  /*Is rate control active?*/
  if (enc->rc.target_bitrate <= 0) {
    /*Rate control is not active; derive quantizer directly from
      quality parameter and frame type. */
    /*Can't use the OD_LOSSLESS macro, as it uses state.quantizer to intuit,
      and we've not set it yet.*/
    if (enc->quality == 0) {
      /*Lossless coding requested.*/
      enc->rc.base_quantizer = 0;
      enc->target_quantizer = 0;
      enc->state.coded_quantizer = 0;
      enc->state.quantizer = 0;
    }
    else {
      int quantizer;
      int64_t log_quantizer;

      /* Adjust the modulation constants using the last frame's quantizer. */
      double mqp_delta = (255 - enc->target_quantizer)/2000.0f;
      mqp_i -= mqp_delta;
      mqp_p += mqp_delta;
      mqp_gp -= mqp_delta;
      mqp_Q12[OD_I_FRAME] = OD_F_Q12(mqp_i);
      mqp_Q12[OD_P_FRAME] = OD_F_Q12(mqp_p);
      mqp_Q12[OD_GOLDEN_P_FRAME] = OD_F_Q12(mqp_gp);
      mqp_Q12[OD_ALTREF_P_FRAME] = OD_F_Q12(mqp_ap);

      if (enc->quality == -1) {
        /*A quality of -1 means quality was unset; use a default.*/
        enc->rc.base_quantizer = quality_to_quantizer(10, enc->bit_depth);
      }
      else {
        enc->rc.base_quantizer = quality_to_quantizer(enc->quality, enc->bit_depth);
      }

      if (!is_golden_frame) {
        if (enc->quality < 96) { /* Low quantizers (below quality == 24) are linear */
          int pattern_rate = (enc->input_queue.goldenframe_rate >> 1);
          int dist_to_golden = enc->ip_frame_count % pattern_rate;
          int dist_away_golden = pattern_rate - dist_to_golden;
          int boost = dist_to_golden;
          if (dist_away_golden > dist_to_golden)
              boost = dist_away_golden;
          boost -= pattern_rate;
          boost *= (enc->rc.base_quantizer)/10;
          enc->rc.base_quantizer = enc->rc.base_quantizer + boost;
        } else { /* Ported from the old RC system, works for non-linear higher Qs */
          const double delta_rate[] = { 0.50, 1.0, 0.85, 1.0, 0.70, 1.0, 0.85, 1.0,
                                        0.50, 1.0, 0.85, 1.0, 0.70, 1.0, 0.85, 1.0,
                                        0.50, 1.0, 0.85, 1.0, 0.70, 1.0, 0.85, 1.0, 0.50 };
          const int delta_qindex = av1_compute_qdelta(enc->alt_rc, enc->rc.base_quantizer,
                                                      enc->rc.base_quantizer * delta_rate[enc->ip_frame_count % enc->input_queue.goldenframe_rate], enc->input_queue.goldenframe_rate);
          enc->rc.base_quantizer += delta_qindex;
        }
      }

      /*As originally written, qp modulation is applied to the coded quantizer.
        Because we now have and use a more precise target quantizer for various
        calculation, that needs to be modulated as well.
        Calculate what is, effectively, a fractional coded quantizer. */
      /*Get the log2 quantizer in Q57 (normalized for coefficient shift).*/
      log_quantizer = od_blog64(enc->rc.base_quantizer) -
       OD_Q57(OD_COEFF_SHIFT);
      /*log_quantizer to Q21.*/
      log_quantizer >>= 36;
      /*scale log quantizer, result is Q33.*/
      log_quantizer *= OD_LOG_QUANTIZER_BASE_Q12;
      /*Add Q33 offset to Q33 log_quantizer.*/
      log_quantizer += OD_LOG_QUANTIZER_OFFSET_Q45 >> 12;
      /*Modulate quantizer according to frame type; result is Q45.*/
      log_quantizer *= mqp_Q12[frame_subtype];
      /*Add Q45 boost/cut to Q45 fractional coded quantizer.*/
      log_quantizer += dqp_Q45[frame_subtype];
      /*Back to log2 quantizer in Q57.*/
      log_quantizer = (log_quantizer - OD_LOG_QUANTIZER_OFFSET_Q45)*
       OD_LOG_QUANTIZER_EXP_Q12 + OD_Q57(OD_COEFF_SHIFT);
      /*Convert Q57 log2 quantizer to unclamped linear target quantizer value.*/
      quantizer = od_bexp64(log_quantizer);
      /*Clamp and save target quantizer.*/
      enc->target_quantizer =
        OD_CLAMPI(lossy_quantizer_min, quantizer, lossy_quantizer_max);
      /*Coded quantizer is modulated independently to preserve the
         desired, coarser integer rouding behavior.
        Specifically, we want to make sure that an integer coded quantizer boost
         produces exactly that coded quantizer boost, and doesn't narrowly miss
         high or low.
        That narrow miss is a relatively much larger/smaller change than would
         be made to the target quantizer.*/
      log_quantizer =
       (int64_t)od_quantizer_to_codedquantizer(enc->rc.base_quantizer) << 33;
      log_quantizer *= mqp_Q12[frame_subtype];
      log_quantizer += dqp_Q45[frame_subtype];
      enc->state.coded_quantizer =
       OD_CLAMPI(1, log_quantizer >> 45, OD_N_CODED_QUANTIZERS - 1);
      enc->state.quantizer =
       od_codedquantizer_to_quantizer(enc->state.coded_quantizer);
    }
  }
  else {
    int clamp;
    int reservoir_frames;
    int nframes[OD_FRAME_NSUBTYPES];
    int64_t rate_bias;
    int64_t rate_total;
    int base_quantizer;
    int64_t log_quantizer;
    int qlo;
    int qhi;
    int i;
    /*We clamp the allowed amount of qi change (after initialization).*/
    clamp = enc->state.cur_time > 0;
    /*Figure out how to re-distribute bits so that we hit our fullness target
       before the last keyframe in our current buffer window (after the current
       frame), or the end of the buffer window, whichever comes first.*/
    /*Single pass only right now.*/
    /*Count the various types and classes of frames.*/
    reservoir_frames = frame_type_count(enc, nframes);
    /*Downgrade the delta frame rate to correspond to the recent drop count
       history.
      At the moment, drop frames can only be one frame type at a time:
       B-frames only if B-frames are in use, otherwise P-frames only.
      In the event this is extended later, the drop tracking watches all
       frame types.*/
    nframes[OD_I_FRAME] = od_rc_scale_drop(&enc->rc, OD_I_FRAME,
     nframes[OD_I_FRAME]);
    nframes[OD_P_FRAME] = od_rc_scale_drop(&enc->rc, OD_P_FRAME,
     nframes[OD_P_FRAME]);
    nframes[OD_GOLDEN_P_FRAME] = od_rc_scale_drop(&enc->rc, OD_GOLDEN_P_FRAME,
     nframes[OD_GOLDEN_P_FRAME]);
    nframes[OD_ALTREF_P_FRAME] = od_rc_scale_drop(&enc->rc, OD_ALTREF_P_FRAME,
     nframes[OD_ALTREF_P_FRAME]);
    /*If we've been missing our target, add a penalty term.*/
    rate_bias = (enc->rc.rate_bias/(enc->state.cur_time + 1000))*
     reservoir_frames;
    /*rate_total is the total bits available over the next
       reservoir_frames frames.*/
    rate_total = enc->rc.reservoir_fullness - enc->rc.reservoir_target +
     rate_bias + reservoir_frames*enc->rc.bits_per_frame;
    /*Find a target quantizer that meets our rate target for the specific mix
       of frame types we'll have over the next frame_delay frames.
      We model the rate<->quantizer relationship as:
       rate = scale*(quantizer**-exp)
      In this case, we have our desired rate, an exponent selected in setup,
       and a scale that's been measured over our frame history, so we're
       solving for the quantizer.
      Exponentiation with arbitrary exponents is expensive, so we work in
       the binary log domain (binary exp and log aren't too bad):
       rate = e2(log2_scale - log2_quantizer * exp)
      There's no easy closed form solution, so we bisection search for it.*/
    /*We do not currently allow rate control to select lossless encoding.*/
    qlo = 1;
    /*If there's a quality specified, it's used to select the
       coarsest base quantizer we can select.
      Otherwise we can use up to and including the coarsest codable
       quantizer.*/
    if(enc->quality > 0)
      qhi = quality_to_quantizer(enc->quality, enc->bit_depth);
    else
      qhi = lossy_quantizer_max;
    base_quantizer = (qlo + qhi) >> 1;
    while (qlo < qhi) {
      volatile int64_t log_base_quantizer;
      int64_t diff;
      int64_t bits;
      /*Count bits contributed by each frame type using the model.*/
      bits = 0;
      log_base_quantizer = od_blog64(base_quantizer);
      for (i = 0; i < OD_FRAME_NSUBTYPES; i++) {
        /*Modulate base quantizer by frame type.*/
        /*Get the log2 quantizer in Q57 (normalized for coefficient shift).*/
        log_quantizer = log_base_quantizer - OD_Q57(OD_COEFF_SHIFT);
        /*log_quantizer to Q21.*/
        log_quantizer >>= 36;
        /*scale log quantizer, result is Q33.*/
        log_quantizer *= OD_LOG_QUANTIZER_BASE_Q12;
        /*Add Q33 offset to Q33 log_quantizer.*/
        log_quantizer += OD_LOG_QUANTIZER_OFFSET_Q45 >> 12;
        /*Modulate quantizer according to frame type; result is Q45.*/
        log_quantizer *= mqp_Q12[i];
        /*Add Q45 boost/cut to Q45 fractional coded quantizer.*/
        log_quantizer += dqp_Q45[i];
        /*Back to log2 quantizer in Q57.*/
        log_quantizer = (log_quantizer - OD_LOG_QUANTIZER_OFFSET_Q45)*
          OD_LOG_QUANTIZER_EXP_Q12 + OD_Q57(OD_COEFF_SHIFT);
        /*Clamp modulated quantizer values.*/
        log_quantizer = OD_CLAMPI(
         od_blog64(lossy_quantizer_min),
         log_quantizer,
         od_blog64(lossy_quantizer_max));
        /* All the fields here are Q57 except for the exponent which is Q6.*/
        bits += nframes[i]*od_bexp64(enc->rc.log_scale[i] +
         enc->rc.log_npixels - (log_quantizer >> 6)*enc->rc.exp[i]);
      }
      diff = bits - rate_total;
      if (diff > 0) {
        qlo = base_quantizer + 1;
      }
      else if (diff < 0) {
        qhi = base_quantizer - 1;
      }
      else {
        break;
      }
      base_quantizer = (qlo + qhi) >> 1;
    }
    /*If this was not one of the initial frames, limit the change in base
       quantizer to within [0.8*Q,1.2*Q], where Q is the previous frame's
       base quantizer.*/
    if (clamp) {
      base_quantizer = OD_CLAMPI(
       (enc->rc.base_quantizer*0x0CCCD + 0x8000) >> 16,
       base_quantizer,
       (enc->rc.base_quantizer*0x13333 + 0x8000) >> 16);
    }
    /*Modulate chosen base quantizer to produce target quantizer.*/
    log_quantizer = od_blog64(base_quantizer);
    /*Get the log2 quantizer in Q57 (normalized for coefficient shift).*/
    log_quantizer -= OD_Q57(OD_COEFF_SHIFT);
    /*log_quantizer to Q21.*/
    log_quantizer >>= 36;
    /*scale log quantizer, result is Q33.*/
    log_quantizer *= OD_LOG_QUANTIZER_BASE_Q12;
    /*Add Q33 offset to Q33 log_quantizer.*/
    log_quantizer += OD_LOG_QUANTIZER_OFFSET_Q45 >> 12;
    /*Modulate quantizer according to frame type; result is Q45.*/
    log_quantizer *= mqp_Q12[frame_subtype];
    /*Add Q45 boost/cut to Q45 fractional coded quantizer.*/
    log_quantizer += dqp_Q45[frame_subtype];
    /*Back to log2 quantizer in Q57.*/
    log_quantizer = (log_quantizer - OD_LOG_QUANTIZER_OFFSET_Q45)*
     OD_LOG_QUANTIZER_EXP_Q12 + OD_Q57(OD_COEFF_SHIFT);
    /*Clamp modulated quantizer values.*/
    log_quantizer = OD_CLAMPI(
     od_blog64(lossy_quantizer_min),
     log_quantizer,
     od_blog64(lossy_quantizer_max));
    /*The above allocation looks only at the total rate we'll accumulate in
       the next reservoir_frame_delay frames.
      However we could overflow the bit reservoir on the very next frame, so
       check for that here if we're not using a soft target.*/
    if (enc->rc.cap_overflow) {
      int64_t margin;
      int64_t soft_limit;
      int64_t log_soft_limit;
      int64_t log_scale_pixels;
      int64_t exp;
      int64_t log_qexp;
      /*Allow 3% of the buffer for prediction error.
        This should be plenty, and we don't mind if we go a bit over; we only
         want to keep these bits from being completely wasted.*/
      margin = (enc->rc.reservoir_max + 31) >> 5;
      /*We want to use at least this many bits next frame.*/
      soft_limit = enc->rc.reservoir_fullness + enc->rc.bits_per_frame -
       (enc->rc.reservoir_max - margin);
      log_soft_limit = od_blog64(soft_limit);
      /*If we're predicting we won't use that many bits...*/
      log_scale_pixels = enc->rc.log_scale[frame_subtype] +
       enc->rc.log_npixels;
      exp = enc->rc.exp[frame_subtype];
      log_qexp = (log_quantizer >> 6)*exp;
      if (log_scale_pixels - log_qexp < log_soft_limit) {
        /*Scale the adjustment based on how far into the margin we are.*/
        log_qexp += ((log_scale_pixels - log_soft_limit - log_qexp) >> 32)*
         (OD_MINI(margin, soft_limit) << 32)/margin;
        log_quantizer = (((log_qexp + (exp >> 1))/exp) << 6);
      }
    }
    /*We just checked we don't overflow the reservoir next frame, now check
       we don't underflow and bust the budget (when not using a soft target).
      Disabled when a quality bound is set; if we saturate quantizer to the
       maximum possible size when we have a limiting max quality, the
       resulting lambda can cause strange behavior.*/
    if (enc->quality == -1) {
      int64_t exp;
      int64_t log_qexp;
      int64_t log_scale_pixels;
      int64_t log_hard_limit;
      /*Compute the maximum number of bits we can use in the next frame.
        Allow 50% of the rate for a single frame for prediction error.
        This may not be enough for keyframes or sudden changes in
         complexity.*/
      log_hard_limit = od_blog64(enc->rc.reservoir_fullness +
       (enc->rc.bits_per_frame >> 1));
      /*If we're predicting we'll use more than this...*/
      log_scale_pixels = enc->rc.log_scale[frame_subtype] +
       enc->rc.log_npixels;
      exp = enc->rc.exp[frame_subtype];
      log_qexp = (log_quantizer >> 6)*exp;
      if (log_scale_pixels - log_qexp > log_hard_limit) {
        /*Force the target to hit our limit exactly.*/
        log_qexp = log_scale_pixels - log_hard_limit;
        log_quantizer = (log_qexp + (exp >> 1))/exp << 6;
        /*If that target is unreasonable, oh well; we'll have to drop.*/
        log_quantizer =
         OD_MAXI(log_quantizer, od_blog64(lossy_quantizer_max));
      }
    }
    /*Compute a final estimate of the number of bits we plan to use, update
       the running rate bias measurement.*/
    {
      int64_t log_qexp;
      int64_t log_scale_pixels;
      log_scale_pixels = enc->rc.log_scale[frame_subtype] +
       enc->rc.log_npixels;
      log_qexp = (log_quantizer >> 6)*enc->rc.exp[frame_subtype];
      enc->rc.rate_bias += od_bexp64(log_scale_pixels - log_qexp);
    }
    enc->target_quantizer = od_bexp64(log_quantizer);
    /*The various cappings and adjustments may have altered the log_quantizer
       target significantly.
      We can either update the base quantizer to be consistent with the
       target or let it track separately.
      Theora behavior effectively keeps them consistent, as it regenerates
       the effective base quantizer from the target each frame rather than
       saving both.
      For Daala, it's easier to allow them to track separately.
      For now, allow them to track separately and see how it behaves.*/
    enc->rc.base_quantizer = base_quantizer;
    /*Determine actual quantizer to code/use from the current ideal
       quantizer.*/
    enc->state.coded_quantizer =
     od_quantizer_to_codedquantizer(enc->target_quantizer);
    enc->state.quantizer =
     od_codedquantizer_to_quantizer(enc->state.coded_quantizer);
  }
  /*Generate encoding lambdas from target and actual quantizers.*/
  /*Motion estimation normalized lambda is 2320000 ~= 0.55313
     (or sqrt(0.30595)) in Q22.
    The lower bound of 40 is there because we do not yet consider PVQ noref
     flags during the motion search, so we waste far too many bits trying to
     predict unpredictable areas when lambda is too small.
    Hopefully when we fix that, we can remove the limit.*/
  enc->mv_rdo_lambda =
   OD_MAXI(((2320000 + (((1 << OD_COEFF_SHIFT) - 1) >> 1)) >> OD_COEFF_SHIFT)*
   enc->target_quantizer >> (22 - OD_LAMBDA_SCALE), 40);
  /*We need a normalized PVQ lambda based on the target (not actual) quantizer
     for use within PVQ after we've already performed quantization.*/
  enc->pvq_norm_lambda = OD_PVQ_LAMBDA;
  /*The PVQ RDO lambda is used for RDO calculations involving unquantized
     data.*/
  enc->pvq_rdo_lambda = OD_PVQ_LAMBDA*
   enc->target_quantizer*enc->target_quantizer;
  /*The blocksize RDO lambda is calculated from the PVQ RDO lambda.*/
  enc->bs_rdo_lambda = OD_PVQ_LAMBDA*(1./(1 << OD_BITRES))*
   enc->target_quantizer*enc->target_quantizer;
  /*The deringing filter uses yet another adjusted lambda*/
  enc->dering_lambda = 0.67*OD_PVQ_LAMBDA*
   enc->target_quantizer*enc->target_quantizer;
  *bottom_idx = enc->target_quantizer;
  *top_idx = lossy_quantizer_max;
  //fprintf(stderr, "RC_QUANT  = %i <- Q:%i T:%i C:%i B:%i -> %i\n", lossy_quantizer_min, enc->state.quantizer, enc->target_quantizer, enc->state.coded_quantizer, enc->rc.base_quantizer, lossy_quantizer_max);
  return enc->target_quantizer;
}

int od_enc_rc_update_state(od_enc_ctx *enc, long bits,
 int is_golden_frame, int is_altref_frame, int frame_type, int droppable) {
  int dropped;
  dropped = 0;
  /*Update rate control only if rate control is active.*/
  if (enc->rc.target_bitrate > 0) {
    int64_t log_scale;
    int frame_subtype;
    frame_subtype = frame_type;
    /*Track non-golden and golden P frame drops separately.*/
    if (is_golden_frame && frame_type == OD_P_FRAME)
      frame_subtype = OD_GOLDEN_P_FRAME;
    else if (is_altref_frame && frame_type == OD_P_FRAME)
      frame_subtype = OD_ALTREF_P_FRAME;
    if (bits <= 0) {
      /*We didn't code any blocks in this frame.*/
      log_scale = OD_Q57(-64);
      bits = 0;
      ++enc->rc.prev_drop_count[frame_subtype];
    }
    else {
      od_iir_bessel2 *f;
      int64_t log_bits;
      int64_t log_qexp;
      /*Compute the estimated scale factor for this frame type.*/
      log_bits = od_blog64(bits);
      log_qexp = od_blog64(enc->target_quantizer);
      log_qexp = (log_qexp >> 6)*(enc->rc.exp[frame_type]);
      log_scale = OD_MINI(log_bits - enc->rc.log_npixels + log_qexp,
       OD_Q57(16));
      /*If this is the first example of the given frame type we've
         seen, we immediately replace the default scale factor guess
         with the estimate we just computed using the first frame.*/
      if (enc->rc.frame_count[frame_type] == 0) {
        f = enc->rc.scalefilter + frame_type;
        f->y[1] = f->y[0] = f->x[1] = f->x[0] = od_q57_to_q24(log_scale);
        enc->rc.log_scale[frame_type] = log_scale;
        /*P frame stats are duplicated into a OD_GOLDEN_P_FRAME slot
           for convenience in rate estimation code.*/
        if (frame_type == OD_P_FRAME)
          enc->rc.log_scale[OD_GOLDEN_P_FRAME] = log_scale;
      }
      else {
        /*Lengthen the time constant for the inter filters as we collect more
           frame statistics, until we reach our target.*/
        if (frame_type == OD_P_FRAME &&
         enc->rc.inter_p_delay < enc->rc.inter_delay_target &&
         enc->rc.frame_count[OD_P_FRAME] >= enc->rc.inter_p_delay) {
          od_iir_bessel2_reinit(&enc->rc.scalefilter[OD_P_FRAME],
           ++enc->rc.inter_p_delay);
        }
        /*Update the low-pass scale filter for this frame type
           regardless of whether or not we drop this frame.*/
        enc->rc.log_scale[frame_type] =
         od_iir_bessel2_update(enc->rc.scalefilter + frame_type,
         od_q57_to_q24(log_scale)) << 33;
      }
      /*If this frame busts our budget, it must be dropped.*/
      if (droppable && enc->rc.reservoir_fullness +
       enc->rc.bits_per_frame < bits) {
        ++enc->rc.prev_drop_count[frame_subtype];
        bits = 0;
        dropped = 1;
      }
      else {
        uint32_t drop_count;
        /*Update a low-pass filter to estimate the "real" frame rate taking
           drops into account.
          This is only done if the frame is coded, as it needs the final
           count of dropped frames.*/
        drop_count = enc->rc.prev_drop_count[frame_subtype] + 1;
        if (drop_count > 0x7F) {
          drop_count = 0x7FFFFFFF;
        }
        else {
          drop_count <<= 24;
        }
        enc->rc.log_drop_scale[frame_subtype] =
         od_blog64(od_iir_bessel2_update(enc->rc.vfrfilter + frame_subtype,
         drop_count)) - OD_Q57(24);
        /*Zero the drop count for this frame.
          It will be increased if we drop frames.*/
        enc->rc.prev_drop_count[frame_subtype] = 0;
      }
      /*Increment the frame count for filter adaptation purposes.*/
      if (enc->rc.frame_count[frame_type] < INT_MAX)
       ++enc->rc.frame_count[frame_type];
    }
    enc->rc.reservoir_fullness += enc->rc.bits_per_frame - bits;
    /*If we're too quick filling the buffer and overflow is capped,
      that rate is lost forever.*/
    if (enc->rc.cap_overflow &&
     enc->rc.reservoir_fullness > enc->rc.reservoir_max) {
      enc->rc.reservoir_fullness = enc->rc.reservoir_max;
    }
    /*If we're too quick draining the buffer and underflow is capped,
      don't try to make up that rate later.*/
    if (enc->rc.cap_underflow && enc->rc.reservoir_fullness < 0) {
      enc->rc.reservoir_fullness = 0;
    }
    /*Adjust the bias for the real bits we've used.*/
    enc->rc.rate_bias -= bits;
  }
  //fprintf(stderr, "RC_UPDATE = %li %i %li %li %li\n", bits, dropped, enc->rc.log_scale[frame_type], enc->rc.reservoir_fullness, enc->rc.rate_bias);
  return dropped;
}

int od_enc_rc_2pass_out(od_enc_ctx *enc, unsigned char **buf) {
  OD_UNUSED(enc);
  OD_UNUSED(buf);
  if (enc->rc.target_bitrate <= 0 ||
   (enc->state.cur_time >=0 && enc->rc.twopass_state != 1)) {
    return OD_EINVAL;
  }
  /* Stub to establish API */
  return OD_EIMPL;
}

int od_enc_rc_2pass_in(od_enc_ctx *enc, unsigned char *buf, size_t bytes) {
  OD_UNUSED(enc);
  OD_UNUSED(buf);
  OD_UNUSED(bytes);
  if(enc->rc.target_bitrate <=0 ||
   (enc->state.cur_time >= 0 && enc->rc.twopass_state != 2)) {
    return OD_EINVAL;
  }
  /* Stub to establish API */
  return OD_EIMPL;
}
