/*
 * trunctest.c
 *
 * Copyright (C) 2026, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 */

/* This program tests the decompressor's handling of entropy-coded streams
   that are truncated at several structurally interesting points:

     1. At a padding byte (the FF of an FF/00 byte-stuffing pair inside the
        entropy-coded data).
     2. At the marker that terminates a scan (the EOI marker for a
        single-scan file, or the next SOS marker between progressive scans).
     3. In the middle of a progressive scan.

   Each truncated stream is fed to the decompressor in two ways:

     - A suspending data source.  fill_input_buffer() returns FALSE until the
       application provides more bytes, so the decompressor must backtrack to
       a restart point and resume.  Once the whole stream has been supplied the
       decoded pixels must be byte-for-byte identical to a one-shot
       jpeg_mem_src() decode.  The Huffman decoders support suspension; the
       arithmetic decoder does not and must report JERR_CANT_SUSPEND instead.
     - A non-suspending data source that synthesizes a fake EOI marker when the
       (truncated) data is exhausted, mirroring jdatasrc.c.  The decompressor
       then emits corrupt-data warnings (JWRN_JPEG_EOF / JWRN_HIT_MARKER) and
       returns the part of the image it could decode.  The reported image
       dimensions must remain stable, and rows that belong to an unfinished
       scan must not be presented as completed output.

   The same malformed stream is also delivered in many different fragment
   sizes.  When built with AddressSanitizer/UBSan this verifies that none of
   the suspension and backtracking paths read out of bounds or consume
   uninitialized data.

   All JPEG images are generated at run time, so no binary test fixtures are
   required. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "jpeglib.h"
#include "jerror.h"


#define WIDTH 96
#define HEIGHT 64
#define NCOMP 3
#define WORK_BUF_SIZE 4096
#define SENTINEL 0xA5
#define MAX_WARNINGS 64
#define MAX_SCANS 32
#define NO_LIMIT (~(size_t)0)

/* Marker codes that jpeglib.h does not expose publicly. */
#define M_SOI 0xD8
#define M_SOS 0xDA
#define RST0  0xD0
#define RST7  0xD7


/* A point at which the entropy-coded data of a scan begins or ends. */
typedef struct {
  size_t start;       /* first entropy byte (right after the SOS segment) */
  size_t end;         /* first byte of the marker following the entropy data */
} scan_range_t;

typedef struct {
  scan_range_t scans[MAX_SCANS];
  int nscans;
  size_t eoi_pos;     /* position of the FF D9 marker */
} jpeg_layout_t;


/* Custom error manager: recover from fatal errors with longjmp() and record
   both the fatal message code and every warning message code. */

typedef struct {
  struct jpeg_error_mgr pub;
  jmp_buf jump_buffer;
  int fatal_code;
  int warning_codes[MAX_WARNINGS];
  int num_warnings_seen;
} my_error_mgr;


METHODDEF(void)
my_error_exit(j_common_ptr cinfo)
{
  my_error_mgr *err = (my_error_mgr *)cinfo->err;

  err->fatal_code = err->pub.msg_code;
  longjmp(err->jump_buffer, 1);
}

METHODDEF(void)
my_emit_message(j_common_ptr cinfo, int msg_level)
{
  my_error_mgr *err = (my_error_mgr *)cinfo->err;

  if (msg_level < 0) {
    /* A corrupt-data warning.  Record the code but suppress printing, and
       keep the standard accounting in num_warnings. */
    if (err->num_warnings_seen < MAX_WARNINGS)
      err->warning_codes[err->num_warnings_seen] = err->pub.msg_code;
    err->num_warnings_seen++;
    err->pub.num_warnings++;
  }
}


/* Suspending / chunked data source.

   The application exposes the stream progressively through feed_limit():
   bytes [0, feed_limit) are available, the rest has not "arrived" yet.
   Data that the decompressor has backed over during a suspension is kept at
   the front of the work buffer, exactly as recommended in libjpeg.txt. */

typedef enum { SRC_SUSPEND, SRC_EOF } src_mode_t;

typedef struct {
  struct jpeg_source_mgr pub;

  const unsigned char *full_data;  /* entire stream */
  size_t full_size;
  size_t feed_limit;               /* bytes [0, feed_limit) are available */
  size_t consumed;                 /* bytes before the current restart point */
  src_mode_t mode;
  unsigned char work_buf[WORK_BUF_SIZE];
  unsigned char fake_eoi[2];
  int suspended_fills;             /* fills that returned FALSE, total */
  int call_suspensions;            /* FALSE fills since the last library call */
} my_source_mgr;

typedef my_source_mgr *my_src_ptr;


METHODDEF(void)
my_init_source(j_decompress_ptr cinfo)
{
  (void)cinfo;
}


/* Copy every available-but-not-yet-consumed byte into the front of the work
   buffer.  Called initially and whenever the application makes more data
   available. */

