/*
 * scantest.c
 *
 * End-to-end test for the scan-by-scan progressive JPEG preview API
 * (jpeg_read_scan_preview()).  It drives the decompressor through a custom
 * suspending data source that makes the JPEG bytes available one chunk at a
 * time, exercising the suspension/resumption machinery of every scan.
 *
 * Usage:
 *   scantest <progressive.jpg> <baseline.jpg>
 *
 * Exits with status 0 and prints "PASS" on success; any failed assertion
 * prints "FAIL: ..." and exits with status 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "jpeglib.h"
#include "jerror.h"


/* Error manager that longjmp()s back to the caller instead of exit()ing. */

struct my_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
  char message[JMSG_LENGTH_MAX];
};

METHODDEF(void)
my_error_exit(j_common_ptr cinfo)
{
  struct my_error_mgr *err = (struct my_error_mgr *)cinfo->err;

  (*cinfo->err->format_message) (cinfo, err->message);
  longjmp(err->setjmp_buffer, 1);
}


/* Suspending data source backed by a fixed, immutable memory image.
 *
 * The JPEG decoder keeps private local copies of the source pointer while
 * working through a byte span and backtracks to a restart point after a
 * suspension.  Consequently a fill routine must never re-offer bytes it has
 * already handed over, and it cannot rely on the (possibly stale) public
 * bytes_in_buffer.  A monotonic "loaded" high-water mark records the first
 * stream byte never yet exposed:
 *
 *   - fill() returns FALSE (a real suspension) while loaded has caught up to
 *     the application-supplied "fed" mark;
 *   - when new bytes [loaded, fed) exist, it exposes exactly that span and
 *     advances loaded to fed.
 *
 * The chunk sizes used by the test are large relative to the decoder's
 * bit-read backtrack depth and to any JPEG marker segment, so a restart
 * point always stays within the most recently exposed span.  This matches
 * the behavior of real streaming buffers (network packets, pipe reads).
 *
 * mode == SRC_SUSPEND:   an exhausted window keeps returning FALSE, so the
 *                        decoder waits indefinitely for more data.
 * mode == SRC_TERMINATE: once the (truncated) window is exhausted, a fake EOI
 *                        marker is returned, mirroring jdatasrc.c's premature
 *                        EOF handling and driving the TRUNCATED status.
 */

#define SRC_SUSPEND    0
#define SRC_TERMINATE  1

typedef struct {
  struct jpeg_source_mgr pub;
  const unsigned char *data;
  size_t size;
  size_t fed;       /* bytes made available by the application so far */
  size_t loaded;    /* first stream byte never yet exposed to the library */
  int mode;
  /* When TRUE, an exhausted window suspends forever even once fed reaches
   * size, modeling a connection that stalls without sending an EOF.
   */
  int never_terminate;
} susp_src_mgr;

static const JOCTET fake_eoi[2] = { (JOCTET)0xFF, (JOCTET)JPEG_EOI };


METHODDEF(void)
susp_init_source(j_decompress_ptr cinfo)
{
  (void)cinfo;
}


METHODDEF(boolean)
susp_fill_input_buffer(j_decompress_ptr cinfo)
{
  susp_src_mgr *src = (susp_src_mgr *)cinfo->src;
  struct jpeg_source_mgr *pub = &src->pub;

  if (src->loaded >= src->fed) {
    /* No newly available bytes. */
    if (src->mode == SRC_TERMINATE) {
      /* Deliberately truncated before the real EOI. */
      pub->next_input_byte = fake_eoi;
      pub->bytes_in_buffer = 2;
      return TRUE;
    }
    if (src->never_terminate || src->fed < src->size) {
      /* More data is still expected: genuine, restartable suspension. */
      pub->bytes_in_buffer = 0;
      return FALSE;
    }
    /* The whole image (real EOI included) was delivered in earlier spans; a
     * read reaching here is the decoder's defensive end-of-file path.
     */
    pub->next_input_byte = fake_eoi;
    pub->bytes_in_buffer = 2;
    return TRUE;
  }

  pub->next_input_byte = src->data + src->loaded;
  pub->bytes_in_buffer = src->fed - src->loaded;
  src->loaded = src->fed;
  return TRUE;
}


