/*
 * jdapistd.c
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2010, 2015-2020, 2022-2026, D. R. Commander.
 * Copyright (C) 2015, Google, Inc.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file contains application interface code for the decompression half
 * of the JPEG library.  These are the "standard" API routines that are
 * used in the normal full-decompression case.  They are not used by a
 * transcoding-only application.  Note that if an application links in
 * jpeg_start_decompress, it will end up linking in the entire decompressor.
 * We thus must separate this file from jdapimin.c to avoid linking the
 * whole decompression library into a transcoder.
 */

#include "jinclude.h"
#if BITS_IN_JSAMPLE != 16 || defined(D_LOSSLESS_SUPPORTED)
#include "jdmainct.h"
#include "jdcoefct.h"
#else
#define JPEG_INTERNALS
#include "jpeglib.h"
#endif
#include "jdmaster.h"
#include "jdmerge.h"
#include "jdsample.h"
#include "jmemsys.h"
#ifdef WITH_PROFILE
#include "tjutil.h"
#endif

#if BITS_IN_JSAMPLE == 8

/* Forward declarations */
LOCAL(boolean) output_pass_setup(j_decompress_ptr cinfo);


/*
 * Decompression initialization.
 * jpeg_read_header must be completed before calling this.
 *
 * If a multipass operating mode was selected, this will do all but the
 * last pass, and thus may take a great deal of time.
 *
 * Returns FALSE if suspended.  The return value need be inspected only if
 * a suspending data source is used.
 */

GLOBAL(boolean)
jpeg_start_decompress(j_decompress_ptr cinfo)
{
  if (cinfo->global_state == DSTATE_READY) {
    /* First call: initialize master control, select active modules.
     * Scan-preview mode drives the buffered-image pipeline internally, so it
     * enables the same full-buffer machinery and state machine.
     */
    if (cinfo->scan_preview)
      cinfo->buffered_image = TRUE;
    jinit_master_decompress(cinfo);
    if (cinfo->buffered_image) {
      /* No more work here; expecting jpeg_start_output (or
       * jpeg_read_scan_preview in scan-preview mode) next.
       */
      cinfo->global_state = DSTATE_BUFIMAGE;
      return TRUE;
    }
    cinfo->global_state = DSTATE_PRELOAD;
  }
  if (cinfo->global_state == DSTATE_PRELOAD) {
    /* If file has multiple scans, absorb them all into the coef buffer */
    if (cinfo->inputctl->has_multiple_scans) {
#ifdef D_MULTISCAN_FILES_SUPPORTED
      for (;;) {
        int retcode;
        /* Call progress monitor hook if present */
        if (cinfo->progress != NULL)
          (*cinfo->progress->progress_monitor) ((j_common_ptr)cinfo);
        /* Absorb some more input */
        retcode = (*cinfo->inputctl->consume_input) (cinfo);
        if (retcode == JPEG_SUSPENDED)
          return FALSE;
        if (retcode == JPEG_REACHED_EOI)
          break;
        /* Advance progress counter if appropriate */
        if (cinfo->progress != NULL &&
            (retcode == JPEG_ROW_COMPLETED || retcode == JPEG_REACHED_SOS)) {
          if (++cinfo->progress->pass_counter >= cinfo->progress->pass_limit) {
            /* jdmaster underestimated number of scans; ratchet up one scan */
            cinfo->progress->pass_limit += (long)cinfo->total_iMCU_rows;
          }
        }
      }
#else
      ERREXIT(cinfo, JERR_NOT_COMPILED);
#endif /* D_MULTISCAN_FILES_SUPPORTED */
    }
    cinfo->output_scan_number = cinfo->input_scan_number;
  } else if (cinfo->global_state != DSTATE_PRESCAN)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);
  /* Perform any dummy output passes, and set up for the final pass */
  return output_pass_setup(cinfo);
}


/*
 * Set up for an output pass, and perform any dummy pass(es) needed.
 * Common subroutine for jpeg_start_decompress and jpeg_start_output.
 * Entry: global_state = DSTATE_PRESCAN only if previously suspended.
 * Exit: If done, returns TRUE and sets global_state for proper output mode.
 *       If suspended, returns FALSE and sets global_state = DSTATE_PRESCAN.
 */