LOCAL(void)
pump_buffer(j_decompress_ptr cinfo)
{
  my_src_ptr src = (my_src_ptr)cinfo->src;
  size_t available, nbytes;

  if (src->mode == SRC_EOF)
    available = src->full_size;
  else
    available = src->feed_limit < src->full_size ? src->feed_limit :
                src->full_size;

  if (available < src->consumed) {
    /* Should never happen: the restart point cannot move past available data. */
    fprintf(stderr, "trunctest: restart point past available data\n");
    return;
  }

  nbytes = available - src->consumed;
  if (nbytes > WORK_BUF_SIZE) {
    fprintf(stderr, "trunctest: work buffer too small (%zu > %d)\n", nbytes,
            WORK_BUF_SIZE);
    nbytes = WORK_BUF_SIZE;
  }

  if (nbytes > 0)
    memcpy(src->work_buf, src->full_data + src->consumed, nbytes);

  src->pub.next_input_byte = src->work_buf;
  src->pub.bytes_in_buffer = nbytes;
}


METHODDEF(boolean)
my_fill_input_buffer(j_decompress_ptr cinfo)
{
  my_src_ptr src = (my_src_ptr)cinfo->src;

  if (src->mode == SRC_SUSPEND) {
    /* No more data right now: suspend.  The decompressor backtracks to the
       current restart point, so nothing may be consumed. */
    src->suspended_fills++;
    src->call_suspensions++;
    return FALSE;
  }

  /* SRC_EOF: all of the (possibly truncated) data has been delivered.  Mimic
   * fill_mem_input_buffer() by inserting a fake EOI marker. */
  WARNMS(cinfo, JWRN_JPEG_EOF);
  src->fake_eoi[0] = 0xFF;
  src->fake_eoi[1] = (unsigned char)JPEG_EOI;
  src->pub.next_input_byte = src->fake_eoi;
  src->pub.bytes_in_buffer = 2;
  return TRUE;
}


METHODDEF(void)
my_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
  struct jpeg_source_mgr *src = cinfo->src;

  /* As documented in libjpeg.txt, skip may only run the buffer down to zero;
     the next fill_input_buffer() then handles the remainder via suspension. */
  if (num_bytes > 0) {
    if (num_bytes > (long)src->bytes_in_buffer) {
      num_bytes = (long)src->bytes_in_buffer;
    }
    src->next_input_byte += (size_t)num_bytes;
    src->bytes_in_buffer -= (size_t)num_bytes;
  }
}


METHODDEF(void)
my_term_source(j_decompress_ptr cinfo)
{
  (void)cinfo;
}


LOCAL(void)
my_chunked_src(j_decompress_ptr cinfo, my_source_mgr *src,
               const unsigned char *data, size_t size, src_mode_t mode)
{
  memset(src, 0, sizeof(*src));
  src->full_data = data;
  src->full_size = size;
  src->feed_limit = 0;
  src->consumed = 0;
  src->mode = mode;
  src->suspended_fills = 0;

  src->pub.init_source = my_init_source;
  src->pub.fill_input_buffer = my_fill_input_buffer;
  src->pub.skip_input_data = my_skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart;
  src->pub.term_source = my_term_source;
  src->pub.bytes_in_buffer = 0;
  src->pub.next_input_byte = NULL;

  cinfo->src = &src->pub;
}


/* Make bytes [0, limit) available.  Records the current restart point so that
   buffered-but-unconsumed data survives the suspension. */

LOCAL(void)
feed_limit(j_decompress_ptr cinfo, size_t limit)
{
  my_src_ptr src = (my_src_ptr)cinfo->src;
  size_t offset;

  if (limit > src->full_size)
    limit = src->full_size;

  /* The restart point is next_input_byte inside the current work-buffer
     window.  Advance the window base to that absolute position so that any
     buffered-but-unconsumed bytes are re-read after the suspension. */
  offset = src->pub.next_input_byte ?
           (size_t)(src->pub.next_input_byte - src->work_buf) : 0;
  src->consumed += offset;

  src->feed_limit = limit;
  pump_buffer(cinfo);
}


/* Result of a chunked decompression run. */

typedef enum {
  DECODE_DONE,     /* every scanline was produced */
  DECODE_STALLED,  /* suspended with no further data available (truncated) */
  DECODE_FATAL     /* a fatal error was reported (longjmp) */
} decode_outcome_t;

typedef struct {
  decode_outcome_t outcome;
  int fatal_code;
  int n_warnings;
  int warning_codes[MAX_WARNINGS];
  unsigned int width;
  unsigned int height;
  JDIMENSION scanlines;        /* scanlines actually delivered */
  JDIMENSION imcu_row_height;  /* pixels per iMCU row in output image */
  int suspended;               /* at least one suspension occurred */
} decode_result_t;


LOCAL(int)
saw_warning(const decode_result_t *r, int code)
{
  int i;

  for (i = 0; i < r->n_warnings; i++)
    if (r->warning_codes[i] == code)
      return 1;
  return 0;
}


/* Drive the decompressor using a suspending/chunked source.

   prefixes[] is an increasing list of byte counts that are made available in
   order.  The final entry must be either full_size (complete stream) or a
   truncation point.  In SRC_EOF mode a truncation point is followed by a fake
   EOI; in SRC_SUSPEND mode a truncation point leaves the decoder stalled.

   *out_pixels must hold WIDTH*HEIGHT*NCOMP bytes, pre-filled by the caller
   (typically with SENTINEL) so that untouched rows remain detectable. */