/* skip_input_data() cannot suspend.  With the test's chunk sizes a skipped
 * marker segment always fits inside the current exposed span, so an overshoot
 * simply drains the span; any remainder is naturally skipped on the next
 * fill once those bytes arrive.
 */
METHODDEF(void)
susp_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
  struct jpeg_source_mgr *pub = cinfo->src;

  if (num_bytes <= 0)
    return;
  if (num_bytes <= (long)pub->bytes_in_buffer) {
    pub->next_input_byte += (size_t)num_bytes;
    pub->bytes_in_buffer -= (size_t)num_bytes;
    return;
  }
  pub->next_input_byte += pub->bytes_in_buffer;
  pub->bytes_in_buffer = 0;
}


METHODDEF(void)
susp_term_source(j_decompress_ptr cinfo)
{
  (void)cinfo;
}


static void
install_susp_source(j_decompress_ptr cinfo, susp_src_mgr *src,
                    const unsigned char *data, size_t size, int mode)
{
  memset(src, 0, sizeof(*src));
  src->data = data;
  src->size = size;
  src->fed = 0;
  src->loaded = 0;
  src->mode = mode;
  src->never_terminate = 0;
  src->pub.init_source = susp_init_source;
  src->pub.fill_input_buffer = susp_fill_input_buffer;
  src->pub.skip_input_data = susp_skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart;
  src->pub.term_source = susp_term_source;
  src->pub.bytes_in_buffer = 0;
  src->pub.next_input_byte = data;
  cinfo->src = &src->pub;
}


static void
feed_more(susp_src_mgr *src, size_t chunk)
{
  size_t nf = src->fed + chunk;

  if (nf > src->size)
    nf = src->size;
  src->fed = nf;
}


/* Count SOS markers (0xFFDA) in a JPEG byte stream. */
static int
count_scans(const unsigned char *data, size_t size)
{
  int n = 0;
  size_t i;

  for (i = 0; i + 1 < size; i++) {
    if (data[i] == 0xFF && data[i + 1] == 0xDA)
      n++;
  }
  return n;
}


static unsigned char *
read_file(const char *path, size_t *out_size)
{
  FILE *f = fopen(path, "rb");
  long sz;
  unsigned char *buf;

  if (!f) {
    fprintf(stderr, "FAIL: cannot open %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fprintf(stderr, "FAIL: cannot size %s\n", path);
    exit(1);
  }
  buf = (unsigned char *)malloc((size_t)sz);
  if (!buf) {
    fprintf(stderr, "FAIL: out of memory\n");
    exit(1);
  }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fprintf(stderr, "FAIL: short read on %s\n", path);
    exit(1);
  }
  fclose(f);
  *out_size = (size_t)sz;
  return buf;
}


static JSAMPARRAY
alloc_output(j_decompress_ptr cinfo, unsigned char **backing_out)
{
  JDIMENSION r;
  size_t row_bytes =
    (size_t)cinfo->output_width * cinfo->output_components;
  unsigned char *backing = (unsigned char *)malloc(row_bytes *
                                                    cinfo->output_height);
  JSAMPARRAY rows =
    (JSAMPARRAY)malloc((size_t)cinfo->output_height * sizeof(JSAMPROW));

  if (!backing || !rows) {
    fprintf(stderr, "FAIL: out of memory\n");
    exit(1);
  }
  for (r = 0; r < cinfo->output_height; r++)
    rows[r] = backing + (size_t)r * row_bytes;
  *backing_out = backing;
  return rows;
}


static size_t
image_bytes(j_decompress_ptr cinfo)
{
  return (size_t)cinfo->output_width * cinfo->output_height *
         cinfo->output_components * sizeof(JSAMPLE);
}