LOCAL(boolean)
output_pass_setup(j_decompress_ptr cinfo)
{
  if (cinfo->global_state != DSTATE_PRESCAN) {
    /* First call: do pass setup */
    (*cinfo->master->prepare_for_output_pass) (cinfo);
    cinfo->output_scanline = 0;
    cinfo->global_state = DSTATE_PRESCAN;
  }
  /* Loop over any required dummy passes */
  while (cinfo->master->is_dummy_pass) {
#ifdef QUANT_2PASS_SUPPORTED
    /* Crank through the dummy pass */
    while (cinfo->output_scanline < cinfo->output_height) {
      JDIMENSION last_scanline;
      /* Call progress monitor hook if present */
      if (cinfo->progress != NULL) {
        cinfo->progress->pass_counter = (long)cinfo->output_scanline;
        cinfo->progress->pass_limit = (long)cinfo->output_height;
        (*cinfo->progress->progress_monitor) ((j_common_ptr)cinfo);
      }
      /* Process some data */
      last_scanline = cinfo->output_scanline;
      if (cinfo->data_precision <= 8) {
        if (cinfo->main->process_data == NULL)
          ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
        (*cinfo->main->process_data) (cinfo, (JSAMPARRAY)NULL,
                                      &cinfo->output_scanline, (JDIMENSION)0);
      } else if (cinfo->data_precision <= 12) {
        if (cinfo->main->process_data_12 == NULL)
          ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
        (*cinfo->main->process_data_12) (cinfo, (J12SAMPARRAY)NULL,
                                         &cinfo->output_scanline,
                                         (JDIMENSION)0);
      } else {
#ifdef D_LOSSLESS_SUPPORTED
        if (cinfo->main->process_data_16 == NULL)
          ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
        (*cinfo->main->process_data_16) (cinfo, (J16SAMPARRAY)NULL,
                                         &cinfo->output_scanline,
                                         (JDIMENSION)0);
#else
        ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
#endif
      }
      if (cinfo->output_scanline == last_scanline)
        return FALSE;           /* No progress made, must suspend */
    }
    /* Finish up dummy pass, and set up for another one */
    (*cinfo->master->finish_output_pass) (cinfo);
    (*cinfo->master->prepare_for_output_pass) (cinfo);
    cinfo->output_scanline = 0;
#else
    ERREXIT(cinfo, JERR_NOT_COMPILED);
#endif /* QUANT_2PASS_SUPPORTED */
  }
  /* Ready for application to drive output pass through
   * _jpeg_read_scanlines or _jpeg_read_raw_data.
   */
  cinfo->global_state = cinfo->raw_data_out ? DSTATE_RAW_OK : DSTATE_SCANNING;
  return TRUE;
}

#endif /* BITS_IN_JSAMPLE == 8 */


#if BITS_IN_JSAMPLE != 16

/*
 * Enable partial scanline decompression
 *
 * Must be called after jpeg_start_decompress() and before any calls to
 * _jpeg_read_scanlines() or _jpeg_skip_scanlines().
 *
 * Refer to libjpeg.txt for more information.
 */

GLOBAL(void)
_jpeg_crop_scanline(j_decompress_ptr cinfo, JDIMENSION *xoffset,
                    JDIMENSION *width)
{
  int ci, align, orig_downsampled_width;
  JDIMENSION input_xoffset;
  boolean reinit_upsampler = FALSE;
  jpeg_component_info *compptr;
#ifdef UPSAMPLE_MERGING_SUPPORTED
  my_master_ptr master = (my_master_ptr)cinfo->master;
#endif

  if (cinfo->data_precision != BITS_IN_JSAMPLE)
    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);

  if (cinfo->master->lossless || cinfo->raw_data_out)
    ERREXIT(cinfo, JERR_NOTIMPL);

  if ((cinfo->global_state != DSTATE_SCANNING &&
       cinfo->global_state != DSTATE_BUFIMAGE) || cinfo->output_scanline != 0)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);

  if (!xoffset || !width)
    ERREXIT(cinfo, JERR_BAD_CROP_SPEC);

  /* xoffset and width must fall within the output image dimensions. */
  if (*width == 0 ||
      (unsigned long long)(*xoffset) + *width > cinfo->output_width)
    ERREXIT(cinfo, JERR_WIDTH_OVERFLOW);

  /* No need to do anything if the caller wants the entire width. */
  if (*width == cinfo->output_width)
    return;

  /* Ensuring the proper alignment of xoffset is tricky.  At minimum, it
   * must align with an MCU boundary, because:
   *
   *   (1) The IDCT is performed in blocks, and it is not feasible to modify
   *       the algorithm so that it can transform partial blocks.
   *   (2) Because of the SIMD extensions, any input buffer passed to the
   *       upsampling and color conversion routines must be aligned to the
   *       SIMD word size (for instance, 128-bit in the case of SSE2.)  The
   *       easiest way to accomplish this without copying data is to ensure
   *       that upsampling and color conversion begin at the start of the
   *       first MCU column that will be inverse transformed.
   *
   * In practice, we actually impose a stricter alignment requirement.  We
   * require that xoffset be a multiple of the maximum MCU column width of all
   * of the components (the "iMCU column width.")  This is to simplify the
   * single-pass decompression case, allowing us to use the same MCU column
   * width for all of the components.
   */
  if (cinfo->comps_in_scan == 1 && cinfo->num_components == 1)
    align = cinfo->_min_DCT_scaled_size;
  else
    align = cinfo->_min_DCT_scaled_size * cinfo->max_h_samp_factor;

  /* Adjust xoffset to the nearest iMCU boundary <= the requested value */
  input_xoffset = *xoffset;
  *xoffset = (input_xoffset / align) * align;

  /* Adjust the width so that the right edge of the output image is as
   * requested (only the left edge is altered.)  It is important that calling
   * programs check this value after this function returns, so that they can
   * allocate an output buffer with the appropriate size.
   */
  *width = *width + input_xoffset - *xoffset;
  cinfo->output_width = *width;