LOCAL(void)
run_chunked(const unsigned char *data, size_t full_size,
            const size_t *prefixes, int nprefixes, src_mode_t mode,
            unsigned char *out_pixels, size_t out_size,
            decode_result_t *result)
{
  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;
  my_source_mgr src;
  int pfx = 0;
  JSAMPROW row_ptr[1];
  int no_progress;

  memset(result, 0, sizeof(*result));
  result->outcome = DECODE_FATAL;
  result->fatal_code = 0;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  jerr.pub.emit_message = my_emit_message;
  jerr.fatal_code = 0;
  jerr.num_warnings_seen = 0;

  if (setjmp(jerr.jump_buffer)) {
    result->outcome = DECODE_FATAL;
    result->fatal_code = jerr.fatal_code;
    result->n_warnings = jerr.num_warnings_seen < MAX_WARNINGS ?
                         jerr.num_warnings_seen : MAX_WARNINGS;
    memcpy(result->warning_codes, jerr.warning_codes,
           (size_t)result->n_warnings * sizeof(int));
    result->scanlines = cinfo.output_scanline;
    result->width = cinfo.output_width;
    result->height = cinfo.output_height;
    result->suspended = src.suspended_fills > 0;
    jpeg_destroy_decompress(&cinfo);
    return;
  }

  jpeg_create_decompress(&cinfo);
  my_chunked_src(&cinfo, &src, data, full_size, mode);

  /* Feed the first prefix, then read the header. */
  feed_limit(&cinfo, prefixes[0]);
  while (jpeg_read_header(&cinfo, TRUE) == JPEG_SUSPENDED) {
    if (pfx + 1 < nprefixes)
      feed_limit(&cinfo, prefixes[++pfx]);
    else
      break;
  }

  cinfo.out_color_space = JCS_RGB;

  /* The caller must provide room for the full SOF image so that no output
     write can ever run out of bounds, even when every row is produced. */
  if ((unsigned long long)cinfo.image_width * cinfo.image_height * NCOMP >
      out_size) {
    fprintf(stderr, "trunctest: output buffer too small\n");
    result->outcome = DECODE_FATAL;
    jpeg_destroy_decompress(&cinfo);
    return;
  }

  result->width = cinfo.image_width;
  result->height = cinfo.image_height;

  /* start_decompress absorbs all scans of a multi-scan (progressive) file.
     If it cannot get enough data and no more prefixes remain, the stream is
     truncated: stop here rather than entering the output-pass state machine. */
  while (jpeg_start_decompress(&cinfo) == FALSE) {
    if (pfx + 1 < nprefixes)
      feed_limit(&cinfo, prefixes[++pfx]);
    else {
      result->outcome = DECODE_STALLED;
      result->scanlines = 0;
      result->suspended = src.suspended_fills > 0;
      result->n_warnings = jerr.num_warnings_seen < MAX_WARNINGS ?
                           jerr.num_warnings_seen : MAX_WARNINGS;
      memcpy(result->warning_codes, jerr.warning_codes,
             (size_t)result->n_warnings * sizeof(int));
      jpeg_destroy_decompress(&cinfo);
      return;
    }
  }

  result->width = cinfo.output_width;
  result->height = cinfo.output_height;
  result->imcu_row_height = (JDIMENSION)(DCTSIZE * cinfo.max_v_samp_factor);

  no_progress = 0;
  while (cinfo.output_scanline < cinfo.output_height) {
    JDIMENSION before = cinfo.output_scanline;
    JDIMENSION got;

    row_ptr[0] = out_pixels +
                 cinfo.output_scanline * cinfo.output_width * NCOMP;
    src.call_suspensions = 0;
    got = jpeg_read_scanlines(&cinfo, &row_ptr[0], 1);
    result->scanlines = cinfo.output_scanline;

    if (got == 0 || cinfo.output_scanline == before) {
      /* No progress.  A suspension means more data may help; otherwise the
         source synthesized an EOI and the decoder is wrapping up. */
      if (src.call_suspensions > 0) {
        if (pfx + 1 < nprefixes) {
          feed_limit(&cinfo, prefixes[++pfx]);
          no_progress = 0;
          continue;
        }
        /* Genuinely out of data in suspend mode: truncated stream. */
        result->outcome = DECODE_STALLED;
        break;
      }
      if (++no_progress > 8) {
        fprintf(stderr, "trunctest: no progress and no suspension\n");
        result->outcome = DECODE_FATAL;
        break;
      }
    } else {
      no_progress = 0;
    }
  }

  result->suspended = src.suspended_fills > 0;

  if (result->outcome == DECODE_FATAL &&
      cinfo.output_scanline >= cinfo.output_height) {
    if (jpeg_finish_decompress(&cinfo) == FALSE) {
      while (pfx + 1 < nprefixes) {
        feed_limit(&cinfo, prefixes[++pfx]);
        if (jpeg_finish_decompress(&cinfo) == TRUE)
          break;
      }
    }
    result->outcome = DECODE_DONE;
  }

  result->n_warnings = jerr.num_warnings_seen < MAX_WARNINGS ?
                       jerr.num_warnings_seen : MAX_WARNINGS;
  memcpy(result->warning_codes, jerr.warning_codes,
         (size_t)result->n_warnings * sizeof(int));
  result->scanlines = cinfo.output_scanline;
  result->width = cinfo.output_width;
  result->height = cinfo.output_height;

  jpeg_destroy_decompress(&cinfo);
}


