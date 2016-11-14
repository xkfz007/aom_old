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

#if !defined(_ratectrl_xiph_H)
# define _ratectrl_xiph_H (1)

#include "av1/encoder/encint.h"

int od_enc_rc_init(od_enc_ctx *enc, long bitrate);

void od_enc_rc_select_quantizers_and_lambdas(od_enc_ctx *enc,
 int is_golden_frame, int frame_type);

int od_enc_rc_update_state(od_enc_ctx *enc, long bits,
 int is_golden_frame, int frame_type, int droppable);

int od_enc_rc_resize(od_enc_ctx *enc);

/*No-op until we get to two-pass support.*/
void od_enc_rc_clear(od_enc_ctx *enc);
int od_enc_rc_2pass_out(od_enc_ctx *enc, unsigned char **buf);
int od_enc_rc_2pass_in(od_enc_ctx *enc, unsigned char *buf, size_t bytes);

#endif