#ifdef UPSAMPLE_MERGING_SUPPORTED
  if (master->using_merged_upsample && cinfo->max_v_samp_factor == 2) {
    my_merged_upsample_ptr upsample = (my_merged_upsample_ptr)cinfo->upsample;
    upsample->out_row_width =
      cinfo->output_width * cinfo->out_color_components;
  }
#endif

  /* Set the first and last iMCU columns that we must decompress.  These values
   * will be used in single-scan decompressions.
   */
  cinfo->master->first_iMCU_col = (JDIMENSION)(long)(*xoffset) / (long)align;
  cinfo->master->last_iMCU_col =
    (JDIMENSION)jdiv_round_up((long)(*xoffset + cinfo->output_width),
                              (long)align) - 1;

  for (ci = 0, compptr = cinfo->comp_info; ci < cinfo->num_components;
       ci++, compptr++) {
    int hsf = (cinfo->comps_in_scan == 1 && cinfo->num_components == 1) ?
              1 : compptr->h_samp_factor;

    /* Set downsampled_width to the new output width. */
    orig_downsampled_width = compptr->downsampled_width;
    compptr->downsampled_width =
      (JDIMENSION)jdiv_round_up((long)cinfo->output_width *
                                (long)(compptr->h_samp_factor *
                                       compptr->_DCT_scaled_size),
                                (long)(cinfo->max_h_samp_factor *
                                       cinfo->_min_DCT_scaled_size));
    if (compptr->downsampled_width < 2 &&
#ifdef UPSAMPLE_MERGING_SUPPORTED
        !master->using_merged_upsample &&
#endif
        orig_downsampled_width >= 2)
      reinit_upsampler = TRUE;

    /* Set the first and last iMCU columns that we must decompress.  These
     * values will be used in multi-scan decompressions.
     */
    cinfo->master->first_MCU_col[ci] =
      (JDIMENSION)(long)(*xoffset * hsf) / (long)align;
    cinfo->master->last_MCU_col[ci] =
      (JDIMENSION)jdiv_round_up((long)((*xoffset + cinfo->output_width) * hsf),
                                (long)align) - 1;
  }

  if (reinit_upsampler) {
    cinfo->master->jinit_upsampler_no_alloc = TRUE;
    _jinit_upsampler(cinfo);
    cinfo->master->jinit_upsampler_no_alloc = FALSE;
  }
}

#endif /* BITS_IN_JSAMPLE != 16 */


/*
 * Read some scanlines of data from the JPEG decompressor.
 *
 * The return value will be the number of lines actually read.
 * This may be less than the number requested in several cases,
 * including bottom of image, data source suspension, and operating
 * modes that emit multiple scanlines at a time.
 *
 * Note: we warn about excess calls to _jpeg_read_scanlines() since
 * this likely signals an application programmer error.  However,
 * an oversize buffer (max_lines > scanlines remaining) is not an error.
 */

GLOBAL(JDIMENSION)
_jpeg_read_scanlines(j_decompress_ptr cinfo, _JSAMPARRAY scanlines,
                     JDIMENSION max_lines)
{
#if BITS_IN_JSAMPLE != 16 || defined(D_LOSSLESS_SUPPORTED)
  JDIMENSION row_ctr;

#ifdef D_LOSSLESS_SUPPORTED
  if (cinfo->master->lossless) {
#if BITS_IN_JSAMPLE == 8
    if (cinfo->data_precision > BITS_IN_JSAMPLE || cinfo->data_precision < 2)
#else
    if (cinfo->data_precision > BITS_IN_JSAMPLE ||
        cinfo->data_precision < BITS_IN_JSAMPLE - 3)
#endif
      ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
  } else
#endif
  {
    if (cinfo->data_precision != BITS_IN_JSAMPLE)
      ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
  }

  if (cinfo->global_state != DSTATE_SCANNING)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);
  if (cinfo->output_scanline >= cinfo->output_height) {
    WARNMS(cinfo, JWRN_TOO_MUCH_DATA);
    return 0;
  }

  /* Call progress monitor hook if present */
  if (cinfo->progress != NULL) {
    cinfo->progress->pass_counter = (long)cinfo->output_scanline;
    cinfo->progress->pass_limit = (long)cinfo->output_height;
    (*cinfo->progress->progress_monitor) ((j_common_ptr)cinfo);
  }

  /* Process some data */
  row_ctr = 0;
  if (cinfo->main->_process_data == NULL)
    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
  (*cinfo->main->_process_data) (cinfo, scanlines, &row_ctr, max_lines);
  cinfo->output_scanline += row_ctr;
  return row_ctr;
#else
  ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
  return 0;
#endif
}


#if BITS_IN_JSAMPLE != 16

/* Dummy color convert function used by _jpeg_skip_scanlines() */
LOCAL(void)
noop_convert(j_decompress_ptr cinfo, _JSAMPIMAGE input_buf,
             JDIMENSION input_row, _JSAMPARRAY output_buf, int num_rows)
{
}


/* Dummy quantize function used by _jpeg_skip_scanlines() */
LOCAL(void)
noop_quantize(j_decompress_ptr cinfo, _JSAMPARRAY input_buf,
              _JSAMPARRAY output_buf, int num_rows)
{
}