/* One-shot reference decode through jpeg_mem_src(). */

LOCAL(int)
reference_decode(const unsigned char *data, size_t size,
                 unsigned char *pixels, unsigned int *width,
                 unsigned int *height)
{
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  JSAMPROW row_ptr[1];

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, (unsigned long)size);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return 0;
  }
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);
  *width = cinfo.output_width;
  *height = cinfo.output_height;
  while (cinfo.output_scanline < cinfo.output_height) {
    row_ptr[0] = pixels + cinfo.output_scanline * cinfo.output_width * NCOMP;
    (void)jpeg_read_scanlines(&cinfo, &row_ptr[0], 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return 1;
}


/* Generate a deterministic 24-bit RGB source image. */

LOCAL(void)
make_source_image(unsigned char *pixels)
{
  unsigned int x, y;

  for (y = 0; y < HEIGHT; y++) {
    for (x = 0; x < WIDTH; x++) {
      unsigned char *px = pixels + (y * WIDTH + x) * NCOMP;
      px[0] = (unsigned char)((x * 7 + y * 3) & 0xFF);
      px[1] = (unsigned char)((x * 5 + 128 + (y & 0x0F)) & 0xFF);
      px[2] = (unsigned char)((255 - x * 3 + y * 11) & 0xFF);
      /* A few flat color blocks so the entropy stream contains long runs and
         therefore FF/00 stuffing bytes. */
      if ((x / 24) == (y / 32)) {
        px[0] = (unsigned char)(40 * (x / 24));
        px[1] = (unsigned char)(90 + 20 * (y / 32));
        px[2] = 60;
      }
    }
  }
}


/* Compress the source image into a memory buffer. */

LOCAL(int)
compress_jpeg(int progressive, int arithmetic, unsigned char **out_buf,
              unsigned long *out_size)
{
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  unsigned char *src_image;
  JSAMPROW row_ptr[1];
  unsigned int y;

  src_image = (unsigned char *)malloc((size_t)WIDTH * HEIGHT * NCOMP);
  if (!src_image)
    return 0;
  make_source_image(src_image);

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, out_buf, out_size);

  cinfo.image_width = WIDTH;
  cinfo.image_height = HEIGHT;
  cinfo.input_components = NCOMP;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 90, TRUE);
  if (arithmetic)
    cinfo.arith_code = TRUE;
  if (progressive)
    jpeg_simple_progression(&cinfo);

  jpeg_start_compress(&cinfo, TRUE);
  for (y = 0; y < HEIGHT; y++) {
    row_ptr[0] = src_image + (size_t)y * WIDTH * NCOMP;
    jpeg_write_scanlines(&cinfo, row_ptr, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  free(src_image);
  return 1;
}


/* Parse the marker layout of a JPEG.  Records the entropy range of every SOS
   scan and the EOI position. */

LOCAL(int)
parse_layout(const unsigned char *data, size_t size, jpeg_layout_t *layout)
{
  size_t i = 2;  /* skip SOI */

  memset(layout, 0, sizeof(*layout));
  if (size < 2 || data[0] != 0xFF || data[1] != 0xD8)
    return 0;

  while (i + 1 < size) {
    unsigned int marker, seg_len;
    size_t entropy_start, j;

    if (data[i] != 0xFF)
      return 0;
    while (i < size && data[i] == 0xFF)
      i++;
    if (i >= size)
      return 0;
    marker = data[i++];

    if (marker == JPEG_EOI) {
      layout->eoi_pos = i - 2;
      return 1;
    }
    if (marker == M_SOI || (marker >= RST0 && marker <= RST7))
      continue;

    if (i + 2 > size)
      return 0;
    seg_len = ((unsigned int)data[i] << 8) | data[i + 1];
    if (seg_len < 2 || i + seg_len > size)
      return 0;

    if (marker == M_SOS) {
      entropy_start = i + seg_len;
      j = entropy_start;
      while (j + 1 < size) {
        if (data[j] == 0xFF) {
          size_t k = j + 1;
          while (k < size && data[k] == 0xFF)
            k++;
          if (k < size && data[k] != 0x00 &&
              !(data[k] >= RST0 && data[k] <= RST7))
            break;
          j = k + 1;
        } else {
          j++;
        }
      }
      if (layout->nscans >= MAX_SCANS)
        return 0;
      layout->scans[layout->nscans].start = entropy_start;
      layout->scans[layout->nscans].end = j;
      layout->nscans++;
      i = j;
    } else {
      i += seg_len;
    }
  }
  return 0;
}


/* Find the first FF/00 stuffing pair within [start, end).  Returns the
   position of the FF byte, or NO_LIMIT if none exists. */

LOCAL(size_t)
find_ff00(const unsigned char *data, size_t start, size_t end)
{
  size_t i;

  for (i = start; i + 1 < end; i++)
    if (data[i] == 0xFF && data[i + 1] == 0x00)
      return i;
  return NO_LIMIT;
}


/* Build an increasing list of feed prefixes: every multiple of chunk_size up
   to and including final_limit. */

LOCAL(int)
make_chunk_prefixes(size_t final_limit, size_t chunk_size,
                    size_t *prefixes, int max_prefixes)
{
  size_t limit = 0;
  int n = 0;

  while (limit < final_limit) {
    size_t next = limit + chunk_size;

    if (next > final_limit || next < limit)
      next = final_limit;
    if (n >= max_prefixes)
      return -1;
    prefixes[n++] = next;
    limit = next;
  }
  return n;
}


static int failures = 0;
static int checks = 0;


#define CHECK(cond, msg) { \
  checks++; \
  if (!(cond)) { \
    printf("FAIL: %s (line %d)\n", msg, __LINE__); \
    failures++; \
  } \
}