static unsigned long long
checksum(const unsigned char *p, size_t n)
{
  unsigned long long h = 1469598103934665603ULL;
  size_t i;

  for (i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}


static void
read_header_suspending(j_decompress_ptr cinfo, susp_src_mgr *src,
                       size_t chunk)
{
  while (jpeg_read_header(cinfo, TRUE) == JPEG_SUSPENDED)
    feed_more(src, chunk);
}


/* Reference one-shot decode into a freshly allocated backing buffer. */
static unsigned char *
oneshot_decode(const unsigned char *data, size_t size, size_t *out_size)
{
  struct jpeg_decompress_struct cinfo;
  struct my_error_mgr jerr;
  unsigned char *out;
  JDIMENSION r;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  if (setjmp(jerr.setjmp_buffer)) {
    fprintf(stderr, "FAIL: reference decode: %s\n", jerr.message);
    exit(1);
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, (unsigned long)size);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    fprintf(stderr, "FAIL: reference header\n");
    exit(1);
  }
  jpeg_start_decompress(&cinfo);
  *out_size = image_bytes(&cinfo);
  out = (unsigned char *)malloc(*out_size);
  for (r = 0; r < cinfo.output_height; r++) {
    JSAMPROW row = out + (size_t)r * cinfo.output_width *
                         cinfo.output_components;
    if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
      fprintf(stderr, "FAIL: reference read_scanlines\n");
      exit(1);
    }
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return out;
}


/* Full-stream preview: every scan delivered, final image matches one-shot. */
static void
run_full_preview(const unsigned char *data, size_t size, size_t chunk,
                 int expect_scans, const unsigned char *reference,
                 size_t reference_size)
{
  struct jpeg_decompress_struct cinfo;
  struct my_error_mgr jerr;
  susp_src_mgr src;
  JSAMPARRAY rows;
  unsigned char *backing;
  size_t nbytes;
  int delivered = 0;
  int idle = 0;
  unsigned long long last_sum = 0;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  if (setjmp(jerr.setjmp_buffer)) {
    fprintf(stderr, "FAIL: library error: %s\n", jerr.message);
    exit(1);
  }

  jpeg_create_decompress(&cinfo);
  install_susp_source(&cinfo, &src, data, size, SRC_SUSPEND);
  feed_more(&src, chunk);
  read_header_suspending(&cinfo, &src, chunk);

  cinfo.scan_preview = TRUE;
  if (!jpeg_start_decompress(&cinfo)) {
    fprintf(stderr, "FAIL: start_decompress suspended\n");
    exit(1);
  }

  rows = alloc_output(&cinfo, &backing);
  nbytes = image_bytes(&cinfo);

  for (;;) {
    int st;

    feed_more(&src, chunk);
    st = jpeg_read_scan_preview(&cinfo, rows, cinfo.output_height);

    if (st == JPEG_PREVIEW_SUSPENDED) {
      /* The output buffer must be byte-identical to the last delivered
       * preview while the decoder waits for more data.
       */
      if (delivered > 0 && checksum(backing, nbytes) != last_sum) {
        fprintf(stderr, "FAIL: pixels changed while suspended\n");
        exit(1);
      }
      if (src.fed >= size) {
        if (++idle > 5) {
          fprintf(stderr, "FAIL: permanent suspension before EOI\n");
          exit(1);
        }
      }
      continue;
    }
    idle = 0;
    if (st == JPEG_PREVIEW_SCAN_READY) {
      delivered++;
      last_sum = checksum(backing, nbytes);
      continue;
    }
    if (st == JPEG_PREVIEW_EOI) {
      delivered++;
      last_sum = checksum(backing, nbytes);
      break;
    }
    fprintf(stderr, "FAIL: unexpected status %d on full stream\n", st);
    exit(1);
  }

  if (delivered != expect_scans) {
    fprintf(stderr, "FAIL: delivered %d previews, expected %d\n",
            delivered, expect_scans);
    exit(1);
  }
  if (nbytes != reference_size ||
      memcmp(backing, reference, nbytes) != 0) {
    fprintf(stderr,
            "FAIL: final preview != one-shot decode (chunk=%zu)\n", chunk);
    exit(1);
  }

  jpeg_destroy_decompress(&cinfo);
  free(backing);
  free(rows);
}


/* Truncation with a truly suspending source: once data stops, the decoder
 * must keep returning SUSPENDED and must never render the unfinished scan.
 */