/*
 * In some cases, it is best to call _jpeg_read_scanlines() and discard the
 * output, rather than skipping the scanlines, because this allows us to
 * maintain the internal state of the context-based upsampler.  In these cases,
 * we set up and tear down a dummy color converter in order to avoid valgrind
 * errors and to achieve the best possible performance.
 */

LOCAL(void)
read_and_discard_scanlines(j_decompress_ptr cinfo, JDIMENSION num_lines)
{
  JDIMENSION n;
#ifdef UPSAMPLE_MERGING_SUPPORTED
  my_master_ptr master = (my_master_ptr)cinfo->master;
#endif
  _JSAMPLE dummy_sample[1] = { 0 };
  _JSAMPROW dummy_row = dummy_sample;
  _JSAMPARRAY scanlines = NULL;
  void (*color_convert) (j_decompress_ptr cinfo, _JSAMPIMAGE input_buf,
                         JDIMENSION input_row, _JSAMPARRAY output_buf,
                         int num_rows) = NULL;
  void (*color_quantize) (j_decompress_ptr cinfo, _JSAMPARRAY input_buf,
                          _JSAMPARRAY output_buf, int num_rows) = NULL;

  if (cinfo->cconvert &&
#ifdef UPSAMPLE_MERGING_SUPPORTED
      !master->using_merged_upsample &&
#endif
      cinfo->cconvert->_color_convert) {
    color_convert = cinfo->cconvert->_color_convert;
    cinfo->cconvert->_color_convert = noop_convert;
    /* This just prevents UBSan from complaining about adding 0 to a NULL
     * pointer.  The pointer isn't actually used.
     */
    scanlines = &dummy_row;
  }

  if (cinfo->quantize_colors && cinfo->cquantize &&
      cinfo->cquantize->_color_quantize) {
    color_quantize = cinfo->cquantize->_color_quantize;
    cinfo->cquantize->_color_quantize = noop_quantize;
  }

#ifdef UPSAMPLE_MERGING_SUPPORTED
  if (master->using_merged_upsample && cinfo->max_v_samp_factor == 2) {
    my_merged_upsample_ptr upsample = (my_merged_upsample_ptr)cinfo->upsample;
    scanlines = &upsample->spare_row;
  }
#endif

  for (n = 0; n < num_lines; n++)
    _jpeg_read_scanlines(cinfo, scanlines, 1);

  if (color_convert)
    cinfo->cconvert->_color_convert = color_convert;

  if (color_quantize)
    cinfo->cquantize->_color_quantize = color_quantize;
}


/*
 * Called by _jpeg_skip_scanlines().  This partially skips a decompress block
 * by incrementing the rowgroup counter.
 */

LOCAL(void)
increment_simple_rowgroup_ctr(j_decompress_ptr cinfo, JDIMENSION rows)
{
  JDIMENSION rows_left;
  my_main_ptr main_ptr = (my_main_ptr)cinfo->main;

  if (!cinfo->upsample->need_context_rows && cinfo->max_v_samp_factor != 1) {
    read_and_discard_scanlines(cinfo, rows);
    return;
  }

  /* Increment the counter to the next row group after the skipped rows. */
  main_ptr->rowgroup_ctr += rows / cinfo->max_v_samp_factor;

  /* Partially skipping a row group would involve modifying the internal state
   * of the upsampler, so read the remaining rows into a dummy buffer instead.
   */
  rows_left = rows % cinfo->max_v_samp_factor;
  cinfo->output_scanline += rows - rows_left;

  read_and_discard_scanlines(cinfo, rows_left);
}

/*
 * Skips some scanlines of data from the JPEG decompressor.
 *
 * The return value will be the number of lines actually skipped.  If skipping
 * num_lines would move beyond the end of the image, then the actual number of
 * lines remaining in the image is returned.  Otherwise, the return value will
 * be equal to num_lines.
 *
 * Refer to libjpeg.txt for more information.
 */