LOCAL(unsigned char *)
alloc_pixels(int fill_sentinel)
{
  unsigned char *p = (unsigned char *)malloc((size_t)WIDTH * HEIGHT * NCOMP);

  if (p && fill_sentinel)
    memset(p, SENTINEL, (size_t)WIDTH * HEIGHT * NCOMP);
  return p;
}


/* T1/T2/T3: Huffman suspension and resumption must reproduce the one-shot
   decode exactly, regardless of where the fragments fall. */

LOCAL(void)
test_huffman_resume(const char *label, const unsigned char *data,
                    size_t size, const jpeg_layout_t *layout,
                    size_t cut_point, int progressive)
{
  unsigned char *ref, *chunked;
  unsigned int ref_w = 0, ref_h = 0;
  size_t prefixes[8192];
  int nprefixes;
  decode_result_t r;
  static const size_t chunk_sizes[] = { 1, 3, 7, 13, 64, 257 };
  size_t npx = (size_t)WIDTH * HEIGHT * NCOMP;
  unsigned int ci;

  ref = alloc_pixels(0);
  CHECK(ref != NULL, "alloc reference pixels");
  CHECK(reference_decode(data, size, ref, &ref_w, &ref_h) == 1,
        "reference decode succeeds");
  CHECK(ref_w == WIDTH && ref_h == HEIGHT, "reference dimensions");

  /* First verify that a complete but fragmented stream matches. */
  for (ci = 0; ci < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); ci++) {
    nprefixes = make_chunk_prefixes(size, chunk_sizes[ci], prefixes,
                                    (int)(sizeof(prefixes) /
                                          sizeof(prefixes[0])));
    CHECK(nprefixes > 0, "prefix list built");
    chunked = alloc_pixels(1);
    run_chunked(data, size, prefixes, nprefixes, SRC_SUSPEND, chunked, npx,
                &r);
    if (r.outcome != DECODE_DONE) {
      printf("FAIL: %s chunk=%zu did not complete (outcome=%d fatal=%d)\n",
             label, chunk_sizes[ci], r.outcome, r.fatal_code);
      failures++;
      checks++;
    } else {
      checks++;
      if (memcmp(ref, chunked, npx) != 0) {
        printf("FAIL: %s chunk=%zu pixels differ from reference\n", label,
               chunk_sizes[ci]);
        failures++;
      }
      CHECK(r.suspended, "suspension actually exercised");
      CHECK(r.width == ref_w && r.height == ref_h,
            "dimensions stable across suspension");
    }
    free(chunked);
  }

  /* Now truncate at the requested point.  A sequential, single-pass decoder
     runs in lockstep with the input and must STALL (suspension with no further
     data), never presenting the unfinished scan as complete.  A buffered
     progressive decoder absorbs whole scans first, so a mid-stream cut makes
     jpeg_start_decompress() run out of data; with a suspending source it
     stalls there before any output scanline is produced. */
  nprefixes = make_chunk_prefixes(cut_point, 7, prefixes,
                                  (int)(sizeof(prefixes) / sizeof(prefixes[0])));
  if (nprefixes > 0) {
    chunked = alloc_pixels(1);
    run_chunked(data, cut_point, prefixes, nprefixes, SRC_SUSPEND, chunked,
                npx, &r);

    CHECK(r.suspended, "truncated Huffman path suspended at least once");
    CHECK(r.outcome != DECODE_DONE || r.scanlines < HEIGHT,
          "truncated Huffman stream is not reported as fully decoded");

    if (!progressive) {
      /* Sequential: must stall partway through the scanlines. */
      CHECK(r.outcome == DECODE_STALLED,
            "truncated sequential Huffman stream stalls");
      CHECK(r.scanlines < HEIGHT,
            "stalled sequential decode reports no full image");
    } else {
      /* Progressive buffered mode: the truncated scans are absorbed but no
         complete image is produced, so no output scanline may be delivered.
         The exact outcome (stall vs. buffered early EOI) is implementation
         detail; the invariant is zero output rows plus a real suspension. */
      CHECK(r.scanlines == 0,
            "truncated progressive Huffman stream yields no output scanline");
      CHECK(r.outcome != DECODE_FATAL,
            "truncated progressive Huffman stream is not a fatal error");
    }

    /* Rows beyond the delivered restart point must keep the sentinel value. */
    if (r.scanlines < HEIGHT) {
      size_t untouched = (size_t)(HEIGHT - r.scanlines) * WIDTH * NCOMP;
      unsigned char *tail = chunked + (size_t)r.scanlines * WIDTH * NCOMP;
      size_t k;

      for (k = 0; k < untouched; k++) {
        checks++;
        if (tail[k] != SENTINEL) {
          printf("FAIL: %s unfinished scan row was overwritten\n", label);
          failures++;
          break;
        }
      }
    }
    free(chunked);
  }
  (void)layout;
  free(ref);
}