static void
run_truncated_suspend(const unsigned char *data, size_t cut, size_t chunk,
                      int total_scans)
{
  struct jpeg_decompress_struct cinfo;
  struct my_error_mgr jerr;
  susp_src_mgr src;
  JSAMPARRAY rows;
  unsigned char *backing;
  int delivered = 0;
  int idle = 0;
  int st;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  if (setjmp(jerr.setjmp_buffer)) {
    fprintf(stderr, "FAIL: library error: %s\n", jerr.message);
    exit(1);
  }

  jpeg_create_decompress(&cinfo);
  /* The source window can never grow past "cut". */
  install_susp_source(&cinfo, &src, data, cut, SRC_SUSPEND);
  src.never_terminate = 1;
  feed_more(&src, chunk);
  read_header_suspending(&cinfo, &src, chunk);

  cinfo.scan_preview = TRUE;
  if (!jpeg_start_decompress(&cinfo)) {
    fprintf(stderr, "FAIL: start_decompress suspended\n");
    exit(1);
  }
  rows = alloc_output(&cinfo, &backing);

  for (;;) {
    feed_more(&src, chunk);
    st = jpeg_read_scan_preview(&cinfo, rows, cinfo.output_height);

    if (st == JPEG_PREVIEW_SCAN_READY || st == JPEG_PREVIEW_EOI) {
      delivered++;
      if (st == JPEG_PREVIEW_EOI)
        break;
      continue;
    }
    if (st == JPEG_PREVIEW_SUSPENDED) {
      if (++idle > 8)
        break;                 /* stable: still waiting for more data */
      continue;
    }
    fprintf(stderr,
            "FAIL: unexpected status %d on suspended truncation\n", st);
    exit(1);
  }

  /* A cut inside a later scan must not deliver all scans. */
  if (delivered >= total_scans) {
    fprintf(stderr,
            "FAIL: suspended truncation delivered %d of %d scans\n",
            delivered, total_scans);
    exit(1);
  }
  /* Repeated calls after exhaustion must keep suspending. */
  st = jpeg_read_scan_preview(&cinfo, rows, cinfo.output_height);
  if (st != JPEG_PREVIEW_SUSPENDED) {
    fprintf(stderr,
            "FAIL: exhausted source returned %d instead of SUSPENDED\n", st);
    exit(1);
  }

  jpeg_destroy_decompress(&cinfo);
  free(backing);
  free(rows);
}


/* Truncation where the source fabricates an EOI at the cut: the decoder must
 * report TRUNCATED and keep the last complete scan's image.
 */
static void
run_truncated_eoi(const unsigned char *data, size_t cut, size_t chunk,
                  int total_scans)
{
  struct jpeg_decompress_struct cinfo;
  struct my_error_mgr jerr;
  susp_src_mgr src;
  JSAMPARRAY rows;
  unsigned char *backing;
  int delivered = 0;
  int saw_truncated = 0;
  int idle = 0;
  int st;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  if (setjmp(jerr.setjmp_buffer)) {
    fprintf(stderr, "FAIL: library error: %s\n", jerr.message);
    exit(1);
  }

  jpeg_create_decompress(&cinfo);
  install_susp_source(&cinfo, &src, data, cut, SRC_TERMINATE);
  feed_more(&src, chunk);
  read_header_suspending(&cinfo, &src, chunk);

  cinfo.scan_preview = TRUE;
  if (!jpeg_start_decompress(&cinfo)) {
    fprintf(stderr, "FAIL: start_decompress suspended\n");
    exit(1);
  }
  rows = alloc_output(&cinfo, &backing);

  for (;;) {
    feed_more(&src, chunk);
    st = jpeg_read_scan_preview(&cinfo, rows, cinfo.output_height);

    if (st == JPEG_PREVIEW_SUSPENDED) {
      if (++idle > 8) {
        fprintf(stderr, "FAIL: EOI truncation stuck suspending\n");
        exit(1);
      }
      continue;
    }
    idle = 0;
    if (st == JPEG_PREVIEW_SCAN_READY || st == JPEG_PREVIEW_EOI) {
      delivered++;
      if (st == JPEG_PREVIEW_EOI)
        break;
      continue;
    }
    if (st == JPEG_PREVIEW_TRUNCATED) {
      saw_truncated = 1;
      break;
    }
    fprintf(stderr, "FAIL: unexpected status %d on EOI truncation\n", st);
    exit(1);
  }

  if (!saw_truncated) {
    fprintf(stderr, "FAIL: EOI-terminated truncation not reported\n");
    exit(1);
  }
  if (delivered >= total_scans) {
    fprintf(stderr,
            "FAIL: EOI truncation delivered %d >= %d scans\n",
            delivered, total_scans);
    exit(1);
  }

  jpeg_destroy_decompress(&cinfo);
  free(backing);
  free(rows);
}