GLOBAL(JDIMENSION)
_jpeg_skip_scanlines(j_decompress_ptr cinfo, JDIMENSION num_lines)
{
  my_main_ptr main_ptr = (my_main_ptr)cinfo->main;
  my_coef_ptr coef = (my_coef_ptr)cinfo->coef;
  my_master_ptr master = (my_master_ptr)cinfo->master;
  JDIMENSION i, x;
  int y;
  JDIMENSION lines_per_iMCU_row, lines_left_in_iMCU_row, lines_after_iMCU_row;
  JDIMENSION lines_to_skip, lines_to_read;

  if (cinfo->data_precision != BITS_IN_JSAMPLE)
    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);

  if (cinfo->master->lossless)
    ERREXIT(cinfo, JERR_NOTIMPL);

  /* Two-pass color quantization is not supported. */
  if (cinfo->quantize_colors && cinfo->two_pass_quantize)
    ERREXIT(cinfo, JERR_NOTIMPL);

  if (cinfo->global_state != DSTATE_SCANNING)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);

  /* Do not skip past the bottom of the image. */
  if ((unsigned long long)cinfo->output_scanline + num_lines >=
      cinfo->output_height) {
    num_lines = cinfo->output_height - cinfo->output_scanline;
    cinfo->output_scanline = cinfo->output_height;
    (*cinfo->inputctl->finish_input_pass) (cinfo);
    cinfo->inputctl->eoi_reached = TRUE;
    return num_lines;
  }

  if (num_lines == 0)
    return 0;

  lines_per_iMCU_row = cinfo->_min_DCT_scaled_size * cinfo->max_v_samp_factor;
  lines_left_in_iMCU_row =
    (lines_per_iMCU_row - (cinfo->output_scanline % lines_per_iMCU_row)) %
    lines_per_iMCU_row;
  lines_after_iMCU_row = num_lines - lines_left_in_iMCU_row;

  /* Skip the lines remaining in the current iMCU row.  When upsampling
   * requires context rows, we need the previous and next rows in order to read
   * the current row.  This adds some complexity.
   */
  if (cinfo->upsample->need_context_rows) {
    /* If the skipped lines would not move us past the current iMCU row, we
     * read the lines and ignore them.  There might be a faster way of doing
     * this, but we are facing increasing complexity for diminishing returns.
     * The increasing complexity would be a by-product of meddling with the
     * state machine used to skip context rows.  Near the end of an iMCU row,
     * the next iMCU row may have already been entropy-decoded.  In this unique
     * case, we will read the next iMCU row if we cannot skip past it as well.
     */
    if ((num_lines < lines_left_in_iMCU_row + 1) ||
        (lines_left_in_iMCU_row <= 1 && main_ptr->buffer_full &&
         lines_after_iMCU_row < lines_per_iMCU_row + 1)) {
      read_and_discard_scanlines(cinfo, num_lines);
      return num_lines;
    }

    /* If the next iMCU row has already been entropy-decoded, make sure that
     * we do not skip too far.
     */
    if (lines_left_in_iMCU_row <= 1 && main_ptr->buffer_full) {
      cinfo->output_scanline += lines_left_in_iMCU_row + lines_per_iMCU_row;
      lines_after_iMCU_row -= lines_per_iMCU_row;
    } else {
      cinfo->output_scanline += lines_left_in_iMCU_row;
    }

    /* If we have just completed the first block, adjust the buffer pointers */
    if (main_ptr->iMCU_row_ctr == 0 ||
        (main_ptr->iMCU_row_ctr == 1 && lines_left_in_iMCU_row > 2))
      set_wraparound_pointers(cinfo);
    main_ptr->buffer_full = FALSE;
    main_ptr->rowgroup_ctr = 0;
    main_ptr->context_state = CTX_PREPARE_FOR_IMCU;
    if (!master->using_merged_upsample) {
      my_upsample_ptr upsample = (my_upsample_ptr)cinfo->upsample;
      upsample->next_row_out = cinfo->max_v_samp_factor;
      upsample->rows_to_go = cinfo->output_height - cinfo->output_scanline;
    }
  }

  /* Skipping is much simpler when context rows are not required. */
  else {
    if (num_lines < lines_left_in_iMCU_row) {
      increment_simple_rowgroup_ctr(cinfo, num_lines);
      return num_lines;
    } else {
      cinfo->output_scanline += lines_left_in_iMCU_row;
      main_ptr->buffer_full = FALSE;
      main_ptr->rowgroup_ctr = 0;
#ifdef UPSAMPLE_MERGING_SUPPORTED
      if (master->using_merged_upsample) {
        my_merged_upsample_ptr upsample =
          (my_merged_upsample_ptr)cinfo->upsample;
        upsample->spare_full = FALSE;
      } else
#endif
      {
        my_upsample_ptr upsample = (my_upsample_ptr)cinfo->upsample;
        upsample->next_row_out = cinfo->max_v_samp_factor;
        upsample->rows_to_go = cinfo->output_height - cinfo->output_scanline;
      }
    }
  }

  /* Calculate how many full iMCU rows we can skip. */
  if (cinfo->upsample->need_context_rows)
    lines_to_skip = ((lines_after_iMCU_row - 1) / lines_per_iMCU_row) *
                    lines_per_iMCU_row;
  else
    lines_to_skip = (lines_after_iMCU_row / lines_per_iMCU_row) *
                    lines_per_iMCU_row;
  /* Calculate the number of lines that remain to be skipped after skipping all
   * of the full iMCU rows that we can.  We will not read these lines unless we
   * have to.
   */
  lines_to_read = lines_after_iMCU_row - lines_to_skip;

  /* For images requiring multiple scans (progressive, non-interleaved, etc.),
   * all of the entropy decoding occurs in jpeg_start_decompress(), assuming
   * that the input data source is non-suspending.  This makes skipping easy.
   */
  if (cinfo->inputctl->has_multiple_scans || cinfo->buffered_image) {
    if (cinfo->upsample->need_context_rows) {
      cinfo->output_scanline += lines_to_skip;
      cinfo->output_iMCU_row += lines_to_skip / lines_per_iMCU_row;
      main_ptr->iMCU_row_ctr += lines_to_skip / lines_per_iMCU_row;
      /* It is complex to properly move to the middle of a context block, so
       * read the remaining lines instead of skipping them.
       */
      read_and_discard_scanlines(cinfo, lines_to_read);
    } else {
      cinfo->output_scanline += lines_to_skip;
      cinfo->output_iMCU_row += lines_to_skip / lines_per_iMCU_row;
      increment_simple_rowgroup_ctr(cinfo, lines_to_read);
    }
#ifdef UPSAMPLE_MERGING_SUPPORTED
    if (master->using_merged_upsample) {
      my_merged_upsample_ptr upsample =
        (my_merged_upsample_ptr)cinfo->upsample;
      upsample->rows_to_go = cinfo->output_height - cinfo->output_scanline;
    } else
#endif
    {
      my_upsample_ptr upsample = (my_upsample_ptr)cinfo->upsample;
      upsample->rows_to_go = cinfo->output_height - cinfo->output_scanline;
    }
    return num_lines;
  }

  /* Skip the iMCU rows that we can safely skip. */
  for (i = 0; i < lines_to_skip; i += lines_per_iMCU_row) {
    for (y = 0; y < coef->MCU_rows_per_iMCU_row; y++) {
      for (x = 0; x < cinfo->MCUs_per_row; x++) {
        /* Calling decode_mcu() with a NULL pointer causes it to discard the
         * decoded coefficients.  This is ~5% faster for large subsets, but
         * it's tough to tell a difference for smaller images.
         */
        if (!cinfo->entropy->insufficient_data)
          cinfo->master->last_good_iMCU_row = cinfo->input_iMCU_row;
#ifdef WITH_PROFILE
        cinfo->master->start = getTime();
#endif
        (*cinfo->entropy->decode_mcu) (cinfo, NULL);
#ifdef WITH_PROFILE
        cinfo->master->entropy_elapsed += getTime() - cinfo->master->start;
        cinfo->master->entropy_mcoeffs +=
          (double)cinfo->blocks_in_MCU * DCTSIZE2 / 1000000.;
#endif
      }
    }
    cinfo->input_iMCU_row++;
    cinfo->output_iMCU_row++;
    if (cinfo->input_iMCU_row < cinfo->total_iMCU_rows)
      start_iMCU_row(cinfo);
    else
      (*cinfo->inputctl->finish_input_pass) (cinfo);
  }
  cinfo->output_scanline += lines_to_skip;

  if (cinfo->upsample->need_context_rows) {
    /* Context-based upsampling keeps track of iMCU rows. */
    main_ptr->iMCU_row_ctr += lines_to_skip / lines_per_iMCU_row;

    /* It is complex to properly move to the middle of a context block, so
     * read the remaining lines instead of skipping them.
     */
    read_and_discard_scanlines(cinfo, lines_to_read);
  } else {
    increment_simple_rowgroup_ctr(cinfo, lines_to_read);
  }

  /* Since skipping lines involves skipping the upsampling step, the value of
   * "rows_to_go" will become invalid unless we set it here.  NOTE: This is a
   * bit odd, since "rows_to_go" seems to be redundantly keeping track of
   * output_scanline.
   */