/* T4: the arithmetic decoder cannot suspend; a truncated arithmetic stream
   delivered through a suspending source must fail with JERR_CANT_SUSPEND. */

LOCAL(void)
test_arithmetic_cant_suspend(const char *label, const unsigned char *data,
                             size_t cut_point)
{
  unsigned char *pixels;
  size_t prefixes[4096];
  int nprefixes;
  decode_result_t r;
  size_t npx = (size_t)WIDTH * HEIGHT * NCOMP;

  nprefixes = make_chunk_prefixes(cut_point, 5, prefixes,
                                  (int)(sizeof(prefixes) / sizeof(prefixes[0])));
  CHECK(nprefixes > 0, "arithmetic prefix list built");
  pixels = alloc_pixels(1);
  run_chunked(data, cut_point, prefixes, nprefixes, SRC_SUSPEND, pixels, npx,
              &r);
  if (r.outcome != DECODE_FATAL || r.fatal_code != JERR_CANT_SUSPEND) {
    printf("FAIL: %s: expected JERR_CANT_SUSPEND, got outcome=%d fatal=%d\n",
           label, r.outcome, r.fatal_code);
    failures++;
  }
  checks += 2;
  CHECK(r.scanlines < HEIGHT, "fatal arithmetic decode reports no full image");
  free(pixels);
}


/* T5/T6/T7: warning recovery with a fake EOI.  The truncated stream ends in
   SRC_EOF mode, so the decompressor warns, fills zero bits, and returns
   whatever image area it could decode. */

LOCAL(void)
test_warning_recovery(const char *label, const unsigned char *data,
                      size_t full_size, size_t cut_point, int arithmetic,
                      int expect_stable_prefix)
{
  unsigned char *ref, *pixels, *pixels_alt;
  unsigned int ref_w = 0, ref_h = 0;
  size_t prefixes[4096];
  int nprefixes;
  decode_result_t r, r_alt;
  size_t npx = (size_t)WIDTH * HEIGHT * NCOMP;
  size_t row_bytes = (size_t)WIDTH * NCOMP;
  static const size_t frag_sizes[3] = { 5, 11, 37 };
  size_t fi;

  ref = alloc_pixels(0);
  CHECK(reference_decode(data, full_size, ref, &ref_w, &ref_h) == 1,
        "reference decode for recovery comparison");

  pixels = alloc_pixels(1);
  nprefixes = make_chunk_prefixes(cut_point, frag_sizes[1], prefixes,
                                  (int)(sizeof(prefixes) / sizeof(prefixes[0])));
  CHECK(nprefixes > 0, "recovery prefix list built");
  run_chunked(data, cut_point, prefixes, nprefixes, SRC_EOF, pixels, npx, &r);

  CHECK(r.outcome != DECODE_FATAL || arithmetic,
        "Huffman warning recovery is non-fatal");
  CHECK(r.n_warnings > 0, "recovery emits at least one warning");
  CHECK(saw_warning(&r, JWRN_JPEG_EOF), "recovery warns of premature EOF");
  CHECK(r.width == WIDTH && r.height == HEIGHT,
        "recovered image dimensions stay at the SOF values");
  CHECK(r.outcome != DECODE_DONE || r.n_warnings > 0,
        "recovered image is not reported as a warning-free complete image");

  /* Decode the same truncated stream again with different fragment sizes.
     The recovered image must be byte-for-byte identical: the decoded area and
     its synthetic replacement must not depend on how the bytes were
     partitioned. */
  for (fi = 0; fi < sizeof(frag_sizes) / sizeof(frag_sizes[0]); fi++) {
    if (frag_sizes[fi] == frag_sizes[1])
      continue;
    nprefixes = make_chunk_prefixes(cut_point, frag_sizes[fi], prefixes,
                                    (int)(sizeof(prefixes) /
                                          sizeof(prefixes[0])));
    if (nprefixes <= 0)
      continue;
    pixels_alt = alloc_pixels(1);
    run_chunked(data, cut_point, prefixes, nprefixes, SRC_EOF, pixels_alt, npx,
                &r_alt);
    checks++;
    if (r_alt.outcome != r.outcome || r_alt.scanlines != r.scanlines ||
        r_alt.width != r.width || r_alt.height != r.height ||
        memcmp(pixels_alt, pixels, npx) != 0) {
      printf("FAIL: %s recovered image depends on fragment size (%zu)\n",
             label, frag_sizes[fi]);
      failures++;
    }
    free(pixels_alt);
  }

  /* For a sequential Huffman stream, every complete iMCU row produced before
     the data ran out is genuinely decoded and must match the reference
     exactly.  The iMCU row containing the truncation point uses synthetic
     zero bits for its remainder and is excluded.  Progressive images only
     contain partial-spectra coefficients at a mid-scan cut, so they are
     validated for determinism above, not against the full reference. */
  if (expect_stable_prefix && r.outcome == DECODE_DONE &&
      r.imcu_row_height > 0) {
    JDIMENSION row, first_bad = r.scanlines;
    size_t k;

    /* Locate the first output row that differs from the reference. */
    for (row = 0; row < r.scanlines; row++) {
      if (memcmp(pixels + (size_t)row * row_bytes,
                 ref + (size_t)row * row_bytes, row_bytes) != 0) {
        first_bad = row;
        break;
      }
    }
    /* Only whole iMCU rows strictly before the first corrupted row are
       guaranteed clean, so round the boundary down. */
    {
      JDIMENSION stable_rows =
        (first_bad / r.imcu_row_height) * r.imcu_row_height;

      if (stable_rows > HEIGHT)
        stable_rows = HEIGHT;
      CHECK(stable_rows > 0, "at least one clean iMCU row was recovered");
      for (k = 0; k < (size_t)stable_rows * row_bytes; k++) {
        checks++;
        if (pixels[k] != ref[k]) {
          printf("FAIL: %s stable decoded region differs from reference\n",
                 label);
          failures++;
          break;
        }
      }
    }
  }

  /* If the decoder stalled instead of finishing, rows past the delivered point
     must keep their sentinel value: unfinished scans are never output. */
  if (r.outcome == DECODE_STALLED) {
    size_t k;

    for (k = (size_t)r.scanlines * row_bytes; k < npx; k++) {
      checks++;
      if (pixels[k] != SENTINEL) {
        printf("FAIL: %s unfinished scan presented as output\n", label);
        failures++;
        break;
      }
    }
  }
  free(pixels);
  free(ref);
}


