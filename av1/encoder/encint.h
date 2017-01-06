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

#if !defined(_encint_H)
# define _encint_H (1)

typedef struct daala_enc_ctx od_enc_ctx;
typedef struct od_params_ctx od_params_ctx;
typedef struct od_rollback_buffer od_rollback_buffer;
typedef struct od_iir_bessel2 od_iir_bessel2;
typedef struct od_input_queue od_input_queue;
typedef struct od_rc_state od_rc_state;

# include "aom_dsp/entenc.h"
# include "av1/common/odintrin.h"
# include "av1/common/pvq_state.h"
# include "av1/encoder/ratectrl_xiph.h"

# define OD_SUCCESS (0)
# define OD_EINVAL (-10)
# define OD_EIMPL (-23)

/*Frame types.*/
# define OD_I_FRAME (0)
# define OD_P_FRAME (1)
# define OD_B_FRAME (2)

# define OD_LAMBDA_SCALE       (2)

/*Frame subtypes that need to be tracked separately by rate control.
  Keep these contiguous with but past the end of the main frame types above.*/
# define OD_GOLDEN_P_FRAME (3)
# define OD_FRAME_NSUBTYPES (4)

/*Constants for frame QP modulation.*/
# define OD_MQP_I (0.98)
# define OD_MQP_P (1.06)
# define OD_MQP_GP (0.99)
# define OD_MQP_B (1.00)
# define OD_DQP_I (-2)
# define OD_DQP_P (0)
# define OD_DQP_GP (-2)
# define OD_DQP_B (1)

/*OD_QUALITY_SHIFT specifies the number of fractional bits in a
   passed in 'quality' parameter.
  For example, an OD_QUALITY_SHIFT of (4) specifies the quality parameter is
   in Q4 format.*/
# define OD_QUALITY_SHIFT (4)

# define OD_MAX_REORDER (16)

/* FIXME */

/*Fractional_coded_quantizer ~=
   log2(quantizer / (1 << OD_COEFF_SHIFT))*6.307 + 6.235*/
/*Base/scale factor for linear quantizer to fractional coded quantizer
   conversion (6.307 * 2^12) */
#define OD_LOG_QUANTIZER_BASE_Q12 (0x0064EB)
/*Inverse of above scale factor.*/
#define OD_LOG_QUANTIZER_EXP_Q12 (0x000289)
/*Offset for linear quantizer to fractional coded quantizer
   conversion (6.235 * 2^45) */
#define OD_LOG_QUANTIZER_OFFSET_Q45 (0x0000C7851EB851ECLL)

/* End of FIXME */

/*A 2nd order low-pass Bessel follower.
  We use this for rate control because it has fast reaction time, but is
   critically damped.*/
struct od_iir_bessel2{
  int32_t c[2];
  int64_t g;
  int32_t x[2];
  int32_t y[2];
};

struct od_input_queue {
  unsigned char *input_img_data;

  /* Circular queue of frame input images in display order. */
  //daala_image images[OD_MAX_REORDER];
  int duration[OD_MAX_REORDER];
  int input_head;
  int input_size;

  /* Circular queue of frame indeces in encode order. */
  //od_input_frame frames[OD_MAX_REORDER];
  int encode_head;
  int encode_size;

  /* Input queue parameters */
  int keyframe_rate;
  int goldenframe_rate;
  int frame_delay;

  /* Input queue state */
  int frame_number;
  int last_keyframe;
  int end_of_input;
  int closed_gop;
};