#ifdef UPSAMPLE_MERGING_SUPPORTED
  if (master->using_merged_upsample) {
    my_merged_upsample_ptr upsample = (my_merged_upsample_ptr)cinfo->upsample;
    upsample->rows_to_go = cinfo->output_height - cinfo->output_scanline;
  } else
#endif
  {
    my_upsample_ptr upsample = (my_upsample_ptr)cinfo->upsample;
    upsample->rows_to_go = cinfo->output_height - cinfo->output_scanline;
  }

  /* Always skip the requested number of lines. */
  return num_lines;
}

/*
 * Alternate entry point to read raw data.
 * Processes exactly one iMCU row per call, unless suspended.
 */

GLOBAL(JDIMENSION)
_jpeg_read_raw_data(j_decompress_ptr cinfo, _JSAMPIMAGE data,
                    JDIMENSION max_lines)
{
  JDIMENSION lines_per_iMCU_row;

  if (cinfo->data_precision != BITS_IN_JSAMPLE)
    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);

  if (cinfo->master->lossless)
    ERREXIT(cinfo, JERR_NOTIMPL);

  if (cinfo->global_state != DSTATE_RAW_OK)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);
  if (cinfo->output_scanline >= cinfo->output_height) {
    WARNMS(cinfo, JWRN_TOO_MUCH_DATA);
    return 0;
  }

  /* Call progress monitor hook if present */
  if (cinfo->progress != NULL) {
    cinfo->progress->pass_counter = (long)cinfo->output_scanline;
    cinfo->progress->pass_limit = (long)cinfo->output_height;
    (*cinfo->progress->progress_monitor) ((j_common_ptr)cinfo);
  }

  /* Verify that at least one iMCU row can be returned. */
  lines_per_iMCU_row = cinfo->max_v_samp_factor * cinfo->_min_DCT_scaled_size;
  if (max_lines < lines_per_iMCU_row)
    ERREXIT(cinfo, JERR_BUFFER_SIZE);

  /* Decompress directly into user's buffer. */
  if (cinfo->coef->_decompress_data == NULL)
    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
  if (!(*cinfo->coef->_decompress_data) (cinfo, data))
    return 0;                   /* suspension forced, can do nothing more */

  /* OK, we processed one iMCU row. */
  cinfo->output_scanline += lines_per_iMCU_row;
  return lines_per_iMCU_row;
}