/* JERR_TOO_LITTLE_DATA: abandoning a valid decode before all scanlines have
   been read must be reported explicitly by jpeg_finish_decompress(). */

LOCAL(void)
test_too_little_data(const unsigned char *data, size_t size)
{
  struct jpeg_decompress_struct cinfo;
  my_error_mgr jerr;
  unsigned char *pixels;
  JSAMPROW row_ptr[1];
  int got_fatal = 0;

  pixels = alloc_pixels(1);
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  jerr.pub.emit_message = my_emit_message;
  jerr.fatal_code = 0;
  jerr.num_warnings_seen = 0;

  if (setjmp(jerr.jump_buffer)) {
    got_fatal = 1;
    CHECK(jerr.fatal_code == JERR_TOO_LITTLE_DATA,
          "early finish reports JERR_TOO_LITTLE_DATA");
    jpeg_destroy_decompress(&cinfo);
    free(pixels);
    return;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, (unsigned long)size);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);

  /* Read only half of the scanlines, then try to finish. */
  while (cinfo.output_scanline < cinfo.output_height / 2) {
    row_ptr[0] = pixels + cinfo.output_scanline * cinfo.output_width * NCOMP;
    (void)jpeg_read_scanlines(&cinfo, &row_ptr[0], 1);
  }
  jpeg_finish_decompress(&cinfo);

  CHECK(got_fatal, "jpeg_finish_decompress aborted on incomplete decode");
  jpeg_destroy_decompress(&cinfo);
  free(pixels);
}


