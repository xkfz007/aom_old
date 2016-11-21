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

/* clang-format off */

#if !defined(_state_H)
# define _state_H (1)

typedef struct od_state     od_state;
typedef struct od_adapt_ctx od_adapt_ctx;
typedef struct daala_info   daala_info;
typedef struct daala_plane_info daala_plane_info;

# include "generic_code.h"
# include "odintrin.h"
# include "pvq.h"

/*Adaptation speed of scalar Laplace encoding.*/
# define OD_SCALAR_ADAPT_SPEED (4)

struct od_adapt_ctx {
  /* Support for PVQ encode/decode */
  od_pvq_adapt_ctx pvq;

  generic_encoder model_dc[OD_NPLANES_MAX];

  int ex_dc[OD_NPLANES_MAX][OD_TXSIZES][3];
  int ex_g[OD_NPLANES_MAX][OD_TXSIZES];

  /* Joint skip flag for DC and AC */
  uint16_t skip_cdf[OD_TXSIZES*2][4];
  int skip_increment;
};

/** Subsampling factors for a plane as a power of 2.
 *  4:2:0 would have {0, 0} for Y and {1, 1} for Cb and Cr. */
struct daala_plane_info {
  unsigned char xdec;
  unsigned char ydec;
};

/** Configuration parameters for a codec instance. */
struct daala_info {
  unsigned char version_major;
  unsigned char version_minor;
  unsigned char version_sub;
  /** pic_width,_height form a region of interest to encode */
  int32_t pic_width;
  int32_t pic_height;
  uint32_t pixel_aspect_numerator;
  uint32_t pixel_aspect_denominator;
  double framerate;
  uint32_t frame_duration;
  /**The amount to shift to extract the last keyframe number from the granule
   *  position. */
  int keyframe_granule_shift;
  /** bitdepth_mode is one of the three OD_BITDEPTH_MODE_X choices allowed
   * above. */
  int bitdepth_mode;
  /**FPR must be on for high-depth, including lossless high-depth.
     When FPR is on for 8-bit or 10-bit content, lossless frames are still
      stored in reference buffers (and input buffers) with 8 + OD_COEFF_SHIFT
      bit depth to allow streams with mixed lossy and lossless frames. Having a
      mix of reference buffers stored in 10-bit and 12-bit precisions would be
      a disaster, so we keep them all at 12-bit internally.
   */
  int full_precision_references;
  int nplanes;
  daala_plane_info plane_info[OD_NPLANES_MAX];
   /** key frame rate defined how often a key frame is emitted by encoder in
    * number of frames. So 10 means every 10th frame is a keyframe.  */
  int keyframe_rate;
};

struct od_state {
  od_adapt_ctx adapt;
  daala_info info;
  int             quantizer;
  int             coded_quantizer;
  int32_t         frame_width;
  int32_t         frame_height;
  /** Increments by 1 for each frame. */
  int64_t         cur_time;

  /* TODO(yushin): Enable this for activity masking,
     when pvq_qm_q4 is available in AOM. */
  /* unsigned char pvq_qm_q4[OD_NPLANES_MAX][OD_QM_SIZE]; */

  /* Quantization matrices and their inverses. */
  int16_t qm[OD_QM_BUFFER_SIZE];
  int16_t qm_inv[OD_QM_BUFFER_SIZE];
};

void od_adapt_ctx_reset(od_adapt_ctx *state, int is_keyframe);
void od_init_skipped_coeffs(int16_t *d, int16_t *pred, int is_keyframe,
 int bo, int n, int w);

#endif