#endif /* BITS_IN_JSAMPLE != 16 */


#if BITS_IN_JSAMPLE == 8

/* Additional entry points for buffered-image mode. */

#ifdef D_MULTISCAN_FILES_SUPPORTED

/*
 * Initialize for an output pass in buffered-image mode.
 */

GLOBAL(boolean)
jpeg_start_output(j_decompress_ptr cinfo, int scan_number)
{
  if (cinfo->global_state != DSTATE_BUFIMAGE &&
      cinfo->global_state != DSTATE_PRESCAN)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);
  /* Limit scan number to valid range */
  if (scan_number <= 0)
    scan_number = 1;
  if (cinfo->inputctl->eoi_reached && scan_number > cinfo->input_scan_number)
    scan_number = cinfo->input_scan_number;
  cinfo->output_scan_number = scan_number;
  /* Perform any dummy output passes, and set up for the real pass */
  return output_pass_setup(cinfo);
}


/*
 * Finish up after an output pass in buffered-image mode.
 *
 * Returns FALSE if suspended.  The return value need be inspected only if
 * a suspending data source is used.
 */

GLOBAL(boolean)
jpeg_finish_output(j_decompress_ptr cinfo)
{
  if ((cinfo->global_state == DSTATE_SCANNING ||
       cinfo->global_state == DSTATE_RAW_OK) && cinfo->buffered_image) {
    /* Terminate this pass. */
    /* We do not require the whole pass to have been completed. */
    (*cinfo->master->finish_output_pass) (cinfo);
    cinfo->global_state = DSTATE_BUFPOST;
  } else if (cinfo->global_state != DSTATE_BUFPOST) {
    /* BUFPOST = repeat call after a suspension, anything else is error */
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);
  }
  /* Read markers looking for SOS or EOI */
  while (cinfo->input_scan_number <= cinfo->output_scan_number &&
         !cinfo->inputctl->eoi_reached) {
    if ((*cinfo->inputctl->consume_input) (cinfo) == JPEG_SUSPENDED)
      return FALSE;             /* Suspend, come back later */
  }
  cinfo->global_state = DSTATE_BUFIMAGE;
  return TRUE;
}


/*
 * Scan-by-scan preview entry point.
 *
 * This is a self-contained driver around the buffered-image primitives.  On
 * each call it:
 *
 *   1. Absorbs source data via jpeg_consume_input() until either the data
 *      source suspends (JPEG_SUSPENDED) or a whole scan has been consumed
 *      into the coefficient buffer (the next SOS marker or EOI is reached).
 *      A suspension is reported back WITHOUT touching scanlines[], so the
 *      caller's previously rendered preview stays pixel-stable.
 *   2. Only when a complete scan is available does it run an output pass
 *      (jpeg_start_output/jpeg_read_scanlines/jpeg_finish_output), rendering
 *      that scan's full image into the caller's buffer.  Coefficients of a
 *      half-received scan are thus never delivered.
 *
 * The routine is reentrant across suspensions: the input/output interlock and
 * the DSTATE_* states maintained by the lower-level routines remember exactly
 * where processing stopped, and a subsequent call resumes in place.
 *
 * Returns one of the JPEG_PREVIEW_* codes.
 */