/* Baseline single-scan JPEG: preview must equal one-shot output. */
static void
run_baseline(const unsigned char *data, size_t size, size_t chunk)
{
  struct jpeg_decompress_struct cinfo;
  struct my_error_mgr jerr;
  susp_src_mgr src;
  JSAMPARRAY rows;
  unsigned char *backing, *reference;
  size_t nbytes, ref_size;
  int delivered = 0;
  int idle = 0;
  int st;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  if (setjmp(jerr.setjmp_buffer)) {
    fprintf(stderr, "FAIL: baseline library error: %s\n", jerr.message);
    exit(1);
  }

  jpeg_create_decompress(&cinfo);
  install_susp_source(&cinfo, &src, data, size, SRC_SUSPEND);
  feed_more(&src, chunk);
  read_header_suspending(&cinfo, &src, chunk);

  cinfo.scan_preview = TRUE;
  if (!jpeg_start_decompress(&cinfo)) {
    fprintf(stderr, "FAIL: baseline start_decompress suspended\n");
    exit(1);
  }
  rows = alloc_output(&cinfo, &backing);
  nbytes = image_bytes(&cinfo);

  for (;;) {
    feed_more(&src, chunk);
    st = jpeg_read_scan_preview(&cinfo, rows, cinfo.output_height);
    if (st == JPEG_PREVIEW_SUSPENDED) {
      if (src.fed >= size) {
        if (++idle > 5) {
          fprintf(stderr, "FAIL: baseline permanent suspension\n");
          exit(1);
        }
      }
      continue;
    }
    if (st == JPEG_PREVIEW_SCAN_READY) {
      delivered++;
      continue;
    }
    if (st == JPEG_PREVIEW_EOI) {
      delivered++;
      break;
    }
    fprintf(stderr, "FAIL: baseline unexpected status %d\n", st);
    exit(1);
  }

  if (delivered != 1) {
    fprintf(stderr, "FAIL: baseline delivered %d times, expected 1\n",
            delivered);
    exit(1);
  }

  reference = oneshot_decode(data, size, &ref_size);
  if (ref_size != nbytes || memcmp(backing, reference, nbytes) != 0) {
    fprintf(stderr, "FAIL: baseline preview != one-shot output\n");
    exit(1);
  }

  jpeg_destroy_decompress(&cinfo);
  free(backing);
  free(rows);
  free(reference);
}


int
main(int argc, char **argv)
{
  /* Chunk sizes are large compared to the decoder's backtrack depth and to
   * any JPEG marker segment, as real streaming buffers are.  They still
   * force many suspension/resumption cycles over the course of the image.
   */
  static const size_t chunk_sizes[] = { 4096, 8192 };
  size_t size, bsize, reference_size, i;
  unsigned char *data, *bdata, *reference;
  int nscans;

  if (argc < 3) {
    fprintf(stderr, "usage: %s <progressive.jpg> <baseline.jpg>\n", argv[0]);
    return 1;
  }

  data = read_file(argv[1], &size);
  nscans = count_scans(data, size);
  if (nscans < 2) {
    fprintf(stderr, "FAIL: %s is not a multi-scan (progressive) JPEG\n",
            argv[1]);
    return 1;
  }

  reference = oneshot_decode(data, size, &reference_size);

  for (i = 0; i < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); i++)
    run_full_preview(data, size, chunk_sizes[i], nscans, reference,
                     reference_size);

  /* Cut at 60%: well past the first several scans but inside a later scan's
   * entropy data for a typical progressive layout.
   */
  run_truncated_suspend(data, size * 6 / 10, 4096, nscans);
  run_truncated_eoi(data, size * 6 / 10, 4096, nscans);

  free(reference);
  free(data);

  bdata = read_file(argv[2], &bsize);
  if (count_scans(bdata, bsize) != 1) {
    fprintf(stderr, "FAIL: %s is not a single-scan (baseline) JPEG\n",
            argv[2]);
    return 1;
  }
  run_baseline(bdata, bsize, 4096);
  free(bdata);

  printf("PASS\n");
  return 0;
}