/*Rate control setup and working state information.*/
struct od_rc_state {
  /*The target bit-rate in bits per second.*/
  long target_bitrate;
  /*The number of frames over which to distribute the reservoir usage.*/
  int reservoir_frame_delay;
  /*Will we drop frames to meet bitrate target?*/
  unsigned char drop_frames;
  /*Do we respect the maximum reservoir fullness?*/
  unsigned char cap_overflow;
  /*Can the reservoir go negative?*/
  unsigned char cap_underflow;
  /*The full-precision, unmodulated quantizer upon which
    our modulated quantizers are based.*/
  int base_quantizer;
  /*Two-pass mode state.
    0 => 1-pass encoding.
    1 => 1st pass of 2-pass encoding.
    2 => 2nd pass of 2-pass encoding.*/
  int twopass_state;
  /*The log of the number of pixels in a frame in Q57 format.*/
  int64_t log_npixels;
  /*The target average bits per frame.*/
  int64_t bits_per_frame;
  /*The current bit reservoir fullness (bits available to be used).*/
  int64_t reservoir_fullness;
  /*The target buffer fullness.
    This is where we'd like to be by the last keyframe the appears in the next
     buf_delay frames.*/
  int64_t reservoir_target;
  /*The maximum buffer fullness (total size of the buffer).*/
  int64_t reservoir_max;
  /*The log of estimated scale factor for the rate model in Q57 format.*/
  int64_t log_scale[OD_FRAME_NSUBTYPES];
  /*The exponent used in the rate model in Q8 format.*/
  unsigned exp[OD_FRAME_NSUBTYPES];
  /*The log of an estimated scale factor used to obtain the real framerate, for
     VFR sources or, e.g., 12 fps content doubled to 24 fps, etc.*/
  int64_t log_drop_scale[OD_FRAME_NSUBTYPES];
  /*The total drop count from the previous frame.*/
  uint32_t prev_drop_count[OD_FRAME_NSUBTYPES];
  /*Second-order lowpass filters to track scale and VFR/drops.*/
  od_iir_bessel2 scalefilter[OD_FRAME_NSUBTYPES];
  od_iir_bessel2 vfrfilter[OD_FRAME_NSUBTYPES];
  int frame_count[OD_FRAME_NSUBTYPES];
  int inter_p_delay;
  int inter_b_delay;
  int inter_delay_target;
  /*The total accumulated estimation bias.*/
  int64_t rate_bias;
};

struct daala_enc_ctx{
  void *alt_rc;
  int bit_depth;
  int quality;
  int target_quantizer;
  int b_frames;
  /*Motion estimation RDO lambda.*/
  int mv_rdo_lambda;
  /*The deringing filter RDO lambda.*/
  double dering_lambda;
  /*The blocksize RDO lambda.*/
  double bs_rdo_lambda;
  /*The PVQ RDO lambda is used for RDO calculations involving unquantized
     data.*/
  double pvq_rdo_lambda;
  /* Stores context-adaptive CDFs for PVQ. */
  od_state state;
  /* Daala entropy encoder. */
  od_ec_enc ec;
  /* Setup and state used to drive rate control. */
  od_rc_state rc;
  int use_activity_masking;
  /* Mode of quantization matrice : FLAT (0) or HVS (1) */
  int qm;
  /** Frame delay. */
  int active_altref;
  int frame_delay;
  /** Displaying order of current frame being encoded. */
  int64_t curr_display_order;
  /** Coding order of current frame being encoded. */
  int64_t curr_coding_order;
  /** Number of I or P frames encoded so far, starting from zero. */
  int64_t ip_frame_count;
  /* This structure manages reordering the input frames from display order
      to encode order.
     It currently supports in order B-frames with a periodic out of order
      P-frame specified by enc->b_frames.*/
  od_input_queue input_queue;
  /*Normalized PVQ lambda for use where we've already performed
     quantization.*/
  double pvq_norm_lambda;
  double pvq_norm_lambda_dc;
};

// from daalaenc.h
/**The encoder context.*/
typedef struct daala_enc_ctx daala_enc_ctx;

/** Holds important encoder information so we can roll back decisions */
struct od_rollback_buffer {
  od_ec_enc ec;
  od_adapt_ctx adapt;
};

void od_encode_checkpoint(const daala_enc_ctx *enc, od_rollback_buffer *rbuf);
void od_encode_rollback(daala_enc_ctx *enc, const od_rollback_buffer *rbuf);

#endif