GLOBAL(int)
jpeg_read_scan_preview(j_decompress_ptr cinfo, JSAMPARRAY scanlines,
                       JDIMENSION max_lines)
{
  int target_scan = 0;
  boolean last_scan_degraded = FALSE;
  int event;
  JDIMENSION row_ctr;

  if (cinfo->data_precision != BITS_IN_JSAMPLE)
    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
  if (!cinfo->scan_preview)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);
  if (cinfo->raw_data_out)
    ERREXIT(cinfo, JERR_NOTIMPL);
  /* Scan preview relies on the lossy full-coefficient-buffer pipeline. */
  if (cinfo->master != NULL && cinfo->master->lossless)
    ERREXIT(cinfo, JERR_NOTIMPL);
  if (cinfo->global_state != DSTATE_BUFIMAGE &&
      cinfo->global_state != DSTATE_PRESCAN &&
      cinfo->global_state != DSTATE_SCANNING &&
      cinfo->global_state != DSTATE_BUFPOST)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);
  if (max_lines < cinfo->output_height)
    ERREXIT(cinfo, JERR_BUFFER_SIZE);

  /* If a previous call already reached the end of the image, the final image
   * remains in the caller's buffer.  Keep reporting the terminal status
   * without re-rendering (and therefore without touching the buffer).
   */
  if (cinfo->global_state == DSTATE_BUFIMAGE &&
      cinfo->inputctl->eoi_reached)
    return cinfo->preview_truncated ? JPEG_PREVIEW_TRUNCATED :
                                      JPEG_PREVIEW_EOI;

  /* A previous call may have rendered the full image and suspended inside
   * jpeg_finish_output() while reading inter-scan/EOI markers.  Finish that
   * pending output pass before doing anything else.
   */
  if (cinfo->global_state == DSTATE_BUFPOST) {
    if (!jpeg_finish_output(cinfo))
      return JPEG_PREVIEW_SUSPENDED;
    return cinfo->preview_truncated ? JPEG_PREVIEW_TRUNCATED :
           (cinfo->inputctl->eoi_reached ? JPEG_PREVIEW_EOI :
                                           JPEG_PREVIEW_SCAN_READY);
  }

  /* A suspended output-pass setup (DSTATE_PRESCAN) or an in-progress render
   * (DSTATE_SCANNING) resumes directly; the input boundary and target scan
   * were already established.  Only DSTATE_BUFIMAGE starts a fresh cycle.
   */
  if (cinfo->global_state == DSTATE_BUFIMAGE) {
    /* Phase 1: drive the input side up to a completed-scan boundary.
     *
     * A scan is "complete" once consume_data() reports JPEG_SCAN_COMPLETED.
     * The following jpeg_consume_input() reads the inter-scan markers and
     * reports JPEG_REACHED_SOS or JPEG_REACHED_EOI.
     * scan_completed_pending lets us resume that marker read across a
     * suspension.  Only the fully received scan is rendered, so a
     * half-received next scan can never leak into the output.
     */
    if (!cinfo->inputctl->scan_completed_pending) {
      /* Advance through the scan's entropy data until it completes. */
      for (;;) {
        event = jpeg_consume_input(cinfo);
        if (event == JPEG_SUSPENDED)
          return JPEG_PREVIEW_SUSPENDED; /* buffer untouched */
        if (event == JPEG_SCAN_COMPLETED) {
          target_scan = cinfo->input_scan_number;
          last_scan_degraded =
            (cinfo->entropy != NULL && cinfo->entropy->insufficient_data);
          break;
        }
        if (event == JPEG_REACHED_EOI) {
          /* Defensive: EOI without a completed scan (empty/abbreviated). */
          return JPEG_PREVIEW_SUSPENDED;
        }
        /* JPEG_REACHED_SOS and JPEG_ROW_COMPLETED: keep advancing. */
      }
    } else {
      /* Resuming after a suspension while reading the completed scan's
       * trailing markers; the target scan is the one just completed.
       */
      target_scan = cinfo->input_scan_number;
      last_scan_degraded =
        (cinfo->entropy != NULL && cinfo->entropy->insufficient_data);
    }

    /* Read the marker(s) following the completed scan.  This opens the next
     * scan, reaches EOI, or suspends (scan_completed_pending stays set).
     */
    event = jpeg_consume_input(cinfo);
    if (event == JPEG_SUSPENDED)
      return JPEG_PREVIEW_SUSPENDED;

    /* The entropy decoder's insufficient_data flag, sampled at the moment
     * the scan completed, reliably distinguishes a genuine complete scan
     * (the flag stays FALSE) from one whose entropy data ran out before its
     * terminating marker (the flag is TRUE and the remaining MCUs were
     * padded with zeroes).  A degraded scan is not delivered; the previous
     * complete scan remains the best preview.
     */
    if (last_scan_degraded) {
      cinfo->preview_truncated = TRUE;
      target_scan--;
    }
    if (target_scan < 1)
      return JPEG_PREVIEW_TRUNCATED; /* no complete scan ever received */

    /* Phase 2a: set up the output pass.  On suspension the state becomes
     * DSTATE_PRESCAN and a later call resumes via the branch below.
     */
    if (!jpeg_start_output(cinfo, target_scan))
      return JPEG_PREVIEW_SUSPENDED;
  } else if (cinfo->global_state == DSTATE_PRESCAN) {
    /* Resume a suspended output-pass setup. */
    if (!jpeg_start_output(cinfo, cinfo->output_scan_number))
      return JPEG_PREVIEW_SUSPENDED;
  }

  /* Phase 2b: render the complete image for the selected scan.  Reached both
   * by a freshly prepared pass and by a DSTATE_SCANNING re-entry.  Because
   * the whole target scan is already buffered, this pass only suspends at the
   * underlying controller's restart points.
   */
  while (cinfo->output_scanline < cinfo->output_height) {
    row_ctr = 0;
    (*cinfo->main->_process_data) (cinfo,
                                   scanlines + cinfo->output_scanline,
                                   &row_ctr,
                                   cinfo->output_height -
                                     cinfo->output_scanline);
    if (row_ctr == 0)
      return JPEG_PREVIEW_SUSPENDED; /* resume on next call */
    cinfo->output_scanline += row_ctr;
  }

  /* Phase 2c: close the output pass and consume markers up to the next
   * SOS/EOI.  On suspension the state becomes DSTATE_BUFPOST for re-entry.
   */
  if (!jpeg_finish_output(cinfo))
    return JPEG_PREVIEW_SUSPENDED;

  return cinfo->preview_truncated ? JPEG_PREVIEW_TRUNCATED :
         (cinfo->inputctl->eoi_reached ? JPEG_PREVIEW_EOI :
                                         JPEG_PREVIEW_SCAN_READY);
}

#endif /* D_MULTISCAN_FILES_SUPPORTED */

#endif /* BITS_IN_JSAMPLE == 8 */