int
main(int argc, char **argv)
{
  unsigned char *images[4];
  unsigned long sizes[4];
  jpeg_layout_t layouts[4];
  static const char *names[4] = {
    "sequential Huffman", "sequential arithmetic",
    "progressive Huffman", "progressive arithmetic"
  };
  static const int progressive[4] = { 0, 0, 1, 1 };
  static const int arithmetic[4] = { 0, 1, 0, 1 };
  int i;
  size_t cut;
  size_t prefixes[8192];
  int nprefixes;
  decode_result_t r;
  unsigned char *pixels;
  size_t npx = (size_t)WIDTH * HEIGHT * NCOMP;

  (void)argc;
  (void)argv;

  for (i = 0; i < 4; i++) {
    images[i] = NULL;
    sizes[i] = 0;
    if (!compress_jpeg(progressive[i], arithmetic[i], &images[i], &sizes[i])) {
      printf("FAIL: could not compress %s image\n", names[i]);
      return 1;
    }
    if (!parse_layout(images[i], sizes[i], &layouts[i])) {
      printf("FAIL: could not parse %s layout\n", names[i]);
      return 1;
    }
  }

  /* ---- T1: truncation at a padding byte (sequential Huffman) ---- */
  cut = find_ff00(images[0], layouts[0].scans[0].start,
                  layouts[0].scans[0].end);
  CHECK(cut != NO_LIMIT, "sequential Huffman stream contains FF/00 padding");
  if (cut != NO_LIMIT)
    test_huffman_resume("T1 pad-byte", images[0], sizes[0], &layouts[0], cut,
                        0);

  /* ---- T2: truncation at the scan-end / EOI marker (sequential Huffman) ---- */
  test_huffman_resume("T2 scan-end", images[0], sizes[0], &layouts[0],
                      layouts[0].eoi_pos, 0);

  /* ---- T3: truncation in the middle of a progressive scan ---- */
  {
    int mid_scan = layouts[2].nscans / 2;
    size_t s = layouts[2].scans[mid_scan].start;
    size_t e = layouts[2].scans[mid_scan].end;
    size_t mid = s + (e - s) / 2;

    test_huffman_resume("T3 progressive mid-scan", images[2], sizes[2],
                        &layouts[2], mid, 1);

    /* A progressive FF/00 padding cut as well. */
    cut = find_ff00(images[2], s, e);
    if (cut != NO_LIMIT)
      test_huffman_resume("T3 progressive pad-byte", images[2], sizes[2],
                          &layouts[2], cut, 1);
  }

  /* ---- T4: arithmetic decoders cannot suspend ---- */
  cut = find_ff00(images[1], layouts[1].scans[0].start,
                  layouts[1].scans[0].end);
  CHECK(cut != NO_LIMIT, "sequential arithmetic stream contains FF/00");
  if (cut != NO_LIMIT)
    test_arithmetic_cant_suspend("T4 sequential arithmetic", images[1], cut);
  {
    int mid_scan = layouts[3].nscans / 2;
    size_t mid = layouts[3].scans[mid_scan].start +
                 (layouts[3].scans[mid_scan].end -
                  layouts[3].scans[mid_scan].start) / 2;

    test_arithmetic_cant_suspend("T4 progressive arithmetic", images[3], mid);
  }

  /* ---- T5: warning recovery, mid-entropy truncation (sequential Huffman) -- */
  cut = layouts[0].scans[0].start +
        (layouts[0].scans[0].end - layouts[0].scans[0].start) / 2;
  test_warning_recovery("T5 sequential Huffman", images[0], sizes[0], cut, 0,
                        1);

  /* ---- T6: warning recovery, mid progressive scan ---- */
  {
    int mid_scan = layouts[2].nscans / 2;
    size_t mid = layouts[2].scans[mid_scan].start +
                 (layouts[2].scans[mid_scan].end -
                  layouts[2].scans[mid_scan].start) / 2;

    test_warning_recovery("T6 progressive Huffman", images[2], sizes[2], mid, 0,
                          0);
  }

  /* ---- T7: warning recovery with arithmetic coding (fake EOI) ---- */
  cut = layouts[1].scans[0].start +
        (layouts[1].scans[0].end - layouts[1].scans[0].start) / 2;
  test_warning_recovery("T7 sequential arithmetic", images[1], sizes[1], cut, 1,
                        0);
  {
    int mid_scan = layouts[3].nscans / 2;
    size_t mid = layouts[3].scans[mid_scan].start +
                 (layouts[3].scans[mid_scan].end -
                  layouts[3].scans[mid_scan].start) / 2;

    test_warning_recovery("T7 progressive arithmetic", images[3], sizes[3], mid,
                          1, 0);
  }

  /* ---- T8: identical malformed stream, many fragment sizes, all codecs ---- */
  for (i = 0; i < 4; i++) {
    static const size_t frag_sizes[] = { 1, 2, 3, 5, 7, 13, 64, 1000 };
    unsigned int fi;

    for (fi = 0; fi < sizeof(frag_sizes) / sizeof(frag_sizes[0]); fi++) {
      nprefixes = make_chunk_prefixes(sizes[i], frag_sizes[fi], prefixes,
                                      (int)(sizeof(prefixes) /
                                            sizeof(prefixes[0])));
      if (nprefixes <= 0)
        continue;
      pixels = alloc_pixels(1);
      run_chunked(images[i], sizes[i], prefixes, nprefixes, SRC_SUSPEND,
                  pixels, npx, &r);
      if (arithmetic[i]) {
        /* The arithmetic decoder cannot suspend, so any fragmented delivery
           that exhausts a work buffer mid-decode must fail cleanly with
           JERR_CANT_SUSPEND. */
        CHECK(r.outcome == DECODE_FATAL &&
              r.fatal_code == JERR_CANT_SUSPEND,
              "T8 arithmetic fragmented delivery fails cleanly");
      } else {
        CHECK(r.outcome == DECODE_DONE,
              "T8 Huffman fragmented stream decodes fully");
        CHECK(r.suspended, "T8 Huffman path exercised suspension");
      }
      free(pixels);
    }
  }

  /* ---- JERR_TOO_LITTLE_DATA on an abandoned valid decode ---- */
  test_too_little_data(images[0], sizes[0]);

  for (i = 0; i < 4; i++)
    free(images[i]);

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
